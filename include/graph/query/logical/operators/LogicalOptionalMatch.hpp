/**
 * @file LogicalOptionalMatch.hpp
 * @brief Logical operator for OPTIONAL MATCH semantics (left outer join).
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
 * @class LogicalOptionalMatch
 * @brief Combines a mandatory input stream with an optional pattern stream.
 *
 * If the optional pattern produces no rows for a given input row, the input
 * row is still passed through with NULLs for the optional pattern variables
 * (left outer join semantics).
 *
 * Child index 0 = input (mandatory side)
 * Child index 1 = optionalPattern (optional side)
 */
class LogicalOptionalMatch : public LogicalOperator {
public:
    LogicalOptionalMatch(std::unique_ptr<LogicalOperator> input,
                         std::unique_ptr<LogicalOperator> optionalPattern,
                         std::vector<std::string> requiredVariables = {})
        : input_(std::move(input))
        , optionalPattern_(std::move(optionalPattern))
        , requiredVariables_(std::move(requiredVariables)) {}

    // -------------------------------------------------------------------------
    // LogicalOperator interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOpType getType() const override {
        return LogicalOpType::LOP_OPTIONAL_MATCH;
    }

    [[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
        return {input_.get(), optionalPattern_.get()};
    }

    [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
        std::vector<std::string> vars;
        if (input_) {
            auto iv = input_->getOutputVariables();
            vars.insert(vars.end(), iv.begin(), iv.end());
        }
        if (optionalPattern_) {
            auto ov = optionalPattern_->getOutputVariables();
            vars.insert(vars.end(), ov.begin(), ov.end());
        }
        return vars;
    }

    [[nodiscard]] std::string toString() const override {
        return "OptionalMatch";
    }

    [[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
        return std::make_unique<LogicalOptionalMatch>(
            input_           ? input_->clone()           : nullptr,
            optionalPattern_ ? optionalPattern_->clone() : nullptr,
            requiredVariables_);
    }

    void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
        if (index == 0) { input_           = std::move(child); return; }
        if (index == 1) { optionalPattern_ = std::move(child); return; }
        throw std::out_of_range("LogicalOptionalMatch has only two children (index 0 or 1)");
    }

    std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
        if (index == 0) return std::move(input_);
        if (index == 1) return std::move(optionalPattern_);
        throw std::out_of_range("LogicalOptionalMatch has only two children (index 0 or 1)");
    }

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOperator *getInput()           const { return input_.get();           }
    [[nodiscard]] LogicalOperator *getOptionalPattern() const { return optionalPattern_.get(); }
    [[nodiscard]] const std::vector<std::string> &getRequiredVariables() const { return requiredVariables_; }

private:
    std::unique_ptr<LogicalOperator> input_;
    std::unique_ptr<LogicalOperator> optionalPattern_;
    std::vector<std::string> requiredVariables_;
};

} // namespace graph::query::logical
