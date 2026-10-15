/**
 * @file LogicalTraversal.hpp
 * @brief Logical operator for single-hop edge traversal.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once
#include "graph/core/OwnedPropertyMap.hpp"
#include "graph/core/OwnedPropertyPredicates.hpp"
#include "graph/utils/OwnedStringVector.hpp"
#include "graph/utils/OwnedStdString.hpp"

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/core/PropertyTypes.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace graph::query::logical {

/**
 * @class LogicalTraversal
 * @brief Traverses one hop from a source variable along edges of an optional
 *        label and direction, binding the edge and target to new variables.
 *
 * Stores optional target label/property filters and edge property filters so
 * that the physical plan converter can generate the appropriate predicates.
 */
class LogicalTraversal : public LogicalOperator {
public:
    LogicalTraversal(std::unique_ptr<LogicalOperator> child,
                     utils::OwnedStdString sourceVar,
                     utils::OwnedStdString edgeVar,
                     utils::OwnedStdString targetVar,
                     utils::OwnedStdString edgeType,
                     utils::OwnedStdString direction,
                     utils::OwnedStringVector targetLabels = {},
                     OwnedPropertyPredicates targetProperties = {},
                     graph::OwnedPropertyMap edgeProperties = {})
        : child_(std::move(child))
        , sourceVar_(std::move(sourceVar))
        , edgeVar_(std::move(edgeVar))
        , targetVar_(std::move(targetVar))
        , edgeType_(std::move(edgeType))
        , direction_(std::move(direction))
        , targetLabels_(std::move(targetLabels))
        , targetProperties_(std::move(targetProperties))
        , edgeProperties_(std::move(edgeProperties)) {}

    // -------------------------------------------------------------------------
    // LogicalOperator interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOpType getType() const override {
        return LogicalOpType::LOP_TRAVERSAL;
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

    [[nodiscard]] utils::OwnedStringVector getOwnedOutputVariables() const override {
        utils::OwnedStringVector vars;
        if (child_) {
            vars = child_->getOwnedOutputVariables();
        }
        if (!edgeVar_.empty())   vars.push_back(edgeVar_);
        if (!targetVar_.empty()) vars.push_back(targetVar_);
        return vars;
    }

    [[nodiscard]] std::string toString() const override {
        return "Traversal(" + sourceVar_ + "-[" + edgeVar_ + ":" + edgeType_ + "]->" + targetVar_ + ")";
    }

    [[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
        return std::make_unique<LogicalTraversal>(
            child_ ? child_->clone() : nullptr,
            sourceVar_, edgeVar_, targetVar_, edgeType_, direction_,
            targetLabels_, targetProperties_, edgeProperties_);
    }

    void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
        if (index != 0) throw std::out_of_range("LogicalTraversal has only one child (index 0)");
        child_ = std::move(child);
    }

    std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
        if (index != 0) throw std::out_of_range("LogicalTraversal has only one child (index 0)");
        return std::move(child_);
    }

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    [[nodiscard]] const std::string &getSourceVar()  const { return sourceVar_;  }
    [[nodiscard]] const std::string &getEdgeVar()    const { return edgeVar_;    }
    [[nodiscard]] const std::string &getTargetVar()  const { return targetVar_;  }
    [[nodiscard]] const std::string &getEdgeType()  const { return edgeType_;  }
    [[nodiscard]] const std::string &getDirection()  const { return direction_;  }
    [[nodiscard]] const std::vector<std::string> &getTargetLabels() const { return targetLabels_; }
    [[nodiscard]] const std::vector<std::pair<std::string, graph::PropertyValue>> &getTargetProperties() const { return targetProperties_; }
    [[nodiscard]] const std::unordered_map<std::string, graph::PropertyValue> &getEdgeProperties() const { return edgeProperties_; }

private:
    std::unique_ptr<LogicalOperator> child_;
    utils::OwnedStdString sourceVar_;
    utils::OwnedStdString edgeVar_;
    utils::OwnedStdString targetVar_;
    utils::OwnedStdString edgeType_;
    utils::OwnedStdString direction_;
    utils::OwnedStringVector targetLabels_;
    OwnedPropertyPredicates targetProperties_;
    graph::OwnedPropertyMap edgeProperties_;
};

} // namespace graph::query::logical
