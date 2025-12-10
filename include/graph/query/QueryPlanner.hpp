/**
 * @file QueryPlanner.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <string>
#include <functional>
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/query/indexes/IndexManager.hpp"
#include "graph/query/execution/Record.hpp"

namespace graph::query {

	class QueryPlanner {
	public:
		QueryPlanner(std::shared_ptr<storage::DataManager> dm, std::shared_ptr<indexes::IndexManager> im);

		// Factory Methods

		// --- Write Operations ---

		/**
		 * @brief Generic CREATE for Nodes (Standalone Entities).
		 * Usage: CREATE (n:Label {props})
		 */
		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator> create(
			const std::string& variable,
			const std::string& label,
			const std::unordered_map<std::string, PropertyValue>& props
		) const;

		/**
		 * @brief Generic CREATE for Edges (Connected Entities).
		 * Usage: CREATE (a)-[e:Label {props}]->(b)
		 */
		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator> create(
			const std::string& variable,
			const std::string& label,
			const std::unordered_map<std::string, PropertyValue>& props,
			const std::string& sourceVar,
			const std::string& targetVar
		) const;

		// --- Read Operations ---

		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator> scan(
			const std::string& variable,
			const std::string& label
		) const;

		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator> filter(
			std::unique_ptr<execution::PhysicalOperator> child,
			std::function<bool(const execution::Record&)> predicate
		) const;

		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator> traverse(
			std::unique_ptr<execution::PhysicalOperator> source,
			const std::string& sourceVar,
			const std::string& edgeVar,
			const std::string& targetVar,
			const std::string& edgeLabel,
			const std::string& direction
		) const;

	private:
		std::shared_ptr<storage::DataManager> dataManager_;
		std::shared_ptr<indexes::IndexManager> indexManager_;
	};

}