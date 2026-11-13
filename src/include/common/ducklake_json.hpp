//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/ducklake_json.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/json_document.hpp"

namespace duckdb {

//! Readers of JSON members that reject a member of another value type
struct DuckLakeJSON {
	static void ExpectObject(const string &key, JSONValue value) {
		if (!value.IsObject()) {
			throw InvalidInputException("\"%s\" must be an object", key);
		}
	}

	static string GetString(const string &key, JSONValue value) {
		if (!value.IsString()) {
			throw InvalidInputException("\"%s\" must be a string", key);
		}
		return value.GetString();
	}

	static bool GetBoolean(const string &key, JSONValue value) {
		if (value.GetType() != JSONValueType::BOOLEAN) {
			throw InvalidInputException("\"%s\" must be a boolean", key);
		}
		return value.GetBoolean();
	}

	static idx_t GetCount(const string &key, JSONValue value) {
		if (value.GetType() != JSONValueType::UNSIGNED_INTEGER) {
			throw InvalidInputException("\"%s\" must be a non-negative integer", key);
		}
		return value.GetUnsignedInteger();
	}

	static void IterateStrings(const string &key, JSONValue value,
	                           const std::function<void(const string &)> &callback) {
		if (!value.IsArray()) {
			throw InvalidInputException("\"%s\" must be an array of strings", key);
		}
		value.IterateArray([&](JSONValue element) { callback(GetString(key, element)); });
	}
};

} // namespace duckdb
