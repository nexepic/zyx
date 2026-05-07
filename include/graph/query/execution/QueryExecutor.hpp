/**
 * @file QueryExecutor.hpp
 * @author Nexepic
 * @date 2025/3/20
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
#include "PhysicalOperator.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace graph::query {

	class QueryExecutor {
	public:
		QueryExecutor() = default;
		~QueryExecutor() = default;

		/**
		 * @brief Executes a physical plan tree.
		 * @param plan The root of the operator tree.
		 * @return QueryResult containing the aggregated data.
		 *
		 * Internal anonymous variables (__anon_*) are filtered from the
		 * output columns as they are pipeline-internal implementation details.
		 */
		static QueryResult execute(std::unique_ptr<execution::PhysicalOperator> plan);
	};

} // namespace graph::query
