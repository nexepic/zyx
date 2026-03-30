/**
 * @file LogicalUnion.hpp
 * @brief Logical operator for UNION / UNION ALL of two sub-queries.
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
 * @class LogicalUnion
 * @brief Combines two result sets.
 *
 * When all_ is false, duplicate rows are eliminated (UNION).
 * When all_ is true, all rows including duplicates are kept (UNION ALL).
 *
 * Child index 0 = left operand, child index 1 = right operand.
 */
class LogicalUnion : public LogicalOperator {
public:
    LogicalUnion(std::unique_ptr<LogicalOperator> left,
                 std::unique_ptr<LogicalOperator> right,
                 bool all)
        : left_(std::move(left))
        , right_(std::move(right))
        , all_(all) {}

    // -------------------------------------------------------------------------
    // LogicalOperator interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOpType getType() const override {
        return LogicalOpType::LOP_UNION;
    }

    [[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
        return {left_.get(), right_.get()};
    }

    [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
        // Both branches must project the same columns; return from the left side.
        return left_ ? left_->getOutputVariables() : std::vector<std::string>{};
    }

    [[nodiscard]] std::string toString() const override {
        return all_ ? "UnionAll" : "Union";
    }

    [[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
        return std::make_unique<LogicalUnion>(
            left_  ? left_->clone()  : nullptr,
            right_ ? right_->clone() : nullptr,
            all_);
    }

    void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
        if (index == 0) { left_  = std::move(child); return; }
        if (index == 1) { right_ = std::move(child); return; }
        throw std::out_of_range("LogicalUnion has only two children (index 0 or 1)");
    }

    std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
        if (index == 0) return std::move(left_);
        if (index == 1) return std::move(right_);
        throw std::out_of_range("LogicalUnion has only two children (index 0 or 1)");
    }

    // -------------------------------------------------------------------------
    // Accessor
    // -------------------------------------------------------------------------

    [[nodiscard]] bool isAll() const { return all_; }
    [[nodiscard]] LogicalOperator *getLeft()  const { return left_.get();  }
    [[nodiscard]] LogicalOperator *getRight() const { return right_.get(); }

private:
    std::unique_ptr<LogicalOperator> left_;
    std::unique_ptr<LogicalOperator> right_;
    bool all_;
};

} // namespace graph::query::logical
