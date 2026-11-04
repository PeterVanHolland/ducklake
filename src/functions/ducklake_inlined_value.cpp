#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

//! The text of a JSON list as the metadata catalogs that store nested values as text wrote it. DuckDB renders a list
//! of strings with quotes around any element that is not a plain word and NULL for a missing element; the JSON
//! extension rendered a JSON list as the JSON texts themselves. So a quoted element holds the JSON text, exactly NULL
//! is a missing element and any other element is the JSON text itself.
struct JSONListText {
	JSONListText(const char *buf, idx_t len) : buf(buf), len(len) {
	}

	const char *buf;
	idx_t len;
	idx_t pos = 0;

	//! Appends the elements to the child of the list vector, false at unexpected text
	bool Read(Vector &list) {
		SkipSpaces();
		if (pos == len || buf[pos] != '[') {
			return false;
		}
		pos++;
		if (pos < len && buf[pos] == ']') {
			pos++;
			SkipSpaces();
			return pos == len;
		}
		// the renderers write one space after a comma and no other space of their own, so an element is its exact text
		while (true) {
			auto start = pos;
			if (!SkipElement() || pos == start) {
				return false;
			}
			Append(list, start, pos);
			if (buf[pos] == ']') {
				pos++;
				SkipSpaces();
				return pos == len;
			}
			pos++;
			if (pos < len && buf[pos] == ' ') {
				pos++;
			}
		}
	}

private:
	void SkipSpaces() {
		while (pos < len && StringUtil::CharacterIsSpace(buf[pos])) {
			pos++;
		}
	}

	//! Moves past a quoted string that starts at pos, with backslash escapes
	bool SkipQuoted() {
		auto quote = buf[pos];
		for (pos++; pos < len; pos++) {
			if (buf[pos] == '\\') {
				pos++;
			} else if (buf[pos] == quote) {
				pos++;
				return true;
			}
		}
		return false;
	}

	//! Moves past the element that starts at pos, up to the comma or bracket that ends it
	bool SkipElement() {
		idx_t depth = 0;
		while (pos < len) {
			auto c = buf[pos];
			if (c == '\'' || c == '"') {
				if (!SkipQuoted()) {
					return false;
				}
				continue;
			}
			if (depth == 0 && (c == ',' || c == ']')) {
				return true;
			}
			if (c == '[' || c == '{' || c == '(') {
				depth++;
			} else if (c == ']' || c == '}' || c == ')') {
				if (depth == 0) {
					return false;
				}
				depth--;
			}
			pos++;
		}
		return false;
	}

	void Append(Vector &list, idx_t start, idx_t end) {
		auto index = ListVector::GetListSize(list);
		ListVector::Reserve(list, index + 1);
		auto &child = ListVector::GetChildMutable(list);
		auto child_data = FlatVector::GetDataMutable<string_t>(child);
		if (end - start == 4 && memcmp(buf + start, "NULL", 4) == 0) {
			FlatVector::SetNull(child, index, true);
		} else if (end - start >= 2 && buf[start] == '\'' && buf[end - 1] == '\'') {
			string json;
			for (idx_t i = start + 1; i + 1 < end; i++) {
				if (buf[i] == '\\' && i + 2 < end) {
					i++;
				}
				json += buf[i];
			}
			child_data[index] = StringVector::AddString(child, json);
		} else {
			child_data[index] = StringVector::AddString(child, buf + start, end - start);
		}
		ListVector::SetListSize(list, index + 1);
	}
};

static bool CastTextToJSONList(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	bool success = true;
	UnaryExecutor::Execute<string_t, list_entry_t>(
	    source, result, count, [&](const string_t &input) -> optional<list_entry_t> {
		    list_entry_t entry;
		    entry.offset = ListVector::GetListSize(result);
		    JSONListText text(input.GetData(), input.GetSize());
		    if (!text.Read(result)) {
			    ListVector::SetListSize(result, entry.offset);
			    if (success) {
				    HandleCastError::AssignError(
				        StringUtil::Format("Unexpected text in the JSON list \"%s\"", input.GetString()), parameters);
				    success = false;
			    }
			    return nullopt;
		    }
		    entry.length = ListVector::GetListSize(result) - entry.offset;
		    return entry;
	    });
	return success;
}

struct InlinedValueBindData : public FunctionData {
	InlinedValueBindData(LogicalType type, BoundCastInfo cast) : type(std::move(type)), cast(std::move(cast)) {
	}

	LogicalType type;
	//! DuckDB's cast of the text to the type, with the JSON lists read as the catalogs wrote them
	BoundCastInfo cast;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<InlinedValueBindData>(type, cast.Copy());
	}
	bool Equals(const FunctionData &other) const override {
		return type == other.Cast<InlinedValueBindData>().type;
	}
};

struct InlinedValueLocalState : public FunctionLocalState {
	explicit InlinedValueLocalState(unique_ptr<FunctionLocalState> cast_state) : cast_state(std::move(cast_state)) {
	}

	unique_ptr<FunctionLocalState> cast_state;
};

//! The second argument only carries the type of the result, as NULL::type
static unique_ptr<FunctionData> InlinedValueBind(BindScalarFunctionInput &input) {
	auto &type = input.GetArguments()[1]->GetReturnType();
	if (type.id() == LogicalTypeId::SQLNULL || type.id() == LogicalTypeId::UNKNOWN) {
		throw BinderException("ducklake_inlined_value needs a typed NULL as its second argument");
	}
	input.GetBoundFunction().SetReturnType(type);
	// DuckDB reads its own text of every type, but its cast of a JSON list wants a JSON array
	CastFunctionSet casts;
	casts.RegisterCastFunction(LogicalType::VARCHAR, LogicalType::LIST(LogicalType::JSON()), CastTextToJSONList);
	GetCastFunctionInput cast_input(input.GetClientContext());
	return make_uniq<InlinedValueBindData>(type, casts.GetCastFunction(LogicalType::VARCHAR, type, cast_input));
}

static unique_ptr<FunctionLocalState>
InlinedValueInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr, FunctionData *bind_data) {
	auto &cast = bind_data->Cast<InlinedValueBindData>().cast;
	unique_ptr<FunctionLocalState> cast_state;
	if (cast.HasInitLocalState()) {
		CastLocalStateParameters parameters(state.GetContext(), cast.GetCastData());
		cast_state = cast.InitLocalState(parameters);
	}
	return make_uniq<InlinedValueLocalState>(std::move(cast_state));
}

static void InlinedValueFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &cast = func_expr.BindInfo()->Cast<InlinedValueBindData>().cast;
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<InlinedValueLocalState>();
	string error;
	CastParameters parameters(cast.GetCastData(), false, &error, lstate.cast_state.get());
	if (!cast.Cast(args.data[0], result, args.size(), parameters)) {
		throw ConversionException("Cannot read the inlined value: %s", error);
	}
}

ScalarFunction DuckLakeInlinedValueFunction() {
	ScalarFunction func("ducklake_inlined_value", {LogicalType::VARCHAR, LogicalType::ANY}, LogicalType::ANY,
	                    InlinedValueFunction, InlinedValueBind, nullptr, InlinedValueInitLocalState);
	func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	func.SetFallible();
	return func;
}

} // namespace duckdb
