#pragma once
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"

namespace graph::query::optimization::detail {
// Reordering changes enumeration order. Windows/optional/correlated or unknown
// operators and unproven expressions are barriers, even when costs are known.
[[nodiscard]] inline bool readJoinOrderIndependentExpression(const expressions::Expression *expression,
    size_t depth = 0) {
    if (!expression || depth >= 64U) return false;
    if (dynamic_cast<const expressions::LiteralExpression *>(expression) ||
        dynamic_cast<const expressions::VariableReferenceExpression *>(expression)) return true;
    if (const auto *binary = dynamic_cast<const expressions::BinaryOpExpression *>(expression))
        return readJoinOrderIndependentExpression(binary->getLeft(), depth + 1) &&
            readJoinOrderIndependentExpression(binary->getRight(), depth + 1);
    if (const auto *unary = dynamic_cast<const expressions::UnaryOpExpression *>(expression))
        return readJoinOrderIndependentExpression(unary->getOperand(), depth + 1);
    if (const auto *function = dynamic_cast<const expressions::FunctionCallExpression *>(expression)) {
        const auto &name = function->getFunctionName();
        // id() consumes an already-bound logical identity; arbitrary/volatile
        // functions must not gain a different row-to-invocation association.
        return name.size() == 2 && (name[0] == 'i' || name[0] == 'I') &&
            (name[1] == 'd' || name[1] == 'D') && !function->isDistinct() && function->getArgumentCount() == 1 &&
            readJoinOrderIndependentExpression(function->getArguments().front().get(), depth + 1);
    }
    return false;
}
[[nodiscard]] inline bool readJoinReorderEnvelope(const logical::LogicalOperator &root) {
    if (root.getType() == logical::LogicalOpType::LOP_PROFILE || root.getType() == logical::LogicalOpType::LOP_EXPLAIN ||
        root.getType() == logical::LogicalOpType::LOP_JOIN || root.getType() == logical::LogicalOpType::LOP_NODE_SCAN ||
        root.getType() == logical::LogicalOpType::LOP_TRAVERSAL) return true;
    if (const auto *project = dynamic_cast<const logical::LogicalProject *>(&root)) {
        if (project->isDistinct()) return false;
        for (const auto &item : project->getItems())
            if (!readJoinOrderIndependentExpression(item.expression.get())) return false;
        return true;
    }
    if (const auto *filter = dynamic_cast<const logical::LogicalFilter *>(&root))
        return readJoinOrderIndependentExpression(filter->getPredicateRaw());
    if (const auto *sort = dynamic_cast<const logical::LogicalSort *>(&root)) {
        for (const auto &item : sort->getSortItems())
            if (!readJoinOrderIndependentExpression(item.expression.get())) return false;
        return true;
    }
    return false;
}
}
