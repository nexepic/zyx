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

#include <map>
#include <memory>
#include "indexes/IndexManager.hpp"

namespace graph::query {

	class QueryPlan {
	public:
		enum class OperationType {
			// --- Node Operations ---
			NODE_LABEL_SCAN, // Use index to find nodes by label
			NODE_PROPERTY_SCAN, // Use index to find nodes by property
			NODE_LABEL_PROPERTY_SCAN, // Use indexes to find nodes by label and property

			// --- Edge Operations ---
			EDGE_LABEL_SCAN, // Use index to find edges by label
			EDGE_PROPERTY_SCAN, // Use index to find edges by property
			// Note: EDGE_LABEL_PROPERTY_SCAN can be added if needed

			// --- Full Scan Operations (Fallbacks) ---
			FULL_NODE_LABEL_SCAN,
			FULL_NODE_PROPERTY_SCAN,
			FULL_NODE_LABEL_PROPERTY_SCAN,
			FULL_EDGE_LABEL_SCAN, // Can be added if needed
			FULL_EDGE_PROPERTY_SCAN, // Can be added if needed

			// --- Traversal Operations (if supported without old indexes) ---
			TRAVERSAL_CONNECTED_NODES,
			TRAVERSAL_SHORTEST_PATH,
			TRAVERSAL_BFS,
		};

		explicit QueryPlan(OperationType type) : type_(type) {}

		// Add parameters for the operation
		void addParameter(const std::string &name, const PropertyValue &value) { params_[name] = value; }

		template<typename T>
		std::optional<T> getParameter(const std::string &key) const {
			auto it = params_.find(key);
			if (it == params_.end()) {
				return std::nullopt; // Key not found.
			}

			// The PropertyValue object associated with the key.
			const auto &propValue = it->second;

			// --- The Magic Happens Here ---
			if constexpr (std::is_same_v<T, PropertyValue>) {
				// Case 1: The user requested the PropertyValue wrapper itself.
				return propValue;
			} else {
				// Case 2: The user requested a specific inner type (int64_t, string, etc.).
				// We attempt to extract it from the variant.
				if (const T *val = std::get_if<T>(&propValue.getVariant())) {
					return *val;
				} else {
					// The key exists, but holds a different type than requested.
					return std::nullopt;
				}
			}
		}

		// Get the operation type
		[[nodiscard]] OperationType getType() const { return type_; }

	private:
		OperationType type_;

		std::map<std::string, PropertyValue> params_;
	};

	class QueryPlanner {
	public:
		explicit QueryPlanner(std::shared_ptr<indexes::IndexManager> indexManager);

		// --- Node Query Planners ---
		[[nodiscard]] QueryPlan createPlanForNodeLabelQuery(const std::string &label) const;
		QueryPlan createPlanForNodePropertyQuery(const std::string &key, const PropertyValue &value) const;
		QueryPlan createPlanForNodeLabelAndPropertyQuery(const std::string &label, const std::string &key,
														 const PropertyValue &value) const;

		// --- Edge Query Planners ---
		[[nodiscard]] QueryPlan createPlanForEdgeLabelQuery(const std::string &label) const;
		QueryPlan createPlanForEdgePropertyQuery(const std::string &key, const PropertyValue &value) const;

		// Create a plan for finding connected nodes
		static QueryPlan createPlanForConnectedNodesQuery(int64_t nodeId, const std::string &nodeLabel,
														  const std::string &edgeLabel, const std::string &direction);

		static QueryPlan createPlanForTraversalShortestPath(int64_t startNodeId, int64_t endNodeId, int maxDepth,
															const std::string &direction);

	private:
		std::shared_ptr<indexes::IndexManager> indexManager_;
	};

} // namespace graph::query
