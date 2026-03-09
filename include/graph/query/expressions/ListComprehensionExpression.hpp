#pragma once

#include <memory>
#include <string>
#include "graph/query/expressions/Expression.hpp"

namespace graph::query::expressions {

/**
 * @brief Represents list comprehension expressions in Cypher
 *
 * Supports three types:
 * - FILTER: [x IN list WHERE x > 5]
 * - EXTRACT: [x IN list | x * 2]
 * - REDUCE: reduce(total = 0, x IN list | total + x)
 */
class ListComprehensionExpression : public Expression {
public:
	/**
	 * @brief Type of list comprehension
	 */
	enum class ComprehensionType {
		COMP_FILTER,   ///< [x IN list WHERE condition] - filter elements
		COMP_EXTRACT,  ///< [x IN list | expression] - transform elements
		COMP_REDUCE    ///< reduce(accum = init, x IN list | accum + x) - reduce to single value
	};

	/**
	 * @brief Constructor
	 * @param variable Iteration variable name (e.g., "x")
	 * @param listExpr Expression that evaluates to a list
	 * @param whereExpr Optional WHERE clause for filtering
	 * @param mapExpr Optional mapping expression after |
	 * @param type Type of comprehension
	 */
	ListComprehensionExpression(
			std::string variable,
			std::unique_ptr<Expression> listExpr,
			std::unique_ptr<Expression> whereExpr,
			std::unique_ptr<Expression> mapExpr,
			ComprehensionType type);

	~ListComprehensionExpression() override = default;

	// Getters
	[[nodiscard]] const std::string& getVariable() const { return variable_; }
	[[nodiscard]] const Expression* getListExpr() const { return listExpr_.get(); }
	[[nodiscard]] const Expression* getWhereExpr() const { return whereExpr_.get(); }
	[[nodiscard]] const Expression* getMapExpr() const { return mapExpr_.get(); }
	[[nodiscard]] ComprehensionType getType() const { return type_; }

	// Expression interface
	[[nodiscard]] ExpressionType getExpressionType() const override { return ExpressionType::LIST_COMPREHENSION; }
	void accept(ExpressionVisitor& visitor) override;
	void accept(ConstExpressionVisitor& visitor) const override;
	[[nodiscard]] std::string toString() const override;
	[[nodiscard]] std::unique_ptr<Expression> clone() const override;

private:
	std::string variable_;
	std::unique_ptr<Expression> listExpr_;
	std::unique_ptr<Expression> whereExpr_;  // Can be nullptr
	std::unique_ptr<Expression> mapExpr_;    // Can be nullptr
	ComprehensionType type_;
};

} // namespace graph::query::expressions
