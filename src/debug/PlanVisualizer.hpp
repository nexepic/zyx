/**
 * @file PlanVisualizer.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/12
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <string>
#include <sstream>
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
		static std::string visualize(const query::execution::PhysicalOperator* root);

	private:
		/**
		 * @brief Recursive helper to traverse and format the tree.
		 *
		 * @param op The current operator.
		 * @param prefix The current indentation string.
		 * @param isLast Whether this child is the last one in the parent's list.
		 * @param oss The output stream.
		 */
		static void printRecursive(const query::execution::PhysicalOperator* op,
								   std::string prefix,
								   bool isLast,
								   std::ostringstream& oss);
	};

} // namespace graph::debug