//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/ducklake_snapshot.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class Serializer;
class Deserializer;

struct DuckLakeSnapshot {
	DuckLakeSnapshot(idx_t snapshot_id, idx_t schema_version, idx_t next_catalog_id, idx_t next_file_id)
	    : snapshot_id(snapshot_id), schema_version(schema_version), next_catalog_id(next_catalog_id),
	      next_file_id(next_file_id) {
	}

	DuckLakeSnapshot()
	    : snapshot_id(DConstants::INVALID_INDEX), schema_version(DConstants::INVALID_INDEX),
	      next_catalog_id(DConstants::INVALID_INDEX), next_file_id(DConstants::INVALID_INDEX) {
	}

	idx_t snapshot_id;
	idx_t schema_version;
	idx_t next_catalog_id;
	idx_t next_file_id;

	bool operator==(const DuckLakeSnapshot &other) const {
		return snapshot_id == other.snapshot_id && schema_version == other.schema_version &&
		       next_catalog_id == other.next_catalog_id && next_file_id == other.next_file_id;
	}
	bool operator!=(const DuckLakeSnapshot &other) const {
		return !(*this == other);
	}

	void Serialize(Serializer &serializer) const;
	static DuckLakeSnapshot Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
