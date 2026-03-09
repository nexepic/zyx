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
	std::string variable,
	std::unique_ptr<Expression> mapExpr,
	std::unique_ptr<Expression> whereExpr
)
	: pattern_(std::move(pattern))
	, variable_(std::move(variable))
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
	oss << "[" << pattern_ << " | " << mapExpr_->toString();
	if (whereExpr_) {
		oss << " WHERE " << whereExpr_->toString();
	}
	oss << "]";
	return oss.str();
}

std::unique_ptr<Expression> PatternComprehensionExpression::clone() const {
	return std::make_unique<PatternComprehensionExpression>(
		pattern_,
		variable_,
		mapExpr_->clone(),
		whereExpr_ ? whereExpr_->clone() : nullptr
	);
}

} // namespace graph::query::expressions
