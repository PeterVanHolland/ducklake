#include "storage/ducklake_inlined_data.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

#include <algorithm>
#include <numeric>

namespace duckdb {

idx_t DuckLakeInlinedData::Count() const {
	if (data) {
		return data->Count();
	}
	return external_row_count;
}

bool DuckLakeInlinedData::HasPreservedRowIds() const {
	return !row_ids.empty();
}

int64_t DuckLakeInlinedData::GetOutputRowId(idx_t position) const {
	if (HasPreservedRowIds()) {
		return row_ids[position];
	}
	return NumericCast<int64_t>(DuckLakeConstants::TRANSACTION_LOCAL_ROW_ID_START + position);
}

void DuckLakeInlinedData::MergeRowIds(const DuckLakeInlinedData &new_data, idx_t new_data_count) {
	if (!new_data.HasPreservedRowIds() && !HasPreservedRowIds()) {
		return;
	}
	if (!HasPreservedRowIds()) {
		// if the existing data doesnt have preserved row ids, we assign them sequentially from
		// transaction local id
		idx_t existing_count = data->Count() - new_data_count;
		row_ids.reserve(existing_count + new_data.row_ids.size());
		row_ids.resize(existing_count);
		std::iota(row_ids.begin(), row_ids.end(),
		          NumericCast<int64_t>(DuckLakeConstants::TRANSACTION_LOCAL_ROW_ID_START));
	}
	if (new_data.HasPreservedRowIds()) {
		// if this is already preserved we can just insert it
		row_ids.insert(row_ids.end(), new_data.row_ids.begin(), new_data.row_ids.end());
	} else {
		// new data doesnt preserve row_ids, we need to use the TRANSACTION_LOCAL_ROW_ID_START
		int64_t next_id = NumericCast<int64_t>(DuckLakeConstants::TRANSACTION_LOCAL_ROW_ID_START);
		if (!row_ids.empty()) {
			// we only use max_id if it's guaranteed to be transactional (over next_id)
			auto max_id = *std::max_element(row_ids.begin(), row_ids.end());
			if (max_id >= next_id) {
				next_id = max_id + 1;
			}
		}
		auto new_start = row_ids.size();
		row_ids.resize(new_start + new_data_count);
		std::iota(row_ids.begin() + NumericCast<int64_t>(new_start), row_ids.end(), next_id);
	}
}

void DuckLakeInlinedData::CastTo(ClientContext &context, const vector<LogicalType> &new_types) {
	auto &existing_types = data->Types();
	if (existing_types == new_types) {
		return;
	}
	auto casted_data = make_uniq<ColumnDataCollection>(context, new_types);
	ColumnDataAppendState append_state;
	casted_data->InitializeAppend(append_state);
	for (auto &chunk : data->Chunks()) {
		DataChunk casted_chunk;
		casted_chunk.Initialize(context, new_types);
		for (idx_t col_idx = 0; col_idx < chunk.ColumnCount(); col_idx++) {
			if (existing_types[col_idx] != new_types[col_idx]) {
				VectorOperations::Cast(context, chunk.data[col_idx], casted_chunk.data[col_idx], chunk.size());
			} else {
				casted_chunk.data[col_idx].Reference(chunk.data[col_idx]);
			}
		}
		casted_chunk.SetChildCardinality(chunk.size());
		casted_data->Append(append_state, casted_chunk);
	}
	data = std::move(casted_data);
}

} // namespace duckdb
