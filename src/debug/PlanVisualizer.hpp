/**
 * @file PlanVisualizer.hpp
 * @author Nexepic
 * @date 2025/12/12
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

#include <sstream>
#include <string>
#include "graph/query/execution/PhysicalOperator.hpp"

namespace graph::debug {

	class PlanVisualizer {
	public:
		/**
		 * @brief Generates a pretty-printed tree string of the execution plan.
		 *
		 * @param root The root operator of the pipeline.
		 * @return std::string The formatted tree.
		 */
		static std::string visualize(const query::execution::PhysicalOperator *root);

	private:
		/**
		 * @brief Recursive helper to traverse and format the tree.
		 *
		 * @param op The current operator.
		 * @param prefix The current indentation string.
		 * @param isLast Whether this child is the last one in the parent's list.
		 * @param oss The output stream.
		 */
		static void printRecursive(const query::execution::PhysicalOperator *op, std::string prefix, bool isLast,
								   std::ostringstream &oss);
	};

} // namespace graph::debug
