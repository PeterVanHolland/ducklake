#include "storage/ducklake_staged_commit.hpp"

#include "common/ducklake_data_file.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_metadata_info.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_transaction_changes.hpp"

namespace duckdb {

// Shared column-stats payload (matches DuckLakeStagedCommit::EmitColumnStatsValues /
// DuckLakeServerSideCommit::ReadColumnStatsRow). Single definition used by both the data-file and
// inlined column-stats staging tables.
static const char *const STAGED_STAT_COLUMNS =
    "column_size_bytes BIGINT, has_num_values BOOLEAN, num_values BIGINT, "
    "has_null_count BOOLEAN, null_count BIGINT, has_min BOOLEAN, min_value VARCHAR, "
    "has_max BOOLEAN, max_value VARCHAR, has_contains_nan BOOLEAN, contains_nan BOOLEAN, "
    "any_valid BOOLEAN, extra_stats VARCHAR, min_is_exact BOOLEAN, max_is_exact BOOLEAN";

const char *DuckLakeStagedTable::BaseName(DuckLakeStagedTableType type) {
	switch (type) {
	case DuckLakeStagedTableType::COMMIT_HEADER:
		return "ducklake_staged_commit";
	case DuckLakeStagedTableType::DATA_FILE:
		return "ducklake_staged_data_file";
	case DuckLakeStagedTableType::DATA_FILE_COLUMN_STATS:
		return "ducklake_staged_data_file_column_stats";
	case DuckLakeStagedTableType::DATA_FILE_PARTITION:
		return "ducklake_staged_data_file_partition";
	case DuckLakeStagedTableType::DELETE_FILE:
		return "ducklake_staged_delete_file";
	case DuckLakeStagedTableType::INLINED_DATA:
		return "ducklake_staged_inlined_data";
	case DuckLakeStagedTableType::INLINED_ROW:
		return "ducklake_staged_inlined_row";
	case DuckLakeStagedTableType::INLINED_COLUMN_STATS:
		return "ducklake_staged_inlined_column_stats";
	case DuckLakeStagedTableType::INLINED_DELETE:
		return "ducklake_staged_inlined_delete";
	case DuckLakeStagedTableType::INLINED_FILE_DELETE:
		return "ducklake_staged_inlined_file_delete";
	case DuckLakeStagedTableType::DROPPED_FILE:
		return "ducklake_staged_dropped_file";
	case DuckLakeStagedTableType::TABLES_DELETED_FROM:
		return "ducklake_staged_tables_deleted_from";
	case DuckLakeStagedTableType::TABLES_DELETE_ATTEMPTED:
		return "ducklake_staged_tables_delete_attempted";
	case DuckLakeStagedTableType::FLUSHED_INLINED:
		return "ducklake_staged_flushed_inlined";
	case DuckLakeStagedTableType::COMPACTION:
		return "ducklake_staged_compaction";
	case DuckLakeStagedTableType::COMPACTION_SOURCE:
		return "ducklake_staged_compaction_source";
	case DuckLakeStagedTableType::NAME_MAP:
		return "ducklake_staged_name_map";
	case DuckLakeStagedTableType::NAME_MAP_ENTRY:
		return "ducklake_staged_name_map_entry";
	default:
		throw InternalException("Unknown DuckLakeStagedTableType");
	}
}

string DuckLakeStagedTable::Columns(DuckLakeStagedTableType type) {
	switch (type) {
	case DuckLakeStagedTableType::COMMIT_HEADER:
		return "commit_author VARCHAR, commit_message VARCHAR, commit_extra_info VARCHAR, "
		       "transaction_snapshot_id BIGINT, data_path VARCHAR, path_separator VARCHAR, "
		       "transaction_next_catalog_id UBIGINT, transaction_next_file_id UBIGINT";
	case DuckLakeStagedTableType::DATA_FILE:
		return "data_file_id BIGINT, table_id BIGINT, file_order BIGINT, "
		       "path VARCHAR, path_is_relative BOOLEAN, file_format VARCHAR, "
		       "record_count BIGINT, file_size_bytes BIGINT, footer_size BIGINT, "
		       "row_id_start BIGINT, partition_id BIGINT, encryption_key VARCHAR, "
		       "mapping_id UBIGINT, partial_max BIGINT, begin_snapshot BIGINT, "
		       "compaction_id BIGINT, row_group_count BIGINT, created_by_ducklake BOOLEAN";
	case DuckLakeStagedTableType::DATA_FILE_COLUMN_STATS:
		return string("data_file_id BIGINT, table_id BIGINT, column_id BIGINT, ") + STAGED_STAT_COLUMNS;
	case DuckLakeStagedTableType::DATA_FILE_PARTITION:
		return "local_file_id BIGINT, partition_column_idx BIGINT, partition_value VARCHAR";
	case DuckLakeStagedTableType::DELETE_FILE:
		return "table_id BIGINT, data_file_path VARCHAR, data_file_id BIGINT, "
		       "file_name VARCHAR, format VARCHAR, delete_count BIGINT, "
		       "file_size_bytes BIGINT, footer_size BIGINT, encryption_key VARCHAR, "
		       "begin_snapshot BIGINT, max_snapshot BIGINT, source VARCHAR, "
		       "overwrites_existing_delete BOOLEAN, "
		       "overwrite_delete_file_id BIGINT, overwrite_delete_file_path VARCHAR, "
		       "attached_local_file_id BIGINT, row_group_count BIGINT";
	case DuckLakeStagedTableType::INLINED_DATA:
		return "table_id BIGINT, has_preserved_row_ids BOOLEAN";
	case DuckLakeStagedTableType::INLINED_ROW:
		return "table_id BIGINT, row_order BIGINT, preserved_row_id BIGINT, value_literals VARCHAR";
	case DuckLakeStagedTableType::INLINED_COLUMN_STATS:
		return string("table_id BIGINT, column_id BIGINT, ") + STAGED_STAT_COLUMNS;
	case DuckLakeStagedTableType::INLINED_DELETE:
		return "table_id BIGINT, inlined_table_name VARCHAR, deleted_row_id BIGINT";
	case DuckLakeStagedTableType::INLINED_FILE_DELETE:
		return "table_id BIGINT, file_id BIGINT, deleted_row_id BIGINT";
	case DuckLakeStagedTableType::DROPPED_FILE:
		return "path VARCHAR, data_file_id BIGINT";
	case DuckLakeStagedTableType::TABLES_DELETED_FROM:
		return "table_id BIGINT";
	case DuckLakeStagedTableType::TABLES_DELETE_ATTEMPTED:
		return "table_id BIGINT";
	case DuckLakeStagedTableType::FLUSHED_INLINED:
		return "inlined_table_name VARCHAR, schema_version BIGINT, flush_snapshot_id BIGINT";
	case DuckLakeStagedTableType::COMPACTION:
		return "compaction_id BIGINT, table_id BIGINT, compaction_type VARCHAR, row_id_start BIGINT";
	case DuckLakeStagedTableType::COMPACTION_SOURCE:
		return "compaction_id BIGINT, source_order BIGINT, "
		       "source_data_file_id BIGINT, source_path VARCHAR, "
		       "source_row_count BIGINT, source_begin_snapshot BIGINT, "
		       "source_max_partial_file_snapshot BIGINT, source_inlined_delete_count BIGINT, "
		       "last_delete_file_id BIGINT, last_delete_path VARCHAR, "
		       "last_delete_row_count BIGINT, last_delete_end_snapshot BIGINT";
	case DuckLakeStagedTableType::NAME_MAP:
		return "map_id UBIGINT, table_id BIGINT";
	case DuckLakeStagedTableType::NAME_MAP_ENTRY:
		return "map_id UBIGINT, entry_id BIGINT, parent_entry_id BIGINT, "
		       "source_name VARCHAR, target_field_id BIGINT, hive_partition BOOLEAN";
	default:
		throw InternalException("Unknown DuckLakeStagedTableType");
	}
}

const vector<DuckLakeStagedTableType> &DuckLakeStagedTable::AllTypes() {
	static const vector<DuckLakeStagedTableType> kinds = {DuckLakeStagedTableType::COMMIT_HEADER,
	                                                      DuckLakeStagedTableType::DATA_FILE,
	                                                      DuckLakeStagedTableType::DATA_FILE_COLUMN_STATS,
	                                                      DuckLakeStagedTableType::DATA_FILE_PARTITION,
	                                                      DuckLakeStagedTableType::DELETE_FILE,
	                                                      DuckLakeStagedTableType::INLINED_DATA,
	                                                      DuckLakeStagedTableType::INLINED_ROW,
	                                                      DuckLakeStagedTableType::INLINED_COLUMN_STATS,
	                                                      DuckLakeStagedTableType::INLINED_DELETE,
	                                                      DuckLakeStagedTableType::INLINED_FILE_DELETE,
	                                                      DuckLakeStagedTableType::DROPPED_FILE,
	                                                      DuckLakeStagedTableType::TABLES_DELETED_FROM,
	                                                      DuckLakeStagedTableType::TABLES_DELETE_ATTEMPTED,
	                                                      DuckLakeStagedTableType::FLUSHED_INLINED,
	                                                      DuckLakeStagedTableType::COMPACTION,
	                                                      DuckLakeStagedTableType::COMPACTION_SOURCE,
	                                                      DuckLakeStagedTableType::NAME_MAP,
	                                                      DuckLakeStagedTableType::NAME_MAP_ENTRY};
	return kinds;
}

string DuckLakeStagedTable::CreateAllSql() {
	string sql;
	for (auto kind : AllTypes()) {
		sql += StringUtil::Format("CREATE TEMPORARY TABLE IF NOT EXISTS %s(%s);", BaseName(kind), Columns(kind));
	}
	return sql;
}

string DuckLakeStagedTable::TruncateAllSql() {
	string sql;
	for (auto kind : AllTypes()) {
		sql += StringUtil::Format("TRUNCATE %s;", BaseName(kind));
	}
	return sql;
}

DuckLakeStagedRows::DuckLakeStagedRows() : rows(DuckLakeStagedTable::AllTypes().size()) {
}

void DuckLakeStagedRows::Add(DuckLakeStagedTableType type, string tuple) {
	rows[static_cast<idx_t>(type)].push_back(std::move(tuple));
}

string DuckLakeStagedRows::InsertSql() const {
	string sql;
	for (auto type : DuckLakeStagedTable::AllTypes()) {
		sql += DuckLakeMetadataManager::InsertValuesIntoSql(DuckLakeStagedTable::BaseName(type),
		                                                    rows[static_cast<idx_t>(type)]);
	}
	return sql;
}

set<DuckLakeStagedTableType> DuckLakeStagedRows::FilledTables() const {
	set<DuckLakeStagedTableType> result;
	for (auto type : DuckLakeStagedTable::AllTypes()) {
		if (!rows[static_cast<idx_t>(type)].empty()) {
			result.insert(type);
		}
	}
	return result;
}

//! The staged table states of a database instance, one per metadata database
struct DuckLakeStagedTableSessions : public ObjectCacheEntry {
	mutex lock;
	vector<pair<weak_ptr<AttachedDatabase>, shared_ptr<DuckLakeStagedTableState>>> sessions;

	static string ObjectType() {
		return "ducklake_staged_table_sessions";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx();
	}
};

shared_ptr<DuckLakeStagedTableState> DuckLakeStagedTableState::Get(DuckLakeTransaction &transaction) {
	auto &catalog = transaction.GetCatalog();
	auto &db = catalog.GetDatabase();
	// quack_query_by_name runs on the metadata database pinned by the metadata transaction
	auto &context = *transaction.GetConnection().context;
	auto pinned = DatabaseManager::Get(context).GetDatabase(context, Identifier(catalog.MetadataDatabaseName()));
	if (!pinned) {
		// LCOV_EXCL_START
		return make_shared_ptr<DuckLakeStagedTableState>();
		// LCOV_EXCL_STOP
	}
	auto metadata_database = pinned->shared_from_this();
	auto registry =
	    db.GetObjectCache().GetOrCreate<DuckLakeStagedTableSessions>(DuckLakeStagedTableSessions::ObjectType());
	lock_guard<mutex> guard(registry->lock);
	auto &sessions = registry->sessions;
	shared_ptr<DuckLakeStagedTableState> result;
	for (idx_t i = 0; i < sessions.size();) {
		// a replaced or detached metadata database ended its session
		auto attached = sessions[i].first.lock();
		if (!attached) {
			sessions.erase(sessions.begin() + static_cast<int64_t>(i));
			continue;
		}
		if (attached.get() == metadata_database.get()) {
			result = sessions[i].second;
		}
		i++;
	}
	if (!result) {
		result = make_shared_ptr<DuckLakeStagedTableState>();
		sessions.emplace_back(metadata_database, result);
	}
	return result;
}

string DuckLakeStagedTableState::SetupSql(const set<DuckLakeStagedTableType> &tables_to_fill) {
	string sql;
	if (!created) {
		sql = DuckLakeStagedTable::CreateAllSql() + DuckLakeStagedTable::TruncateAllSql();
	} else {
		for (auto type : filled) {
			sql += StringUtil::Format("TRUNCATE %s;", DuckLakeStagedTable::BaseName(type));
		}
	}
	// the statements run one by one on the server, so a batch that fails midway can leave rows in any of them
	filled.insert(tables_to_fill.begin(), tables_to_fill.end());
	batch_tables = tables_to_fill;
	return sql;
}

void DuckLakeStagedTableState::BatchFinished(bool success) {
	created = success;
	if (success) {
		filled = std::move(batch_tables);
	}
	batch_tables.clear();
}

const char *DuckLakeStagedTable::DeleteFileSourceToString(DeleteFileSource source) {
	return source == DeleteFileSource::FLUSH ? "FLUSH" : "REGULAR";
}

DeleteFileSource DuckLakeStagedTable::DeleteFileSourceFromString(const string &source) {
	return source == "FLUSH" ? DeleteFileSource::FLUSH : DeleteFileSource::REGULAR;
}

void DuckLakeStagedCommit::EmitDataFileRow(DuckLakeStagedRows &rows, const DuckLakeDataFile &file, idx_t local_file_id,
                                           TableIndex table_id, idx_t file_order,
                                           const string &compaction_id_literal) const {
	rows.Add(
	    DuckLakeStagedTableType::DATA_FILE,
	    StringUtil::Format(
	        "(%llu, %llu, %llu, %s, false, 'parquet', %llu, %llu, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)",
	        local_file_id, table_id.index, file_order, SQLString(file.file_name), file.row_count, file.file_size_bytes,
	        DuckLakeUtil::OptionalIdxOrNull(file.footer_size), DuckLakeUtil::OptionalIdxOrNull(file.flush_row_id_start),
	        DuckLakeUtil::OptionalIdxOrNull(file.partition_id), DuckLakeUtil::EncryptionKeyLiteral(file.encryption_key),
	        DuckLakeUtil::MappingIdOrNull(file.mapping_id),
	        DuckLakeUtil::OptionalIdxOrNull(file.max_partial_file_snapshot),
	        DuckLakeUtil::OptionalIdxOrNull(file.begin_snapshot), compaction_id_literal,
	        DuckLakeUtil::OptionalIdxOrNull(file.row_group_count),
	        DuckLakeUtil::BoolLiteral(file.created_by_ducklake)));
	for (auto &stat : file.column_stats) {
		rows.Add(DuckLakeStagedTableType::DATA_FILE_COLUMN_STATS,
		         StringUtil::Format("(%llu, %llu, %llu, %s)", local_file_id, table_id.index, stat.first.index,
		                            EmitColumnStatsValues(stat.second)));
	}
	for (auto &part : file.partition_values) {
		rows.Add(DuckLakeStagedTableType::DATA_FILE_PARTITION,
		         StringUtil::Format("(%llu, %llu, %s)", local_file_id, part.partition_column_idx,
		                            DuckLakeUtil::PartitionValueLiteral(part.partition_value)));
	}
}

void DuckLakeStagedCommit::EmitDeleteFileRow(DuckLakeStagedRows &rows, const DuckLakeDeleteFile &file,
                                             TableIndex table_id, optional_ptr<const string> data_file_path,
                                             optional_idx attached_local_file_id) const {
	string data_file_path_literal = "NULL";
	string data_file_id = "NULL";
	bool overwrites_existing_delete = false;
	string overwrite_id = "NULL";
	string overwrite_path = "NULL";
	if (data_file_path) {
		data_file_path_literal = SQLString::ToString(*data_file_path);
		data_file_id = std::to_string(file.data_file_id.index);
		overwrites_existing_delete = file.overwrites_existing_delete;
		auto &overwritten = file.overwritten_delete_file;
		if (overwritten.delete_file_id.IsValid()) {
			// LCOV_EXCL_START
			overwrite_id = std::to_string(overwritten.delete_file_id.index);
			// LCOV_EXCL_STOP
		}
		if (!overwritten.path.empty()) {
			// LCOV_EXCL_START
			overwrite_path = SQLString::ToString(overwritten.path);
			// LCOV_EXCL_STOP
		}
	}
	rows.Add(DuckLakeStagedTableType::DELETE_FILE,
	         StringUtil::Format("(%llu, %s, %s, %s, %s, %llu, %llu, %llu, %s, %s, %s, %s, %s, %s, %s, %s, %s)",
	                            table_id.index, data_file_path_literal, data_file_id, SQLString(file.file_name),
	                            SQLString(DeleteFileFormatToString(file.format)), file.delete_count,
	                            file.file_size_bytes, file.footer_size,
	                            DuckLakeUtil::EncryptionKeyLiteral(file.encryption_key),
	                            DuckLakeUtil::OptionalIdxOrNull(file.begin_snapshot),
	                            DuckLakeUtil::OptionalIdxOrNull(file.max_snapshot),
	                            SQLString(DuckLakeStagedTable::DeleteFileSourceToString(file.source)),
	                            DuckLakeUtil::BoolLiteral(overwrites_existing_delete), overwrite_id, overwrite_path,
	                            DuckLakeUtil::OptionalIdxOrNull(attached_local_file_id),
	                            DuckLakeUtil::OptionalIdxOrNull(file.row_group_count)));
}

string DuckLakeStagedCommit::EmitColumnStatsValues(const DuckLakeColumnStats &s) {
	string num_values = s.has_num_values ? std::to_string(s.num_values) : "NULL";
	string null_count = s.has_null_count ? std::to_string(s.null_count) : "NULL";
	string min_val = s.has_min ? DuckLakeUtil::StatsToString(s.min) : "NULL";
	string max_val = s.has_max ? DuckLakeUtil::StatsToString(s.max) : "NULL";
	bool has_min_emit = s.has_min && min_val != "NULL";
	bool has_max_emit = s.has_max && max_val != "NULL";
	string contains_nan = s.has_contains_nan ? DuckLakeUtil::BoolLiteral(s.contains_nan) : "NULL";
	string extra_stats = "NULL";
	if (s.extra_stats) {
		string serialized;
		if (s.extra_stats->TrySerialize(serialized) && !serialized.empty()) {
			// TrySerialize already returns a ready-to-emit SQL literal; emit it as-is.
			extra_stats = serialized;
		}
	}
	return StringUtil::Format(
	    "%llu, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s", s.column_size_bytes,
	    DuckLakeUtil::BoolLiteral(s.has_num_values), num_values, DuckLakeUtil::BoolLiteral(s.has_null_count),
	    null_count, DuckLakeUtil::BoolLiteral(has_min_emit), min_val, DuckLakeUtil::BoolLiteral(has_max_emit), max_val,
	    DuckLakeUtil::BoolLiteral(s.has_contains_nan), contains_nan, DuckLakeUtil::BoolLiteral(s.any_valid),
	    extra_stats, DuckLakeUtil::BoolLiteral(s.min_is_exact), DuckLakeUtil::BoolLiteral(s.max_is_exact));
}

void DuckLakeStagedCommit::EmitInlinedColumnStatsRow(DuckLakeStagedRows &rows, TableIndex table_id,
                                                     FieldIndex column_id, const DuckLakeColumnStats &s) const {
	rows.Add(DuckLakeStagedTableType::INLINED_COLUMN_STATS,
	         StringUtil::Format("(%llu, %llu, %s)", table_id.index, column_id.index, EmitColumnStatsValues(s)));
}

void DuckLakeStagedCommit::EmitCommitHeader(DuckLakeStagedRows &rows, const DuckLakeSnapshotCommit &h,
                                            const DuckLakeSnapshot &snap, const string &data_path,
                                            const string &separator) const {
	bool has_snapshot = snap.snapshot_id != DConstants::INVALID_INDEX;
	string snap_id = has_snapshot ? std::to_string(snap.snapshot_id) : "NULL";
	string next_cat = has_snapshot ? std::to_string(snap.next_catalog_id) : "NULL";
	string next_file = has_snapshot ? std::to_string(snap.next_file_id) : "NULL";
	rows.Add(DuckLakeStagedTableType::COMMIT_HEADER,
	         StringUtil::Format("(%s, %s, %s, %s, %s, %s, %s, %s)", h.author.ToSQLString(),
	                            h.commit_message.ToSQLString(), h.commit_extra_info.ToSQLString(), snap_id,
	                            SQLString::ToString(data_path), SQLString::ToString(separator), next_cat, next_file));
}

void DuckLakeStagedCommit::EmitDataFiles(DuckLakeStagedRows &rows, const LocalTableChanges &local_changes,
                                         idx_t &local_file_id) const {
	for (auto &entry : local_changes.Changes()) {
		auto table_id = entry.first;
		auto &table_changes = entry.second;
		idx_t file_order = 0;
		for (auto &file : table_changes.new_data_files) {
			EmitDataFileRow(rows, file, local_file_id, table_id, file_order, "NULL");
			for (auto &del : file.delete_files) {
				EmitDeleteFileRow(rows, del, table_id, nullptr, local_file_id);
			}
			local_file_id++;
			file_order++;
		}
	}
}

void DuckLakeStagedCommit::EmitInlinedData(DuckLakeStagedRows &rows, const LocalTableChanges &local_changes,
                                           DuckLakeTransaction &transaction) const {
	for (auto &entry : local_changes.Changes()) {
		auto table_id = entry.first;
		auto &table_changes = entry.second;
		if (!table_changes.new_inlined_data) {
			continue;
		}
		auto &inlined = *table_changes.new_inlined_data;
		const bool has_preserved = inlined.HasPreservedRowIds();
		rows.Add(DuckLakeStagedTableType::INLINED_DATA,
		         StringUtil::Format("(%llu, %s)", table_id.index, DuckLakeUtil::BoolLiteral(has_preserved)));
		auto inlined_rows = DuckLakeUtil::InlinedDataToSQL(transaction, *inlined.data);
		for (idx_t row_order = 0; row_order < inlined_rows.size(); row_order++) {
			string tuple = "(" + inlined_rows[row_order] + ")";
			string preserved_row_id = "NULL";
			if (has_preserved) {
				auto rid = inlined.row_ids[row_order];
				if (!DuckLakeConstants::IsTransactionLocalRowId(rid)) {
					preserved_row_id = std::to_string(rid);
				}
			}
			rows.Add(DuckLakeStagedTableType::INLINED_ROW,
			         StringUtil::Format("(%llu, %llu, %s, %s)", table_id.index, row_order, preserved_row_id,
			                            SQLString(tuple)));
		}
		for (auto &stat : inlined.column_stats) {
			EmitInlinedColumnStatsRow(rows, table_id, stat.first, stat.second);
		}
	}
}

void DuckLakeStagedCommit::EmitInlinedDeletes(DuckLakeStagedRows &rows, const LocalTableChanges &local_changes) const {
	for (auto &entry : local_changes.Changes()) {
		auto table_id = entry.first;
		auto &table_changes = entry.second;
		for (auto &deletes_entry : table_changes.new_inlined_data_deletes) {
			auto inlined_table_name = SQLString(deletes_entry.first);
			for (auto &row_id : deletes_entry.second->rows) {
				rows.Add(DuckLakeStagedTableType::INLINED_DELETE,
				         StringUtil::Format("(%llu, %s, %llu)", table_id.index, inlined_table_name, row_id));
			}
		}
	}
}

void DuckLakeStagedCommit::EmitInlinedFileDeletes(DuckLakeStagedRows &rows,
                                                  const LocalTableChanges &local_changes) const {
	for (auto &entry : local_changes.Changes()) {
		auto table_id = entry.first;
		auto &table_changes = entry.second;
		if (!table_changes.new_inlined_file_deletes) {
			continue;
		}
		for (auto &file_entry : table_changes.new_inlined_file_deletes->file_deletes) {
			auto file_id = file_entry.first;
			for (auto &row_id : file_entry.second) {
				rows.Add(DuckLakeStagedTableType::INLINED_FILE_DELETE,
				         StringUtil::Format("(%llu, %llu, %llu)", table_id.index, file_id, row_id));
			}
		}
	}
}

void DuckLakeStagedCommit::EmitDeleteFiles(DuckLakeStagedRows &rows, const LocalTableChanges &local_changes) const {
	for (auto &entry : local_changes.Changes()) {
		auto table_id = entry.first;
		auto &table_changes = entry.second;
		for (auto &delete_entry : table_changes.new_delete_files) {
			auto &data_file_path = delete_entry.first;
			for (auto &file : delete_entry.second) {
				EmitDeleteFileRow(rows, file, table_id, data_file_path, optional_idx());
			}
		}
	}
}

void DuckLakeStagedCommit::EmitCompactions(DuckLakeStagedRows &rows, const LocalTableChanges &local_changes,
                                           idx_t &local_file_id) const {
	idx_t compaction_id = 0;
	for (auto &entry : local_changes.Changes()) {
		auto table_id = entry.first;
		auto &table_changes = entry.second;
		for (auto &compaction : table_changes.compactions) {
			idx_t output_order = 0;
			for (auto &written_file : compaction.written_files) {
				EmitDataFileRow(rows, written_file, local_file_id, table_id, output_order,
				                std::to_string(compaction_id));
				local_file_id++;
				output_order++;
			}

			rows.Add(DuckLakeStagedTableType::COMPACTION,
			         StringUtil::Format("(%llu, %llu, %s, %s)", compaction_id, table_id.index,
			                            SQLString(CompactionTypeToString(compaction.type)),
			                            DuckLakeUtil::OptionalIdxOrNull(compaction.row_id_start)));

			idx_t source_order = 0;
			for (auto &source : compaction.source_files) {
				string last_id = "NULL";
				string last_path = "NULL";
				string last_row_count = "NULL";
				string last_end_snap = "NULL";
				if (!source.delete_files.empty()) {
					auto &last = source.delete_files.back();
					last_id = std::to_string(last.delete_file_id.index);
					last_path = SQLString::ToString(last.data.path);
					last_row_count = std::to_string(last.row_count);
					last_end_snap = DuckLakeUtil::OptionalIdxOrNull(last.end_snapshot);
				}
				rows.Add(DuckLakeStagedTableType::COMPACTION_SOURCE,
				         StringUtil::Format("(%llu, %llu, %llu, %s, %llu, %llu, %s, %llu, %s, %s, %s, %s)",
				                            compaction_id, source_order, source.file.id.index,
				                            SQLString::ToString(source.file.data.path), source.file.row_count,
				                            source.file.begin_snapshot,
				                            DuckLakeUtil::OptionalIdxOrNull(source.max_partial_file_snapshot),
				                            static_cast<idx_t>(source.inlined_file_deletions.size()), last_id,
				                            last_path, last_row_count, last_end_snap));
				source_order++;
			}
			compaction_id++;
		}
	}
}

void DuckLakeStagedCommit::EmitNameMaps(DuckLakeStagedRows &rows, const DuckLakeNameMapSet &name_maps) const {
	for (auto &entry : name_maps.name_maps) {
		auto &map = *entry.second;
		rows.Add(DuckLakeStagedTableType::NAME_MAP,
		         StringUtil::Format("(%llu, %llu)", map.id.index, map.table_id.index));
		for (auto &col : map.FlattenColumns()) {
			auto parent = col.parent_column.IsValid() ? std::to_string(col.parent_column.GetIndex()) : "NULL";
			rows.Add(DuckLakeStagedTableType::NAME_MAP_ENTRY,
			         StringUtil::Format("(%llu, %llu, %s, %s, %llu, %s)", map.id.index, col.column_id, parent,
			                            SQLString(col.source_name), col.target_field_id.index,
			                            DuckLakeUtil::BoolLiteral(col.hive_partition)));
		}
	}
}

void DuckLakeStagedCommit::EmitFlushedInlinedTables(DuckLakeStagedRows &rows,
                                                    const vector<FlushedInlinedTableInfo> &flushed) const {
	for (auto &entry : flushed) {
		// LCOV_EXCL_START
		rows.Add(DuckLakeStagedTableType::FLUSHED_INLINED,
		         StringUtil::Format("(%s, %llu, %llu)", SQLString(entry.inlined_table.table_name),
		                            entry.inlined_table.schema_version, entry.flush_snapshot_id));
		// LCOV_EXCL_STOP
	}
}

void DuckLakeStagedCommit::EmitDroppedFiles(DuckLakeStagedRows &rows, DuckLakeTransaction &transaction) const {
	for (auto &entry : transaction.GetDroppedFiles()) {
		rows.Add(DuckLakeStagedTableType::DROPPED_FILE,
		         StringUtil::Format("(%s, %llu)", SQLString(entry.first), entry.second.index));
	}
	for (auto &table_id : transaction.GetTablesDeletedFrom()) {
		rows.Add(DuckLakeStagedTableType::TABLES_DELETED_FROM, StringUtil::Format("(%llu)", table_id.index));
	}
	for (auto &table_id : transaction.GetTablesDeleteAttempted()) {
		rows.Add(DuckLakeStagedTableType::TABLES_DELETE_ATTEMPTED, StringUtil::Format("(%llu)", table_id.index));
	}
}

string DuckLakeStagedCommit::Build(DuckLakeTransaction &transaction, const DuckLakeSnapshot &transaction_snapshot,
                                   const DuckLakeRetryConfig &retry_config,
                                   DuckLakeStagedTableState &staged_tables) const {
	auto &ducklake_catalog = transaction.GetCatalog();
	auto &local_changes = transaction.GetLocalChanges();

	DuckLakeStagedRows rows;
	EmitCommitHeader(rows, transaction.GetCommitInfo(), transaction_snapshot, ducklake_catalog.DataPath(),
	                 ducklake_catalog.Separator());
	idx_t local_file_id = 0;
	EmitDataFiles(rows, local_changes, local_file_id);
	EmitCompactions(rows, local_changes, local_file_id);
	EmitInlinedData(rows, local_changes, transaction);
	EmitInlinedDeletes(rows, local_changes);
	EmitInlinedFileDeletes(rows, local_changes);
	EmitDeleteFiles(rows, local_changes);
	EmitDroppedFiles(rows, transaction);
	EmitFlushedInlinedTables(rows, transaction.GetFlushedInlinedTables());
	EmitNameMaps(rows, transaction.GetNewNameMaps());

	string batch = staged_tables.SetupSql(rows.FilledTables());
	batch += rows.InsertSql();
	int64_t schema_version_param = transaction_snapshot.snapshot_id != DConstants::INVALID_INDEX
	                                   ? static_cast<int64_t>(transaction_snapshot.schema_version)
	                                   : -1;
	batch += StringUtil::Format("SELECT * FROM ducklake_commit(%s, %lld, "
	                            "max_retry_count => %llu, retry_wait_ms => %llu, retry_backoff => %f);",
	                            SQLString::ToString(ducklake_catalog.MetadataSchemaName().GetIdentifierName()),
	                            schema_version_param, retry_config.max_retry_count, retry_config.retry_wait_ms,
	                            retry_config.retry_backoff);
	return batch;
}

} // namespace duckdb
