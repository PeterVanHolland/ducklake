//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_inlined_data_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file/base_file_reader.hpp"
#include "storage/ducklake_inlined_data.hpp"
#include "common/ducklake_snapshot.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {
class DuckLakeFieldData;
class DuckLakeMetadataManager;
struct DuckLakeFunctionInfo;
struct DuckLakeInlinedColNames;

enum class InlinedVirtualColumn { NONE, COLUMN_ROW_ID, COLUMN_FILE_ROW_NUMBER, COLUMN_EMPTY };

//! Extracts the field a pushed-down extract reads from the whole column
struct InlinedFieldExtract {
	unique_ptr<Expression> expression;
	unique_ptr<ExpressionExecutor> executor;
};

class DuckLakeInlinedDataReader : public BaseFileReader {
public:
	//! Initialize an inlined data reader over a set of data stored within a table in the metadata catalog
	DuckLakeInlinedDataReader(DuckLakeFunctionInfo &read_info, const OpenFileInfo &info, Identifier table_name,
	                          vector<MultiFileColumnDefinition> columns);
	//! Initialize an inlined data reader over a set of data
	DuckLakeInlinedDataReader(DuckLakeFunctionInfo &read_info, const OpenFileInfo &info,
	                          shared_ptr<DuckLakeInlinedData> data, vector<MultiFileColumnDefinition> columns);

public:
	bool TryInitializeScan(ClientContext &context, GlobalTableFunctionState &gstate,
	                       LocalTableFunctionState &lstate) override;
	AsyncResult Scan(ClientContext &context, GlobalTableFunctionState &global_state,
	                 LocalTableFunctionState &local_state, DataChunk &chunk) override;

	string GetReaderType() const override;

	void AddVirtualColumn(column_t virtual_column_id) override;

	//! Scan rows read before instead of querying the inlined table
	void SetCommittedRows(shared_ptr<DuckLakeCommittedInlinedRows> rows);

	//! Read the columns of a committed inlined table, and the next file id of the catalog when it has rows
	static shared_ptr<DuckLakeCommittedInlinedRows> ReadCommittedRows(DuckLakeFunctionInfo &read_info,
	                                                                  const Identifier &table_name,
	                                                                  const vector<MultiFileColumnDefinition> &columns,
	                                                                  optional_idx &next_file_id);

private:
	static void GetColumnToRead(const DuckLakeFunctionInfo &read_info, DuckLakeMetadataManager &metadata_manager,
	                            const DuckLakeInlinedColNames &col_names, const MultiFileColumnDefinition &col,
	                            string &column, LogicalType &type);
	bool TryEvaluateExpression(ClientContext &context, idx_t virtual_col_idx, Vector &input_vector,
	                           const LogicalType &input_type, Vector &output_vector);
	void AddFieldExtract(ClientContext &context, idx_t column_idx, const LogicalType &column_type, idx_t scan_idx);
	//! Apply the filters on the columns that are not field extracts
	idx_t ApplyFilters(ClientContext &context, DataChunk &chunk, SelectionVector &sel, idx_t approved_tuple_count);
	void ComputeFieldExtract(ClientContext &context, idx_t column_idx, DataChunk &chunk);

	mutex lock;
	DuckLakeFunctionInfo &read_info;
	Identifier table_name;
	shared_ptr<DuckLakeInlinedData> data;
	shared_ptr<DuckLakeCommittedInlinedRows> committed_rows;
	bool initialized_scan = false;
	vector<InlinedVirtualColumn> virtual_columns;
	int64_t file_row_number = 0;
	vector<column_t> scan_column_ids;
	ColumnDataScanState state;
	DataChunk scan_chunk;
	//! Expression executors for expression_map entries, keyed by column_t (from column_ids)
	unordered_map<column_t, unique_ptr<ExpressionExecutor>> expression_executors;
	//! Field extracts of pushed-down extracts, keyed by output column
	unordered_map<idx_t, InlinedFieldExtract> field_extracts;
};

} // namespace duckdb
