/**
 * @file PatternComprehensionExpression.cpp
 * @date 2025/3/9
 *
 * Implementation of PatternComprehensionExpression
 */

#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/expressions/Expression.hpp"
#include <sstream>

namespace graph::query::expressions {

PatternComprehensionExpression::PatternComprehensionExpression(
	std::string pattern,
	std::string sourceVar,
	std::string targetVar,
	std::string relType,
	std::string targetLabel,
	PatternDirection direction,
	std::unique_ptr<Expression> mapExpr,
	std::unique_ptr<Expression> whereExpr
)
	: pattern_(std::move(pattern))
	, sourceVar_(std::move(sourceVar))
	, targetVar_(std::move(targetVar))
	, relType_(std::move(relType))
	, targetLabel_(std::move(targetLabel))
	, direction_(direction)
	, mapExpr_(std::move(mapExpr))
	, whereExpr_(std::move(whereExpr)) {
}

void PatternComprehensionExpression::accept(ExpressionVisitor &visitor) {
	visitor.visit(this);
}

void PatternComprehensionExpression::accept(ConstExpressionVisitor &visitor) const {
	visitor.visit(this);
}

std::string PatternComprehensionExpression::toString() const {
	std::ostringstream oss;
	oss << "[" << pattern_;
	if (whereExpr_) {
		oss << " WHERE " << whereExpr_->toString();
	}
	if (mapExpr_) {
		oss << " | " << mapExpr_->toString();
	}
	oss << "]";
	return oss.str();
}

std::unique_ptr<Expression> PatternComprehensionExpression::clone() const {
	return std::make_unique<PatternComprehensionExpression>(
		pattern_,
		sourceVar_,
		targetVar_,
		relType_,
		targetLabel_,
		direction_,
		mapExpr_ ? mapExpr_->clone() : nullptr,
		whereExpr_ ? whereExpr_->clone() : nullptr
	);
}


PatternComprehensionExpression::PatternComprehensionExpression(
	std::string pattern,
	std::string variable,
	std::unique_ptr<Expression> mapExpr,
	std::unique_ptr<Expression> whereExpr
)
	: pattern_(std::move(pattern))
	, sourceVar_()
	, targetVar_(std::move(variable))
	, relType_()
	, targetLabel_()
	, direction_(PatternDirection::PAT_BOTH)
	, mapExpr_(std::move(mapExpr))
	, whereExpr_(std::move(whereExpr)) {
}

} // namespace graph::query::expressions
