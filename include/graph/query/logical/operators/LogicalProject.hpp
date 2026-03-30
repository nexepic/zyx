/**
 * @file LogicalProject.hpp
 * @brief Logical operator for projecting (selecting/renaming) columns.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/expressions/Expression.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace graph::query::logical {

/**
 * @struct LogicalProjectItem
 * @brief A single projection item: an expression evaluated to a named alias.
 */
struct LogicalProjectItem {
    std::shared_ptr<graph::query::expressions::Expression> expression;
    std::string alias;

    LogicalProjectItem(std::shared_ptr<graph::query::expressions::Expression> expr, std::string al)
        : expression(std::move(expr)), alias(std::move(al)) {}
};

/**
 * @class LogicalProject
 * @brief Projects (and optionally de-duplicates) a set of expressions from its
 *        child operator.
 *
 * The requiredColumns_ set is populated by ProjectionPushdownRule to communicate
 * which columns must be kept alive by upstream operators.
 */
class LogicalProject : public LogicalOperator {
public:
    LogicalProject(std::unique_ptr<LogicalOperator> child,
                   std::vector<LogicalProjectItem> items,
                   bool distinct = false)
        : child_(std::move(child))
        , items_(std::move(items))
        , distinct_(distinct) {}

    // -------------------------------------------------------------------------
    // LogicalOperator interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOpType getType() const override {
        return LogicalOpType::LOP_PROJECT;
    }

    [[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
        return {child_.get()};
    }

    [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
        std::vector<std::string> vars;
        vars.reserve(items_.size());
        for (const auto &item : items_) {
            vars.push_back(item.alias);
        }
        return vars;
    }

    [[nodiscard]] std::string toString() const override {
        std::string result = distinct_ ? "DistinctProject(" : "Project(";
        for (size_t i = 0; i < items_.size(); ++i) {
            result += items_[i].alias;
            if (i + 1 < items_.size()) result += ", ";
        }
        result += ")";
        return result;
    }

    [[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
        auto clonedChild = child_ ? child_->clone() : nullptr;
        std::vector<LogicalProjectItem> clonedItems;
        clonedItems.reserve(items_.size());
        for (const auto &item : items_) {
            auto clonedExpr = item.expression ? item.expression->clone() : nullptr;
            std::shared_ptr<graph::query::expressions::Expression> sharedExpr(std::move(clonedExpr));
            clonedItems.emplace_back(std::move(sharedExpr), item.alias);
        }
        auto op = std::make_unique<LogicalProject>(std::move(clonedChild), std::move(clonedItems), distinct_);
        op->requiredColumns_ = requiredColumns_;
        return op;
    }

    void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
        if (index != 0) throw std::out_of_range("LogicalProject has only one child (index 0)");
        child_ = std::move(child);
    }

    std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
        if (index != 0) throw std::out_of_range("LogicalProject has only one child (index 0)");
        return std::move(child_);
    }

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    [[nodiscard]] const std::vector<LogicalProjectItem> &getItems() const { return items_; }
    [[nodiscard]] bool isDistinct() const { return distinct_; }
    [[nodiscard]] const std::unordered_set<std::string> &getRequiredColumns() const { return requiredColumns_; }

    // -------------------------------------------------------------------------
    // Mutators (for optimizer use)
    // -------------------------------------------------------------------------

    void setRequiredColumns(std::unordered_set<std::string> cols) {
        requiredColumns_ = std::move(cols);
    }

private:
    std::unique_ptr<LogicalOperator> child_;
    std::vector<LogicalProjectItem> items_;
    bool distinct_;
    std::unordered_set<std::string> requiredColumns_;
};

} // namespace graph::query::logical
