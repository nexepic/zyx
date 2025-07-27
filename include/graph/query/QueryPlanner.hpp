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

		explicit QueryPlan(OperationType type) : type_(type) {}

		// Add parameters for the operation
		void addParameter(const std::string &key, const std::string &value) { stringParams_[key] = value; }

		void addParameter(const std::string &key, const double value) { doubleParams_[key] = value; }

		void addParameter(const std::string &key, const int64_t value) { int64Params_[key] = value; }

		// Get the operation type
		[[nodiscard]] OperationType getType() const { return type_; }

		// Get parameters
		[[nodiscard]] const std::unordered_map<std::string, std::string> &getStringParams() const {
			return stringParams_;
		}

		[[nodiscard]] const std::unordered_map<std::string, double> &getDoubleParams() const { return doubleParams_; }

		[[nodiscard]] const std::unordered_map<std::string, int64_t> &getInt64Params() const { return int64Params_; }

	private:
		OperationType type_;
		std::unordered_map<std::string, std::string> stringParams_;
		std::unordered_map<std::string, double> doubleParams_;
		std::unordered_map<std::string, int64_t> int64Params_;
	};

	class QueryPlanner {
	public:
		explicit QueryPlanner(std::shared_ptr<indexes::IndexManager> indexManager);

		// Create a plan for finding nodes by label
		[[nodiscard]] QueryPlan createPlanForLabelQuery(const std::string &label) const;

		// Create a plan for finding nodes by property
		[[nodiscard]] QueryPlan createPlanForPropertyQuery(const std::string &key, const std::string &value) const;

		// Create a plan for finding nodes by label and property
		[[nodiscard]] QueryPlan createPlanForLabelAndPropertyQuery(const std::string &label, const std::string &key,
													 const std::string &value) const;

		// Create a plan for finding nodes by property range
		static QueryPlan createPlanForPropertyRangeQuery(const std::string &key, double minValue, double maxValue);

		// Create a plan for text search
		static QueryPlan createPlanForTextSearchQuery(const std::string &key, const std::string &searchText);

		// Create a plan for finding relationships
		static QueryPlan createPlanForRelationshipQuery(int64_t nodeId, const std::string &edgeLabel);

		// Create a plan for finding connected nodes
		static QueryPlan createPlanForConnectedNodesQuery(int64_t nodeId, const std::string &nodeLabel,
														  const std::string &edgeLabel, const std::string &direction);

		static QueryPlan createPlanForTraversalShortestPath(int64_t startNodeId, int64_t endNodeId, int maxDepth,
													 const std::string &direction);

	private:
		std::shared_ptr<indexes::IndexManager> indexManager_;
	};

} // namespace graph::query
