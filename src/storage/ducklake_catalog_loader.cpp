#include "storage/ducklake_catalog_loader.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

DuckLakeCatalogLoader::DuckLakeCatalogLoader(DuckLakeCatalog &catalog, DuckLakeSnapshot snapshot,
                                             vector<DuckLakeSchemaInfo> schema_infos_p,
                                             set<SchemaIndex> skipped_schemas_p)
    : catalog(catalog), snapshot(snapshot), schema_infos(std::move(schema_infos_p)),
      skipped_schemas(std::move(skipped_schemas_p)) {
}

void DuckLakeCatalogLoader::SetSchemas(ducklake_entries_map_t schema_map) {
	schemas = DuckLakeCatalogSet(std::move(schema_map));
}

void DuckLakeCatalogLoader::Load(DuckLakeTransaction &transaction, const DuckLakeCatalogLoadFilter &filter) {
	DuckLakeCatalogInfo info;
	info.schemas = schema_infos;
	transaction.GetMetadataManager().LoadCatalogEntries(info, snapshot, filter);
	AddEstimatedMemory(catalog.AddCatalogEntries(schemas, skipped_schemas, info));
}

optional_ptr<CatalogEntry> DuckLakeCatalogLoader::GetEntry(DuckLakeTransaction &transaction,
                                                           DuckLakeSchemaEntry &schema, const Identifier &name) {
	optional_ptr<CatalogEntry> entry;
	{
		lock_guard<mutex> guard(lock);
		auto &tables = schema.GetCatalogSet(CatalogType::TABLE_ENTRY);
		entry = tables.GetEntry(name);
		auto schema_id = schema.GetSchemaId();
		if (entry || all_loaded || loaded_schemas.find(schema_id) != loaded_schemas.end()) {
			return entry;
		}
		auto &missing = missing_names[schema_id];
		auto &name_string = name.GetIdentifierName();
		if (missing.find(name_string) != missing.end()) {
			return nullptr;
		}
		for (auto &entry_id : transaction.GetMetadataManager().GetEntryIdsByName(snapshot, schema_id, name)) {
			if (!schemas.GetEntryById(entry_id)) {
				Load(transaction, DuckLakeCatalogLoadFilter::Entry(entry_id));
			}
		}
		entry = tables.GetEntry(name);
		if (!entry) {
			missing.insert(name_string);
		}
	}
	RefreshReservation();
	return entry;
}

optional_ptr<CatalogEntry> DuckLakeCatalogLoader::GetEntryById(DuckLakeTransaction &transaction, TableIndex entry_id) {
	optional_ptr<CatalogEntry> entry;
	{
		lock_guard<mutex> guard(lock);
		entry = schemas.GetEntryById(entry_id);
		if (entry || all_loaded || DuckLakeTransaction::IsTransactionLocal(entry_id.index) ||
		    missing_ids.find(entry_id) != missing_ids.end()) {
			return entry;
		}
		Load(transaction, DuckLakeCatalogLoadFilter::Entry(entry_id));
		entry = schemas.GetEntryById(entry_id);
		if (!entry) {
			missing_ids.insert(entry_id);
		}
	}
	RefreshReservation();
	return entry;
}

void DuckLakeCatalogLoader::LoadSchema(DuckLakeTransaction &transaction, DuckLakeSchemaEntry &schema) {
	{
		lock_guard<mutex> guard(lock);
		auto schema_id = schema.GetSchemaId();
		if (all_loaded || loaded_schemas.find(schema_id) != loaded_schemas.end()) {
			return;
		}
		Load(transaction, DuckLakeCatalogLoadFilter::OfSchema(schema_id));
		loaded_schemas.insert(schema_id);
		missing_names.erase(schema_id);
	}
	RefreshReservation();
}

void DuckLakeCatalogLoader::LoadAll(DuckLakeTransaction &transaction) {
	{
		lock_guard<mutex> guard(lock);
		if (all_loaded) {
			return;
		}
		Load(transaction, DuckLakeCatalogLoadFilter::All());
		all_loaded = true;
		loaded_schemas.clear();
		missing_names.clear();
		missing_ids.clear();
	}
	RefreshReservation();
}

vector<TableIndex> DuckLakeCatalogLoader::LoadedEntryIds(bool &all_loaded_p) {
	lock_guard<mutex> guard(lock);
	vector<TableIndex> ids;
	for (auto &entry : schemas.GetTableIdMap()) {
		ids.push_back(entry.first);
	}
	all_loaded_p = all_loaded;
	return ids;
}

void DuckLakeCatalogLoader::AddEstimatedMemory(idx_t bytes) {
	estimated_memory += bytes;
}

idx_t DuckLakeCatalogLoader::ReserveEstimatedMemory() const {
	auto bytes = estimated_memory.load();
	reserved_memory = bytes;
	return bytes;
}

void DuckLakeCatalogLoader::RefreshReservation() {
	auto bytes = estimated_memory.load();
	if (bytes < 2 * reserved_memory.load()) {
		return;
	}
	if (!catalog.RefreshSchemaCacheMemory(snapshot.schema_version)) {
		// the entry left the cache, the transactions that pinned it keep using it
		reserved_memory = bytes;
	}
}

} // namespace duckdb
