/**
 * @file WithClauseHandler.hpp
 * @author ZYX Contributors
 * @date 2025
 *
 * @copyright Copyright (c) 2025
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
#include "generated/CypherParser.h"
#include "graph/query/logical/LogicalOperator.hpp"

namespace graph::query {
	class QueryPlanner;
}

namespace graph::parser::cypher::clauses {

/**
 * @class WithClauseHandler
 * @brief Handles the WITH clause in Cypher queries.
 *
 * The WITH clause allows:
 * 1. Projecting intermediate results (like RETURN)
 * 2. Continuing the query (unlike RETURN which terminates)
 * 3. Filtering with WHERE clause
 *
 * Example:
 *   MATCH (n) WITH n WHERE n.age > 18 RETURN n
 *   MATCH (n)-[r]->(m) WITH n, m RETURN n, m
 *
 * The WITH clause is similar to RETURN but:
 * - It doesn't terminate the query
 * - Results are projected for use in subsequent clauses
 * - It can have a WHERE clause for filtering
 */
class WithClauseHandler {
public:
	/**
	 * @brief Handle a WITH clause in the query.
	 *
	 * @param ctx The WITH clause context from the parser
	 * @param rootOp The current root operator in the pipeline
	 * @param planner The query planner for creating operators
	 * @return The resulting logical operator after processing WITH
	 */
	static std::unique_ptr<query::logical::LogicalOperator> handleWith(
		CypherParser::WithClauseContext *ctx,
		std::unique_ptr<query::logical::LogicalOperator> rootOp,
		const std::shared_ptr<query::QueryPlanner> &planner);
};

} // namespace graph::parser::cypher::clauses
