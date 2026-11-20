//===----------------------------------------------------------------------===//
//                         DuckDB
//
// functions/ducklake_inlined_variant.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/vector.hpp"

namespace duckdb {

//! The bytes of an inlined VARIANT: its Parquet Variant followed by the declared types of its decimals, which the
//! Parquet encoding does not hold
struct DuckLakeInlinedVariant {
	//! Writes the VARIANT rows of the input as BLOB rows
	static void Encode(Vector &input, idx_t count, Vector &result);
	//! Reads BLOB rows back into VARIANT rows
	static void Decode(Vector &input, idx_t count, Vector &result);
};

} // namespace duckdb
