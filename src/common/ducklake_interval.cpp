#include "common/ducklake_interval.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

static unique_ptr<Expression> BindFunction(ClientContext &context, const string &name,
                                           vector<unique_ptr<Expression>> children) {
	FunctionBinder binder(context);
	return binder.BindScalarFunction(Identifier::DefaultSchema(), Identifier(name), std::move(children));
}

//! date_part of the interval, as a BIGINT
static unique_ptr<Expression> DatePart(ClientContext &context, const char *part, const Expression &expr) {
	vector<unique_ptr<Expression>> children;
	children.push_back(make_uniq<BoundConstantExpression>(Value(part)));
	children.push_back(expr.Copy());
	return BindFunction(context, "date_part", std::move(children));
}

static unique_ptr<Expression> Multiply(ClientContext &context, unique_ptr<Expression> expr, int64_t factor) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(expr));
	children.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(factor)));
	return BindFunction(context, "*", std::move(children));
}

static unique_ptr<Expression> Add(ClientContext &context, unique_ptr<Expression> left, unique_ptr<Expression> right) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(left));
	children.push_back(std::move(right));
	return BindFunction(context, "+", std::move(children));
}

LogicalType DuckLakeInterval::StorageType() {
	child_list_t<LogicalType> fields;
	fields.emplace_back(MONTHS_FIELD, LogicalType::INTEGER);
	fields.emplace_back(DAYS_FIELD, LogicalType::INTEGER);
	fields.emplace_back(MICROS_FIELD, LogicalType::BIGINT);
	return LogicalType::STRUCT(std::move(fields));
}

unique_ptr<Expression> DuckLakeInterval::ToStorage(ClientContext &context, unique_ptr<Expression> expr) {
	// date_part splits the months into years and months, and the microseconds into hours, minutes and the rest
	auto months = Add(context, Multiply(context, DatePart(context, "year", *expr), Interval::MONTHS_PER_YEAR),
	                  DatePart(context, "month", *expr));
	auto days = DatePart(context, "day", *expr);
	auto micros = Add(context, Multiply(context, DatePart(context, "hour", *expr), Interval::MICROS_PER_HOUR),
	                  Add(context, Multiply(context, DatePart(context, "minute", *expr), Interval::MICROS_PER_MINUTE),
	                      DatePart(context, "microsecond", *expr)));
	months = BoundCastExpression::AddCastToType(context, std::move(months), LogicalType::INTEGER);
	days = BoundCastExpression::AddCastToType(context, std::move(days), LogicalType::INTEGER);
	months->SetAlias(Identifier(MONTHS_FIELD));
	days->SetAlias(Identifier(DAYS_FIELD));
	micros->SetAlias(Identifier(MICROS_FIELD));
	vector<unique_ptr<Expression>> fields;
	fields.push_back(std::move(months));
	fields.push_back(std::move(days));
	fields.push_back(std::move(micros));
	// a NULL INTERVAL is a NULL struct, so that NULL filters on the file see it
	auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
	is_null->GetChildrenMutable().push_back(std::move(expr));
	return make_uniq<BoundCaseExpression>(std::move(is_null), make_uniq<BoundConstantExpression>(Value(StorageType())),
	                                      BindFunction(context, "struct_pack", std::move(fields)));
}

static unique_ptr<Expression> ExtractField(ClientContext &context, const Expression &expr, const char *field) {
	vector<unique_ptr<Expression>> children;
	children.push_back(expr.Copy());
	children.push_back(make_uniq<BoundConstantExpression>(Value(field)));
	return BindFunction(context, "struct_extract", std::move(children));
}

static unique_ptr<Expression> ToInterval(ClientContext &context, const char *function, unique_ptr<Expression> expr) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(expr));
	return BindFunction(context, function, std::move(children));
}

unique_ptr<Expression> DuckLakeInterval::FromStorage(ClientContext &context, unique_ptr<Expression> expr) {
	auto months = ToInterval(context, "to_months", ExtractField(context, *expr, MONTHS_FIELD));
	auto days = ToInterval(context, "to_days", ExtractField(context, *expr, DAYS_FIELD));
	auto micros = ToInterval(context, "to_microseconds", ExtractField(context, *expr, MICROS_FIELD));
	return Add(context, Add(context, std::move(months), std::move(days)), std::move(micros));
}

void DuckLakeInterval::ReplaceStorageCasts(ClientContext &context, unique_ptr<Expression> &expr) {
	if (BoundCastExpression::IsCast(*expr)) {
		auto &cast = expr->Cast<BoundFunctionExpression>();
		if (BoundCastExpression::TargetType(cast).id() == LogicalTypeId::INTERVAL &&
		    BoundCastExpression::SourceType(cast) == StorageType()) {
			expr = FromStorage(context, std::move(BoundCastExpression::ChildMutable(cast)));
			return;
		}
	}
	ExpressionIterator::EnumerateChildren(*expr,
	                                      [&](unique_ptr<Expression> &child) { ReplaceStorageCasts(context, child); });
}

} // namespace duckdb
