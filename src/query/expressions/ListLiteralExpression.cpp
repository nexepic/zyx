/**
 * @file ListLiteralExpression.cpp
 * @date 2026/1/4
 */

#include "graph/query/expressions/ListLiteralExpression.hpp"
#include <sstream>
#include <stdexcept>

namespace graph::query::expressions {

ListLiteralExpression::ListLiteralExpression(PropertyValue listValue)
	: value_(std::move(listValue)) {
	// Validate that it's actually a list
	if (value_.getType() != PropertyType::LIST) {
		throw std::runtime_error("ListLiteralExpression requires a LIST type PropertyValue");
	}
}

ListLiteralExpression::ListLiteralExpression(std::vector<std::unique_ptr<Expression>> elements)
	: value_(std::vector<PropertyValue>{}), elements_(std::move(elements)) {
	for (const auto &element: elements_) {
		if (!element) {
			throw std::invalid_argument("ListLiteralExpression cannot contain a null element");
		}
	}
}

void ListLiteralExpression::accept(ExpressionVisitor &visitor) {
	visitor.visit(this);
}

void ListLiteralExpression::accept(ConstExpressionVisitor &visitor) const {
	visitor.visit(this);
}

std::string ListLiteralExpression::toString() const {
	if (!isStatic()) {
		std::ostringstream stream;
		stream << '[';
		for (size_t index = 0U; index < elements_.size(); ++index) {
			if (index != 0U) {
				stream << ", ";
			}
			stream << elements_[index]->toString();
		}
		stream << ']';
		return stream.str();
	}
	return value_.toString();
}

std::unique_ptr<Expression> ListLiteralExpression::clone() const {
	if (!isStatic()) {
		std::vector<std::unique_ptr<Expression>> clonedElements;
		clonedElements.reserve(elements_.size());
		for (const auto &element: elements_) {
			clonedElements.push_back(element->clone());
		}
		return std::make_unique<ListLiteralExpression>(std::move(clonedElements));
	}
	return std::make_unique<ListLiteralExpression>(value_);
}

const PropertyValue &ListLiteralExpression::getValue() const {
	if (!isStatic()) {
		throw std::logic_error("dynamic ListLiteralExpression has no static value");
	}
	return value_;
}

} // namespace graph::query::expressions
