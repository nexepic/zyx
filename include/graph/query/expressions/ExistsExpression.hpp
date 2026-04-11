/**
 * @file ExistsExpression.hpp
 * @date 2025/3/9
 *
 * Expression for EXISTS pattern expressions
 */

#pragma once

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/PatternDirection.hpp"
#include <memory>
#include <string>

namespace graph::query::expressions {

/**
 * @class ExistsExpression
 * @brief Expression for EXISTS pattern existence checks
 *
 * Syntax: EXISTS((n)-[:FRIENDS]->())
 * or: exists((n)-[:FRIENDS]->())
 *
 * Checks if a pattern exists in the graph. Returns TRUE if at least one
 * match is found, FALSE otherwise.
 */
class ExistsExpression : public Expression {
public:
	/**
	 * @brief Constructor for EXISTS with structured pattern info
	 */
	ExistsExpression(
		std::string pattern,
		std::string sourceVar,
		std::string relType,
		std::string targetLabel,
		PatternDirection direction,
		std::unique_ptr<Expression> whereExpr = nullptr
	);

	/**
	 * @brief Backward-compatible constructor with pattern string only
	 */
	ExistsExpression(
		std::string pattern,
		std::unique_ptr<Expression> whereExpr = nullptr
	);

	[[nodiscard]] ExpressionType getExpressionType() const override {
		return ExpressionType::EXPR_EXISTS;
	}

	void accept(ExpressionVisitor &visitor) override;
	void accept(ConstExpressionVisitor &visitor) const override;

	[[nodiscard]] std::string toString() const override;

	[[nodiscard]] std::unique_ptr<Expression> clone() const override;

	[[nodiscard]] const std::string& getPattern() const { return pattern_; }
	[[nodiscard]] const std::string& getSourceVar() const { return sourceVar_; }
	[[nodiscard]] const std::string& getRelType() const { return relType_; }
	[[nodiscard]] const std::string& getTargetLabel() const { return targetLabel_; }
	[[nodiscard]] PatternDirection getDirection() const { return direction_; }
	[[nodiscard]] bool hasStructuredPattern() const { return !sourceVar_.empty(); }

	[[nodiscard]] Expression* getWhereExpression() const { return whereExpr_.get(); }
	[[nodiscard]] bool hasWhereClause() const { return whereExpr_ != nullptr; }

private:
	std::string pattern_;
	std::string sourceVar_;
	std::string relType_;
	std::string targetLabel_;
	PatternDirection direction_ = PatternDirection::PAT_OUTGOING;
	std::unique_ptr<Expression> whereExpr_;
};

} // namespace graph::query::expressions
