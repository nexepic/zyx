/**
 * @file ReadTraversalAnchorEnvelope.hpp
 * @brief Shared semantic boundary for anchor costing and statistics discovery.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"

namespace graph::query::optimization::detail {
// Reversal changes enumeration order. Only proven transparent output layers
// may carry the current rule or its statistics requests into child plans.
[[nodiscard]] inline bool readTraversalAnchorEnvelope(const logical::LogicalOperator &root) {
    if (root.getType() == logical::LogicalOpType::LOP_PROFILE ||
        root.getType() == logical::LogicalOpType::LOP_EXPLAIN) return true;
    const auto *project = dynamic_cast<const logical::LogicalProject *>(&root);
    if (!project || project->isDistinct()) return false;
    for (const auto &item : project->getItems()) {
        if (const auto *variable = dynamic_cast<const expressions::VariableReferenceExpression *>(item.expression.get())) {
            if (!variable->hasProperty()) continue;
        } else if (dynamic_cast<const expressions::LiteralExpression *>(item.expression.get())) continue;
        return false;
    }
    return true;
}
} // namespace graph::query::optimization::detail
