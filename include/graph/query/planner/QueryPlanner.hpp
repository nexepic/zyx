/**
 * @file QueryPlanner.hpp
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

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/operators/ProjectOperator.hpp"
#include "graph/query/execution/operators/RemoveOperator.hpp"
#include "graph/query/execution/operators/SetOperator.hpp"
#include "graph/query/execution/operators/SortOperator.hpp"

// Forward declarations to reduce compile-time dependencies
namespace graph::storage {
	class DataManager;
}
namespace graph::query::indexes {
	class IndexManager;
}
namespace graph::query::optimizer {
	class Optimizer;
}
namespace graph::query::execution {
	class PhysicalOperator;
	class Record;
} // namespace graph::query::execution

namespace graph::query {

	class QueryPlanner {
	public:
		/**
		 * @brief Constructs the planner and initializes the internal Optimizer.
		 */
		QueryPlanner(std::shared_ptr<storage::DataManager> dm, const std::shared_ptr<indexes::IndexManager> &im);

		~QueryPlanner();

		// =================================================================
		// Read Operations (Factories)
		// =================================================================

		/**
		 * @brief Creates a Scan operator.
		 *        The internal Optimizer determines if an IndexScan or FullScan is used.
		 *        If IndexScan is not possible, the Planner appends a FilterOperator automatically.
		 *
		 * @param variable The variable name (e.g., "n").
		 * @param label The label filter (e.g., "User").
		 * @param key Optional property key for index pushdown.
		 * @param value Optional property value for index pushdown.
		 */
		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator>
		scanOp(const std::string &variable, const std::string &label, const std::string &key = "",
			   const PropertyValue &value = PropertyValue()) const;

		/**
		 * @brief Wraps an operator with a generic predicate filter.
		 *
		 * @param child
		 * @param predicate
		 * @param description A string describing the logic (e.g., "n.age > 10") for debugging.
		 */
		[[nodiscard]] static std::unique_ptr<execution::PhysicalOperator>
		filterOp(std::unique_ptr<execution::PhysicalOperator> child,
				 std::function<bool(const execution::Record &)> predicate, const std::string &description);

		/**
		 * @brief Expands a node into connected edges and target nodes.
		 */
		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator>
		traverseOp(std::unique_ptr<execution::PhysicalOperator> source, const std::string &sourceVar,
				   const std::string &edgeVar, const std::string &targetVar, const std::string &edgeLabel,
				   const std::string &direction) const;

		/**
		 * @brief Projects specific variables from the stream (SELECT/RETURN).
		 */
		[[nodiscard]] static std::unique_ptr<execution::PhysicalOperator>
		projectOp(std::unique_ptr<execution::PhysicalOperator> child,
				  const std::vector<execution::operators::ProjectItem> &items);

		// =================================================================
		// Write Operations (Factories)
		// =================================================================

		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator>
		callProcedureOp(const std::string &procedure, const std::vector<PropertyValue> &args) const;

		// Create Node
		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator>
		createOp(const std::string &variable, const std::string &label,
				 const std::unordered_map<std::string, PropertyValue> &props) const;

		// Create Edge
		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator>
		createOp(const std::string &variable, const std::string &label,
				 const std::unordered_map<std::string, PropertyValue> &props, const std::string &sourceVar,
				 const std::string &targetVar) const;

		// Create Index (DDL)
		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator>
		createIndexOp(const std::string &indexName, const std::string &label, const std::string &propertyKey) const;

		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator>
		mergeOp(const std::string &variable, const std::string &label,
				const std::unordered_map<std::string, PropertyValue> &matchProps,
				const std::vector<execution::operators::SetItem> &onCreateItems,
				const std::vector<execution::operators::SetItem> &onMatchItems) const;

		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator> dropIndexOp(const std::string &indexName) const;

		// Drop Index By Definition (Legacy/Implicit name)
		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator> dropIndexOp(const std::string &label,
																			   const std::string &propertyKey) const;

		// --- Administration ---
		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator> showIndexesOp() const;

		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator>
		deleteOp(std::unique_ptr<execution::PhysicalOperator> child, const std::vector<std::string> &variables,
				 bool detach) const;

		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator>
		setOp(std::unique_ptr<execution::PhysicalOperator> child,
			  const std::vector<execution::operators::SetItem> &items) const;

		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator>
		removeOp(std::unique_ptr<execution::PhysicalOperator> child,
				 const std::vector<execution::operators::RemoveItem> &items) const;

		// Result Set Control
		[[nodiscard]] static std::unique_ptr<execution::PhysicalOperator>
		limitOp(std::unique_ptr<execution::PhysicalOperator> child, int64_t limit);

		[[nodiscard]] static std::unique_ptr<execution::PhysicalOperator>
		skipOp(std::unique_ptr<execution::PhysicalOperator> child, int64_t offset);

		// Sort
		[[nodiscard]] static std::unique_ptr<execution::PhysicalOperator>
		sortOp(std::unique_ptr<execution::PhysicalOperator> child,
			   const std::vector<execution::operators::SortItem> &items);

		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator>
		traverseVarLengthOp(std::unique_ptr<execution::PhysicalOperator> source, const std::string &sourceVar,
							const std::string &targetVar, const std::string &edgeLabel, int minHops, int maxHops,
							const std::string &direction) const;

		static std::unique_ptr<execution::PhysicalOperator>
		cartesianProductOp(std::unique_ptr<execution::PhysicalOperator> left,
						   std::unique_ptr<execution::PhysicalOperator> right);

		[[nodiscard]] static std::unique_ptr<execution::PhysicalOperator>
		unwindOp(std::unique_ptr<execution::PhysicalOperator> child, const std::string &alias,
				 const std::vector<PropertyValue> &list);

		// Helper for standalone queries (RETURN 1)
		[[nodiscard]] static std::unique_ptr<execution::PhysicalOperator> singleRowOp();

		[[nodiscard]] std::shared_ptr<storage::DataManager> getDataManager() const { return dm_; }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;

		// The Brain: Handles logic for selecting the best scan strategy
		std::unique_ptr<optimizer::Optimizer> optimizer_;
	};

} // namespace graph::query
