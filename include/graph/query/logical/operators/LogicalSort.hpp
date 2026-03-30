/**
 * @file LogicalSort.hpp
 * @brief Logical operator for sorting rows by one or more expressions.
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
 * @struct LogicalSortItem
 * @brief A single sort key: an expression and its sort direction.
 */
struct LogicalSortItem {
    std::shared_ptr<graph::query::expressions::Expression> expression;
    bool ascending;

    LogicalSortItem(std::shared_ptr<graph::query::expressions::Expression> expr, bool asc)
        : expression(std::move(expr)), ascending(asc) {}
};

/**
 * @class LogicalSort
 * @brief Sorts the output of its child operator by a list of sort items.
 */
class LogicalSort : public LogicalOperator {
public:
    LogicalSort(std::unique_ptr<LogicalOperator> child,
                std::vector<LogicalSortItem> sortItems)
        : child_(std::move(child))
        , sortItems_(std::move(sortItems)) {}

    // -------------------------------------------------------------------------
    // LogicalOperator interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOpType getType() const override {
        return LogicalOpType::LOP_SORT;
    }

    [[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
        return {child_.get()};
    }

    [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
        return child_ ? child_->getOutputVariables() : std::vector<std::string>{};
    }

    [[nodiscard]] std::string toString() const override {
        return "Sort";
    }

    [[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
        std::vector<LogicalSortItem> clonedItems;
        clonedItems.reserve(sortItems_.size());
        for (const auto &item : sortItems_) {
            auto clonedExpr = item.expression ? item.expression->clone() : nullptr;
            std::shared_ptr<graph::query::expressions::Expression> sharedExpr(std::move(clonedExpr));
            clonedItems.emplace_back(std::move(sharedExpr), item.ascending);
        }
        return std::make_unique<LogicalSort>(
            child_ ? child_->clone() : nullptr,
            std::move(clonedItems));
    }

    void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
        if (index != 0) throw std::out_of_range("LogicalSort has only one child (index 0)");
        child_ = std::move(child);
    }

    std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
        if (index != 0) throw std::out_of_range("LogicalSort has only one child (index 0)");
        return std::move(child_);
    }

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    [[nodiscard]] const std::vector<LogicalSortItem> &getSortItems() const { return sortItems_; }

private:
    std::unique_ptr<LogicalOperator> child_;
    std::vector<LogicalSortItem> sortItems_;
};

} // namespace graph::query::logical
