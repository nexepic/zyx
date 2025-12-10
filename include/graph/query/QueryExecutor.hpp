/**
 * @file QueryExecutor.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "QueryResult.hpp"
#include "execution/PhysicalOperator.hpp"
#include <memory>

namespace graph::query {

	class QueryExecutor {
	public:
		QueryExecutor() = default;
		~QueryExecutor() = default;

		/**
		 * @brief Executes a physical plan tree.
		 * @param plan The root of the operator tree.
		 * @return QueryResult containing the aggregated data.
		 */
		QueryResult execute(std::unique_ptr<execution::PhysicalOperator> plan) const;
	};

} // namespace graph::query