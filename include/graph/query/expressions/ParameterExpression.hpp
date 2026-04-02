/**
 * @file ParameterExpression.hpp
 * @brief Expression node for query parameters ($paramName).
 **/

#pragma once

#include "graph/query/expressions/Expression.hpp"
#include <string>

namespace graph::query::expressions {

/**
 * @class ParameterExpression
 * @brief Represents a $paramName reference in a Cypher query.
 *
 * Resolved at evaluation time from the EvaluationContext's parameter map.
 */
class ParameterExpression : public Expression {
public:
	explicit ParameterExpression(std::string paramName)
		: paramName_(std::move(paramName)) {}

	[[nodiscard]] ExpressionType getExpressionType() const override {
		return ExpressionType::PARAMETER;
	}

	[[nodiscard]] const std::string &getParameterName() const { return paramName_; }

	void accept(ExpressionVisitor &visitor) override { visitor.visit(this); }
	void accept(ConstExpressionVisitor &visitor) const override { visitor.visit(this); }

	[[nodiscard]] std::string toString() const override {
		return "$" + paramName_;
	}

	[[nodiscard]] std::unique_ptr<Expression> clone() const override {
		return std::make_unique<ParameterExpression>(paramName_);
	}

private:
	std::string paramName_;
};

} // namespace graph::query::expressions
