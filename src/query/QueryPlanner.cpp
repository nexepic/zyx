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
        // Check if label index exists
        if (indexManager_->getLabelIndex() && !indexManager_->getLabelIndex()->isEmpty()) {
            QueryPlan plan(QueryPlan::OperationType::LABEL_SCAN);
            plan.addParameter("label", label);
            return plan;
        } else {
            // Fallback to full scan
            QueryPlan plan(QueryPlan::OperationType::FULL_NODE_LABEL_SCAN);
            plan.addParameter("label", label);
            return plan;
        }
    }

	QueryPlan QueryPlanner::createPlanForPropertyQuery(const std::string &key, const std::string &value) {
		// Check if property index exists for this key
		auto propertyIndex = indexManager_->getPropertyIndex();
		if (propertyIndex && !propertyIndex->isEmpty() && propertyIndex->hasKeyIndexed(key)) {
			QueryPlan plan(QueryPlan::OperationType::PROPERTY_SCAN);
			plan.addParameter("key", key);
			plan.addParameter("value", value);
			return plan;
		} else {
			// Fallback to full scan
			QueryPlan plan(QueryPlan::OperationType::FULL_NODE_PROPERTY_SCAN);
			plan.addParameter("key", key);
			plan.addParameter("value", value);
			return plan;
		}
	}

	QueryPlan QueryPlanner::createPlanForLabelAndPropertyQuery(const std::string &label, const std::string &key,
                                                           const std::string &value) {
        auto labelIndex = indexManager_->getLabelIndex();
        auto propertyIndex = indexManager_->getPropertyIndex();
        bool hasLabelIndex = labelIndex && !labelIndex->isEmpty();
        bool hasPropertyIndex = propertyIndex && !propertyIndex->isEmpty() && propertyIndex->hasKeyIndexed(key);

        if (hasLabelIndex && hasPropertyIndex) {
            // Determine which index is more selective
            auto labelNodeIds = indexManager_->findNodeIdsByLabel(label);
            auto propertyNodeIds = indexManager_->findNodeIdsByProperty(key, value);

            // Use the index that returns fewer matches
            if (labelNodeIds.size() <= propertyNodeIds.size()) {
                QueryPlan plan(QueryPlan::OperationType::LABEL_PROPERTY_SCAN);
                plan.addParameter("label", label);
                plan.addParameter("key", key);
                plan.addParameter("value", value);
                plan.addParameter("scanType", "labelFirst");
                return plan;
            } else {
                QueryPlan plan(QueryPlan::OperationType::LABEL_PROPERTY_SCAN);
                plan.addParameter("label", label);
                plan.addParameter("key", key);
                plan.addParameter("value", value);
                plan.addParameter("scanType", "propertyFirst");
                return plan;
            }
        } else if (hasLabelIndex) {
            // Only label index exists, use label scan with property filter
            QueryPlan plan(QueryPlan::OperationType::LABEL_PROPERTY_SCAN);
            plan.addParameter("label", label);
            plan.addParameter("key", key);
            plan.addParameter("value", value);
            plan.addParameter("scanType", "labelFirst");
            return plan;
        } else if (hasPropertyIndex) {
            // Only property index exists, use property scan with label filter
            QueryPlan plan(QueryPlan::OperationType::LABEL_PROPERTY_SCAN);
            plan.addParameter("label", label);
            plan.addParameter("key", key);
            plan.addParameter("value", value);
            plan.addParameter("scanType", "propertyFirst");
            return plan;
        } else {
            // No indexes, full scan
            QueryPlan plan(QueryPlan::OperationType::FULL_NODE_LABEL_PROPERTY_SCAN);
            plan.addParameter("label", label);
            plan.addParameter("key", key);
            plan.addParameter("value", value);
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
