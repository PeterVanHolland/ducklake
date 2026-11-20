//===----------------------------------------------------------------------===//
//                         DuckDB
//
// rest/ducklake_rest_arrow.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {
class ClientContext;

//! The feature of the config of a server that sends and reads VARIANT columns as DuckLake variant bytes
static constexpr const char *REST_FEATURE_VARIANT_BYTES = "variant_bytes";
//! The parameter of the Accept media type with which a client asks for variant bytes
static constexpr const char *REST_ACCEPT_VARIANT_BYTES = "variant=ducklake";
//! The Arrow extension name of a BLOB field that holds variant bytes
static constexpr const char *REST_VARIANT_EXTENSION = "ducklake.variant";

//! The tables that the REST API sends as Arrow IPC streams: the rows of the columns of a table
struct RESTArrow {
	//! The Arrow IPC stream of rows. Types that Arrow does not have (UUID, HUGEINT, GEOMETRY, ...) are extension types.
	//! With variant_bytes, a VARIANT travels as a BLOB of its DuckLake variant bytes, which keep the declared types of
	//! its decimals, instead of as the Parquet Variant extension type
	static string Write(ClientContext &context, const vector<string> &names, const ColumnDataCollection &rows,
	                    bool variant_bytes = false);
	//! Reads the rows of an Arrow IPC stream from a region of the data. The columns are cast to the types, if there
	//! are any
	static unique_ptr<ColumnDataCollection> Read(ClientContext &context, shared_ptr<const string> data, idx_t offset,
	                                             idx_t size, const vector<LogicalType> &types, vector<string> &names);
};

} // namespace duckdb
