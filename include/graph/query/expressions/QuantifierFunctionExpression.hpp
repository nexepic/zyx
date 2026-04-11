/**
 * @file QuantifierFunctionExpression.hpp
 * @date 2025/3/9
 *
 * Expression for quantifier functions (all, any, none, single)
 */

#pragma once

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/PredicateFunction.hpp"
#include <memory>
#include <string>

namespace graph::query::expressions {

/**
 * @class QuantifierFunctionExpression
 * @brief Expression for quantifier functions with special Cypher syntax
 *
 * Syntax: all(variable IN list WHERE condition)
 *
 * This expression stores the components needed to evaluate a quantifier function:
 * - The function name (all, any, none, single)
 * - The iteration variable name
 * - The list expression
 * - The WHERE condition expression
 *
 * During evaluation, this expression looks up the appropriate PredicateFunction
 * and calls its evaluate method with the stored components.
 */
class QuantifierFunctionExpression : public Expression {
public:
	/**
	 * @brief Constructor
	 *
	 * @param functionName The quantifier function name (all, any, none, single)
	 * @param variable The iteration variable name
	 * @param listExpr The expression that evaluates to a list
	 * @param whereExpr The WHERE condition expression
	 */
	QuantifierFunctionExpression(
		std::string functionName,
		std::string variable,
		std::unique_ptr<Expression> listExpr,
		std::unique_ptr<Expression> whereExpr
	);

	[[nodiscard]] ExpressionType getExpressionType() const override {
		return ExpressionType::EXPR_QUANTIFIER_FUNCTION;
	}

	void accept(ExpressionVisitor &visitor) override;
	void accept(ConstExpressionVisitor &visitor) const override;

	[[nodiscard]] std::string toString() const override;

	[[nodiscard]] std::unique_ptr<Expression> clone() const override;

	/**
	 * @brief Gets the function name
	 */
	[[nodiscard]] const std::string& getFunctionName() const { return functionName_; }

	/**
	 * @brief Gets the iteration variable name
	 */
	[[nodiscard]] const std::string& getVariable() const { return variable_; }

	/**
	 * @brief Gets the list expression
	 */
	[[nodiscard]] const Expression* getListExpression() const { return listExpr_.get(); }

	/**
	 * @brief Gets the WHERE condition expression
	 */
	[[nodiscard]] const Expression* getWhereExpression() const { return whereExpr_.get(); }

private:
	std::string functionName_;
	std::string variable_;
	std::unique_ptr<Expression> listExpr_;
	std::unique_ptr<Expression> whereExpr_;
};

} // namespace graph::query::expressions
