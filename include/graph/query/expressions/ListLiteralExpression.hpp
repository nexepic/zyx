/**
 * @file ListLiteralExpression.hpp
 * @date 2026/1/4
 *
 * List literal expression for Cypher: [1, 2, 3], ['a', 'b'], [1, 'mixed', true]
 */

#pragma once

#include "Expression.hpp"
#include "graph/core/PropertyTypes.hpp"
#include <memory>

namespace graph::query::expressions {

/**
 * Represents a list literal expression in Cypher.
 *
 * Examples:
 * - [1, 2, 3] - list of integers
 * - ['a', 'b', 'c'] - list of strings
 * - [1, 'mixed', true, 3.14] - heterogeneous list
 * - [[1, 2], [3, 4]] - nested lists
 */
class ListLiteralExpression : public Expression {
public:
	/**
	 * Construct a list literal expression
	 * @param listValue The PropertyValue containing the list
	 */
	explicit ListLiteralExpression(PropertyValue listValue);

	~ListLiteralExpression() override = default;

	[[nodiscard]] ExpressionType getExpressionType() const override {
		return ExpressionType::EXPR_LIST_LITERAL;
	}

	void accept(ExpressionVisitor &visitor) override;
	void accept(ConstExpressionVisitor &visitor) const override;
	[[nodiscard]] std::string toString() const override;
	[[nodiscard]] std::unique_ptr<Expression> clone() const override;

	/**
	 * Get the list value
	 */
	[[nodiscard]] const PropertyValue& getValue() const { return value_; }

private:
	PropertyValue value_;
};

} // namespace graph::query::expressions
