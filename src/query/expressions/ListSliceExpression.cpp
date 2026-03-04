/**
 * @file ListSliceExpression.cpp
 */

#include "graph/query/expressions/ListSliceExpression.hpp"

namespace graph::query::expressions {

ListSliceExpression::ListSliceExpression(
    std::unique_ptr<Expression> list,
    std::unique_ptr<Expression> start,
    std::unique_ptr<Expression> end,
    bool hasRange)
    : list_(std::move(list)),
      start_(std::move(start)),
      end_(std::move(end)),
      hasRange_(hasRange) {}

ExpressionType ListSliceExpression::getExpressionType() const {
    return ExpressionType::LIST_SLICE;
}

void ListSliceExpression::accept(ExpressionVisitor &visitor) {
    visitor.visit(this);
}

void ListSliceExpression::accept(ConstExpressionVisitor &visitor) const {
    visitor.visit(this);
}

std::string ListSliceExpression::toString() const {
    std::string result = list_->toString() + "[";

    if (start_) {
        result += start_->toString();
    }

    if (hasRange_) {
        result += "..";
        if (end_) {
            result += end_->toString();
        }
    }

    result += "]";
    return result;
}

std::unique_ptr<Expression> ListSliceExpression::clone() const {
    return std::make_unique<ListSliceExpression>(
        list_->clone(),
        start_ ? start_->clone() : nullptr,
        end_ ? end_->clone() : nullptr,
        hasRange_
    );
}

} // namespace graph::query::expressions
