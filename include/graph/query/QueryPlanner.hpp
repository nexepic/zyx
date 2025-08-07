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
			// --- Node Operations ---
			NODE_LABEL_SCAN,            // Use index to find nodes by label
			NODE_PROPERTY_SCAN,         // Use index to find nodes by property
			NODE_LABEL_PROPERTY_SCAN,   // Use indexes to find nodes by label and property

			// --- Edge Operations ---
			EDGE_LABEL_SCAN,            // Use index to find edges by label
			EDGE_PROPERTY_SCAN,         // Use index to find edges by property
			// Note: EDGE_LABEL_PROPERTY_SCAN can be added if needed

			// --- Full Scan Operations (Fallbacks) ---
			FULL_NODE_LABEL_SCAN,
			FULL_NODE_PROPERTY_SCAN,
			FULL_NODE_LABEL_PROPERTY_SCAN,
			FULL_EDGE_LABEL_SCAN,       // Can be added if needed
			FULL_EDGE_PROPERTY_SCAN,    // Can be added if needed

			// --- Traversal Operations (if supported without old indexes) ---
			TRAVERSAL_CONNECTED_NODES,
			TRAVERSAL_SHORTEST_PATH,
			TRAVERSAL_BFS,
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

		// --- Node Query Planners ---
		[[nodiscard]] QueryPlan createPlanForNodeLabelQuery(const std::string& label) const;
		[[nodiscard]] QueryPlan createPlanForNodePropertyQuery(const std::string& key, const std::string& value) const;
		[[nodiscard]] QueryPlan createPlanForNodeLabelAndPropertyQuery(const std::string& label, const std::string& key, const std::string& value) const;

		// --- Edge Query Planners ---
		[[nodiscard]] QueryPlan createPlanForEdgeLabelQuery(const std::string& label) const;
		[[nodiscard]] QueryPlan createPlanForEdgePropertyQuery(const std::string& key, const std::string& value) const;

		// Create a plan for finding connected nodes
		static QueryPlan createPlanForConnectedNodesQuery(int64_t nodeId, const std::string &nodeLabel,
														  const std::string &edgeLabel, const std::string &direction);

		static QueryPlan createPlanForTraversalShortestPath(int64_t startNodeId, int64_t endNodeId, int maxDepth,
													 const std::string &direction);

	private:
		std::shared_ptr<indexes::IndexManager> indexManager_;
	};

} // namespace graph::query
