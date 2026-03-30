/**
 * @file LogicalFilter.hpp
 * @brief Logical operator for filtering rows by a predicate expression.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/expressions/Expression.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace graph::query::logical {

/**
 * @class LogicalFilter
 * @brief Filters the output of its child operator using an inspectable
 *        Expression AST predicate.
 *
 * The predicate is stored as a shared_ptr<Expression> so that optimizer rules
 * can inspect, rewrite, and share sub-expressions without copying.
 */
class LogicalFilter : public LogicalOperator {
public:
    LogicalFilter(std::unique_ptr<LogicalOperator> child,
                  std::shared_ptr<graph::query::expressions::Expression> predicate)
        : child_(std::move(child))
        , predicate_(std::move(predicate)) {}

    // -------------------------------------------------------------------------
    // LogicalOperator interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOpType getType() const override {
        return LogicalOpType::LOP_FILTER;
    }

    [[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
        return {child_.get()};
    }

    [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
        return child_ ? child_->getOutputVariables() : std::vector<std::string>{};
    }

    [[nodiscard]] std::string toString() const override {
        return "Filter(" + (predicate_ ? predicate_->toString() : "null") + ")";
    }

    [[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
        auto clonedChild = child_ ? child_->clone() : nullptr;
        auto clonedPred  = predicate_ ? predicate_->clone() : nullptr;
        std::shared_ptr<graph::query::expressions::Expression> sharedPred(std::move(clonedPred));
        return std::make_unique<LogicalFilter>(std::move(clonedChild), std::move(sharedPred));
    }

    void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
        if (index != 0) throw std::out_of_range("LogicalFilter has only one child (index 0)");
        child_ = std::move(child);
    }

    std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
        if (index != 0) throw std::out_of_range("LogicalFilter has only one child (index 0)");
        return std::move(child_);
    }

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    [[nodiscard]] const std::shared_ptr<graph::query::expressions::Expression> &getPredicate() const {
        return predicate_;
    }

    [[nodiscard]] graph::query::expressions::Expression *getPredicateRaw() const {
        return predicate_.get();
    }

private:
    std::unique_ptr<LogicalOperator> child_;
    std::shared_ptr<graph::query::expressions::Expression> predicate_;
};

} // namespace graph::query::logical
