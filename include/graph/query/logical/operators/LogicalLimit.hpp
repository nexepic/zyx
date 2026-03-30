/**
 * @file LogicalLimit.hpp
 * @brief Logical operator that limits the number of output rows.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace graph::query::logical {

/**
 * @class LogicalLimit
 * @brief Passes at most limit_ rows from its child operator downstream.
 */
class LogicalLimit : public LogicalOperator {
public:
    LogicalLimit(std::unique_ptr<LogicalOperator> child, int64_t limit)
        : child_(std::move(child))
        , limit_(limit) {}

    // -------------------------------------------------------------------------
    // LogicalOperator interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOpType getType() const override {
        return LogicalOpType::LOP_LIMIT;
    }

    [[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
        return {child_.get()};
    }

    [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
        return child_ ? child_->getOutputVariables() : std::vector<std::string>{};
    }

    [[nodiscard]] std::string toString() const override {
        return "Limit(" + std::to_string(limit_) + ")";
    }

    [[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
        return std::make_unique<LogicalLimit>(
            child_ ? child_->clone() : nullptr,
            limit_);
    }

    void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
        if (index != 0) throw std::out_of_range("LogicalLimit has only one child (index 0)");
        child_ = std::move(child);
    }

    std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
        if (index != 0) throw std::out_of_range("LogicalLimit has only one child (index 0)");
        return std::move(child_);
    }

    // -------------------------------------------------------------------------
    // Accessor
    // -------------------------------------------------------------------------

    [[nodiscard]] int64_t getLimit() const { return limit_; }

private:
    std::unique_ptr<LogicalOperator> child_;
    int64_t limit_;
};

} // namespace graph::query::logical
