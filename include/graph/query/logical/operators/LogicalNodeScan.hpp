/**
 * @file LogicalNodeScan.hpp
 * @brief Logical operator for scanning nodes from the graph.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/core/PropertyTypes.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace graph::query::logical {

/**
 * @class LogicalNodeScan
 * @brief Leaf operator that scans nodes matching a variable, optional labels,
 *        and optional property predicates.
 *
 * Optimizer rules may enrich this operator with additional property predicates
 * (predicate pushdown) before physical planning.
 */
class LogicalNodeScan : public LogicalOperator {
public:
    explicit LogicalNodeScan(std::string variable,
                             std::vector<std::string> labels = {},
                             std::vector<std::pair<std::string, graph::PropertyValue>> propertyPredicates = {})
        : variable_(std::move(variable))
        , labels_(std::move(labels))
        , propertyPredicates_(std::move(propertyPredicates)) {}

    // -------------------------------------------------------------------------
    // LogicalOperator interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOpType getType() const override {
        return LogicalOpType::LOP_NODE_SCAN;
    }

    [[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
        return {};
    }

    [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
        return {variable_};
    }

    [[nodiscard]] std::string toString() const override {
        std::string result = "NodeScan(" + variable_;
        if (!labels_.empty()) {
            result += ":";
            for (size_t i = 0; i < labels_.size(); ++i) {
                result += labels_[i];
                if (i + 1 < labels_.size()) result += ":";
            }
        }
        result += ")";
        return result;
    }

    [[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
        return std::make_unique<LogicalNodeScan>(variable_, labels_, propertyPredicates_);
    }

    void setChild(size_t /*index*/, std::unique_ptr<LogicalOperator> /*child*/) override {
        throw std::logic_error("LogicalNodeScan is a leaf node and does not accept children");
    }

    std::unique_ptr<LogicalOperator> detachChild(size_t /*index*/) override {
        throw std::logic_error("LogicalNodeScan is a leaf node and has no children to detach");
    }

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    [[nodiscard]] const std::string &getVariable() const { return variable_; }
    [[nodiscard]] const std::vector<std::string> &getLabels() const { return labels_; }
    [[nodiscard]] const std::vector<std::pair<std::string, graph::PropertyValue>> &getPropertyPredicates() const {
        return propertyPredicates_;
    }

    // -------------------------------------------------------------------------
    // Mutators (for optimizer use)
    // -------------------------------------------------------------------------

    void setPropertyPredicates(std::vector<std::pair<std::string, graph::PropertyValue>> predicates) {
        propertyPredicates_ = std::move(predicates);
    }

    /** @brief Sets the preferred scan strategy chosen by EnhancedIndexSelectionRule. */
    void setPreferredScanType(graph::query::execution::ScanType t) {
        preferredScanType_ = t;
    }

    /** @brief Returns the scan-strategy hint set by the optimizer (default: FULL_SCAN). */
    [[nodiscard]] graph::query::execution::ScanType getPreferredScanType() const {
        return preferredScanType_;
    }

private:
    std::string variable_;
    std::vector<std::string> labels_;
    std::vector<std::pair<std::string, graph::PropertyValue>> propertyPredicates_;
    graph::query::execution::ScanType preferredScanType_ =
        graph::query::execution::ScanType::FULL_SCAN;
};

} // namespace graph::query::logical
