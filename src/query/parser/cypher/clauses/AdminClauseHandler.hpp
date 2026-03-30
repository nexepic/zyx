/**
 * @file AdminClauseHandler.hpp
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
#include "generated/CypherParser.h"
#include "graph/query/logical/LogicalOperator.hpp"

namespace graph::query {
	class QueryPlanner;
}

namespace graph::parser::cypher::clauses {

/**
 * @class AdminClauseHandler
 * @brief Handles Cypher administrative clauses: SHOW INDEXES, CREATE INDEX, DROP INDEX, CREATE VECTOR INDEX.
 *
 * This class processes clauses that manage database indexes and metadata.
 */
class AdminClauseHandler {
public:
	/**
	 * @brief Handle SHOW INDEXES statement.
	 *
	 * @param ctx The show indexes statement context
	 * @param planner The query planner for creating operators
	 * @return The resulting logical operator that shows indexes
	 */
	static std::unique_ptr<query::logical::LogicalOperator> handleShowIndexes(
		CypherParser::ShowIndexesStatementContext *ctx,
		const std::shared_ptr<query::QueryPlanner> &planner);

	/**
	 * @brief Handle CREATE INDEX with pattern syntax.
	 * Syntax: CREATE INDEX [name] FOR (n:Label) ON (n.prop)
	 *
	 * @param ctx The create index by pattern context
	 * @param planner The query planner for creating operators
	 * @return The resulting logical operator that creates the index
	 */
	static std::unique_ptr<query::logical::LogicalOperator> handleCreateIndexByPattern(
		CypherParser::CreateIndexByPatternContext *ctx,
		const std::shared_ptr<query::QueryPlanner> &planner);

	/**
	 * @brief Handle CREATE INDEX with label syntax.
	 * Syntax: CREATE INDEX [name] ON :Label(prop)
	 *
	 * @param ctx The create index by label context
	 * @param planner The query planner for creating operators
	 * @return The resulting logical operator that creates the index
	 */
	static std::unique_ptr<query::logical::LogicalOperator> handleCreateIndexByLabel(
		CypherParser::CreateIndexByLabelContext *ctx,
		const std::shared_ptr<query::QueryPlanner> &planner);

	/**
	 * @brief Handle CREATE VECTOR INDEX statement.
	 *
	 * @param ctx The create vector index context
	 * @param planner The query planner for creating operators
	 * @return The resulting logical operator that creates the vector index
	 */
	static std::unique_ptr<query::logical::LogicalOperator> handleCreateVectorIndex(
		CypherParser::CreateVectorIndexContext *ctx,
		const std::shared_ptr<query::QueryPlanner> &planner);

	/**
	 * @brief Handle DROP INDEX by name.
	 * Syntax: DROP INDEX name
	 *
	 * @param ctx The drop index by name context
	 * @param planner The query planner for creating operators
	 * @return The resulting logical operator that drops the index
	 */
	static std::unique_ptr<query::logical::LogicalOperator> handleDropIndexByName(
		CypherParser::DropIndexByNameContext *ctx,
		const std::shared_ptr<query::QueryPlanner> &planner);

	/**
	 * @brief Handle DROP INDEX by label.
	 * Syntax: DROP INDEX ON :Label(prop)
	 *
	 * @param ctx The drop index by label context
	 * @param planner The query planner for creating operators
	 * @return The resulting logical operator that drops the index
	 */
	static std::unique_ptr<query::logical::LogicalOperator> handleDropIndexByLabel(
		CypherParser::DropIndexByLabelContext *ctx,
		const std::shared_ptr<query::QueryPlanner> &planner);

	// --- Constraint DDL ---

	static std::unique_ptr<query::logical::LogicalOperator> handleCreateNodeConstraint(
		CypherParser::CreateNodeConstraintContext *ctx,
		const std::shared_ptr<query::QueryPlanner> &planner);

	static std::unique_ptr<query::logical::LogicalOperator> handleCreateEdgeConstraint(
		CypherParser::CreateEdgeConstraintContext *ctx,
		const std::shared_ptr<query::QueryPlanner> &planner);

	static std::unique_ptr<query::logical::LogicalOperator> handleDropConstraint(
		const std::string &name, bool ifExists,
		const std::shared_ptr<query::QueryPlanner> &planner);

	static std::unique_ptr<query::logical::LogicalOperator> handleShowConstraints(
		const std::shared_ptr<query::QueryPlanner> &planner);
};

} // namespace graph::parser::cypher::clauses
