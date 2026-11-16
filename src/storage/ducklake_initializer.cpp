#include "duckdb/main/attached_database.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/path.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/storage/storage_manager.hpp"

#include "storage/ducklake_initializer.hpp"
#include "storage/ducklake_metadata_profile.hpp"
#include "storage/ducklake_catalog.hpp"
#include "rest/ducklake_rest_client.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "common/ducklake_version.hpp"
#include "metadata_manager/ducklake_metadata_manager_v1_1.hpp"
#include "metadata_manager/ducklake_metadata_manager_v2_0.hpp"
#include "metadata_manager/postgres_metadata_manager.hpp"
#include "metadata_manager/quack_metadata_manager.hpp"

namespace duckdb {

DuckLakeInitializer::DuckLakeInitializer(ClientContext &context, DuckLakeCatalog &catalog, DuckLakeOptions &options_p)
    : context(context), catalog(catalog), options(options_p) {
	InitializeDataPath();
}

string DuckLakeInitializer::GetAttachOptions() {
	vector<string> attach_options;
	const string metadata_type = catalog.MetadataType();
	// Quack does not enforce read-only attaches and newer versions reject them, the DuckLake catalog refuses writes
	bool is_quack = metadata_type == "quack" || metadata_type == "quack_scanner";
	if (options.access_mode != AccessMode::AUTOMATIC && !(is_quack && options.access_mode == AccessMode::READ_ONLY)) {
		attach_options.push_back(EnumUtil::ToString(options.access_mode));
	}
	for (auto &option : options.metadata_parameters) {
		attach_options.push_back(option.first + " " + option.second.ToSQLString());
	}
	bool user_set_type = options.metadata_parameters.find("type") != options.metadata_parameters.end();
	if (metadata_type.empty() && !user_set_type && !options.hide_metadata_catalog) {
		// open a visible metadata catalog as DuckDB explicitly so DuckDB does not warn that it is a DuckLake catalog
		attach_options.push_back("TYPE duckdb");
	}
	if (metadata_type.empty() || metadata_type == "duckdb") {
		// this is duckdb, we always do latest storage
		attach_options.push_back(StringUtil::Format("STORAGE_VERSION '%s'", "latest"));
	}
	// scope the underlying Postgres attach to the metadata schema - otherwise the postgres extension reflects
	// every schema in the database on attach, which is very slow on large or multi-tenant catalogs.
	// only done when the user explicitly set METADATA_SCHEMA - the default schema is not known until after attach.
	// an explicit META_SCHEMA (or metadata_parameters 'schema') takes precedence over this.
	bool is_postgres = metadata_type == "postgres" || metadata_type == "postgres_scanner";
	bool user_set_schema = options.metadata_parameters.find("schema") != options.metadata_parameters.end();
	if (is_postgres && !user_set_schema && !options.metadata_schema.empty()) {
		attach_options.push_back("SCHEMA " + SQLString::ToString(options.metadata_schema.GetIdentifierName()));
	}
	if (options.hide_metadata_catalog) {
		attach_options.push_back("HIDDEN true");
	}

	if (attach_options.empty()) {
		return string();
	}
	return " (" + StringUtil::Join(attach_options, ", ") + ")";
}

void DuckLakeInitializer::Initialize() {
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	if (transaction.GetMetadataManager().IsRemote()) {
		catalog.ConnectREST();
		// a remote metadata server has no metadata database to attach or migrate
		LoadExistingDuckLake(transaction);
		transaction.RefreshDataPath();
		if (options.at_clause) {
			transaction.GetSnapshot();
		}
		return;
	}
	// attach the metadata database
	AttachMetadata(transaction);
	// a relative DATA_PATH of a DuckDB file catalog lies next to the catalog file, wherever the session runs
	ResolveRelativeDataPath(transaction);

	bool has_explicit_schema = !options.metadata_schema.empty();
	if (options.metadata_schema.empty()) {
		// if the schema is not explicitly set by the user - set it to the default schema in the catalog
		options.metadata_schema = transaction.GetDefaultSchemaName();
	}
	// the metadata database would truncate a longer name, and the catalog is then not found under its name
	auto max_name_length = transaction.GetMetadataManager().MaxIdentifierLength();
	if (options.metadata_schema.GetIdentifierName().size() > max_name_length) {
		throw InvalidInputException("METADATA_SCHEMA %s is longer than the %d bytes that the metadata database "
		                            "allows for a name",
		                            options.metadata_schema, max_name_length);
	}
	// after the metadata database is attached initialize the ducklake
	// check if we are loading an existing DuckLake or creating a new one
	// directly query a known ducklake metadata table to avoid scanning all attached catalogs via duckdb_tables()
	// this prevents a corrupted ducklake catalog from blocking initialization of unrelated ducklake databases
	// FIXME: verify that all ducklake tables are in the correct format
	if (transaction.GetMetadataManager().MetadataExists()) {
		LoadExistingDuckLakeWithRetries(transaction);
	} else {
		if (!options.create_if_not_exists) {
			throw InvalidInputException("Existing DuckLake at metadata catalog \"%s\" does not exist - and creating a "
			                            "new DuckLake is explicitly disabled",
			                            options.metadata_path);
		}
		catalog.VerifyWritable();
		try {
			InitializeNewDuckLake(transaction, has_explicit_schema);
		} catch (std::exception &ex) {
			// another attach may have created the same DuckLake at the same time
			ErrorData error(ex);
			RestartMetadataTransaction(transaction);
			if (!DuckLakeIsInitialized(transaction)) {
				error.Throw();
			}
			LoadExistingDuckLakeWithRetries(transaction);
		}
	}
	// the transaction of the attach copied the data path before it was loaded or resolved
	transaction.RefreshDataPath();
	auto &current_metadata_manager = transaction.GetMetadataManager();
	// probe the metadata server for optional capabilities (e.g. server-side commit retries) once per attach
	current_metadata_manager.ProbeServerCapabilities();
	if (metadata_cache_stale) {
		current_metadata_manager.ClearCache();
	}
	if (options.at_clause) {
		// if the user specified a snapshot try to load it to trigger an error if it does not exist
		transaction.GetSnapshot();
	}
}

static bool PermanentLoadError(const ErrorData &error) {
	switch (error.Type()) {
	case ExceptionType::INVALID_INPUT:
	case ExceptionType::INVALID_CONFIGURATION:
	case ExceptionType::NOT_IMPLEMENTED:
	case ExceptionType::BINDER:
		return true;
	default:
		return false;
	}
}

void DuckLakeInitializer::LoadExistingDuckLakeWithRetries(DuckLakeTransaction &transaction) {
	// wait for a migration of another attach, whose errors we cannot tell from a real failure
	static constexpr idx_t MAX_WAIT_MS = 3000;
	auto retry_config = DuckLakeRetryConfig::FromContext(context);
	ErrorData first_error;
	idx_t waited_ms = 0;
	for (idx_t attempt = 0;; attempt++) {
		try {
			LoadExistingDuckLake(transaction);
			return;
		} catch (std::exception &ex) {
			ErrorData error(ex);
			// another attach migrating at the same time can make a migration fail with any error
			if (PermanentLoadError(error) && !migrating) {
				error.Throw();
			}
			if (attempt == 0) {
				first_error = error;
			}
		}
		if (waited_ms >= MAX_WAIT_MS || attempt >= retry_config.max_retry_count) {
			first_error.Throw();
		}
		auto wait_ms = retry_config.WaitMs(attempt, MAX_WAIT_MS - waited_ms);
#ifndef DUCKDB_NO_THREADS
		ThreadUtil::SleepMs(wait_ms, context);
#endif
		waited_ms += wait_ms;
		RestartMetadataTransaction(transaction);
	}
}

void DuckLakeInitializer::RestartMetadataTransaction(DuckLakeTransaction &transaction) {
	transaction.Rollback();
	AttachMetadata(transaction);
}

void DuckLakeInitializer::AttachMetadata(DuckLakeTransaction &transaction) {
	// ATTACH OR REPLACE may reuse an attached catalog and its cached table definitions.
	if (DatabaseManager::Get(context).GetDatabase(catalog.MetadataDatabaseName())) {
		metadata_cache_stale = true;
	}
	const string attach_query =
	    "ATTACH OR REPLACE {METADATA_PATH} AS {METADATA_CATALOG_NAME_IDENTIFIER}" + GetAttachOptions();
	auto result = transaction.GetMetadataManager().AttachMetadata(attach_query);
	result->ThrowIfError("Failed to attach DuckLake MetaData \"" + catalog.MetadataDatabaseName() + "\" at path + \"" +
	                     catalog.MetadataPath() + "\"");
	// the extension of the metadata database registers its profile when the ATTACH loads it
	auto catalog_type = transaction.GetMetadataCatalog().GetCatalogType();
	catalog.SetMetadataCatalogType(catalog_type);
	catalog.SetMetadataProfile(DuckLakeMetadataProfiles::Get(*context.db).Find(catalog_type));
	// the connection was opened before the profile was known
	DuckLakeUtil::ResetExtensionSettings(*transaction.GetConnection().context,
	                                     catalog.MetadataProfile().reset_settings);
	// a rollback of the transaction can undo the attach and the creation or migration of the catalog
	transaction.MarkMetadataWritten();
}

bool DuckLakeInitializer::DuckLakeIsInitialized(DuckLakeTransaction &transaction) {
	auto &metadata_manager = transaction.GetMetadataManager();
	if (!metadata_manager.MetadataExists()) {
		return false;
	}
	for (auto &tag : metadata_manager.LoadDuckLake().tags) {
		if (tag.key == "version") {
			return true;
		}
	}
	// LCOV_EXCL_START
	return false;
	// LCOV_EXCL_STOP
}

void DuckLakeInitializer::InitializeDataPath() {
	auto &data_path = options.data_path;
	if (data_path.empty()) {
		return;
	}
	ExtensionHelper::AutoLoadExtensionForPath(*context.db, data_path, "Data path");

	auto &fs = FileSystem::GetFileSystem(context);
	auto separator = DuckLakeUtil::PathSeparator(fs, data_path);
	StringUtil::RTrim(data_path, "/\\");
	// ensure the paths we store always end in a path separator
	data_path += separator;
	catalog.Separator() = separator;
}

static bool HasPathScheme(const string &path) {
	return path.find("://") != string::npos || StringUtil::StartsWith(path, "file:");
}

//! Whether the file system would resolve the path against the working directory
static bool IsRelativeLocalPath(FileSystem &fs, const string &path) {
	// a leading separator starts an absolute or a network path on every platform, a drive letter a Windows path
	if (path.empty() || path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':') ||
	    HasPathScheme(path) || StringUtil::StartsWith(path, "~")) {
		return false;
	}
	return !fs.IsPathAbsolute(path);
}

//! Whether two paths name the same directory, one of them possibly relative to the working directory
static bool SameLocalPath(FileSystem &fs, const string &a, const string &b) {
	if (HasPathScheme(a) || HasPathScheme(b)) {
		return false;
	}
	try {
		return fs.CanonicalizePath(a) == fs.CanonicalizePath(b);
	} catch (std::exception &) {
		return false;
	}
}

string DuckLakeInitializer::CatalogDirectory(DuckLakeTransaction &transaction) {
	if (catalog.MetadataType() == "rest") {
		return string();
	}
	auto &metadata_catalog = Catalog::GetCatalog(*transaction.GetConnection().context, options.metadata_database);
	if (!metadata_catalog.IsDuckCatalog()) {
		return string();
	}
	auto &storage = metadata_catalog.GetAttached().GetStorageManager();
	if (storage.InMemory()) {
		return string();
	}
	auto path = storage.GetDBPath();
	// a catalog file on remote storage keeps its paths as they are
	if (HasPathScheme(path)) {
		return string();
	}
	auto &fs = FileSystem::GetFileSystem(context);
	return path.substr(0, path.size() - fs.ExtractName(path).size());
}

string DuckLakeInitializer::ResolvedDataPath(DuckLakeTransaction &transaction, const string &data_path) {
	auto &fs = FileSystem::GetFileSystem(context);
	if (!IsRelativeLocalPath(fs, data_path)) {
		return data_path;
	}
	auto directory = CatalogDirectory(transaction);
	if (directory.empty()) {
		return data_path;
	}
	// the file system reports written files under the normalized form of their directory
	auto resolved = Path::FromString(directory + data_path).ToString();
	// a lake created before stored the path relative to the working directory, where it is kept when only that exists
	try {
		if (!fs.DirectoryExists(resolved) && fs.DirectoryExists(data_path)) {
			return data_path;
		}
	} catch (std::exception &) {
		// a directory the session may not look at is not the one of a lake created before
	}
	return resolved;
}

void DuckLakeInitializer::ResolveRelativeDataPath(DuckLakeTransaction &transaction) {
	auto resolved = ResolvedDataPath(transaction, options.data_path);
	if (resolved == options.data_path) {
		return;
	}
	if (options.stored_data_path.empty()) {
		options.stored_data_path = options.data_path;
	}
	options.data_path = std::move(resolved);
}

void DuckLakeInitializer::InitializeNewDuckLake(DuckLakeTransaction &transaction, bool has_explicit_schema) {
	metadata_cache_stale = true;
	// without a version in the ATTACH, the ducklake_default_version setting or else the latest version
	auto version = options.ducklake_version;
	Value setting_val;
	if (version == DuckLakeVersion::UNSET && context.TryGetCurrentSetting("ducklake_default_version", setting_val) &&
	    !setting_val.IsNull()) {
		version = ParseWritableDuckLakeVersion(setting_val.ToString(), "ducklake_default_version");
	}
	if (version == DuckLakeVersion::UNSET) {
		version = DUCKLAKE_LATEST_VERSION;
	}
	if (options.data_path.empty()) {
		auto &metadata_catalog = Catalog::GetCatalog(*transaction.GetConnection().context, options.metadata_database);
		if (!metadata_catalog.IsDuckCatalog()) {
			throw InvalidInputException(
			    "Attempting to create a new ducklake instance but data_path is not set - set the "
			    "DATA_PATH parameter to the desired location of the data files");
		}
		// for DuckDB instances - use a default data path next to the catalog file
		auto path = metadata_catalog.GetAttached().GetStorageManager().GetDBPath();
		options.data_path = path + ".files";
		auto directory = CatalogDirectory(transaction);
		if (!directory.empty()) {
			// stored relative to the catalog file, so that the lake can be moved as a whole
			auto name = path.substr(directory.size()) + ".files/";
			// a "./" prefix for a name the file system would not read as a relative path, such as a leading "~"
			options.stored_data_path =
			    IsRelativeLocalPath(FileSystem::GetFileSystem(context), name) ? name : "./" + name;
		}
		InitializeDataPath();
	}
	if (!options.stored_data_path.empty()) {
		// the directory next to the catalog file marks where the stored relative path resolves to from now on
		FileSystem::GetFileSystem(context).CreateDirectoriesRecursive(options.data_path);
	}
	SetVersionedMetadataManager(transaction, version);
	auto &metadata_manager = transaction.GetMetadataManager();
	metadata_manager.InitializeDuckLake(has_explicit_schema, catalog.Encryption());
	if (catalog.Encryption() == DuckLakeEncryption::AUTOMATIC) {
		// default to unencrypted
		catalog.SetEncryption(DuckLakeEncryption::UNENCRYPTED);
	}
}

void DuckLakeInitializer::LoadExistingDuckLake(DuckLakeTransaction &transaction) {
	migrating = false;
	// load the data path from the existing duck lake
	auto &metadata_manager = transaction.GetMetadataManager();
	auto metadata = metadata_manager.LoadDuckLake();
	DuckLakeVersion resolved_version = DuckLakeVersion::UNSET;
	for (auto &tag : metadata.tags) {
		if (tag.key == "version" && metadata_manager.IsRemote()) {
			// the server takes care of migrating the metadata
			resolved_version = DuckLakeVersionFromString(tag.value);
		} else if (tag.key == "version") {
			auto catalog_version = DuckLakeVersionFromString(tag.value);
			auto target_version = ResolveTargetVersion(catalog_version, tag.value);
			if (catalog_version > target_version) {
				// catalog is newer than the requested version, no FWC
				throw InvalidInputException(
				    "DuckLake catalog version is '%s', which is newer than the requested version '%s'. "
				    "Cannot downgrade a DuckLake catalog.",
				    tag.value, DuckLakeVersionToString(target_version));
			}
			if (catalog_version < target_version && !options.automatic_migration) {
				throw InvalidInputException(
				    "DuckLake catalog version mismatch: catalog version is %s, but the extension requires version "
				    "%s. To automatically migrate, set AUTOMATIC_MIGRATION to TRUE when attaching.",
				    tag.value, DuckLakeVersionToString(target_version));
			}
			if (catalog_version < target_version) {
				catalog.VerifyWritable();
				metadata_cache_stale = true;
			}
			migrating = catalog_version < target_version;
			if (catalog_version == DuckLakeVersion::V0_1) {
				metadata_manager.MigrateV01();
				catalog_version = DuckLakeVersion::V0_2;
			}
			if (catalog_version == DuckLakeVersion::V0_2) {
				metadata_manager.MigrateV02();
				catalog_version = DuckLakeVersion::V0_3;
			}
			if (catalog_version == DuckLakeVersion::V0_3_DEV1) {
				metadata_manager.MigrateV02(true);
				catalog_version = DuckLakeVersion::V0_3;
			}
			if (catalog_version == DuckLakeVersion::V0_3) {
				metadata_manager.MigrateV03();
				catalog_version = DuckLakeVersion::V0_4;
			}
			if (catalog_version == DuckLakeVersion::V0_4_DEV1) {
				metadata_manager.MigrateV03(true);
				catalog_version = DuckLakeVersion::V0_4;
			}
			if (catalog_version == DuckLakeVersion::V0_4) {
				metadata_manager.MigrateV04();
				catalog_version = DuckLakeVersion::V1_0;
			}
			if (catalog_version >= target_version) {
				migrating = false;
				resolved_version = catalog_version;
				continue;
			}
			if (catalog_version == DuckLakeVersion::V1_0 || catalog_version == DuckLakeVersion::V1_1_DEV_1) {
				metadata_manager.MigrateV10(catalog_version == DuckLakeVersion::V1_1_DEV_1);
				catalog_version = DuckLakeVersion::V1_1;
			}
			if (catalog_version == DuckLakeVersion::V1_1 && catalog_version < target_version) {
				metadata_manager.MigrateV11();
				catalog_version = DuckLakeVersion::V2_0_DEV1;
			}
			migrating = false;
			if (catalog_version != target_version) {
				// LCOV_EXCL_START
				throw NotImplementedException("Unsupported DuckLake version '%s'",
				                              DuckLakeVersionToString(catalog_version));
				// LCOV_EXCL_STOP
			}
			resolved_version = catalog_version;
		}
		if (tag.key == "data_path") {
			if (options.data_path.empty()) {
				options.data_path = metadata_manager.LoadPath(tag.value);
				InitializeDataPath();
				ResolveRelativeDataPath(transaction);
			} else {
				// the client of a REST catalog can have another view of the data path than the server
				bool is_rest = catalog.MetadataType() == "rest";
				if (is_rest && options.data_path != metadata_manager.LoadPath(tag.value)) {
					catalog.GetRESTClient().SetPathMapping(options.data_path, metadata_manager.LoadPath(tag.value));
				}
				// verify that they match if override_data_path is not set to true, a stored relative path also by
				// where it resolves to
				if (!options.override_data_path && !is_rest) {
					auto given_path = metadata_manager.StorePath(options.data_path);
					auto resolved_path = ResolvedDataPath(transaction, metadata_manager.LoadPath(tag.value));
					if (given_path != tag.value && given_path != metadata_manager.StorePath(resolved_path)) {
						if (!SameLocalPath(FileSystem::GetFileSystem(context), options.data_path, resolved_path)) {
							throw InvalidConfigurationException(
							    "DATA_PATH parameter \"%s\" does not match existing data path in the catalog "
							    "\"%s\".\nYou "
							    "can override the DATA_PATH by setting OVERRIDE_DATA_PATH to True.",
							    options.data_path, tag.value);
						}
						// another spelling of the stored directory, the session uses the stored one
						options.data_path = std::move(resolved_path);
					}
				}
			}
		}
		if (tag.key == "encrypted") {
			if (tag.value == "true") {
				catalog.SetEncryption(DuckLakeEncryption::ENCRYPTED);
			} else if (tag.value == "false") {
				catalog.SetEncryption(DuckLakeEncryption::UNENCRYPTED);
			} else {
				throw NotImplementedException("Encrypted should be either true or false");
			}
		}
		options.config_options[tag.key] = tag.value;
	}
	for (auto &entry : metadata.schema_settings) {
		options.schema_options[entry.schema_id][entry.tag.key] = entry.tag.value;
	}
	for (auto &entry : metadata.table_settings) {
		options.table_options[entry.table_id][entry.tag.key] = entry.tag.value;
	}
	// set correct version metadata manager
	if (resolved_version != DuckLakeVersion::UNSET) {
		SetVersionedMetadataManager(transaction, resolved_version);
	}
}

DuckLakeVersion DuckLakeInitializer::ResolveTargetVersion(DuckLakeVersion catalog_version,
                                                          const string &catalog_version_str) {
	if (options.ducklake_version != DuckLakeVersion::UNSET) {
		// If the user pinned a version, we use that
		return options.ducklake_version;
	}
	if (options.automatic_migration) {
		// If automatic_migration is on, use to latest
		return DUCKLAKE_LATEST_VERSION;
	}
	if (DuckLakeVersionIsSupported(catalog_version)) {
		// otherwise, use the catalog's current version
		return catalog_version;
	}
	// pre-1.0 and dev catalogs always require migration
	throw InvalidInputException("DuckLake catalog version mismatch: catalog version is %s, but the extension requires "
	                            "version %s. To automatically migrate, set AUTOMATIC_MIGRATION to TRUE when attaching.",
	                            catalog_version_str, DuckLakeVersionToString(DUCKLAKE_LATEST_VERSION));
}

//! The versioned manager for the metadata backend of the transaction
template <template <class> class MANAGER>
static unique_ptr<DuckLakeMetadataManager> MakeVersionedManager(DuckLakeTransaction &transaction) {
	auto &current = transaction.GetMetadataManager();
	if (dynamic_cast<QuackMetadataManager *>(&current)) {
		return make_uniq<MANAGER<QuackMetadataManager>>(transaction);
	}
	if (dynamic_cast<PostgresMetadataManager *>(&current)) {
		return make_uniq<MANAGER<PostgresMetadataManager>>(transaction);
	}
	return make_uniq<MANAGER<DuckLakeMetadataManager>>(transaction);
}

void DuckLakeInitializer::SetVersionedMetadataManager(DuckLakeTransaction &transaction, DuckLakeVersion version) {
	catalog.SetDuckLakeVersion(version);
	if (version == DuckLakeVersion::V1_0) {
		// base metadata managers are already V1.0, nop
		return;
	}
	if (transaction.GetMetadataManager().IsRemote()) {
		// the server takes care of the metadata format
		return;
	}
	unique_ptr<DuckLakeMetadataManager> new_manager;
	if (version == DuckLakeVersion::V1_1) {
		new_manager = MakeVersionedManager<DuckLakeMetadataManagerV1_1>(transaction);
	} else if (version == DuckLakeVersion::V2_0_DEV1) {
		new_manager = MakeVersionedManager<DuckLakeMetadataManagerV2_0>(transaction);
	} else {
		throw InternalException("SetVersionedMetadataManager: unsupported version");
	}
	transaction.SetMetadataManager(std::move(new_manager));
}

} // namespace duckdb
