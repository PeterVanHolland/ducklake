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

bool SQLiteMetadataManager::IsRetryableCommitError(const string &message) const {
	auto lower_message = StringUtil::Lower(message);
	StringUtil::Trim(lower_message);
	return StringUtil::EndsWith(lower_message, "database is locked") &&
	       !StringUtil::Contains(lower_message, "failed to execute query \"commit\": database is locked");
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

case_insensitive_set_t SQLiteMetadataManager::GetLegacyNamedInlinedTables() {
	// the catalog loads the columns of each table with its own query, SQLite lists them all at once
	auto remote_query = StringUtil::Format(
	    "SELECT m.name FROM sqlite_master m, pragma_table_info(m.name) p WHERE m.type = 'table' AND m.name GLOB "
	    "'ducklake_inlined_data_*' AND p.cid = 0 AND p.name = %s",
	    SQLString::ToString(DuckLakeInlinedColNames(false).row_id));
	auto result = Query(StringUtil::Format("SELECT name FROM sqlite_query({METADATA_CATALOG_NAME_LITERAL}, %s)",
	                                       SQLString::ToString(remote_query)));
	result->ThrowIfError("Failed to list the inlined-data tables with v1.0 column names: ");
	case_insensitive_set_t legacy_tables;
	for (auto &row : *result) {
		legacy_tables.insert(row.GetValue<string>(0));
	}
	return legacy_tables;
}

} // namespace duckdb
