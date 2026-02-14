/**
 * @file WritingClauseHandler.hpp
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
#include <vector>
#include "generated/CypherParser.h"
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/execution/operators/SetOperator.hpp"

namespace graph::query {
	class QueryPlanner;
}

namespace graph::parser::cypher::clauses {

/**
 * @class WritingClauseHandler
 * @brief Handles Cypher writing clauses: CREATE, SET, DELETE, REMOVE, MERGE.
 *
 * This class processes clauses that modify data in the database.
 */
class WritingClauseHandler {
public:
	/**
	 * @brief Handle a CREATE statement.
	 *
	 * @param ctx The create statement context
	 * @param rootOp The current root operator (may be null)
	 * @param planner The query planner for creating operators
	 * @return The resulting operator after processing CREATE
	 */
	static std::unique_ptr<query::execution::PhysicalOperator> handleCreate(
		CypherParser::CreateStatementContext *ctx,
		std::unique_ptr<query::execution::PhysicalOperator> rootOp,
		const std::shared_ptr<query::QueryPlanner> &planner);

	/**
	 * @brief Handle a SET statement.
	 *
	 * @param ctx The set statement context
	 * @param rootOp The current root operator (must not be null)
	 * @param planner The query planner for creating operators
	 * @return The resulting operator after processing SET
	 * @throws std::runtime_error if rootOp is null
	 */
	static std::unique_ptr<query::execution::PhysicalOperator> handleSet(
		CypherParser::SetStatementContext *ctx,
		std::unique_ptr<query::execution::PhysicalOperator> rootOp,
		const std::shared_ptr<query::QueryPlanner> &planner);

	/**
	 * @brief Handle a DELETE statement.
	 *
	 * @param ctx The delete statement context
	 * @param rootOp The current root operator (must not be null)
	 * @param planner The query planner for creating operators
	 * @return The resulting operator after processing DELETE
	 * @throws std::runtime_error if rootOp is null
	 */
	static std::unique_ptr<query::execution::PhysicalOperator> handleDelete(
		CypherParser::DeleteStatementContext *ctx,
		std::unique_ptr<query::execution::PhysicalOperator> rootOp,
		const std::shared_ptr<query::QueryPlanner> &planner);

	/**
	 * @brief Handle a REMOVE statement.
	 *
	 * @param ctx The remove statement context
	 * @param rootOp The current root operator (must not be null)
	 * @param planner The query planner for creating operators
	 * @return The resulting operator after processing REMOVE
	 * @throws std::runtime_error if rootOp is null
	 */
	static std::unique_ptr<query::execution::PhysicalOperator> handleRemove(
		CypherParser::RemoveStatementContext *ctx,
		std::unique_ptr<query::execution::PhysicalOperator> rootOp,
		const std::shared_ptr<query::QueryPlanner> &planner);

	/**
	 * @brief Handle a MERGE statement.
	 *
	 * @param ctx The merge statement context
	 * @param rootOp The current root operator (may be null)
	 * @param planner The query planner for creating operators
	 * @return The resulting operator after processing MERGE
	 */
	static std::unique_ptr<query::execution::PhysicalOperator> handleMerge(
		CypherParser::MergeStatementContext *ctx,
		std::unique_ptr<query::execution::PhysicalOperator> rootOp,
		const std::shared_ptr<query::QueryPlanner> &planner);
};

} // namespace graph::parser::cypher::clauses
