/**
 * @file LogicalAggregate.hpp
 * @brief Logical operator for aggregation (GROUP BY + aggregate functions).
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
 * @struct LogicalAggItem
 * @brief Describes a single aggregate function application.
 */
struct LogicalAggItem {
    std::string functionName;
    std::shared_ptr<graph::query::expressions::Expression> argument;
    std::string alias;
    bool distinct;
    std::shared_ptr<graph::query::expressions::Expression> extraArg; // percentile literal

    LogicalAggItem(std::string fn,
                   std::shared_ptr<graph::query::expressions::Expression> arg,
                   std::string al,
                   bool dist = false,
                   std::shared_ptr<graph::query::expressions::Expression> extra = nullptr)
        : functionName(std::move(fn))
        , argument(std::move(arg))
        , alias(std::move(al))
        , distinct(dist)
        , extraArg(std::move(extra)) {}
};

/**
 * @class LogicalAggregate
 * @brief Groups the child output by a set of expressions and computes one or
 *        more aggregate functions over each group.
 */
class LogicalAggregate : public LogicalOperator {
public:
    LogicalAggregate(std::unique_ptr<LogicalOperator> child,
                     std::vector<std::shared_ptr<graph::query::expressions::Expression>> groupByExprs,
                     std::vector<LogicalAggItem> aggregations,
                     std::vector<std::string> groupByAliases = {})
        : child_(std::move(child))
        , groupByExprs_(std::move(groupByExprs))
        , aggregations_(std::move(aggregations))
        , groupByAliases_(std::move(groupByAliases)) {}

    // -------------------------------------------------------------------------
    // LogicalOperator interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOpType getType() const override {
        return LogicalOpType::LOP_AGGREGATE;
    }

    [[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
        return {child_.get()};
    }

    [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
        std::vector<std::string> vars;
        // Include group-by variable names (toString of each group-by expression)
        for (const auto &expr : groupByExprs_) {
            if (expr) vars.push_back(expr->toString());
        }
        // Include aliases from aggregation items
        for (const auto &agg : aggregations_) {
            vars.push_back(agg.alias);
        }
        return vars;
    }

    [[nodiscard]] std::string toString() const override {
        return "Aggregate";
    }

    [[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
        std::vector<std::shared_ptr<graph::query::expressions::Expression>> clonedGroupBy;
        clonedGroupBy.reserve(groupByExprs_.size());
        for (const auto &expr : groupByExprs_) {
            std::shared_ptr<graph::query::expressions::Expression> cloned(expr ? expr->clone() : nullptr);
            clonedGroupBy.push_back(std::move(cloned));
        }

        std::vector<LogicalAggItem> clonedAggs;
        clonedAggs.reserve(aggregations_.size());
        for (const auto &agg : aggregations_) {
            std::shared_ptr<graph::query::expressions::Expression> clonedArg(
                agg.argument ? agg.argument->clone() : nullptr);
            std::shared_ptr<graph::query::expressions::Expression> clonedExtra(
                agg.extraArg ? agg.extraArg->clone() : nullptr);
            clonedAggs.emplace_back(agg.functionName, std::move(clonedArg), agg.alias, agg.distinct, std::move(clonedExtra));
        }

        return std::make_unique<LogicalAggregate>(
            child_ ? child_->clone() : nullptr,
            std::move(clonedGroupBy),
            std::move(clonedAggs),
            groupByAliases_);
    }

    void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
        if (index != 0) throw std::out_of_range("LogicalAggregate has only one child (index 0)");
        child_ = std::move(child);
    }

    std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
        if (index != 0) throw std::out_of_range("LogicalAggregate has only one child (index 0)");
        return std::move(child_);
    }

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    [[nodiscard]] const std::vector<std::shared_ptr<graph::query::expressions::Expression>> &getGroupByExprs() const {
        return groupByExprs_;
    }
    [[nodiscard]] const std::vector<LogicalAggItem> &getAggregations() const { return aggregations_; }
    [[nodiscard]] const std::vector<std::string> &getGroupByAliases() const { return groupByAliases_; }

private:
    std::unique_ptr<LogicalOperator> child_;
    std::vector<std::shared_ptr<graph::query::expressions::Expression>> groupByExprs_;
    std::vector<LogicalAggItem> aggregations_;
    std::vector<std::string> groupByAliases_;
};

} // namespace graph::query::logical
