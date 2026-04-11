/**
 * @file PatternComprehensionExpression.hpp
 * @date 2025/3/9
 *
 * Expression for pattern comprehensions
 */

#pragma once

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/PatternDirection.hpp"
#include <memory>
#include <string>
#include <vector>

namespace graph::query::expressions {


/**
 * @class PatternComprehensionExpression
 * @brief Expression for pattern comprehensions in Cypher
 *
 * Syntax: [(n)-[:FRIENDS]->(m) | m.name]
 */
class PatternComprehensionExpression : public Expression {
public:
	PatternComprehensionExpression(
		std::string pattern,
		std::string sourceVar,
		std::string targetVar,
		std::string relType,
		std::string targetLabel,
		PatternDirection direction,
		std::unique_ptr<Expression> mapExpr,
		std::unique_ptr<Expression> whereExpr = nullptr
	);

	/**
	 * @brief Backward-compatible constructor with pattern string only
	 */
	PatternComprehensionExpression(
		std::string pattern,
		std::string variable,
		std::unique_ptr<Expression> mapExpr,
		std::unique_ptr<Expression> whereExpr = nullptr
	);

	[[nodiscard]] ExpressionType getExpressionType() const override {
		return ExpressionType::EXPR_PATTERN_COMPREHENSION;
	}

	void accept(ExpressionVisitor &visitor) override;
	void accept(ConstExpressionVisitor &visitor) const override;

	[[nodiscard]] std::string toString() const override;
	[[nodiscard]] std::unique_ptr<Expression> clone() const override;

	[[nodiscard]] const std::string& getPattern() const { return pattern_; }
	[[nodiscard]] const std::string& getSourceVar() const { return sourceVar_; }
	[[nodiscard]] const std::string& getTargetVar() const { return targetVar_; }
	[[nodiscard]] const std::string& getRelType() const { return relType_; }
	[[nodiscard]] const std::string& getTargetLabel() const { return targetLabel_; }
	[[nodiscard]] PatternDirection getDirection() const { return direction_; }
	[[nodiscard]] Expression* getMapExpression() const { return mapExpr_.get(); }
	[[nodiscard]] Expression* getWhereExpression() const { return whereExpr_.get(); }
	[[nodiscard]] bool hasWhereClause() const { return whereExpr_ != nullptr; }

private:
	std::string pattern_;
	std::string sourceVar_;
	std::string targetVar_;
	std::string relType_;
	std::string targetLabel_;
	PatternDirection direction_;
	std::unique_ptr<Expression> mapExpr_;
	std::unique_ptr<Expression> whereExpr_;
};

} // namespace graph::query::expressions
