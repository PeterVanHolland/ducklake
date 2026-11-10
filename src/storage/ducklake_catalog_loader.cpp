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

DuckLakeCatalogLoader::entry_names_t &DuckLakeCatalogLoader::EntryNames(DuckLakeTransaction &transaction,
                                                                        SchemaIndex schema_id) {
	auto existing = entry_names.find(schema_id);
	if (existing != entry_names.end()) {
		return existing->second;
	}
	// the index is published once it is read, a failed query leaves none behind
	entry_names_t names;
	idx_t estimate = NODE_OVERHEAD;
	for (auto &entry : transaction.GetMetadataManager().GetEntryNames(snapshot, schema_id)) {
		auto &name = entry.first.GetIdentifierName();
		names[name].push_back(entry.second);
		estimate += name.size() + sizeof(TableIndex) + NODE_OVERHEAD;
	}
	AddEstimatedMemory(estimate);
	return entry_names.emplace(schema_id, std::move(names)).first->second;
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
		auto &names = EntryNames(transaction, schema_id);
		auto ids = names.find(name.GetIdentifierName());
		if (ids != names.end()) {
			for (auto &entry_id : ids->second) {
				if (!schemas.GetEntryById(entry_id)) {
					Load(transaction, DuckLakeCatalogLoadFilter::Entry(entry_id));
				}
			}
			entry = tables.GetEntry(name);
		}
	}
	// the index of the names grows the estimate as well
	RefreshReservation();
	return entry;
}

optional_ptr<CatalogEntry> DuckLakeCatalogLoader::GetEntryById(DuckLakeTransaction &transaction, TableIndex entry_id) {
	optional_ptr<CatalogEntry> entry;
	{
		lock_guard<mutex> guard(lock);
		entry = schemas.GetEntryById(entry_id);
		if (entry || all_loaded || missing_ids.find(entry_id) != missing_ids.end()) {
			return entry;
		}
		// a commit assigns ids from the next catalog id on, a snapshot without it loads any id
		bool after_snapshot = snapshot.next_catalog_id > 0 && entry_id.index >= snapshot.next_catalog_id;
		if (after_snapshot || DuckLakeTransaction::IsTransactionLocal(entry_id.index)) {
			return nullptr;
		}
		Load(transaction, DuckLakeCatalogLoadFilter::Entry(entry_id));
		entry = schemas.GetEntryById(entry_id);
		if (!entry) {
			missing_ids.insert(entry_id);
			AddEstimatedMemory(sizeof(TableIndex) + NODE_OVERHEAD);
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
		entry_names.erase(schema_id);
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
		entry_names.clear();
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
	if (!catalog.RefreshSchemaCacheMemory(snapshot.schema_version, *this)) {
		// the entry left the cache, the transactions that pinned it keep using it
		reserved_memory = bytes;
	}
}

} // namespace duckdb
