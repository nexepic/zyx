/**
 * @file ExistsExpression.cpp
 * @date 2025/3/9
 *
 * Implementation of ExistsExpression
 */

#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/Expression.hpp"
#include <sstream>

namespace graph::query::expressions {

ExistsExpression::ExistsExpression(
	std::string pattern,
	std::unique_ptr<Expression> whereExpr
)
	: pattern_(std::move(pattern))
	, whereExpr_(std::move(whereExpr)) {
}

ExistsExpression::ExistsExpression(
	std::string pattern,
	std::string sourceVar,
	std::string relType,
	std::string targetLabel,
	PatternDirection direction,
	std::unique_ptr<Expression> whereExpr
)
	: pattern_(std::move(pattern))
	, whereExpr_(std::move(whereExpr))
	, sourceVar_(std::move(sourceVar))
	, relType_(std::move(relType))
	, targetLabel_(std::move(targetLabel))
	, direction_(direction)
	 {
}

void ExistsExpression::accept(ExpressionVisitor &visitor) {
	visitor.visit(this);
}

void ExistsExpression::accept(ConstExpressionVisitor &visitor) const {
	visitor.visit(this);
}

std::string ExistsExpression::toString() const {
	std::ostringstream oss;
	oss << "EXISTS(" << pattern_;
	if (whereExpr_) {
		oss << " WHERE " << whereExpr_->toString();
	}
	oss << ")";
	return oss.str();
}

std::unique_ptr<Expression> ExistsExpression::clone() const {
	if (!sourceVar_.empty()) {
		return std::make_unique<ExistsExpression>(
			pattern_, sourceVar_, relType_, targetLabel_, direction_,
			whereExpr_ ? whereExpr_->clone() : nullptr
		);
	}
	return std::make_unique<ExistsExpression>(
		pattern_,
		whereExpr_ ? whereExpr_->clone() : nullptr
	);
}

} // namespace graph::query::expressions
