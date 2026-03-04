/**
 * @file ListLiteralExpression.cpp
 * @date 2026/1/4
 */

#include "graph/query/expressions/ListLiteralExpression.hpp"
#include <sstream>

namespace graph::query::expressions {

ListLiteralExpression::ListLiteralExpression(PropertyValue listValue)
	: value_(std::move(listValue)) {
	// Validate that it's actually a list
	if (value_.getType() != PropertyType::LIST) {
		throw std::runtime_error("ListLiteralExpression requires a LIST type PropertyValue");
	}
}

void ListLiteralExpression::accept(ExpressionVisitor &visitor) {
	visitor.visit(this);
}

void ListLiteralExpression::accept(ConstExpressionVisitor &visitor) const {
	visitor.visit(this);
}

std::string ListLiteralExpression::toString() const {
	return value_.toString();
}

std::unique_ptr<Expression> ListLiteralExpression::clone() const {
	return std::make_unique<ListLiteralExpression>(value_);
}

} // namespace graph::query::expressions
