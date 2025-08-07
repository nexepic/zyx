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

namespace graph::query {

	QueryPlanner::QueryPlanner(std::shared_ptr<indexes::IndexManager> indexManager) :
		indexManager_(std::move(indexManager)) {}

	// --- Node Query Plans ---

	QueryPlan QueryPlanner::createPlanForNodeLabelQuery(const std::string &label) const {
		// Check if node label index exists
		auto nodeIndexManager = indexManager_->getNodeIndexManager();
		if (nodeIndexManager && !nodeIndexManager->getLabelIndex()->isEmpty()) {
			QueryPlan plan(QueryPlan::OperationType::NODE_LABEL_SCAN);
			plan.addParameter("label", label);
			return plan;
		}
		// Fallback to full scan
		QueryPlan plan(QueryPlan::OperationType::FULL_NODE_LABEL_SCAN);
		plan.addParameter("label", label);
		return plan;
	}

	QueryPlan QueryPlanner::createPlanForNodePropertyQuery(const std::string &key, const PropertyValue &value) const {
		auto nodeIndexManager = indexManager_->getNodeIndexManager();
		auto propertyIndex = nodeIndexManager->getPropertyIndex();

		if (propertyIndex && propertyIndex->hasKeyIndexed(key)) {
			QueryPlan plan(QueryPlan::OperationType::NODE_PROPERTY_SCAN);
			plan.addParameter("key", key);
			plan.addParameter("value", value);
			return plan;
		}
		// Fallback to full scan
		QueryPlan plan(QueryPlan::OperationType::FULL_NODE_PROPERTY_SCAN);
		plan.addParameter("key", key);
		plan.addParameter("value", value);
		// highlight-end
		return plan;
	}

	QueryPlan QueryPlanner::createPlanForNodeLabelAndPropertyQuery(const std::string &label, const std::string &key,
																   const PropertyValue &value) const {
		auto nodeIndexManager = indexManager_->getNodeIndexManager();
		bool hasLabelIndex = !nodeIndexManager->getLabelIndex()->isEmpty();
		bool hasPropertyIndex = !nodeIndexManager->getPropertyIndex()->isEmpty() &&
								nodeIndexManager->getPropertyIndex()->hasKeyIndexed(key);

		if (hasLabelIndex && hasPropertyIndex) {
			QueryPlan plan(QueryPlan::OperationType::NODE_LABEL_PROPERTY_SCAN);
			plan.addParameter("label", label);
			plan.addParameter("key", key);
			plan.addParameter("value", value);
			return plan;
		}
		// Fallback
		QueryPlan plan(QueryPlan::OperationType::FULL_NODE_LABEL_PROPERTY_SCAN);
		plan.addParameter("label", label);
		plan.addParameter("key", key);
		plan.addParameter("value", value);
		return plan;
	}

	// --- Edge Query Plans ---

	QueryPlan QueryPlanner::createPlanForEdgeLabelQuery(const std::string &label) const {
		auto edgeIndexManager = indexManager_->getEdgeIndexManager();
		if (edgeIndexManager && !edgeIndexManager->getLabelIndex()->isEmpty()) {
			QueryPlan plan(QueryPlan::OperationType::EDGE_LABEL_SCAN);
			plan.addParameter("label", label);
			return plan;
		}
		// Fallback to full edge scan
		QueryPlan plan(QueryPlan::OperationType::FULL_EDGE_LABEL_SCAN);
		plan.addParameter("label", label);
		return plan;
	}

	QueryPlan QueryPlanner::createPlanForEdgePropertyQuery(const std::string &key, const PropertyValue &value) const {
		auto edgeIndexManager = indexManager_->getEdgeIndexManager();
		auto propertyIndex = edgeIndexManager->getPropertyIndex();

		if (propertyIndex && propertyIndex->hasKeyIndexed(key)) {
			QueryPlan plan(QueryPlan::OperationType::EDGE_PROPERTY_SCAN);
			plan.addParameter("key", key);
			plan.addParameter("value", value);
			return plan;
		}
		// Fallback
		QueryPlan plan(QueryPlan::OperationType::FULL_EDGE_PROPERTY_SCAN);
		plan.addParameter("key", key);
		plan.addParameter("value", value);
		return plan;
	}

	QueryPlan QueryPlanner::createPlanForConnectedNodesQuery(int64_t nodeId, const std::string &nodeLabel,
															 const std::string &edgeLabel,
															 const std::string &direction) {
		QueryPlan plan(QueryPlan::OperationType::TRAVERSAL_CONNECTED_NODES);
		plan.addParameter("startNodeId", nodeId);
		plan.addParameter("direction", direction);

		if (!nodeLabel.empty()) {
			plan.addParameter("nodeLabel", nodeLabel);
		}

		if (!edgeLabel.empty()) {
			plan.addParameter("edgeLabel", edgeLabel);
		}

		return plan;
	}

	QueryPlan QueryPlanner::createPlanForTraversalShortestPath(int64_t startNodeId, int64_t endNodeId, int maxDepth,
															   const std::string &direction) {
		QueryPlan plan(QueryPlan::OperationType::TRAVERSAL_SHORTEST_PATH);
		plan.addParameter("startNodeId", startNodeId);
		plan.addParameter("endNodeId", endNodeId);
		plan.addParameter("maxDepth", maxDepth);
		plan.addParameter("direction", direction);

		return plan;
	}

} // namespace graph::query
