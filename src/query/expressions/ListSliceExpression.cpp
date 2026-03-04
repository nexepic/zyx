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

} // namespace graph::query::expressions
