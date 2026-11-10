//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_catalog_loader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/ducklake_snapshot.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "storage/ducklake_catalog_set.hpp"
#include "storage/ducklake_metadata_info.hpp"

namespace duckdb {
class DuckLakeCatalog;
class DuckLakeSchemaEntry;
class DuckLakeTransaction;

//! The schemas of a schema version, whose tables and views are loaded from the metadata as they are looked up
class DuckLakeCatalogLoader {
public:
	DuckLakeCatalogLoader(DuckLakeCatalog &catalog, DuckLakeSnapshot snapshot, vector<DuckLakeSchemaInfo> schema_infos,
	                      set<SchemaIndex> skipped_schemas);

public:
	DuckLakeCatalogSet &Schemas() {
		return schemas;
	}
	void SetSchemas(ducklake_entries_map_t schema_map);
	//! The committed table or view with the name in the schema
	optional_ptr<CatalogEntry> GetEntry(DuckLakeTransaction &transaction, DuckLakeSchemaEntry &schema,
	                                    const Identifier &name);
	//! The committed table or view with the id
	optional_ptr<CatalogEntry> GetEntryById(DuckLakeTransaction &transaction, TableIndex entry_id);
	//! Loads every table and view of the schema
	void LoadSchema(DuckLakeTransaction &transaction, DuckLakeSchemaEntry &schema);
	//! Loads every table and view
	void LoadAll(DuckLakeTransaction &transaction);
	//! The ids of the loaded tables, and whether every table and view of the snapshot is loaded
	vector<TableIndex> LoadedTableIds(bool &all_loaded);
	//! Adds the estimated memory of loaded entries
	void AddEstimatedMemory(idx_t bytes);
	//! The estimated memory of the loaded entries, which the cache reserves when it is called
	idx_t ReserveEstimatedMemory() const;

private:
	using entry_names_t = case_insensitive_map_t<vector<TableIndex>>;

	//! Loads the entries the filter selects, requires the lock
	void Load(DuckLakeTransaction &transaction, const DuckLakeCatalogLoadFilter &filter);
	//! The names of the tables and views of the schema, read once, requires the lock
	entry_names_t &EntryNames(DuckLakeTransaction &transaction, SchemaIndex schema_id);
	//! Renews the reservation of the cache once the loaded entries outgrew it
	void RefreshReservation();

private:
	//! The memory an entry of a map or set takes beyond its key
	static constexpr idx_t NODE_OVERHEAD = 48;

	DuckLakeCatalog &catalog;
	//! The snapshot the entries are loaded at
	DuckLakeSnapshot snapshot;
	//! The schemas of the snapshot, against which the paths of the tables resolve
	vector<DuckLakeSchemaInfo> schema_infos;
	//! Schemas whose parent is not visible at the snapshot, their entries are not loaded
	set<SchemaIndex> skipped_schemas;
	mutex lock;
	DuckLakeCatalogSet schemas;
	bool all_loaded = false;
	set<SchemaIndex> loaded_schemas;
	//! The ids of the tables and views of a schema by name, for the schemas that are not loaded as a whole
	map<SchemaIndex, entry_names_t> entry_names;
	//! Ids looked up before that the snapshot does not have
	set<TableIndex> missing_ids;
	atomic<idx_t> estimated_memory {0};
	mutable atomic<idx_t> reserved_memory {0};
};

} // namespace duckdb
