/**
 * @file LogicalUnwind.hpp
 * @brief Logical operator for UNWIND: expands a list into individual rows.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/query/expressions/Expression.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace graph::query::logical {

/**
 * @class LogicalUnwind
 * @brief Unwinds (flattens) a list into one row per element.
 *
 * Two construction modes:
 *  - Literal list: the list values are known at plan time.
 *  - Expression list: the list is computed from an Expression at execution time.
 */
class LogicalUnwind : public LogicalOperator {
public:
    /**
     * @brief Constructor for a literal list UNWIND.
     */
    LogicalUnwind(std::unique_ptr<LogicalOperator> child,
                  std::string alias,
                  std::vector<graph::PropertyValue> listValues)
        : child_(std::move(child))
        , alias_(std::move(alias))
        , listValues_(std::move(listValues))
        , listExpr_(nullptr) {}

    /**
     * @brief Constructor for an expression-based UNWIND.
     */
    LogicalUnwind(std::unique_ptr<LogicalOperator> child,
                  std::string alias,
                  std::shared_ptr<graph::query::expressions::Expression> listExpr)
        : child_(std::move(child))
        , alias_(std::move(alias))
        , listExpr_(std::move(listExpr)) {}

    // -------------------------------------------------------------------------
    // LogicalOperator interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOpType getType() const override {
        return LogicalOpType::LOP_UNWIND;
    }

    [[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
        return {child_.get()};
    }

    [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
        std::vector<std::string> vars;
        if (child_) {
            vars = child_->getOutputVariables();
        }
        vars.push_back(alias_);
        return vars;
    }

    [[nodiscard]] std::string toString() const override {
        return "Unwind(" + alias_ + ")";
    }

    [[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
        if (listExpr_) {
            std::shared_ptr<graph::query::expressions::Expression> clonedExpr(listExpr_->clone());
            return std::make_unique<LogicalUnwind>(
                child_ ? child_->clone() : nullptr,
                alias_,
                std::move(clonedExpr));
        }
        return std::make_unique<LogicalUnwind>(
            child_ ? child_->clone() : nullptr,
            alias_,
            listValues_);
    }

    void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
        if (index != 0) throw std::out_of_range("LogicalUnwind has only one child (index 0)");
        child_ = std::move(child);
    }

    std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
        if (index != 0) throw std::out_of_range("LogicalUnwind has only one child (index 0)");
        return std::move(child_);
    }

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    [[nodiscard]] const std::string &getAlias() const { return alias_; }
    [[nodiscard]] const std::vector<graph::PropertyValue> &getListValues() const { return listValues_; }
    [[nodiscard]] const std::shared_ptr<graph::query::expressions::Expression> &getListExpr() const { return listExpr_; }
    [[nodiscard]] bool hasLiteralList() const { return listExpr_ == nullptr; }

private:
    std::unique_ptr<LogicalOperator> child_;
    std::string alias_;
    std::vector<graph::PropertyValue> listValues_;
    std::shared_ptr<graph::query::expressions::Expression> listExpr_;
};

} // namespace graph::query::logical
