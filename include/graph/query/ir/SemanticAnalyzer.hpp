/**
 * @file SemanticAnalyzer.hpp
 * @brief Semantic analysis phase for QueryAST.
 *
 * Annotates ProjectionBody with aggregate classification and resolves
 * ORDER BY aliases. Runs after CypherASTBuilder, before LogicalPlanBuilder.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/ir/QueryAST.hpp"

namespace graph::query::ir {

/**
 * @class SemanticAnalyzer
 * @brief Analyzes and annotates ProjectionBody structures.
 *
 * - Classifies each projection item as aggregate/non-aggregate
 * - Sets the body-level hasAggregates flag
 * - Resolves ORDER BY alias references for non-aggregate queries
 */
class SemanticAnalyzer {
public:
	/**
	 * @brief Analyze a projection body (used by both RETURN and WITH).
	 *
	 * After this call:
	 * - body.hasAggregates is set
	 * - Each item's containsAggregate and isTopLevelAggregate are set
	 * - For non-aggregate queries, ORDER BY expressions referencing aliases
	 *   are expanded to the original expressions
	 */
	static void analyzeProjection(ProjectionBody& body);
};

} // namespace graph::query::ir
