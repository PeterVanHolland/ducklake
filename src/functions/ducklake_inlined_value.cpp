#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

//! Reads the text of an inlined nested value, as the metadata catalogs that store nested values as text wrote it.
//! The text is DuckDB's rendering of the value. A JSON leaf is either a quoted string holding the JSON text, or the
//! JSON text itself with NULL for a missing element, as older versions rendered it: the generic text parser would
//! read a JSON null there as a missing value and a JSON string as its unquoted contents.
struct InlinedTextParser {
	InlinedTextParser(const char *buf, idx_t len) : buf(buf), len(len) {
	}

	const char *buf;
	idx_t len;

	Value Parse(const LogicalType &type) {
		return ParseValue(type, 0, len);
	}

private:
	[[noreturn]] void Fail(idx_t pos) {
		throw ConversionException("Cannot read the inlined value \"%s\": unexpected text at byte %llu",
		                          string(buf, len), pos);
	}

	bool IsNullToken(idx_t start, idx_t end) {
		return end - start == 4 && StringUtil::CIEquals(buf + start, 4, "null", 4);
	}

	//! Moves past a quoted string that starts at pos, with backslash escapes
	void SkipQuoted(idx_t &pos) {
		auto quote = buf[pos];
		pos++;
		while (pos < len) {
			if (buf[pos] == '\\') {
				pos += 2;
				continue;
			}
			if (buf[pos] == quote) {
				pos++;
				return;
			}
			pos++;
		}
		Fail(pos);
	}

	//! Moves past a bracketed value that starts at pos, with quoted strings and nested brackets inside
	void SkipBracketed(idx_t &pos) {
		vector<char> closers;
		while (pos < len) {
			auto c = buf[pos];
			if (c == '\'' || c == '"') {
				SkipQuoted(pos);
				continue;
			}
			if (c == '\\') {
				pos += 2;
				continue;
			}
			if (c == '[' || c == '{' || c == '(') {
				closers.push_back(c == '[' ? ']' : c == '{' ? '}' : ')');
			} else if (!closers.empty() && c == closers.back()) {
				closers.pop_back();
				if (closers.empty()) {
					pos++;
					return;
				}
			}
			pos++;
		}
		Fail(pos);
	}

	static bool IsStop(char c, const char *stops) {
		// a NUL byte inside a string is a regular character, not the end of the stop list
		for (auto stop = stops; *stop; stop++) {
			if (c == *stop) {
				return true;
			}
		}
		return false;
	}

	//! The raw range of the value that starts at pos and ends before one of the stop characters at the top level;
	//! the renderer puts one space after a comma or colon, which the caller skips, and no other space of its own
	pair<idx_t, idx_t> ScanValue(idx_t &pos, const char *stops) {
		auto start = pos;
		while (pos < len) {
			auto c = buf[pos];
			if (IsStop(c, stops)) {
				break;
			}
			if (c == '\'' || c == '"') {
				SkipQuoted(pos);
			} else if (c == '[' || c == '{' || c == '(') {
				SkipBracketed(pos);
			} else if (c == '\\' && pos + 1 < len) {
				pos += 2;
			} else {
				pos++;
			}
		}
		if (pos == len) {
			Fail(pos);
		}
		return make_pair(start, pos);
	}

	//! Skips the one space the renderer writes after a separator
	void SkipSeparatorSpace(idx_t &pos) {
		if (pos < len && buf[pos] == ' ') {
			pos++;
		}
	}

	//! The contents of a quoted string or of a plain text, with the escapes removed as DuckDB reads them
	string Unquote(idx_t start, idx_t end) {
		string result;
		bool quoted = end > start + 1 && (buf[start] == '\'' || buf[start] == '"') && buf[end - 1] == buf[start];
		if (quoted) {
			start++;
			end--;
		}
		for (idx_t i = start; i < end; i++) {
			auto c = buf[i];
			if (c == '\\' && i + 1 < end && (quoted || buf[i + 1] == '\'' || buf[i + 1] == '"')) {
				i++;
				c = buf[i];
			}
			result += c;
		}
		return result;
	}

	Value ParseValue(const LogicalType &type, idx_t start, idx_t end) {
		if (type.IsJSONType()) {
			// the JSON text keeps the spaces at its ends, the generic parser would drop them
			return ParseJSON(start, end);
		}
		while (start < end && StringUtil::CharacterIsSpace(buf[start])) {
			start++;
		}
		while (end > start && StringUtil::CharacterIsSpace(buf[end - 1])) {
			end--;
		}
		if (IsNullToken(start, end)) {
			return Value(type);
		}
		switch (type.id()) {
		case LogicalTypeId::LIST:
		case LogicalTypeId::ARRAY:
			return ParseList(type, start, end);
		case LogicalTypeId::STRUCT:
		case LogicalTypeId::TUPLE:
			return ParseStruct(type, start, end);
		case LogicalTypeId::MAP:
			return ParseMap(type, start, end);
		default:
			if (type.IsNested()) {
				// a type this parser does not know is read by DuckDB from its own rendering
				return Value(string(buf + start, end - start)).DefaultCastAs(type);
			}
			return Value(Unquote(start, end)).DefaultCastAs(type);
		}
	}

	Value ParseJSON(idx_t start, idx_t end) {
		if (start == end) {
			Fail(start);
		}
		if (buf[start] == '\'' && buf[end - 1] == '\'' && end > start + 1) {
			// the quoted form, the JSON text is the contents of the string
			return Value(Unquote(start, end)).DefaultCastAs(LogicalType::JSON());
		}
		if (end - start == 4 && memcmp(buf + start, "NULL", 4) == 0) {
			// older versions rendered a missing element as NULL and a JSON null in lowercase
			return Value(LogicalType::JSON());
		}
		return Value(string(buf + start, end - start)).DefaultCastAs(LogicalType::JSON());
	}

	void ExpectBrackets(idx_t start, idx_t end, char open, char close) {
		if (end <= start || buf[start] != open || buf[end - 1] != close) {
			Fail(start);
		}
	}

	//! The ranges of the values between the brackets, separated by commas
	vector<pair<idx_t, idx_t>> SplitElements(idx_t start, idx_t end) {
		vector<pair<idx_t, idx_t>> elements;
		idx_t pos = start + 1;
		if (pos == end - 1) {
			return elements;
		}
		while (true) {
			elements.push_back(ScanValue(pos, ",]})"));
			if (pos == end - 1) {
				return elements;
			}
			if (buf[pos] != ',') {
				Fail(pos);
			}
			pos++;
			SkipSeparatorSpace(pos);
		}
	}

	Value ParseList(const LogicalType &type, idx_t start, idx_t end) {
		ExpectBrackets(start, end, '[', ']');
		auto &child_type =
		    type.id() == LogicalTypeId::ARRAY ? ArrayType::GetChildType(type) : ListType::GetChildType(type);
		vector<Value> values;
		for (auto &element : SplitElements(start, end)) {
			values.push_back(ParseValue(child_type, element.first, element.second));
		}
		if (type.id() == LogicalTypeId::ARRAY) {
			if (values.size() != ArrayType::GetSize(type)) {
				Fail(start);
			}
			return Value::ARRAY(child_type, std::move(values));
		}
		return Value::LIST(child_type, std::move(values));
	}

	Value ParseStruct(const LogicalType &type, idx_t start, idx_t end) {
		auto &child_types = StructType::GetChildTypes(type);
		vector<Value> values(child_types.size());
		if (StructType::IsUnnamed(type)) {
			ExpectBrackets(start, end, '(', ')');
			auto elements = SplitElements(start, end);
			if (elements.size() != child_types.size()) {
				Fail(start);
			}
			for (idx_t i = 0; i < elements.size(); i++) {
				values[i] = ParseValue(child_types[i].second, elements[i].first, elements[i].second);
			}
			return Value::STRUCT(type, std::move(values));
		}
		ExpectBrackets(start, end, '{', '}');
		vector<bool> seen(child_types.size(), false);
		idx_t pos = start + 1;
		while (pos != end - 1) {
			auto key = ScanValue(pos, ":");
			auto name = Unquote(key.first, key.second);
			pos++;
			SkipSeparatorSpace(pos);
			auto value = ScanValue(pos, ",}");
			idx_t index = child_types.size();
			for (idx_t i = 0; i < child_types.size(); i++) {
				if (!seen[i] && StringUtil::CIEquals(child_types[i].first.GetIdentifierName(), name)) {
					index = i;
					break;
				}
			}
			if (index == child_types.size()) {
				Fail(key.first);
			}
			seen[index] = true;
			values[index] = ParseValue(child_types[index].second, value.first, value.second);
			if (buf[pos] == ',') {
				pos++;
				SkipSeparatorSpace(pos);
			}
		}
		for (idx_t i = 0; i < child_types.size(); i++) {
			if (!seen[i]) {
				values[i] = Value(child_types[i].second);
			}
		}
		return Value::STRUCT(type, std::move(values));
	}

	Value ParseMap(const LogicalType &type, idx_t start, idx_t end) {
		ExpectBrackets(start, end, '{', '}');
		auto &key_type = MapType::KeyType(type);
		auto &value_type = MapType::ValueType(type);
		vector<Value> keys;
		vector<Value> values;
		idx_t pos = start + 1;
		while (pos != end - 1) {
			auto key = ScanValue(pos, "=");
			pos++;
			auto value = ScanValue(pos, ",}");
			keys.push_back(ParseValue(key_type, key.first, key.second));
			values.push_back(ParseValue(value_type, value.first, value.second));
			if (buf[pos] == ',') {
				pos++;
				SkipSeparatorSpace(pos);
			}
		}
		return Value::MAP(key_type, value_type, std::move(keys), std::move(values));
	}
};

struct InlinedValueBindData : public FunctionData {
	explicit InlinedValueBindData(LogicalType type) : type(std::move(type)) {
	}

	LogicalType type;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<InlinedValueBindData>(type);
	}
	bool Equals(const FunctionData &other) const override {
		return type == other.Cast<InlinedValueBindData>().type;
	}
};

//! The second argument only carries the type of the result, as NULL::type
static unique_ptr<FunctionData> InlinedValueBind(BindScalarFunctionInput &input) {
	auto &type = input.GetArguments()[1]->GetReturnType();
	if (type.id() == LogicalTypeId::SQLNULL || type.id() == LogicalTypeId::UNKNOWN) {
		throw BinderException("ducklake_inlined_value needs a typed NULL as its second argument");
	}
	input.GetBoundFunction().SetReturnType(type);
	return make_uniq<InlinedValueBindData>(type);
}

static void InlinedValueFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &type = func_expr.BindInfo()->Cast<InlinedValueBindData>().type;
	auto &text = args.data[0];
	UnifiedVectorFormat format;
	text.ToUnifiedFormat(format);
	auto strings = UnifiedVectorFormat::GetData<string_t>(format);
	for (idx_t i = 0; i < args.size(); i++) {
		auto index = format.sel->get_index(i);
		if (!format.validity.RowIsValid(index)) {
			result.SetValue(i, Value(type));
			continue;
		}
		InlinedTextParser parser(strings[index].GetData(), strings[index].GetSize());
		result.SetValue(i, parser.Parse(type));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

ScalarFunction DuckLakeInlinedValueFunction() {
	ScalarFunction func("ducklake_inlined_value", {LogicalType::VARCHAR, LogicalType::ANY}, LogicalType::ANY,
	                    InlinedValueFunction, InlinedValueBind);
	func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	func.SetFallible();
	return func;
}

} // namespace duckdb
