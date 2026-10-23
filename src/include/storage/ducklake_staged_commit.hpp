//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_staged_commit.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/ducklake_snapshot.hpp"
#include "common/index.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/set.hpp"

#include <vector>

namespace duckdb {

class ClientContext;
class DataChunk;
class DuckLakeMetadataManager;
class DuckLakeTransaction;
class Value;
enum class CompactionType;
enum class DeleteFileSource : uint8_t;
struct DuckLakeColumnStats;
struct DuckLakeDataFile;
struct DuckLakeDeleteFile;
struct DuckLakeNameMapEntry;
struct DuckLakeNameMapSet;
struct DuckLakeRetryConfig;
struct DuckLakeSnapshotCommit;
struct FlushedInlinedTableInfo;
struct LocalTableChanges;
struct TransactionChangeInformation;

enum class DuckLakeStagedTableType : uint8_t {
	COMMIT_HEADER,
	DATA_FILE,
	DATA_FILE_COLUMN_STATS,
	DATA_FILE_PARTITION,
	DELETE_FILE,
	INLINED_DATA,
	INLINED_ROW,
	INLINED_COLUMN_STATS,
	INLINED_DELETE,
	INLINED_FILE_DELETE,
	DROPPED_FILE,
	TABLES_DELETED_FROM,
	TABLES_DELETE_ATTEMPTED,
	FLUSHED_INLINED,
	COMPACTION,
	COMPACTION_SOURCE,
	NAME_MAP,
	NAME_MAP_ENTRY,
};

//! A staging table bound to a specific commit (schema + identifier suffix).
class DuckLakeStagedTable {
public:
	static const char *BaseName(DuckLakeStagedTableType type);
	static string Columns(DuckLakeStagedTableType type);
	static const vector<DuckLakeStagedTableType> &AllTypes();
	static string CreateAllSql();
	static string TruncateAllSql();
	static const char *DeleteFileSourceToString(DeleteFileSource source);
	static DeleteFileSource DeleteFileSourceFromString(const string &source);
};

//! The rows a commit stages, sent as one INSERT per staged table
class DuckLakeStagedRows {
public:
	DuckLakeStagedRows();

	void Add(DuckLakeStagedTableType type, string tuple);
	string InsertSql() const;
	set<DuckLakeStagedTableType> FilledTables() const;

private:
	vector<vector<string>> rows;
};

//! The staged tables in the Quack session of a metadata database, used under its lock until the batch ran
class DuckLakeStagedTableState {
public:
	//! Shared by every DuckLake on the metadata database the transaction uses, since they use the same session
	static shared_ptr<DuckLakeStagedTableState> Get(DuckLakeTransaction &transaction);
	//! Creates the tables on first use and empties the ones an earlier batch can have filled
	string SetupSql(const set<DuckLakeStagedTableType> &tables_to_fill);
	//! After a failed batch the next one sets up every table again
	void BatchFinished(bool success);

	mutex lock;

private:
	bool created = false;
	set<DuckLakeStagedTableType> filled;
	set<DuckLakeStagedTableType> batch_tables;
};

//! Builds the SQL batch that stages a commit into temporary tables and calls ducklake_commit.
class DuckLakeStagedCommit {
public:
	//! Build the full SQL batch from transaction state.
	string Build(DuckLakeTransaction &transaction, const DuckLakeSnapshot &transaction_snapshot,
	             const DuckLakeRetryConfig &retry_config, DuckLakeStagedTableState &staged_tables) const;

private:
	//! Stages a single data file, its column stats, and partition values.
	void EmitDataFileRow(DuckLakeStagedRows &rows, const DuckLakeDataFile &file, idx_t local_file_id,
	                     TableIndex table_id, idx_t file_order, const string &compaction_id_literal) const;
	void EmitDeleteFileRow(DuckLakeStagedRows &rows, const DuckLakeDeleteFile &file, TableIndex table_id,
	                       optional_ptr<const string> data_file_path, optional_idx attached_local_file_id) const;
	//! Stages a single inlined column stats row.
	void EmitInlinedColumnStatsRow(DuckLakeStagedRows &rows, TableIndex table_id, FieldIndex column_id,
	                               const DuckLakeColumnStats &s) const;
	//! Builds the shared 13-value column-stats payload (column_size_bytes .. extra_stats) used by both
	//! the data-file and inlined column-stats staging rows. Read back by ReadColumnStatsRow.
	static string EmitColumnStatsValues(const DuckLakeColumnStats &s);

	//! Stages the commit header row (author, message, snapshot info, paths).
	void EmitCommitHeader(DuckLakeStagedRows &rows, const DuckLakeSnapshotCommit &h, const DuckLakeSnapshot &snap,
	                      const string &data_path, const string &separator) const;
	//! Stages all new data files and their attached deletes.
	void EmitDataFiles(DuckLakeStagedRows &rows, const LocalTableChanges &local_changes, idx_t &local_file_id) const;
	//! Stages inlined data rows, tuple literals, and per-column stats.
	void EmitInlinedData(DuckLakeStagedRows &rows, const LocalTableChanges &local_changes,
	                     DuckLakeTransaction &transaction) const;
	//! Stages inlined row-level deletes (by inlined table name).
	void EmitInlinedDeletes(DuckLakeStagedRows &rows, const LocalTableChanges &local_changes) const;
	//! Stages inlined file-level deletes (by file id + row id).
	void EmitInlinedFileDeletes(DuckLakeStagedRows &rows, const LocalTableChanges &local_changes) const;
	//! Stages loose delete files against pre-existing parquet files.
	void EmitDeleteFiles(DuckLakeStagedRows &rows, const LocalTableChanges &local_changes) const;
	//! Stages compaction headers, output files, and source file entries.
	void EmitCompactions(DuckLakeStagedRows &rows, const LocalTableChanges &local_changes, idx_t &local_file_id) const;
	//! Stages column name mapping entries.
	void EmitNameMaps(DuckLakeStagedRows &rows, const DuckLakeNameMapSet &name_maps) const;
	//! Stages flushed inlined table references.
	void EmitFlushedInlinedTables(DuckLakeStagedRows &rows, const vector<FlushedInlinedTableInfo> &flushed) const;
	//! Stages dropped file paths and tables-deleted-from markers.
	void EmitDroppedFiles(DuckLakeStagedRows &rows, DuckLakeTransaction &transaction) const;
};

} // namespace duckdb
