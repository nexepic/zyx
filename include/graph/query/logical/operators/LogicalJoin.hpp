/**
 * @file LogicalJoin.hpp
 * @brief Logical operator representing a cross join (Cartesian product) of two
 *        child operators.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace graph::query::logical {

/**
 * @class LogicalJoin
 * @brief Combines two input streams as a cross join.
 *
 * At the physical level this maps to a CartesianProduct operator. Optimizer
 * rules may later push filter predicates down to convert this into a more
 * selective join.
 */
class LogicalJoin : public LogicalOperator {
public:
    LogicalJoin(std::unique_ptr<LogicalOperator> left,
                std::unique_ptr<LogicalOperator> right)
        : left_(std::move(left))
        , right_(std::move(right)) {}

    // -------------------------------------------------------------------------
    // LogicalOperator interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOpType getType() const override {
        return LogicalOpType::LOP_JOIN;
    }

    [[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
        return {left_.get(), right_.get()};
    }

    [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
        std::vector<std::string> vars;
        if (left_) {
            auto lv = left_->getOutputVariables();
            vars.insert(vars.end(), lv.begin(), lv.end());
        }
        if (right_) {
            auto rv = right_->getOutputVariables();
            vars.insert(vars.end(), rv.begin(), rv.end());
        }
        return vars;
    }

    [[nodiscard]] std::string toString() const override {
        return "Join";
    }

    [[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
        return std::make_unique<LogicalJoin>(
            left_  ? left_->clone()  : nullptr,
            right_ ? right_->clone() : nullptr);
    }

    void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
        if (index == 0)      { left_  = std::move(child); return; }
        if (index == 1)      { right_ = std::move(child); return; }
        throw std::out_of_range("LogicalJoin has only two children (index 0 or 1)");
    }

    std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
        if (index == 0) return std::move(left_);
        if (index == 1) return std::move(right_);
        throw std::out_of_range("LogicalJoin has only two children (index 0 or 1)");
    }

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOperator *getLeft()  const { return left_.get();  }
    [[nodiscard]] LogicalOperator *getRight() const { return right_.get(); }

private:
    std::unique_ptr<LogicalOperator> left_;
    std::unique_ptr<LogicalOperator> right_;
};

} // namespace graph::query::logical
