#include "storage/ducklake_delete_filter.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "storage/ducklake_deletion_vector.hpp"
#include "storage/ducklake_puffin.hpp"
#include "common/parquet_file_scanner.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"

namespace duckdb {

DuckLakeDeleteFilter::DuckLakeDeleteFilter() : delete_data(make_shared_ptr<DuckLakeDeleteData>()) {
}

idx_t DuckLakeDeleteData::FilterScanRows(row_t start_row_index, idx_t count, SelectionVector &result_sel) const {
	// return the rows of this range whose bit is set, skipping empty entries of 64 rows
	auto start = NumericCast<idx_t>(start_row_index);
	auto end = start + count;
	result_sel.Initialize(STANDARD_VECTOR_SIZE);
	idx_t result_count = 0;
	idx_t row = start;
	while (row < end) {
		idx_t entry_idx, idx_in_entry;
		ValidityMask::GetEntryIndex(row, entry_idx, idx_in_entry);
		auto entry = scan_rows.GetValidityEntryUnsafe(entry_idx);
		auto entry_end = MinValue<idx_t>(end, row + ValidityMask::BITS_PER_VALUE - idx_in_entry);
		if (ValidityMask::NoneValid(entry)) {
			row = entry_end;
			continue;
		}
		for (; row < entry_end; row++, idx_in_entry++) {
			if (ValidityMask::RowIsValid(entry, idx_in_entry)) {
				result_sel.set_index(result_count++, row - start);
			}
		}
	}
	return result_count;
}

idx_t DuckLakeDeleteData::Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel,
                                 optional_idx snapshot_filter) const {
	if (scan_rows.IsMaskSet()) {
		return FilterScanRows(start_row_index, count, result_sel);
	}
	auto entry = std::lower_bound(deleted_rows.begin(), deleted_rows.end(), start_row_index);
	if (entry == deleted_rows.end()) {
		// no filter found for this entry
		return count;
	}
	idx_t end_pos = start_row_index + count;
	auto delete_idx = NumericCast<idx_t>(entry - deleted_rows.begin());
	if (deleted_rows[delete_idx] > end_pos) {
		// nothing in this range is deleted - skip
		return count;
	}
	// we have deletes in this range
	result_sel.Initialize(STANDARD_VECTOR_SIZE);
	idx_t result_count = 0;
	bool check_snapshots = snapshot_filter.IsValid() && !snapshot_ids.empty();
	for (idx_t i = 0; i < count; i++) {
		if (delete_idx < deleted_rows.size() && start_row_index + i == deleted_rows[delete_idx]) {
			bool is_deleted = true;
			if (check_snapshots) {
				// only consider deletions where snapshot_id <= snapshot_filter
				is_deleted = snapshot_ids[delete_idx] <= snapshot_filter.GetIndex();
			}
			delete_idx++;
			if (is_deleted) {
				continue;
			}
		}
		result_sel.set_index(result_count++, i);
	}
	return result_count;
}

bool DuckLakeDeleteData::HasEmbeddedSnapshots() const {
	return !snapshot_ids.empty();
}

bool DuckLakeDeleteData::HasScanSnapshots() const {
	return !scan_snapshot_positions.empty();
}

optional_idx DuckLakeDeleteData::GetSnapshotForPosition(idx_t position) const {
	auto it = std::lower_bound(scan_snapshot_positions.begin(), scan_snapshot_positions.end(), position);
	if (it != scan_snapshot_positions.end() && *it == position) {
		return scan_snapshot_ids[NumericCast<idx_t>(it - scan_snapshot_positions.begin())];
	}
	return optional_idx();
}

idx_t DuckLakeDeleteFilter::Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel) {
	return delete_data->Filter(start_row_index, count, result_sel, snapshot_filter);
}

DeleteFileScanResult DuckLakeDeleteFilter::ScanDeletionVectorFile(ClientContext &context,
                                                                  const DuckLakeFileData &delete_file,
                                                                  optional_idx snapshot_filter_min,
                                                                  optional_idx snapshot_filter_max) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto file_handle = fs.OpenFile(delete_file.path, FileOpenFlags::FILE_FLAGS_READ);
	auto file_size = NumericCast<idx_t>(file_handle->GetFileSize());

	auto buffer = make_unsafe_uniq_array<data_t>(file_size);
	file_handle->Read(buffer.get(), file_size);

	DuckLakePuffinReader reader(buffer.get(), file_size, delete_file.path);
	auto &blobs = reader.Blobs();
	if (blobs.empty()) {
		throw InvalidInputException("Deletion vector file \"%s\" does not contain any deletion vectors",
		                            delete_file.path);
	}
	idx_t snapshot_blob_count = 0;
	for (auto &blob : blobs) {
		if (blob.snapshot_id.IsValid()) {
			snapshot_blob_count++;
		}
	}

	DeleteFileScanResult result;
	if (snapshot_blob_count == 0) {
		result.has_embedded_snapshots = false;
		roaring::Roaring64Map deleted;
		for (auto &blob : blobs) {
			deleted |= reader.DecodeBlob(blob)->bitmap;
		}
		result.deleted_rows.resize(deleted.cardinality());
		deleted.toUint64Array(result.deleted_rows.data());
		return result;
	}
	if (snapshot_blob_count != blobs.size()) {
		throw InvalidInputException(
		    "Deletion vector file \"%s\" is corrupt - it mixes deletion vectors with and without a snapshot",
		    delete_file.path);
	}

	// sort the blob metadata by ascending snapshot
	auto sorted_blobs = blobs;
	std::sort(sorted_blobs.begin(), sorted_blobs.end(), [](const DuckLakePuffinBlob &a, const DuckLakePuffinBlob &b) {
		return a.snapshot_id.GetIndex() < b.snapshot_id.GetIndex();
	});

	map<idx_t, idx_t> position_to_snapshot;
	for (auto &blob : sorted_blobs) {
		auto snapshot_id = blob.snapshot_id.GetIndex();
		if (snapshot_filter_max.IsValid() && snapshot_id > snapshot_filter_max.GetIndex()) {
			break;
		}
		auto deletion_vector = reader.DecodeBlob(blob);
		for (auto position : deletion_vector->bitmap) {
			position_to_snapshot.emplace(position, snapshot_id);
		}
	}

	result.has_embedded_snapshots = true;
	for (auto &entry : position_to_snapshot) {
		if (snapshot_filter_min.IsValid() && entry.second < snapshot_filter_min.GetIndex()) {
			continue;
		}
		result.deleted_rows.push_back(entry.first);
		result.snapshot_ids.push_back(entry.second);
	}
	return result;
}

DeleteFileScanResult DuckLakeDeleteFilter::ScanDeleteFile(ClientContext &context, const DuckLakeFileData &delete_file,
                                                          optional_idx snapshot_filter_min,
                                                          optional_idx snapshot_filter_max) {
	// Check if this is a puffin deletion vector file
	if (delete_file.format == DeleteFileFormat::PUFFIN) {
		return ScanDeletionVectorFile(context, delete_file, snapshot_filter_min, snapshot_filter_max);
	}

	ParquetFileScanner scanner(context, delete_file, true);

	auto &return_types = scanner.GetTypes();
	auto &return_names = scanner.GetNames();

	// Check for valid schema, there are three possibilities:
	// 1. 2 columns: file_path (VARCHAR), pos (BIGINT) -> standard delete file
	// 2. 3 columns: file_path (VARCHAR), pos (BIGINT), _ducklake_internal_snapshot_id (BIGINT) -> delete file with
	// snapshots
	// 3. 3 columns: file_path (VARCHAR), pos (BIGINT), row (?) -> iceberg format (third column ignored)
	bool valid_two_col = return_types.size() == 2 && return_types[0].id() == LogicalTypeId::VARCHAR &&
	                     return_types[1].id() == LogicalTypeId::BIGINT;
	bool valid_three_col = return_types.size() == 3 && return_types[0].id() == LogicalTypeId::VARCHAR &&
	                       return_types[1].id() == LogicalTypeId::BIGINT;

	if (!valid_two_col && !valid_three_col) {
		throw InvalidInputException(
		    "Invalid schema contained in the delete file %s - expected file_name/position/[snapshot_id or row]",
		    delete_file.path);
	}

	// is this from ducklake?
	bool has_snapshot_id = false;
	if (return_types.size() == 3 && return_types[2].id() == LogicalTypeId::BIGINT) {
		// check if the name matches _ducklake_internal_snapshot_id
		if (return_names.size() > 2 && return_names[2] == "_ducklake_internal_snapshot_id") {
			has_snapshot_id = true;
		}
	}

	// Create snapshot filters if we have a snapshot column and filter range is specified
	if (has_snapshot_id && (snapshot_filter_min.IsValid() || snapshot_filter_max.IsValid())) {
		auto filters = make_uniq<TableFilterSet>();
		ProjectionIndex snapshot_col_idx(2); // snapshot_id is column 2

		if (snapshot_filter_min.IsValid()) {
			auto min_constant = Value::BIGINT(NumericCast<int64_t>(snapshot_filter_min.GetIndex()));
			filters->PushFilter(snapshot_col_idx,
			                    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
			                                                             std::move(min_constant)));
		}
		if (snapshot_filter_max.IsValid()) {
			auto max_constant = Value::BIGINT(NumericCast<int64_t>(snapshot_filter_max.GetIndex()));
			filters->PushFilter(snapshot_col_idx,
			                    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_LESSTHANOREQUALTO,
			                                                             std::move(max_constant)));
		}
		scanner.SetFilters(std::move(filters));
	}

	DataChunk scan_chunk;
	scan_chunk.Initialize(context, return_types);

	DeleteFileScanResult result;
	result.has_embedded_snapshots = has_snapshot_id;
	int64_t last_delete = -1;

	while (scanner.Scan(scan_chunk)) {
		auto positions = scan_chunk.data[1].Values<int64_t>();
		for (idx_t i = 0; i < scan_chunk.size(); i++) {
			auto position = positions[i];
			if (!position.IsValid()) {
				throw InvalidInputException("Invalid delete data - delete data cannot have NULL values");
			}
			auto row_id = position.GetValue();
			if (row_id <= last_delete) {
				throw InvalidInputException(
				    "Invalid delete data - row ids must be sorted and strictly increasing - but found %d after %d",
				    row_id, last_delete);
			}

			result.deleted_rows.push_back(row_id);
			last_delete = row_id;
		}
		if (!has_snapshot_id) {
			continue;
		}
		auto snapshot_ids = scan_chunk.data[2].Values<int64_t>();
		for (idx_t i = 0; i < scan_chunk.size(); i++) {
			auto snapshot_id = snapshot_ids[i];
			if (!snapshot_id.IsValid()) {
				throw InvalidInputException("Invalid delete data - snapshot_id cannot be NULL");
			}
			result.snapshot_ids.push_back(NumericCast<idx_t>(snapshot_id.GetValue()));
		}
	}
	return result;
}

void DuckLakeDeleteFilter::Initialize(ClientContext &context, const DuckLakeFileData &delete_file) {
	auto scan_result = ScanDeleteFile(context, delete_file, optional_idx(), optional_idx());
	delete_data->deleted_rows = std::move(scan_result.deleted_rows);
	delete_data->snapshot_ids = std::move(scan_result.snapshot_ids);
}

void DuckLakeDeleteFilter::Initialize(const DuckLakeInlinedDataDeletes &inlined_deletes) {
	D_ASSERT(std::is_sorted(delete_data->deleted_rows.begin(), delete_data->deleted_rows.end()));

	// Fast path: existing delete data has no per-row snapshot tracking.
	if (delete_data->snapshot_ids.empty()) {
		auto mid_idx = delete_data->deleted_rows.size();
		delete_data->deleted_rows.insert(delete_data->deleted_rows.end(), inlined_deletes.rows.begin(),
		                                 inlined_deletes.rows.end());
		std::inplace_merge(delete_data->deleted_rows.begin(), delete_data->deleted_rows.begin() + mid_idx,
		                   delete_data->deleted_rows.end());
		// a row can be deleted in both stores
		delete_data->deleted_rows.erase(std::unique(delete_data->deleted_rows.begin(), delete_data->deleted_rows.end()),
		                                delete_data->deleted_rows.end());
		return;
	}

	// Merge existing deletes with inlined deletes.
	const idx_t merged_size = delete_data->deleted_rows.size() + inlined_deletes.rows.size();
	vector<idx_t> merged_rows;
	vector<idx_t> merged_snapshot_ids;
	merged_rows.reserve(merged_size);
	merged_snapshot_ids.reserve(merged_size);

	auto lhs_row_it = delete_data->deleted_rows.begin();
	auto lhs_snapshot_it = delete_data->snapshot_ids.begin();
	auto rhs_row_it = inlined_deletes.rows.begin();
	while (lhs_row_it != delete_data->deleted_rows.end() || rhs_row_it != inlined_deletes.rows.end()) {
		if (rhs_row_it == inlined_deletes.rows.end() ||
		    (lhs_row_it != delete_data->deleted_rows.end() && *lhs_row_it < *rhs_row_it)) {
			merged_rows.push_back(*lhs_row_it);
			merged_snapshot_ids.push_back(*lhs_snapshot_it);
			lhs_row_it++;
			lhs_snapshot_it++;
		} else {
			if (lhs_row_it != delete_data->deleted_rows.end() && *lhs_row_it == *rhs_row_it) {
				// the inlined delete is visible, so it replaces the delete of the same row
				lhs_row_it++;
				lhs_snapshot_it++;
			}
			merged_rows.push_back(*rhs_row_it);
			merged_snapshot_ids.push_back(0);
			rhs_row_it++;
		}
	}

	delete_data->deleted_rows = std::move(merged_rows);
	delete_data->snapshot_ids = std::move(merged_snapshot_ids);
}

static void CheckDeleteIndex(idx_t delete_idx, const DuckLakeDeleteScanEntry &delete_scan,
                             const DuckLakeFileData &delete_file) {
	if (delete_idx >= delete_scan.row_count) {
		throw InvalidInputException(
		    "Invalid delete data - delete index read from file %s is out of range for data file %s", delete_file.path,
		    delete_scan.file.path);
	}
}

void DuckLakeDeleteFilter::Initialize(ClientContext &context, const DuckLakeDeleteScanEntry &delete_scan) {
	// Scanning deletes - we need to scan the opposite (i.e. only the rows that were deleted)
	// a whole file delete returns every row but the excluded ones, any other source marks the rows to return
	bool whole_file = delete_scan.delete_file.path.empty() && delete_scan.file_deleted;
	auto &rows = delete_data->scan_rows;
	rows.Reset(delete_scan.row_count);
	if (!whole_file) {
		rows.SetAllInvalid(delete_scan.row_count);
	}
	// the rows deleted before the scanned range, cleared after every source has added its rows
	vector<idx_t> excluded;
	// the rows whose deleting snapshot is not the snapshot of the file, later entries override earlier ones
	vector<pair<idx_t, idx_t>> snapshot_overrides;
	bool has_embedded_snapshots = false;

	if (!delete_scan.delete_file.path.empty()) {
		// Partial delete, read deletions from the delete file
		auto current_deletes =
		    ScanDeleteFile(context, delete_scan.delete_file, delete_scan.start_snapshot, delete_scan.end_snapshot);
		has_embedded_snapshots = current_deletes.has_embedded_snapshots;
		for (idx_t i = 0; i < current_deletes.deleted_rows.size(); i++) {
			auto delete_idx = current_deletes.deleted_rows[i];
			CheckDeleteIndex(delete_idx, delete_scan, delete_scan.delete_file);
			bool has_snapshot = i < current_deletes.snapshot_ids.size();
			auto delete_snapshot = has_snapshot ? current_deletes.snapshot_ids[i] : delete_scan.snapshot_id.GetIndex();
			if (delete_scan.insert_snapshot.IsValid() && delete_snapshot == delete_scan.insert_snapshot.GetIndex()) {
				// a row deleted in the snapshot that inserted it was never visible
				continue;
			}
			rows.SetValidUnsafe(delete_idx);
			if (has_snapshot) {
				snapshot_overrides.emplace_back(delete_idx, delete_snapshot);
			}
		}
	}

	// inlined file deletions add rows, or exclude rows deleted before the scanned range
	for (auto &inlined_delete : delete_scan.inlined_file_deletions) {
		if (inlined_delete.second < delete_scan.start_snapshot.GetIndex()) {
			excluded.push_back(inlined_delete.first);
			continue;
		}
		rows.SetValid(inlined_delete.first);
		snapshot_overrides.emplace_back(inlined_delete.first, inlined_delete.second);
	}

	if (!delete_scan.previous_delete_file.path.empty() && !has_embedded_snapshots) {
		// if we have a previous delete file - scan that set of deletes
		// This only matters if we do not have a partial deletion file, since thes have all deletes
		auto previous_deletes =
		    ScanDeleteFile(context, delete_scan.previous_delete_file, optional_idx(), optional_idx());
		// these deletes are not new - we should not scan them
		for (auto delete_idx : previous_deletes.deleted_rows) {
			CheckDeleteIndex(delete_idx, delete_scan, delete_scan.previous_delete_file);
			excluded.push_back(delete_idx);
		}
	}

	// the mask of a wholly deleted file is only allocated when a row is excluded
	for (auto excluded_idx : excluded) {
		rows.SetInvalid(excluded_idx);
	}

	std::stable_sort(
	    snapshot_overrides.begin(), snapshot_overrides.end(),
	    [](const pair<idx_t, idx_t> &lhs, const pair<idx_t, idx_t> &rhs) { return lhs.first < rhs.first; });
	auto &positions = delete_data->scan_snapshot_positions;
	auto &snapshots = delete_data->scan_snapshot_ids;
	for (auto &entry : snapshot_overrides) {
		if (!positions.empty() && positions.back() == entry.first) {
			snapshots.back() = entry.second;
			continue;
		}
		positions.push_back(entry.first);
		snapshots.push_back(entry.second);
	}
}

void DuckLakeDeleteFilter::SetSnapshotFilter(idx_t snapshot_filter_p) {
	snapshot_filter = snapshot_filter_p;
}

} // namespace duckdb
