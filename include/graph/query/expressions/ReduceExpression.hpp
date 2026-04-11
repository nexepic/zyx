/**
 * @file ReduceExpression.hpp
 * @date 2026/3/22
 *
 * Expression for REDUCE function: reduce(accumulator = initial, x IN list | expression)
 */

#pragma once

#include "graph/query/expressions/Expression.hpp"
#include <memory>
#include <string>

namespace graph::query::expressions {

/**
 * @class ReduceExpression
 * @brief Expression for the REDUCE function in Cypher
 *
 * Syntax: reduce(accumulator = initial, variable IN list | expression)
 *
 * Iterates over a list, maintaining an accumulator that is updated
 * by the body expression for each element.
 *
 * Example:
 *   reduce(total = 0, x IN [1,2,3] | total + x) → 6
 *   reduce(s = '', x IN ['a','b','c'] | s + x) → 'abc'
 */
class ReduceExpression : public Expression {
public:
	/**
	 * @brief Constructor
	 *
	 * @param accumulator The accumulator variable name (e.g., "total")
	 * @param initialExpr Expression for the initial accumulator value
	 * @param variable The iteration variable name (e.g., "x")
	 * @param listExpr Expression that evaluates to a list
	 * @param bodyExpr Expression evaluated for each element (updates accumulator)
	 */
	ReduceExpression(
		std::string accumulator,
		std::unique_ptr<Expression> initialExpr,
		std::string variable,
		std::unique_ptr<Expression> listExpr,
		std::unique_ptr<Expression> bodyExpr
	);

	[[nodiscard]] ExpressionType getExpressionType() const override {
		return ExpressionType::EXPR_REDUCE;
	}

	void accept(ExpressionVisitor &visitor) override;
	void accept(ConstExpressionVisitor &visitor) const override;

	[[nodiscard]] std::string toString() const override;
	[[nodiscard]] std::unique_ptr<Expression> clone() const override;

	[[nodiscard]] const std::string& getAccumulator() const { return accumulator_; }
	[[nodiscard]] const Expression* getInitialExpr() const { return initialExpr_.get(); }
	[[nodiscard]] const std::string& getVariable() const { return variable_; }
	[[nodiscard]] const Expression* getListExpr() const { return listExpr_.get(); }
	[[nodiscard]] const Expression* getBodyExpr() const { return bodyExpr_.get(); }

private:
	std::string accumulator_;
	std::unique_ptr<Expression> initialExpr_;
	std::string variable_;
	std::unique_ptr<Expression> listExpr_;
	std::unique_ptr<Expression> bodyExpr_;
};

} // namespace graph::query::expressions
