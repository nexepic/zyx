/**
 * @file LogicalAggregate.hpp
 * @brief Logical operator for aggregation (GROUP BY + aggregate functions).
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once
#include "graph/utils/OwnedStringVector.hpp"
#include "graph/utils/OwnedStdVector.hpp"
#include "graph/utils/OwnedStdString.hpp"

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
    using OwnedHeapElement = void;
    utils::OwnedStdString functionName;
    std::shared_ptr<graph::query::expressions::Expression> argument;
    utils::OwnedStdString alias;
    bool distinct;
    std::shared_ptr<graph::query::expressions::Expression> extraArg; // percentile literal

    LogicalAggItem(utils::OwnedStdString fn,
                   std::shared_ptr<graph::query::expressions::Expression> arg,
                   utils::OwnedStdString al,
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
                     utils::OwnedStdVector<std::shared_ptr<graph::query::expressions::Expression>> groupByExprs,
                     utils::OwnedStdVector<LogicalAggItem> aggregations,
                     utils::OwnedStringVector groupByAliases = {})
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
        for (size_t index = 0U; index < groupByExprs_.size(); ++index) {
            const auto &expr = groupByExprs_[index];
            if (!expr) continue;
            if (index < groupByAliases_.size() && !groupByAliases_[index].empty()) {
                vars.push_back(groupByAliases_[index]);
            } else {
                vars.push_back(expr->toString());
            }
        }
        // Include aliases from aggregation items
        for (const auto &agg : aggregations_) {
            vars.push_back(agg.alias);
        }
        return vars;
    }

    [[nodiscard]] utils::OwnedStringVector getOwnedOutputVariables() const override {
        utils::OwnedStringVector vars;
        for (size_t index = 0U; index < groupByExprs_.size(); ++index) {
            const auto &expr = groupByExprs_[index];
            if (!expr) continue;
            if (index < groupByAliases_.size() && !groupByAliases_[index].empty()) {
                vars.push_back(groupByAliases_[index]);
            } else {
                vars.push_back(expr->toString());
            }
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
        utils::OwnedStdVector<std::shared_ptr<graph::query::expressions::Expression>> clonedGroupBy;
        clonedGroupBy.reserve(groupByExprs_.size());
        for (const auto &expr : groupByExprs_) {
            auto cloned = utils::shareOwned(expr ? expr->clone() : nullptr);
            clonedGroupBy.push_back(std::move(cloned));
        }

        utils::OwnedStdVector<LogicalAggItem> clonedAggs;
        clonedAggs.reserve(aggregations_.size());
        for (const auto &agg : aggregations_) {
            auto clonedArg = utils::shareOwned(agg.argument ? agg.argument->clone() : nullptr);
            auto clonedExtra = utils::shareOwned(agg.extraArg ? agg.extraArg->clone() : nullptr);
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
    utils::OwnedStdVector<std::shared_ptr<graph::query::expressions::Expression>> groupByExprs_;
    utils::OwnedStdVector<LogicalAggItem> aggregations_;
    utils::OwnedStringVector groupByAliases_;
};

} // namespace graph::query::logical
