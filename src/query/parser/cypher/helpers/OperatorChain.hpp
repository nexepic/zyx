/**
 * @file OperatorChain.hpp
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
#include "graph/query/logical/LogicalOperator.hpp"

namespace graph::parser::cypher::helpers {

/**
 * @class OperatorChain
 * @brief Utility class for managing logical operator chaining in query plans.
 *
 * This class handles the logic of properly chaining logical operators together:
 * - Write operators (LogicalCreateEdge, LogicalCreateNode) wrap the current pipeline
 *   by setting the existing root as their child
 * - Other operators replace the current pipeline
 *
 * This simplifies the common pattern of building operator chains during query parsing.
 */
class OperatorChain {
public:
	/**
	 * @brief Chain a new logical operator with the current root operator.
	 *
	 * @param rootOp The current root operator (may be null)
	 * @param newOp The new operator to chain
	 * @return The updated root operator after chaining
	 */
	static std::unique_ptr<query::logical::LogicalOperator> chain(
		std::unique_ptr<query::logical::LogicalOperator> rootOp,
		std::unique_ptr<query::logical::LogicalOperator> newOp);
};

} // namespace graph::parser::cypher::helpers
