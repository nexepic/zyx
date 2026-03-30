/**
 * @file LogicalSingleRow.hpp
 * @brief Logical operator that produces a single empty row (identity element).
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
 * @class LogicalSingleRow
 * @brief Leaf operator that emits exactly one row with no columns.
 *
 * Used as the starting point for queries that do not match any graph pattern
 * (e.g., standalone RETURN clauses with no MATCH) and as the identity element
 * when building query trees bottom-up.
 */
class LogicalSingleRow : public LogicalOperator {
public:
    LogicalSingleRow() = default;

    // -------------------------------------------------------------------------
    // LogicalOperator interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOpType getType() const override {
        return LogicalOpType::LOP_SINGLE_ROW;
    }

    [[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
        return {};
    }

    [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
        return {};
    }

    [[nodiscard]] std::string toString() const override {
        return "SingleRow";
    }

    [[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
        return std::make_unique<LogicalSingleRow>();
    }

    void setChild(size_t /*index*/, std::unique_ptr<LogicalOperator> /*child*/) override {
        throw std::logic_error("LogicalSingleRow is a leaf node and does not accept children");
    }

    std::unique_ptr<LogicalOperator> detachChild(size_t /*index*/) override {
        throw std::logic_error("LogicalSingleRow is a leaf node and has no children to detach");
    }
};

} // namespace graph::query::logical
