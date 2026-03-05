#pragma once

#include <memory>
#include "graph/query/expressions/Expression.hpp"

namespace graph::query::expressions {

/**
 * @class IsNullExpression
 * @brief Represents IS NULL and IS NOT NULL operations in Cypher
 *
 * This expression handles the special NULL checking semantics:
 * - NULL IS NULL → true
 * - 1 IS NULL → false
 * - NULL IS NOT NULL → false
 * - 1 IS NOT NULL → true
 *
 * Unlike standard equality operators, IS NULL follows Cypher's three-valued logic.
 */
class IsNullExpression : public Expression {
public:
	/**
	 * @brief Constructor
	 * @param expr The expression to check for NULL
	 * @param isNegated True for IS NOT NULL, false for IS NULL
	 */
	IsNullExpression(std::unique_ptr<Expression> expr, bool isNegated);

	~IsNullExpression() override = default;

	// Getters
	[[nodiscard]] const Expression* getExpression() const { return expr_.get(); }
	[[nodiscard]] Expression* getExpression() { return expr_.get(); }
	[[nodiscard]] bool isNegated() const { return isNegated_; }

	// Expression interface
	void accept(ExpressionVisitor& visitor) override;
	void accept(ConstExpressionVisitor& visitor) const override;
	[[nodiscard]] std::unique_ptr<Expression> clone() const override;
	[[nodiscard]] ExpressionType getExpressionType() const override;
	[[nodiscard]] std::string toString() const override;

private:
	std::unique_ptr<Expression> expr_;
	bool isNegated_;  // true for IS NOT NULL, false for IS NULL
};

} // namespace graph::query::expressions
