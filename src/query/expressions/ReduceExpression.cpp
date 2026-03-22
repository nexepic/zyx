/**
 * @file ReduceExpression.cpp
 * @date 2026/3/22
 *
 * Implementation of ReduceExpression
 */

#include "graph/query/expressions/ReduceExpression.hpp"
#include "graph/query/expressions/Expression.hpp"
#include <sstream>

namespace graph::query::expressions {

ReduceExpression::ReduceExpression(
	std::string accumulator,
	std::unique_ptr<Expression> initialExpr,
	std::string variable,
	std::unique_ptr<Expression> listExpr,
	std::unique_ptr<Expression> bodyExpr
)
	: accumulator_(std::move(accumulator))
	, initialExpr_(std::move(initialExpr))
	, variable_(std::move(variable))
	, listExpr_(std::move(listExpr))
	, bodyExpr_(std::move(bodyExpr)) {
}

void ReduceExpression::accept(ExpressionVisitor &visitor) {
	visitor.visit(this);
}

void ReduceExpression::accept(ConstExpressionVisitor &visitor) const {
	visitor.visit(this);
}

std::string ReduceExpression::toString() const {
	std::ostringstream oss;
	oss << "reduce(" << accumulator_ << " = " << initialExpr_->toString()
		<< ", " << variable_ << " IN " << listExpr_->toString()
		<< " | " << bodyExpr_->toString() << ")";
	return oss.str();
}

std::unique_ptr<Expression> ReduceExpression::clone() const {
	return std::make_unique<ReduceExpression>(
		accumulator_,
		initialExpr_->clone(),
		variable_,
		listExpr_->clone(),
		bodyExpr_->clone()
	);
}

} // namespace graph::query::expressions
