/**
 * @file LogicalVarLengthTraversal.hpp
 * @brief Logical operator for variable-length (multi-hop) edge traversal.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/core/PropertyTypes.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace graph::query::logical {

/**
 * @class LogicalVarLengthTraversal
 * @brief Traverses between minHops_ and maxHops_ hops from a source variable,
 *        binding the edge collection and target to new variables.
 *
 * Stores optional target label/property filters so that the physical plan
 * converter can generate the appropriate predicates.
 */
class LogicalVarLengthTraversal : public LogicalOperator {
public:
    LogicalVarLengthTraversal(std::unique_ptr<LogicalOperator> child,
                              std::string sourceVar,
                              std::string edgeVar,
                              std::string targetVar,
                              std::string edgeLabel,
                              std::string direction,
                              int minHops,
                              int maxHops,
                              std::vector<std::string> targetLabels = {},
                              std::vector<std::pair<std::string, graph::PropertyValue>> targetProperties = {})
        : child_(std::move(child))
        , sourceVar_(std::move(sourceVar))
        , edgeVar_(std::move(edgeVar))
        , targetVar_(std::move(targetVar))
        , edgeLabel_(std::move(edgeLabel))
        , direction_(std::move(direction))
        , minHops_(minHops)
        , maxHops_(maxHops)
        , targetLabels_(std::move(targetLabels))
        , targetProperties_(std::move(targetProperties)) {}

    // -------------------------------------------------------------------------
    // LogicalOperator interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOpType getType() const override {
        return LogicalOpType::LOP_VAR_LENGTH_TRAVERSAL;
    }

    [[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
        return {child_.get()};
    }

    [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
        std::vector<std::string> vars;
        if (child_) {
            vars = child_->getOutputVariables();
        }
        if (!edgeVar_.empty())   vars.push_back(edgeVar_);
        if (!targetVar_.empty()) vars.push_back(targetVar_);
        return vars;
    }

    [[nodiscard]] std::string toString() const override {
        return "VarLengthTraversal(" + sourceVar_ + "-[" + edgeVar_ + ":" + edgeLabel_
               + "*" + std::to_string(minHops_) + ".." + std::to_string(maxHops_)
               + "]->" + targetVar_ + ")";
    }

    [[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
        return std::make_unique<LogicalVarLengthTraversal>(
            child_ ? child_->clone() : nullptr,
            sourceVar_, edgeVar_, targetVar_, edgeLabel_, direction_,
            minHops_, maxHops_,
            targetLabels_, targetProperties_);
    }

    void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
        if (index != 0) throw std::out_of_range("LogicalVarLengthTraversal has only one child (index 0)");
        child_ = std::move(child);
    }

    std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
        if (index != 0) throw std::out_of_range("LogicalVarLengthTraversal has only one child (index 0)");
        return std::move(child_);
    }

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    [[nodiscard]] const std::string &getSourceVar()  const { return sourceVar_;  }
    [[nodiscard]] const std::string &getEdgeVar()    const { return edgeVar_;    }
    [[nodiscard]] const std::string &getTargetVar()  const { return targetVar_;  }
    [[nodiscard]] const std::string &getEdgeLabel()  const { return edgeLabel_;  }
    [[nodiscard]] const std::string &getDirection()  const { return direction_;  }
    [[nodiscard]] int getMinHops() const { return minHops_; }
    [[nodiscard]] int getMaxHops() const { return maxHops_; }
    [[nodiscard]] const std::vector<std::string> &getTargetLabels() const { return targetLabels_; }
    [[nodiscard]] const std::vector<std::pair<std::string, graph::PropertyValue>> &getTargetProperties() const { return targetProperties_; }

private:
    std::unique_ptr<LogicalOperator> child_;
    std::string sourceVar_;
    std::string edgeVar_;
    std::string targetVar_;
    std::string edgeLabel_;
    std::string direction_;
    int minHops_;
    int maxHops_;
    std::vector<std::string> targetLabels_;
    std::vector<std::pair<std::string, graph::PropertyValue>> targetProperties_;
};

} // namespace graph::query::logical
