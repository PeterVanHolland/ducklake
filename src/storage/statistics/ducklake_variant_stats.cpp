#include "common/ducklake_json.hpp"
#include "duckdb/common/error_data.hpp"
#include "storage/ducklake_variant_stats.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/json_document.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/storage/statistics/variant_stats.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "storage/ducklake_insert.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "storage/ducklake_metadata_info.hpp"

namespace duckdb {

DuckLakeColumnVariantStats::DuckLakeColumnVariantStats() : DuckLakeColumnExtraStats(DuckLakeExtraStatsType::VARIANT) {
}

DuckLakeVariantStats::DuckLakeVariantStats(LogicalType shredded_type_p, DuckLakeColumnStats field_stats_p)
    : shredded_type(std::move(shredded_type_p)), field_stats(std::move(field_stats_p)) {
}

void DuckLakeColumnVariantStats::Merge(const DuckLakeColumnExtraStats &new_stats_p) {
	// the merged statistics replace the invalid ones, with no shredded fields
	invalid_reason.clear();
	if (shredded_field_stats.empty()) {
		// nothing to merge
		return;
	}
	auto &new_stats = new_stats_p.Cast<DuckLakeColumnVariantStats>();
	vector<string> stats_to_erase;
	for (auto &entry : shredded_field_stats) {
		auto other_entry = new_stats.shredded_field_stats.find(entry.first);
		if (other_entry == new_stats.shredded_field_stats.end()) {
			// no stats - erase it
			stats_to_erase.push_back(entry.first);
			continue;
		}
		if (entry.second.shredded_type != other_entry->second.shredded_type) {
			// incompatible shredded types - drop the field
			stats_to_erase.push_back(entry.first);
			continue;
		}
		// merge stats
		entry.second.field_stats.MergeStats(other_entry->second.field_stats);
	}
	// erase any stats that do not occur in both
	for (auto &entry : stats_to_erase) {
		shredded_field_stats.erase(entry);
	}
}

unique_ptr<DuckLakeColumnExtraStats> DuckLakeColumnVariantStats::Copy() const {
	return make_uniq<DuckLakeColumnVariantStats>(*this);
}

void DuckLakeColumnVariantStats::Serialize(DuckLakeColumnStatsInfo &column_stats) const {
	for (auto &entry : shredded_field_stats) {
		DuckLakeVariantStatsInfo shredded_stats;
		shredded_stats.field_name = entry.first;
		shredded_stats.shredded_type = DuckLakeTypes::ToString(entry.second.shredded_type);
		shredded_stats.field_stats =
		    DuckLakeColumnStatsInfo::FromColumnStats(column_stats.column_id, entry.second.field_stats);
		column_stats.variant_stats.push_back(std::move(shredded_stats));
	}
	TrySerialize(column_stats.extra_stats);
}

static bool TryGetCount(const JSONValue &obj, const string &key, idx_t &result) {
	auto member = obj.GetMember(key);
	if (!member.IsValid()) {
		return false;
	}
	result = DuckLakeJSON::GetCount(key, member);
	return true;
}

static bool TryGetBoolean(const JSONValue &obj, const string &key, bool &result) {
	auto member = obj.GetMember(key);
	if (!member.IsValid()) {
		return false;
	}
	result = DuckLakeJSON::GetBoolean(key, member);
	return true;
}

static bool TryGetString(const JSONValue &obj, const string &key, string &result) {
	auto member = obj.GetMember(key);
	if (!member.IsValid()) {
		return false;
	}
	result = DuckLakeJSON::GetString(key, member);
	return true;
}

static DuckLakeVariantStats DeserializeShreddedStats(const LogicalType &shredded_type, const JSONValue &obj) {
	DuckLakeColumnStats column_stats(shredded_type);
	DuckLakeVariantStats variant_stats(shredded_type, std::move(column_stats));

	auto &stats = variant_stats.field_stats;
	stats.has_null_count = TryGetCount(obj, "null_count", stats.null_count);
	stats.has_num_values = TryGetCount(obj, "num_values", stats.num_values);
	if (stats.has_null_count && stats.has_num_values && stats.null_count > stats.num_values) {
		throw InvalidInputException("\"null_count\" exceeds \"num_values\"");
	}
	TryGetCount(obj, "column_size_bytes", stats.column_size_bytes);

	stats.has_min = TryGetString(obj, "min", stats.min);
	stats.has_max = TryGetString(obj, "max", stats.max);
	stats.has_contains_nan = TryGetBoolean(obj, "contains_nan", stats.contains_nan);

	bool any_valid;
	if (TryGetBoolean(obj, "any_valid", any_valid)) {
		stats.any_valid = any_valid;
	}

	string extra_stats;
	if (stats.extra_stats && TryGetString(obj, "extra_stats", extra_stats)) {
		// LCOV_EXCL_START
		stats.extra_stats->Deserialize(extra_stats);
		// LCOV_EXCL_STOP
	}

	return variant_stats;
}

static JSONMutableValue SerializeShreddedStats(JSONWriter &writer, const string &field_name,
                                               const DuckLakeVariantStats &variant_stats) {
	auto obj = writer.CreateObject();
	obj.AddString("field_name", field_name);
	obj.AddString("shredded_type", DuckLakeTypes::ToString(variant_stats.shredded_type));

	auto &stats = variant_stats.field_stats;
	if (stats.has_null_count) {
		obj.Add("null_count", writer.CreateUnsignedInteger(stats.null_count));
	}

	if (stats.has_min) {
		obj.AddString("min", stats.min);
	}

	if (stats.has_max) {
		obj.AddString("max", stats.max);
	}

	if (stats.has_num_values) {
		obj.Add("num_values", writer.CreateUnsignedInteger(stats.num_values));
	}

	if (stats.has_contains_nan) {
		obj.Add("contains_nan", writer.CreateBoolean(stats.contains_nan));
	}

	obj.Add("column_size_bytes", writer.CreateUnsignedInteger(stats.column_size_bytes));
	obj.Add("any_valid", writer.CreateBoolean(stats.any_valid));

	if (stats.extra_stats) {
		// LCOV_EXCL_START
		string extra_stats_str;
		if (stats.extra_stats->TrySerialize(extra_stats_str)) {
			obj.AddString("extra_stats", extra_stats_str);
			// LCOV_EXCL_STOP
		}
	}
	return obj;
}

bool DuckLakeColumnVariantStats::TrySerialize(string &result) const {
	if (shredded_field_stats.empty()) {
		// no shredded stats
		return false;
	}

	JSONWriter writer;
	auto root = writer.CreateArray();
	for (auto &entry : shredded_field_stats) {
		root.Append(SerializeShreddedStats(writer, entry.first, entry.second));
	}
	writer.SetRoot(root);
	result = SQLString::ToString(writer.ToString());
	return true;
}

struct NestedVariantStats {
	// field name, if leaf
	string full_field_name;
	// child stats, if any
	vector<NestedVariantStats> child_stats;
	vector<string> child_names;

	LogicalType ToType(const unordered_map<string, DuckLakeVariantStats> &field_stats) {
		if (!full_field_name.empty()) {
			// field, find it in the stats
			auto entry = field_stats.find(full_field_name);
			if (entry == field_stats.end()) {
				throw InternalException("NestedVariantStats failed to find field name");
			}
			if (!child_stats.empty()) {
				throw InvalidInputException("Field \"%s\" has shredded statistics and child fields", full_field_name);
			}
			return entry->second.shredded_type;
		}
		if (child_names.empty()) {
			// list
			if (child_stats.size() != 1) {
				throw InvalidInputException("A list field needs exactly its element field in the VARIANT stats");
			}
			return LogicalType::LIST(child_stats[0].ToType(field_stats));
		}
		if (child_names.size() != child_stats.size()) {
			throw InvalidInputException("The VARIANT stats mix a list element and struct fields in one field");
		}
		// not a leaf - this is a struct
		// recurse into the children
		child_list_t<LogicalType> children;
		for (idx_t i = 0; i < child_names.size(); i++) {
			children.emplace_back(child_names[i], child_stats[i].ToType(field_stats));
		}
		return LogicalType::STRUCT(std::move(children));
	}

	void ConvertStats(const unordered_map<string, DuckLakeVariantStats> &field_stats, BaseStatistics &main_stats) {
		auto &stats = StructStats::GetChildStats(main_stats, VariantStats::TYPED_VALUE_INDEX);

		auto &untyped_value_index_stats = StructStats::GetChildStats(main_stats, VariantStats::UNTYPED_VALUE_INDEX);
		untyped_value_index_stats.SetHasNull();
		if (!full_field_name.empty()) {
			// field, find it in the stats
			auto entry = field_stats.find(full_field_name);
			if (entry == field_stats.end()) {
				throw InternalException("NestedVariantStats failed to find field name");
			}
			// unknown statistics when the bounds do not parse
			StructStats::SetChildStats(main_stats, VariantStats::TYPED_VALUE_INDEX,
			                           entry->second.field_stats.ToStats());
			return;
		}
		if (child_names.empty()) {
			// list
			child_stats[0].ConvertStats(field_stats, ListStats::GetChildStats(stats));
			return;
		}
		// struct - recurse into children
		for (idx_t struct_idx = 0; struct_idx < child_stats.size(); struct_idx++) {
			auto &struct_child_stats = StructStats::GetChildStats(stats, struct_idx);
			child_stats[struct_idx].ConvertStats(field_stats, struct_child_stats);
		}
	}
};

//! The tree of the stored paths is walked recursively, so their depth is bounded
static constexpr idx_t MAX_SHREDDED_FIELD_DEPTH = 1000;

void ToNestedVariantStats(const string &field_name, reference<NestedVariantStats> stats) {
	if (field_name == "root") {
		// the top level variant itself
		stats.get().full_field_name = field_name;
		return;
	}
	idx_t pos = 0;
	idx_t depth = 0;
	while (pos < field_name.size()) {
		if (++depth > MAX_SHREDDED_FIELD_DEPTH) {
			throw InvalidInputException("Field %s is nested too deeply", field_name);
		}
		if (field_name[pos] != '"') {
			// an unquoted segment runs to the next dot
			idx_t start_pos = pos;
			for (; pos < field_name.size(); pos++) {
				if (field_name[pos] == '.') {
					break;
				}
			}
			auto subfield = field_name.substr(start_pos, pos - start_pos);
			if (subfield != "element") {
				throw InvalidInputException("Invalid unquoted field %s, expected element or root", subfield);
			}
			if (!stats.get().child_names.empty()) {
				throw InvalidInputException("Field %s mixes a list element with struct fields", field_name);
			}
			if (stats.get().child_stats.empty()) {
				stats.get().child_stats.emplace_back();
			}
			stats = stats.get().child_stats.back();
			if (pos >= field_name.size()) {
				// the list element is the leaf
				stats.get().full_field_name = field_name;
				return;
			} else {
				// a nested list
				pos++;
			}
			continue;
		}
		// a quoted segment is a struct field name
		string current_field_name;
		if (!StringUtil::TryParseQuotedString(field_name, pos, current_field_name)) {
			break;
		}
		// the field may already have a node from an earlier path
		auto &child_names = stats.get().child_names;
		auto &child_stats = stats.get().child_stats;
		if (child_names.size() != child_stats.size()) {
			throw InvalidInputException("Field %s mixes a list element with struct fields", field_name);
		}
		idx_t struct_idx;
		for (struct_idx = 0; struct_idx < child_names.size(); struct_idx++) {
			if (child_names[struct_idx] == current_field_name) {
				break;
			}
		}
		if (struct_idx >= child_names.size()) {
			// a new field
			child_names.push_back(current_field_name);
			child_stats.emplace_back();
		}
		stats = child_stats[struct_idx];
		if (pos >= field_name.size()) {
			// the struct field is the leaf
			stats.get().full_field_name = field_name;
			return;
		} else {
			if (field_name[pos] != '.') {
				// a dot has to separate the segments
				break;
			}
			pos++;
		}
	}
	throw InvalidInputException("Incorrectly formatted field name %s", field_name);
}

//! The tree of the stored field paths and the shredded type it describes
static LogicalType BuildShreddedType(const unordered_map<string, DuckLakeVariantStats> &field_stats,
                                     NestedVariantStats &nested_stats) {
	for (auto &entry : field_stats) {
		ToNestedVariantStats(entry.first, nested_stats);
	}
	return nested_stats.ToType(field_stats);
}

static void ParseShreddedFieldStats(const string &stats, unordered_map<string, DuckLakeVariantStats> &result) {
	JSONParseError error;
	auto doc = JSONDocument::TryParse(stats.c_str(), stats.size(), error);
	if (!doc || !doc->GetRoot().IsArray()) {
		throw InvalidInputException("VARIANT stats JSON \"%s\" is not an array of fields", stats);
	}
	doc->GetRoot().IterateArray([&](JSONValue obj) {
		auto field_name_val = obj.GetMember("field_name");
		auto shredded_type_val = obj.GetMember("shredded_type");
		if (!field_name_val.IsString() || !shredded_type_val.IsString()) {
			throw InvalidInputException(
			    "A field without a field_name and shredded_type string in VARIANT stats JSON \"%s\"", stats);
		}
		auto field_name = field_name_val.GetString();
		auto type_string = shredded_type_val.GetString();
		LogicalType shredded_type;
		try {
			shredded_type = DuckLakeTypes::FromString(type_string);
		} catch (InvalidInputException &ex) {
			throw InvalidInputException("Field \"%s\" has the shredded type \"%s\" in VARIANT stats JSON \"%s\", %s",
			                            field_name, type_string, stats, ErrorData(ex).RawMessage());
		}
		// nested children are stored as fields of their own
		if (!shredded_type.IsComplete()) {
			throw InvalidInputException("Field \"%s\" has the incomplete shredded type \"%s\" in VARIANT stats JSON "
			                            "\"%s\", a nested type has its children as fields of their own",
			                            field_name, type_string, stats);
		}
		try {
			if (!result.emplace(field_name, DeserializeShreddedStats(shredded_type, obj)).second) {
				throw InvalidInputException("the field appears twice");
			}
		} catch (InvalidInputException &ex) {
			throw InvalidInputException("Field \"%s\" in VARIANT stats JSON \"%s\", %s", field_name, stats,
			                            ErrorData(ex).RawMessage());
		}
	});
	// the field paths have to form one tree of struct fields and list elements
	if (!result.empty()) {
		NestedVariantStats nested_stats;
		BuildShreddedType(result, nested_stats);
	}
}

void DuckLakeColumnVariantStats::Deserialize(const string &stats) {
	// defer invalid statistics errors until the column is read
	try {
		ParseShreddedFieldStats(stats, shredded_field_stats);
	} catch (InvalidInputException &ex) {
		shredded_field_stats.clear();
		invalid_reason = ErrorData(ex).RawMessage();
	}
}

unique_ptr<BaseStatistics> DuckLakeColumnVariantStats::ToStats() const {
	if (!invalid_reason.empty()) {
		throw InvalidInputException(invalid_reason);
	}
	if (shredded_field_stats.empty()) {
		return nullptr;
	}
	// create a nested structure that holds all the stats in the original nested order
	NestedVariantStats nested_stats;
	auto shredded_type = BuildShreddedType(shredded_field_stats, nested_stats);
	auto variant_stats = VariantStats::CreateShredded(VariantStats::GetShreddingType(shredded_type));

	auto &shredded_stats = VariantStats::GetShreddedStats(variant_stats);
	nested_stats.ConvertStats(shredded_field_stats, shredded_stats);

	variant_stats.SetHasNull();
	variant_stats.SetHasNoNull();
	return variant_stats.ToUnique();
}

bool DuckLakeColumnVariantStats::ParseStats(const string &stats_name, const vector<Value> &stats_children) {
	if (stats_name == "variant_type") {
		auto type_str = StringValue::Get(stats_children[1]);
		variant_type = UnboundType::TryParseAndDefaultBind(type_str);
		return true;
	}
	// LCOV_EXCL_START
	return false;
	// LCOV_EXCL_STOP
}

vector<string> ExtractVariantFieldNames(const vector<string> &path, idx_t variant_field_start) {
	vector<string> field_names;
	for (idx_t i = variant_field_start; i + 1 < path.size(); i += 2) {
		if (path[i] != "typed_value") {
			// LCOV_EXCL_START
			throw InvalidInputException("Expected typed_value at position %d in path %s", i,
			                            StringUtil::Join(path, "."));
			// LCOV_EXCL_STOP
		}
		field_names.push_back(path[i + 1]);
	}
	return field_names;
}

LogicalType ExtractVariantType(const LogicalType &variant_type, const vector<string> &field_names,
                               string &variant_field_name, idx_t field_idx = 0) {
	if (field_idx == 0 && field_names.empty()) {
		variant_field_name = "root";
	}
	if (variant_type.id() != LogicalTypeId::STRUCT) {
		// LCOV_EXCL_START
		throw InvalidInputException(
		    "Expected variant type to be struct at this layer while looking for field %s - but found %s",
		    StringUtil::Join(field_names, "."), variant_type);
		// LCOV_EXCL_STOP
	}
	// find the "typed_value" within this entry
	for (auto &entry : StructType::GetChildTypes(variant_type)) {
		if (entry.first == "typed_value") {
			// found!
			if (field_idx >= field_names.size()) {
				// reached the final type - this is the type
				return entry.second;
			}
			auto &field_name = field_names[field_idx];
			if (entry.second.id() == LogicalTypeId::LIST) {
				if (field_name != "element") {
					// LCOV_EXCL_START
					throw InvalidInputException(
					    "Found a list at this layer - expected a field named \"element\" but got a field named \"%s\"",
					    field_name);
					// LCOV_EXCL_STOP
				}
				if (!variant_field_name.empty()) {
					variant_field_name += ".";
				}
				variant_field_name += "element";
				return ExtractVariantType(ListType::GetChildType(entry.second), field_names, variant_field_name,
				                          field_idx + 1);
			}
			if (entry.second.id() != LogicalTypeId::STRUCT) {
				// LCOV_EXCL_START
				throw InvalidInputException(
				    "Expected variant type to be struct at this layer while looking for nested field %s - but found %s",
				    StringUtil::Join(field_names, "."), variant_type);
				// LCOV_EXCL_STOP
			}
			// not the final field - recurse to find the field
			for (auto &typed_child : StructType::GetChildTypes(entry.second)) {
				if (typed_child.first == field_name) {
					// found the field to recurse on
					if (!variant_field_name.empty()) {
						variant_field_name += ".";
					}
					variant_field_name += SQLQuotedIdentifier::ToString(field_name);
					return ExtractVariantType(typed_child.second, field_names, variant_field_name, field_idx + 1);
				}
			}
			throw InvalidInputException("Could not find field %s in type %s", field_name, variant_type);
		}
	}
	throw InvalidInputException("Could not find typed_value field in type %s", variant_type);
}

PartialVariantStats::PartialVariantStats() : result(LogicalTypeId::VARIANT) {
}

void PartialVariantStats::ParseVariantStats(const vector<string> &path, idx_t variant_field_start,
                                            const vector<Value> &col_stats) {
	if (path.size() == variant_field_start + 1 && path.back() == "metadata") {
		// metadata provides the top-level metadata, together with the shredding types
		auto metadata_stats = DuckLakeInsert::ParseColumnStats(LogicalTypeId::VARIANT, col_stats);
		// metadata is written for NULL variants too, so its null count does not show them
		if (metadata_stats.has_num_values) {
			result.has_num_values = true;
			result.num_values = metadata_stats.num_values;
		}
		result.column_size_bytes += metadata_stats.column_size_bytes;

		variant_type = std::move(metadata_stats.extra_stats->Cast<DuckLakeColumnVariantStats>().variant_type);
		return;
	}
	// this is information about a field within the variant
	// this must be either a "value" (untyped info) or "typed_value" (shredded info)
	auto variant_field_names = ExtractVariantFieldNames(path, variant_field_start);
	if (path.back() == "typed_value") {
		// typed info - extract the shredded type from the variant type
		string variant_field_name;
		auto shredded_type = ExtractVariantType(variant_type, variant_field_names, variant_field_name);
		auto shredded_stats = DuckLakeInsert::ParseColumnStats(shredded_type, col_stats);
		result.column_size_bytes += shredded_stats.column_size_bytes;
		DuckLakeVariantStats variant_field_stats(std::move(shredded_type), std::move(shredded_stats));
		field_names[variant_field_names] = std::move(variant_field_name);
		shredded_field_stats.insert(make_pair(variant_field_names, std::move(variant_field_stats)));
		return;
	}
	if (path.back() == "value") {
		// untyped info - blob stats
		auto untyped_stats = DuckLakeInsert::ParseColumnStats(LogicalTypeId::BLOB, col_stats);
		result.column_size_bytes += untyped_stats.column_size_bytes;
		// check if there are any untyped values - if they are ALL null this is fully shredded
		// for partially shredded data we don't write any stats to ducklake
		bool is_fully_shredded = false;
		if (untyped_stats.has_null_count && untyped_stats.has_num_values &&
		    untyped_stats.null_count == untyped_stats.num_values) {
			// all untyped values are NULL - fully shredded
			is_fully_shredded = true;
		}
		if (untyped_stats.has_min && untyped_stats.has_max && untyped_stats.min == "00" && untyped_stats.max == "00") {
			// we have untyped values - but they are all NULL
			is_fully_shredded = true;
		}
		if (is_fully_shredded) {
			fully_shredded_fields.insert(variant_field_names);
		}
		return;
	}
	// LCOV_EXCL_START
	throw InvalidInputException("Variant stats - unknown path element %s - expected metadata, typed_value or value",
	                            path.back());
	// LCOV_EXCL_STOP
}

DuckLakeColumnStats PartialVariantStats::Finalize() {
	// for any fully shredded variant - copy over the stats
	auto &variant_stats = result.extra_stats->Cast<DuckLakeColumnVariantStats>();
	for (auto &entry : fully_shredded_fields) {
		auto shredded_entry = shredded_field_stats.find(entry);
		if (shredded_entry != shredded_field_stats.end()) {
			auto field_name_entry = field_names.find(shredded_entry->first);
			if (field_name_entry == field_names.end()) {
				throw InternalException("Found shredded stats but not shredded field name");
			}
			variant_stats.shredded_field_stats.emplace(std::move(field_name_entry->second),
			                                           std::move(shredded_entry->second));
			field_names.erase(field_name_entry);
			shredded_field_stats.erase(entry);
		}
	}
	return std::move(result);
}

} // namespace duckdb
