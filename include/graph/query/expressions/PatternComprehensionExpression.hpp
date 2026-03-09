/**
 * @file PatternComprehensionExpression.hpp
 * @date 2025/3/9
 *
 * Expression for pattern comprehensions
 */

#pragma once

#include "graph/query/expressions/Expression.hpp"
#include <memory>
#include <string>
#include <vector>

namespace graph::query::expressions {

/**
 * @class PatternComprehensionExpression
 * @brief Expression for pattern comprehensions in Cypher
 *
 * Syntax: [(n)-[:FRIENDS]->(m) | m.name]
 *
 * Pattern comprehensions extract values from graph patterns.
 * Similar to list comprehensions but work with graph patterns.
 *
 * Components:
 * - Pattern: The graph pattern to match (e.g., (n)-[:FRIENDS]->(m))
 * - Variable: The iteration variable name
 * - Map expression: The expression to evaluate for each match
 * - WHERE clause: Optional filter for matches
 *
 * Example:
 *   [(n)-[:FRIENDS]->(m) | m.name]
 *   Returns: List of friend names
 *
 *   [(n)-[:FRIENDS]->(m) WHERE m.age > 18 | m.name]
 *   Returns: List of adult friend names
 */
class PatternComprehensionExpression : public Expression {
public:
	/**
	 * @brief Constructor for pattern comprehension
	 *
	 * @param pattern The graph pattern string (e.g., "(n)-[:FRIENDS]->(m)")
	 * @param variable The iteration variable name
	 * @param mapExpr The expression to evaluate for each match
	 * @param whereExpr Optional WHERE clause for filtering
	 */
	PatternComprehensionExpression(
		std::string pattern,
		std::string variable,
		std::unique_ptr<Expression> mapExpr,
		std::unique_ptr<Expression> whereExpr = nullptr
	);

	[[nodiscard]] ExpressionType getExpressionType() const override {
		return ExpressionType::LIST_COMPREHENSION;
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
	 * @brief Gets the iteration variable name
	 */
	[[nodiscard]] const std::string& getVariable() const { return variable_; }

	/**
	 * @brief Gets the map expression
	 */
	[[nodiscard]] Expression* getMapExpression() const { return mapExpr_.get(); }

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
	std::string variable_;
	std::unique_ptr<Expression> mapExpr_;
	std::unique_ptr<Expression> whereExpr_;
};

} // namespace graph::query::expressions
