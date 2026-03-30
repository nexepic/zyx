/**
 * @file LogicalSkip.hpp
 * @brief Logical operator that skips the first N rows from its child.
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
 * @class LogicalSkip
 * @brief Discards the first offset_ rows produced by its child operator.
 */
class LogicalSkip : public LogicalOperator {
public:
    LogicalSkip(std::unique_ptr<LogicalOperator> child, int64_t offset)
        : child_(std::move(child))
        , offset_(offset) {}

    // -------------------------------------------------------------------------
    // LogicalOperator interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOpType getType() const override {
        return LogicalOpType::LOP_SKIP;
    }

    [[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
        return {child_.get()};
    }

    [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
        return child_ ? child_->getOutputVariables() : std::vector<std::string>{};
    }

    [[nodiscard]] std::string toString() const override {
        return "Skip(" + std::to_string(offset_) + ")";
    }

    [[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
        return std::make_unique<LogicalSkip>(
            child_ ? child_->clone() : nullptr,
            offset_);
    }

    void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
        if (index != 0) throw std::out_of_range("LogicalSkip has only one child (index 0)");
        child_ = std::move(child);
    }

    std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
        if (index != 0) throw std::out_of_range("LogicalSkip has only one child (index 0)");
        return std::move(child_);
    }

    // -------------------------------------------------------------------------
    // Accessor
    // -------------------------------------------------------------------------

    [[nodiscard]] int64_t getOffset() const { return offset_; }

private:
    std::unique_ptr<LogicalOperator> child_;
    int64_t offset_;
};

} // namespace graph::query::logical
