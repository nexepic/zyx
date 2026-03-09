/**
 * @file ExistsExpression.hpp
 * @date 2025/3/9
 *
 * Expression for EXISTS pattern expressions
 */

#pragma once

#include "graph/query/expressions/Expression.hpp"
#include <memory>
#include <string>

namespace graph::query::expressions {

/**
 * @class ExistsExpression
 * @brief Expression for EXISTS pattern existence checks
 *
 * Syntax: EXISTS((n)-[:FRIENDS]->())
 * or: EXISTS { MATCH (n)-[:FRIENDS]->() }
 *
 * Checks if a pattern exists in the graph. Returns TRUE if at least one
 * match is found, FALSE otherwise.
 *
 * The EXISTS expression stores:
 * - The pattern to match (as a string or AST representation)
 * - Optional WHERE clause for filtering
 *
 * During evaluation, the expression:
 * 1. Searches the graph for matches to the pattern
 * 2. Returns true if at least one match exists
 * 3. Returns false if no matches exist
 */
class ExistsExpression : public Expression {
public:
	/**
	 * @brief Constructor for EXISTS with pattern string
	 *
	 * @param pattern The pattern string (e.g., "(n)-[:FRIENDS]->()")
	 * @param whereExpr Optional WHERE clause for filtering
	 */
	ExistsExpression(
		std::string pattern,
		std::unique_ptr<Expression> whereExpr = nullptr
	);

	[[nodiscard]] ExpressionType getExpressionType() const override {
		return ExpressionType::FUNCTION_CALL;
	}

	void accept(ExpressionVisitor &visitor) override;
	void accept(ConstExpressionVisitor &visitor) const override;

	[[nodiscard]] std::string toString() const override;

	[[nodiscard]] std::unique_ptr<Expression> clone() const override;

	/**
	 * @brief Gets the pattern string
	 */
	[[nodiscard]] const std::string& getPattern() const { return pattern_; }

	/**
	 * @brief Gets the WHERE clause expression (if any)
	 */
	[[nodiscard]] Expression* getWhereExpression() const { return whereExpr_.get(); }

	/**
	 * @brief Checks if there's a WHERE clause
	 */
	[[nodiscard]] bool hasWhereClause() const { return whereExpr_ != nullptr; }

private:
	std::string pattern_;
	std::unique_ptr<Expression> whereExpr_;
};

} // namespace graph::query::expressions
