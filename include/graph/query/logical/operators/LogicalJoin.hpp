/**
 * @file LogicalJoin.hpp
 * @brief Logical operator representing a cross join (Cartesian product) of two
 *        child operators.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once
#include <algorithm>

#include "graph/query/logical/LogicalOperator.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace graph::query::logical {

/**
 * @class LogicalJoin
 * @brief Combines two input streams as a cross join.
 *
 * The logical read runtime evaluates the Cartesian product directly. Future
 * logical-plan transformations may replace it with a more selective join
 * without introducing a storage-specific execution operator.
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
        std::unordered_set<std::string> seen;
        if (left_) {
            auto lv = left_->getOutputVariables();
            for (std::string &variable : lv) {
                if (seen.insert(variable).second) {
                    vars.push_back(std::move(variable));
                }
            }
        }
        if (right_) {
            auto rv = right_->getOutputVariables();
            for (std::string &variable : rv) {
                if (seen.insert(variable).second) {
                    vars.push_back(std::move(variable));
                }
            }
        }
        return vars;
    }

    [[nodiscard]] utils::OwnedStringVector getOwnedOutputVariables() const override {
        utils::OwnedStringVector vars;

        if (left_) {
            auto lv = left_->getOwnedOutputVariables();
            for (const std::string &variable : lv) {
                if (std::find(vars.begin(), vars.end(), variable) == vars.end()) {
                    vars.push_back(variable);
                }
            }
        }
        if (right_) {
            auto rv = right_->getOwnedOutputVariables();
            for (const std::string &variable : rv) {
                if (std::find(vars.begin(), vars.end(), variable) == vars.end()) {
                    vars.push_back(variable);
                }
            }
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
