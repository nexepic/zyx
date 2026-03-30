/**
 * @file Optimizer.cpp
 * @brief Registers the default set of optimization rules for the query optimizer.
 *
 * Rule application order is significant:
 *  1. PredicateSimplificationRule  — simplify and merge filters before moving them.
 *  2. FilterPushdownRule           — push filters toward data sources.
 *  3. ProjectionPushdownRule       — annotate projects with required-column sets.
 *  4. EnhancedIndexSelectionRule   — cost-based scan strategy selection.
 *  5. JoinReorderRule              — reorder joins by estimated cardinality.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "graph/query/optimizer/Optimizer.hpp"
#include "graph/query/optimizer/rules/EnhancedIndexSelectionRule.hpp"
#include "graph/query/optimizer/rules/FilterPushdownRule.hpp"
#include "graph/query/optimizer/rules/JoinReorderRule.hpp"
#include "graph/query/optimizer/rules/PredicateSimplificationRule.hpp"
#include "graph/query/optimizer/rules/ProjectionPushdownRule.hpp"

namespace graph::query::optimizer {

void Optimizer::registerDefaultRules() {
    // Simplify predicates first so that subsequent rules see a canonical form.
    rules_.push_back(std::make_unique<rules::PredicateSimplificationRule>());

    // Push filters as close to the data as possible.
    rules_.push_back(std::make_unique<rules::FilterPushdownRule>());

    // Annotate projections with required-column sets.
    rules_.push_back(std::make_unique<rules::ProjectionPushdownRule>());

    // Use cost model to choose the best scan strategy for each NodeScan.
    rules_.push_back(
        std::make_unique<rules::EnhancedIndexSelectionRule>(indexManager_));

    // Reorder joins so that smaller relations are joined first.
    rules_.push_back(std::make_unique<rules::JoinReorderRule>());
}

} // namespace graph::query::optimizer
