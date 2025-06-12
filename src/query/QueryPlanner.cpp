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

	QueryPlan QueryPlanner::createPlanForLabelQuery(const std::string &label) {
		QueryPlan plan(QueryPlan::OperationType::LABEL_SCAN);
		plan.addParameter("label", label);
		return plan;
	}

	QueryPlan QueryPlanner::createPlanForPropertyQuery(const std::string &key, const std::string &value) {
		QueryPlan plan(QueryPlan::OperationType::PROPERTY_SCAN);
		plan.addParameter("key", key);
		plan.addParameter("value", value);
		return plan;
	}

	QueryPlan QueryPlanner::createPlanForLabelAndPropertyQuery(const std::string &label, const std::string &key,
															   const std::string &value) {
		// Determine which index is more selective (has fewer matching nodes)
		auto labelNodeIds = indexManager_->findNodeIdsByLabel(label);
		auto propertyNodeIds = indexManager_->findNodeIdsByProperty(key, value);

		// In a more sophisticated implementation, we would use statistics to make this decision
		// For simplicity, we'll just compare the number of matching nodes
		if (labelNodeIds.size() <= propertyNodeIds.size()) {
			// Label index is more selective, use label scan first
			QueryPlan plan(QueryPlan::OperationType::LABEL_PROPERTY_SCAN);
			plan.addParameter("label", label);
			plan.addParameter("key", key);
			plan.addParameter("value", value);
			plan.addParameter("scanType", "labelFirst");
			return plan;
		} else {
			// Property index is more selective, use property scan first
			QueryPlan plan(QueryPlan::OperationType::LABEL_PROPERTY_SCAN);
			plan.addParameter("label", label);
			plan.addParameter("key", key);
			plan.addParameter("value", value);
			plan.addParameter("scanType", "propertyFirst");
			return plan;
		}
	}

	QueryPlan QueryPlanner::createPlanForPropertyRangeQuery(const std::string &key, double minValue, double maxValue) {
		QueryPlan plan(QueryPlan::OperationType::PROPERTY_RANGE_SCAN);
		plan.addParameter("key", key);
		plan.addParameter("minValue", minValue);
		plan.addParameter("maxValue", maxValue);
		return plan;
	}

	QueryPlan QueryPlanner::createPlanForTextSearchQuery(const std::string &key, const std::string &searchText) {
		QueryPlan plan(QueryPlan::OperationType::TEXT_SEARCH);
		plan.addParameter("key", key);
		plan.addParameter("searchText", searchText);
		return plan;
	}

	QueryPlan QueryPlanner::createPlanForRelationshipQuery(uint64_t nodeId, const std::string &edgeLabel) {
		QueryPlan plan(QueryPlan::OperationType::RELATIONSHIP_SCAN);
		plan.addParameter("nodeId", nodeId);

		if (!edgeLabel.empty()) {
			plan.addParameter("label", edgeLabel);
		}

		return plan;
	}

	QueryPlan QueryPlanner::createPlanForConnectedNodesQuery(uint64_t nodeId, const std::string &edgeLabel,
															 const std::string &direction) {
		QueryPlan plan(QueryPlan::OperationType::RELATIONSHIP_SCAN);
		plan.addParameter("nodeId", nodeId);

		if (!edgeLabel.empty()) {
			plan.addParameter("label", edgeLabel);
		}

		plan.addParameter("direction", direction);
		plan.addParameter("includeNodes", "true");

		return plan;
	}

	QueryPlan QueryPlanner::createPlanForTraversalConnectedNodes(int64_t startNodeId, const std::string &direction,
																 const std::string &edgeLabel,
																 const std::string &nodeLabel) {
		QueryPlan plan(QueryPlan::OperationType::TRAVERSAL_CONNECTED_NODES);
		plan.addParameter("startNodeId", static_cast<uint64_t>(startNodeId));
		plan.addParameter("direction", direction);

		if (!edgeLabel.empty()) {
			plan.addParameter("edgeLabel", edgeLabel);
		}

		if (!nodeLabel.empty()) {
			plan.addParameter("nodeLabel", nodeLabel);
		}

		return plan;
	}

	QueryPlan QueryPlanner::createPlanForTraversalShortestPath(int64_t startNodeId, int64_t endNodeId, int maxDepth,
															   const std::string &direction) {
		QueryPlan plan(QueryPlan::OperationType::TRAVERSAL_SHORTEST_PATH);
		plan.addParameter("startNodeId", static_cast<uint64_t>(startNodeId));
		plan.addParameter("endNodeId", static_cast<uint64_t>(endNodeId));
		plan.addParameter("maxDepth", static_cast<double>(maxDepth));
		plan.addParameter("direction", direction);

		return plan;
	}

} // namespace graph::query
