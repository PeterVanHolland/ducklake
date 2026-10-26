//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/ducklake_types.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "common/ducklake_version.hpp"
#include "duckdb/parser/column_list.hpp"

namespace duckdb {
struct DuckLakeColumnInfo;

class DuckLakeTypes {
public:
	static LogicalType FromString(const string &str);
	static LogicalType FromColumnInfo(const DuckLakeColumnInfo &col);
	//! Binds a DuckDB type string without catalog lookups, the types that are not built in are DuckLake types
	static LogicalType FromDuckDBString(const string &type);
	//! Binds a DuckDB type string in the context, child types it leaves unbound are DuckLake types
	static LogicalType FromDuckDBString(const string &type, ClientContext &context);
	static string ToString(const LogicalType &str);
	static void CheckSupportedType(const LogicalType &type, DuckLakeVersion version);
	static void CheckSupportedTypes(const ColumnList &columns, DuckLakeVersion version);
	//! Whether a VARIANT is nested inside a LIST, STRUCT or MAP, which the Parquet writer cannot write yet
	static bool HasNestedVariant(const LogicalType &type);
	static optional_ptr<const ColumnDefinition> GetNestedVariantColumn(const ColumnList &columns);
	static NotImplementedException NestedVariantWriteError(const string &action, const string &table_name,
	                                                       const ColumnDefinition &column);

	//! VARCHAR and BLOB, whose values can be the text NULL
	static bool IsStringType(const LogicalType &type);
	//! Nested types other than VARIANT, which DuckLake stores without child types
	static bool IsNested(const LogicalType &type);
};

} // namespace duckdb
