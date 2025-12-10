/**
 * @file QueryPlanner.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/QueryPlanner.hpp"

#include "graph/query/execution/operators/CreateEdgeOperator.hpp"
#include "graph/query/execution/operators/CreateNodeOperator.hpp"
#include "graph/query/execution/operators/FilterOperator.hpp"
#include "graph/query/execution/operators/NodeScanOperator.hpp"

namespace graph::query {

	QueryPlanner::QueryPlanner(std::shared_ptr<storage::DataManager> dm, std::shared_ptr<indexes::IndexManager> im)
		: dataManager_(std::move(dm)), indexManager_(std::move(im)) {}

	// Implementation for Node Creation
	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::create(
		const std::string& variable,
		const std::string& label,
		const std::unordered_map<std::string, PropertyValue>& props
	) const {
		// Factory logic: return specific Node operator
		return std::make_unique<execution::operators::CreateNodeOperator>(
			dataManager_, variable, label, props
		);
	}

	// Implementation for Edge Creation
	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::create(
		const std::string& variable,
		const std::string& label,
		const std::unordered_map<std::string, PropertyValue>& props,
		const std::string& sourceVar,
		const std::string& targetVar
	) const {
		// Factory logic: return specific Edge operator
		return std::make_unique<execution::operators::CreateEdgeOperator>(
			dataManager_, variable, label, props, sourceVar, targetVar
		);
	}

	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::scan(const std::string& variable, const std::string& label) const {
		return std::make_unique<execution::operators::NodeScanOperator>(
			dataManager_, indexManager_, variable, label
		);
	}

	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::filter(
		std::unique_ptr<execution::PhysicalOperator> child,
		std::function<bool(const execution::Record&)> predicate) const {

		return std::make_unique<execution::operators::FilterOperator>(
			std::move(child), std::move(predicate)
		);
	}

	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::traverse(
		std::unique_ptr<execution::PhysicalOperator> source,
		const std::string& sourceVar, const std::string& edgeVar, const std::string& targetVar,
		const std::string& edgeLabel, const std::string& direction) const {

		// return std::make_unique<execution::operators::TraversalOperator>(...);
		return nullptr; // Placeholder until you implement TraversalOperator
	}
}