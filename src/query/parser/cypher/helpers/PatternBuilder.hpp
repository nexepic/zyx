/**
 * @file PatternBuilder.hpp
 * @author Nexepic
 * @date 2025/12/9
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "generated/CypherParser.h"
#include "graph/core/PropertyTypes.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/execution/operators/SetOperator.hpp"  // for SetItem (used by WritingClauseHandler)

namespace graph::query {
	class QueryPlanner;
}

namespace graph::parser::cypher::helpers {

/**
 * @class PatternBuilder
 * @brief Builds logical operators for MATCH, CREATE, and MERGE patterns.
 *
 * This class handles the complex logic of translating Cypher pattern syntax
 * into logical operators. It supports:
 * - Node patterns with labels and properties
 * - Relationship patterns with direction, type, and properties
 * - Pattern chains (traversal)
 * - Variable-length relationships
 * - MERGE with ON MATCH and ON CREATE actions
 */
class PatternBuilder {
public:
	/**
	 * @brief Build logical operators for a MATCH pattern.
	 *
	 * Creates LogicalNodeScan and LogicalTraversal operators based on the pattern.
	 * Handles multiple pattern parts (e.g., MATCH (a), (b)).
	 *
	 * @param pattern The pattern context from MATCH
	 * @param rootOp The current root operator (may be null)
	 * @param planner The query planner for creating operators
	 * @param where Optional WHERE clause context
	 * @param isOptional Whether this is an OPTIONAL MATCH (left outer join semantics)
	 * @return The resulting logical operator after processing the pattern
	 */
	static std::unique_ptr<query::logical::LogicalOperator> buildMatchPattern(
		CypherParser::PatternContext *pattern,
		std::unique_ptr<query::logical::LogicalOperator> rootOp,
		const std::shared_ptr<query::QueryPlanner> &planner,
		CypherParser::WhereContext *where = nullptr,
		bool isOptional = false);

	/**
	 * @brief Build logical operators for a CREATE pattern.
	 *
	 * Creates LogicalCreateNode and LogicalCreateEdge instances.
	 *
	 * @param pattern The pattern context from CREATE
	 * @param rootOp The current root operator (may be null)
	 * @param planner The query planner for creating operators
	 * @return The resulting logical operator after processing the pattern
	 */
	static std::unique_ptr<query::logical::LogicalOperator> buildCreatePattern(
		CypherParser::PatternContext *pattern,
		std::unique_ptr<query::logical::LogicalOperator> rootOp,
		const std::shared_ptr<query::QueryPlanner> &planner);

	/**
	 * @brief Build a logical operator for a MERGE pattern.
	 *
	 * Currently supports single node MERGE only.
	 *
	 * @param patternPart The pattern part from MERGE
	 * @param onCreateItems SET items for ON CREATE clause
	 * @param onMatchItems SET items for ON MATCH clause
	 * @param planner The query planner for creating operators
	 * @return The resulting logical merge operator
	 */
	static std::unique_ptr<query::logical::LogicalOperator> buildMergePattern(
		CypherParser::PatternPartContext *patternPart,
		const std::vector<query::execution::operators::SetItem> &onCreateItems,
		const std::vector<query::execution::operators::SetItem> &onMatchItems,
		const std::shared_ptr<query::QueryPlanner> &planner);

	/**
	 * @brief Extract SET items from a SetStatementContext.
	 *
	 * @param ctx The set statement context
	 * @return Vector of SetItem structures
	 */
	static std::vector<query::execution::operators::SetItem> extractSetItems(
		CypherParser::SetStatementContext *ctx);

private:
	/**
	 * @brief Process a single pattern element for MATCH.
	 */
	static std::unique_ptr<query::logical::LogicalOperator> processMatchPatternElement(
		CypherParser::PatternElementContext *element,
		std::unique_ptr<query::logical::LogicalOperator> rootOp);

	/**
	 * @brief Process a single pattern element for CREATE.
	 */
	static void processCreatePatternElement(
		CypherParser::PatternElementContext *element,
		std::unique_ptr<query::logical::LogicalOperator> &rootOp);

	/**
	 * @brief Apply WHERE clause filter to a logical operator.
	 */
	static std::unique_ptr<query::logical::LogicalOperator> applyWhereFilter(
		std::unique_ptr<query::logical::LogicalOperator> rootOp,
		CypherParser::WhereContext *where);

	/**
	 * @brief Collect all variable names introduced by a pattern element.
	 *
	 * @param element The pattern element to analyze
	 * @param variables Output vector to collect variable names
	 */
	static void collectVariablesFromPatternElement(
		CypherParser::PatternElementContext *element,
		std::vector<std::string> &variables);
};

} // namespace graph::parser::cypher::helpers
