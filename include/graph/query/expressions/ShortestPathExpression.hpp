/**
 * @file ShortestPathExpression.hpp
 * @date 2025
 *
 * Expression for shortestPath() and allShortestPaths() functions
 */

#pragma once

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/PatternDirection.hpp"
#include <memory>
#include <string>

namespace graph::query::expressions {

/**
 * @class ShortestPathExpression
 * @brief Expression for shortestPath/allShortestPaths pattern matching
 *
 * Syntax: shortestPath((a)-[*]-(b))
 *         allShortestPaths((a)-[*]-(b))
 */
class ShortestPathExpression : public Expression {
public:
	ShortestPathExpression(
		std::string startVar,
		std::string endVar,
		std::string relType,
		PatternDirection direction,
		int minHops,
		int maxHops,
		bool isAll
	) : startVar_(std::move(startVar)),
		endVar_(std::move(endVar)),
		relType_(std::move(relType)),
		direction_(direction),
		minHops_(minHops),
		maxHops_(maxHops),
		isAll_(isAll) {}

	[[nodiscard]] ExpressionType getExpressionType() const override {
		return ExpressionType::SHORTEST_PATH;
	}

	void accept(ExpressionVisitor &visitor) override { visitor.visit(this); }
	void accept(ConstExpressionVisitor &visitor) const override { visitor.visit(this); }

	[[nodiscard]] std::string toString() const override {
		return (isAll_ ? "allShortestPaths" : "shortestPath") +
			std::string("(") + startVar_ + "-[" + relType_ + "]-" + endVar_ + ")";
	}

	[[nodiscard]] std::unique_ptr<Expression> clone() const override {
		return std::make_unique<ShortestPathExpression>(
			startVar_, endVar_, relType_, direction_, minHops_, maxHops_, isAll_);
	}

	[[nodiscard]] const std::string& getStartVar() const { return startVar_; }
	[[nodiscard]] const std::string& getEndVar() const { return endVar_; }
	[[nodiscard]] const std::string& getRelType() const { return relType_; }
	[[nodiscard]] PatternDirection getDirection() const { return direction_; }
	[[nodiscard]] int getMinHops() const { return minHops_; }
	[[nodiscard]] int getMaxHops() const { return maxHops_; }
	[[nodiscard]] bool isAll() const { return isAll_; }

private:
	std::string startVar_;
	std::string endVar_;
	std::string relType_;
	PatternDirection direction_;
	int minHops_;
	int maxHops_;
	bool isAll_;
};

} // namespace graph::query::expressions
