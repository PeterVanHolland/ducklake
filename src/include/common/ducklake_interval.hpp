//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/ducklake_interval.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {
class ClientContext;

//! An INTERVAL column is written to data files of 1.1 catalogs as a struct of its months, days and microseconds,
//! because the Parquet INTERVAL keeps milliseconds only
struct DuckLakeInterval {
	static constexpr const char *MONTHS_FIELD = "months";
	static constexpr const char *DAYS_FIELD = "days";
	static constexpr const char *MICROS_FIELD = "micros";

	static LogicalType StorageType();
	static unique_ptr<Expression> ToStorage(ClientContext &context, unique_ptr<Expression> expr);
	static unique_ptr<Expression> FromStorage(ClientContext &context, unique_ptr<Expression> expr);
	//! Replaces the casts of stored structs to INTERVAL by the conversion from storage
	static void ReplaceStorageCasts(ClientContext &context, unique_ptr<Expression> &expr);
};

} // namespace duckdb
