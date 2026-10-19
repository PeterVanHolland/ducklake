#include "metadata_manager/sqlite_metadata_manager.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/main/database.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

SQLiteMetadataManager::SQLiteMetadataManager(DuckLakeTransaction &transaction) : DuckLakeMetadataManager(transaction) {
}

bool SQLiteMetadataManager::TypeIsNativelySupported(const LogicalType &type) {
	switch (type.id()) {
	// Unnamed composite types are not supported.
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::MAP:
	case LogicalTypeId::LIST:
	// SQLite converts IEEE 754 NaN to NULL when storing double values,
	// so FLOAT/DOUBLE must be stored as VARCHAR to preserve NaN through the round-trip
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::VARIANT:
		return false;
	default:
		return true;
	}
}

static bool IsLockedCommit(const string &lower_message) {
	return StringUtil::Contains(lower_message, "failed to execute query \"commit\": database is locked");
}

bool SQLiteMetadataManager::IsRetryableCommitError(const string &message) const {
	auto lower_message = StringUtil::Lower(message);
	StringUtil::Trim(lower_message);
	return StringUtil::EndsWith(lower_message, "database is locked") && !IsLockedCommit(lower_message);
}

string SQLiteMetadataManager::CommitErrorHint(const string &message) const {
	if (!IsLockedCommit(StringUtil::Lower(message))) {
		return string();
	}
	auto &parameters = transaction.GetCatalog().MetadataParameters();
	auto journal_mode = parameters.find("journal_mode");
	if (journal_mode != parameters.end() && StringUtil::CIEquals(journal_mode->second.ToString(), "wal")) {
		return string();
	}
	return "In its rollback journal mode, a SQLite catalog cannot commit while another transaction is open on it. "
	       "Attach the catalog with META_JOURNAL_MODE 'WAL' to let open transactions and a commit run at the same "
	       "time.\n";
}

string SQLiteMetadataManager::GetColumnTypeInternal(const LogicalType &column_type) {
	switch (column_type.id()) {
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return "VARCHAR";
	case LogicalTypeId::SQLNULL:
		return "INTEGER";
	default:
		return column_type.ToString();
	}
}

void SQLiteMetadataManager::RenameInlinedColumns(const map<string, vector<pair<string, string>>> &renames) {
	// SQLite parses its whole schema for every renamed column, so the tables are copied with the new names instead
	string query;
	for (auto &table : renames) {
		auto table_name = SQLIdentifier(table.first);
		vector<string> renamed_columns;
		for (auto &column : table.second) {
			renamed_columns.push_back(StringUtil::Format("%s AS %s", column.first, column.second));
		}
		query += StringUtil::Format(
		    "CREATE TEMP TABLE ducklake_renamed_inlined AS SELECT * FROM {METADATA_CATALOG}.%s;", table_name);
		query += StringUtil::Format("DROP TABLE {METADATA_CATALOG}.%s;", table_name);
		query += StringUtil::Format(
		    "CREATE TABLE {METADATA_CATALOG}.%s AS SELECT * RENAME (%s) FROM temp.ducklake_renamed_inlined;",
		    table_name, StringUtil::Join(renamed_columns, ", "));
		query += "DROP TABLE temp.ducklake_renamed_inlined;";
	}
	auto result = Execute(query);
	result->ThrowIfError("Failed to rename inlined-data metadata columns while migrating to v1.1: ");
}

} // namespace duckdb
