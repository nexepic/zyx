/**
 * @file QuantifierFunctionExpression.cpp
 * @date 2025/3/9
 *
 * Implementation of QuantifierFunctionExpression
 */

#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/Expression.hpp"
#include <sstream>

namespace graph::query::expressions {

QuantifierFunctionExpression::QuantifierFunctionExpression(
	std::string functionName,
	std::string variable,
	std::unique_ptr<Expression> listExpr,
	std::unique_ptr<Expression> whereExpr
)
	: functionName_(std::move(functionName))
	, variable_(std::move(variable))
	, listExpr_(std::move(listExpr))
	, whereExpr_(std::move(whereExpr)) {
}

void QuantifierFunctionExpression::accept(ExpressionVisitor &visitor) {
	visitor.visit(this);
}

void QuantifierFunctionExpression::accept(ConstExpressionVisitor &visitor) const {
	visitor.visit(this);
}

std::string QuantifierFunctionExpression::toString() const {
	std::ostringstream oss;
	oss << functionName_ << "(" << variable_ << " IN "
	    << listExpr_->toString() << " WHERE " << whereExpr_->toString() << ")";
	return oss.str();
}

std::unique_ptr<Expression> QuantifierFunctionExpression::clone() const {
	return std::make_unique<QuantifierFunctionExpression>(
		functionName_,
		variable_,
		listExpr_->clone(),
		whereExpr_->clone()
	);
}

} // namespace graph::query::expressions
