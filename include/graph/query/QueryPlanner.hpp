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
#include <vector>
#include "indexes/IndexManager.hpp"

namespace graph::query {

	class QueryPlan {
	public:
		enum class OperationType {
			LABEL_SCAN,
			PROPERTY_SCAN,
			LABEL_PROPERTY_SCAN,
			PROPERTY_RANGE_SCAN,
			TEXT_SEARCH,
			RELATIONSHIP_SCAN,
			NODE_FILTER,
			EDGE_FILTER,
			TRAVERSAL_CONNECTED_NODES,
			TRAVERSAL_SHORTEST_PATH,
			TRAVERSAL_BFS,
			FULL_NODE_SCAN,
			FULL_NODE_LABEL_SCAN,
			FULL_NODE_PROPERTY_SCAN,
			FULL_NODE_LABEL_PROPERTY_SCAN,
			FULL_RELATIONSHIP_SCAN
		};

		QueryPlan(OperationType type) : type_(type) {}

		// Add parameters for the operation
		void addParameter(const std::string &key, const std::string &value) { stringParams_[key] = value; }

		void addParameter(const std::string &key, double value) { doubleParams_[key] = value; }

		void addParameter(const std::string &key, uint64_t value) { uint64Params_[key] = value; }

		// Get the operation type
		[[nodiscard]] OperationType getType() const { return type_; }

		// Get parameters
		[[nodiscard]] const std::unordered_map<std::string, std::string> &getStringParams() const {
			return stringParams_;
		}

		[[nodiscard]] const std::unordered_map<std::string, double> &getDoubleParams() const { return doubleParams_; }

		[[nodiscard]] const std::unordered_map<std::string, uint64_t> &getUint64Params() const { return uint64Params_; }

	private:
		OperationType type_;
		std::unordered_map<std::string, std::string> stringParams_;
		std::unordered_map<std::string, double> doubleParams_;
		std::unordered_map<std::string, uint64_t> uint64Params_;
	};

	class QueryPlanner {
	public:
		explicit QueryPlanner(std::shared_ptr<indexes::IndexManager> indexManager);

		// Create a plan for finding nodes by label
		QueryPlan createPlanForLabelQuery(const std::string &label);

		// Create a plan for finding nodes by property
		QueryPlan createPlanForPropertyQuery(const std::string &key, const std::string &value);

		// Create a plan for finding nodes by label and property
		QueryPlan createPlanForLabelAndPropertyQuery(const std::string &label, const std::string &key,
													 const std::string &value);

		// Create a plan for finding nodes by property range
		QueryPlan createPlanForPropertyRangeQuery(const std::string &key, double minValue, double maxValue);

		// Create a plan for text search
		QueryPlan createPlanForTextSearchQuery(const std::string &key, const std::string &searchText);

		// Create a plan for finding relationships
		QueryPlan createPlanForRelationshipQuery(uint64_t nodeId, const std::string &edgeLabel);

		// Create a plan for finding connected nodes
		QueryPlan createPlanForConnectedNodesQuery(uint64_t nodeId, const std::string &nodeLabel,
												   const std::string &edgeLabel, const std::string &direction);

		QueryPlan createPlanForTraversalShortestPath(int64_t startNodeId, int64_t endNodeId, int maxDepth,
													 const std::string &direction);

	private:
		std::shared_ptr<indexes::IndexManager> indexManager_;
	};

} // namespace graph::query
