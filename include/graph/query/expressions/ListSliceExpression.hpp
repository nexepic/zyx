/**
 * @file ListSliceExpression.hpp
 * @date 2025/1/4
 *
 * List slicing expression for Cypher: list[0..2], list[-1], etc.
 */

#pragma once

#include "Expression.hpp"
#include <memory>

namespace graph::query::expressions {

/**
 * Represents list slicing operations:
 * - list[0]       - single element
 * - list[0..2]    - range slice
 * - list[0..]     - from start to end
 * - list[..-1]    - from start to end-1
 * - list[-1]      - last element
 */
class ListSliceExpression : public Expression {
public:
    /**
     * Construct list slice expression
     * @param list The list expression to slice
     * @param start Start index (null for ..expr)
     * @param end End index (null for expr.. or omitted)
     * @param hasRange True if .. is present
     */
    ListSliceExpression(std::unique_ptr<Expression> list,
                       std::unique_ptr<Expression> start,
                       std::unique_ptr<Expression> end,
                       bool hasRange);

    ~ListSliceExpression() override = default;

    [[nodiscard]] ExpressionType getExpressionType() const override;
    void accept(ExpressionVisitor &visitor) override;
    void accept(ConstExpressionVisitor &visitor) const override;
    [[nodiscard]] std::string toString() const override;
    [[nodiscard]] std::unique_ptr<Expression> clone() const override;

    [[nodiscard]] const Expression* getList() const { return list_.get(); }
    [[nodiscard]] const Expression* getStart() const { return start_.get(); }
    [[nodiscard]] const Expression* getEnd() const { return end_.get(); }
    [[nodiscard]] bool hasRange() const { return hasRange_; }

private:
    std::unique_ptr<Expression> list_;
    std::unique_ptr<Expression> start_;
    std::unique_ptr<Expression> end_;
    bool hasRange_;
};

} // namespace graph::query::expressions
