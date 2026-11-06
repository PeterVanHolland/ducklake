//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_delete_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/types/validity_mask.hpp"
#include "storage/ducklake_metadata_info.hpp"

namespace duckdb {

struct DuckLakeDeleteData {
	//! The sorted positions of the rows to skip
	vector<idx_t> deleted_rows;
	vector<idx_t> snapshot_ids;
	//! The rows a deletion scan returns, one bit per row of the data file, unset when it returns every row
	ValidityMask scan_rows;
	//! The sorted positions whose deleting snapshot differs from the snapshot of the file, used in deletion scans
	vector<idx_t> scan_snapshot_positions;
	//! The deleting snapshot of each position in scan_snapshot_positions
	vector<idx_t> scan_snapshot_ids;

	idx_t Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel,
	             optional_idx snapshot_filter = optional_idx()) const;

	idx_t FilterScanRows(row_t start_row_index, idx_t count, SelectionVector &result_sel) const;

	bool HasEmbeddedSnapshots() const;

	bool HasScanSnapshots() const;

	optional_idx GetSnapshotForPosition(idx_t position) const;
};

struct DeleteFileScanResult {
	vector<idx_t> deleted_rows;
	vector<idx_t> snapshot_ids;
	//! Whether the delete file has embedded snapshot_id column
	bool has_embedded_snapshots = false;
};

class DuckLakeDeleteFilter : public DeleteFilter {
public:
	DuckLakeDeleteFilter();

	shared_ptr<DuckLakeDeleteData> delete_data;

	optional_idx snapshot_filter;

	idx_t Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel) override;
	void Initialize(ClientContext &context, const DuckLakeFileData &delete_file);
	void Initialize(const DuckLakeInlinedDataDeletes &inlined_deletes);
	void Initialize(ClientContext &context, const DuckLakeDeleteScanEntry &delete_scan);
	void SetSnapshotFilter(idx_t snapshot_filter);

	static DeleteFileScanResult ScanDeleteFile(ClientContext &context, const DuckLakeFileData &delete_file,
	                                           optional_idx snapshot_filter_min = optional_idx(),
	                                           optional_idx snapshot_filter_max = optional_idx());
	//! Scan a puffin deletion vector file and return the deleted row positions
	static DeleteFileScanResult ScanDeletionVectorFile(ClientContext &context, const DuckLakeFileData &delete_file,
	                                                   optional_idx snapshot_filter_min = optional_idx(),
	                                                   optional_idx snapshot_filter_max = optional_idx());
};

} // namespace duckdb
