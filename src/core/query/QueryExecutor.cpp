/**
 * @file QueryExecutor.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/query/QueryExecutor.h"
#include <algorithm>

namespace graph::query {

QueryExecutor::QueryExecutor(std::shared_ptr<indexes::IndexManager> indexManager,
                           std::shared_ptr<storage::FileStorage> storage)
    : indexManager_(std::move(indexManager)),
      storage_(std::move(storage)) {
}

QueryResult QueryExecutor::execute(const QueryPlan& plan) {
    switch (plan.getType()) {
        case QueryPlan::OperationType::LABEL_SCAN:
            return executeLabelScan(plan);
        case QueryPlan::OperationType::PROPERTY_SCAN:
            return executePropertyScan(plan);
        case QueryPlan::OperationType::LABEL_PROPERTY_SCAN:
            return executeLabelPropertyScan(plan);
        case QueryPlan::OperationType::PROPERTY_RANGE_SCAN:
            return executePropertyRangeScan(plan);
        case QueryPlan::OperationType::TEXT_SEARCH:
            return executeTextSearch(plan);
        case QueryPlan::OperationType::RELATIONSHIP_SCAN:
            return executeRelationshipScan(plan);
        default:
            // Unknown operation type
            return QueryResult();
    }
}

QueryResult QueryExecutor::executeLabelScan(const QueryPlan& plan) {
    QueryResult result;

    // Get parameters
    const auto& params = plan.getStringParams();
    auto it = params.find("label");
    if (it == params.end()) {
        return result;  // No label specified
    }

    const std::string& label = it->second;

    // Find nodes by label using the index
    auto nodeIds = indexManager_->findNodeIdsByLabel(label);

    // Retrieve the actual nodes
	auto fetchedNodes = storage_->getNodes(nodeIds);
	for (auto &node : fetchedNodes) {
		// node.getId() can be checked if needed
		result.addNode(node);
	}

    return result;
}

QueryResult QueryExecutor::executePropertyScan(const QueryPlan& plan) {
    QueryResult result;

    // Get parameters
    const auto& params = plan.getStringParams();
    auto keyIt = params.find("key");
    auto valueIt = params.find("value");

    if (keyIt == params.end() || valueIt == params.end()) {
        return result;  // Missing parameters
    }

    const std::string& key = keyIt->second;
    const std::string& value = valueIt->second;

    // Find nodes by property using the index
    auto nodeIds = indexManager_->findNodeIdsByProperty(key, value);

    // Retrieve the actual nodes
	auto fetchedNodes = storage_->getNodes(nodeIds);
	for (auto &node : fetchedNodes) {
		result.addNode(node);
	}

    return result;
}

QueryResult QueryExecutor::executeLabelPropertyScan(const QueryPlan& plan) {
    QueryResult result;

    // Get parameters
    const auto& params = plan.getStringParams();
    auto labelIt = params.find("label");
    auto keyIt = params.find("key");
    auto valueIt = params.find("value");

    if (labelIt == params.end() || keyIt == params.end() || valueIt == params.end()) {
        return result;  // Missing parameters
    }

    const std::string& label = labelIt->second;
    const std::string& key = keyIt->second;
    const std::string& value = valueIt->second;

    // Find nodes by label and property using the indexes
    // Use the smaller set for iteration and filtering
    auto labelNodeIds = indexManager_->findNodeIdsByLabel(label);
    auto propertyNodeIds = indexManager_->findNodeIdsByProperty(key, value);

    // Use set intersection approach for optimization
    if (labelNodeIds.size() > propertyNodeIds.size()) {
        std::swap(labelNodeIds, propertyNodeIds);
    }

    std::vector<int64_t> intersection;
    for (int64_t nodeId : labelNodeIds) {
        if (std::find(propertyNodeIds.begin(), propertyNodeIds.end(), nodeId) != propertyNodeIds.end()) {
            intersection.push_back(nodeId);
        }
    }

    // Retrieve the actual nodes
	auto fetchedNodes = storage_->getNodes(intersection);
	for (auto &node : fetchedNodes) {
		result.addNode(node);
	}

    return result;
}

QueryResult QueryExecutor::executePropertyRangeScan(const QueryPlan& plan) {
    QueryResult result;

    // Get parameters
    const auto& stringParams = plan.getStringParams();
    const auto& doubleParams = plan.getDoubleParams();

    auto keyIt = stringParams.find("key");
    auto minIt = doubleParams.find("minValue");
    auto maxIt = doubleParams.find("maxValue");

    if (keyIt == stringParams.end() || minIt == doubleParams.end() || maxIt == doubleParams.end()) {
        return result;  // Missing parameters
    }

    const std::string& key = keyIt->second;
    double minValue = minIt->second;
    double maxValue = maxIt->second;

    // Find nodes by property range using the index
    auto nodeIds = indexManager_->findNodeIdsByPropertyRange(key, minValue, maxValue);

    // Retrieve the actual nodes
	auto fetchedNodes = storage_->getNodes(nodeIds);
	for (auto &node : fetchedNodes) {
		result.addNode(node);
	}

    return result;
}

QueryResult QueryExecutor::executeTextSearch(const QueryPlan& plan) {
    QueryResult result;

    // Get parameters
    const auto& params = plan.getStringParams();
    auto keyIt = params.find("key");
    auto searchTextIt = params.find("searchText");

    if (keyIt == params.end() || searchTextIt == params.end()) {
        return result;  // Missing parameters
    }

    const std::string& key = keyIt->second;
    const std::string& searchText = searchTextIt->second;

    // Find nodes by text search using the index
    auto nodeIds = indexManager_->findNodeIdsByTextSearch(key, searchText);

    // Retrieve the actual nodes
	auto fetchedNodes = storage_->getNodes(nodeIds);
	for (auto &node : fetchedNodes) {
		result.addNode(node);
	}

    return result;
}

QueryResult QueryExecutor::executeRelationshipScan(const QueryPlan& plan) {
    QueryResult result;

    // Get parameters
    const auto& stringParams = plan.getStringParams();
    const auto& uint64Params = plan.getUint64Params();

    auto nodeIdIt = uint64Params.find("nodeId");
    auto labelIt = stringParams.find("label");
    auto directionIt = stringParams.find("direction");

    if (nodeIdIt == uint64Params.end()) {
        return result;  // Missing node ID
    }

    uint64_t nodeId = nodeIdIt->second;
    std::string label = (labelIt != stringParams.end()) ? labelIt->second : "";
    std::string direction = (directionIt != stringParams.end()) ? directionIt->second : "outgoing";

    std::vector<int64_t> edgeIds;

    // Find edges based on direction
    if (direction == "outgoing" || direction == "both") {
        auto outEdgeIds = indexManager_->findOutgoingEdgeIds(nodeId, label);
        edgeIds.insert(edgeIds.end(), outEdgeIds.begin(), outEdgeIds.end());
    }

    if (direction == "incoming" || direction == "both") {
        auto inEdgeIds = indexManager_->findIncomingEdgeIds(nodeId, label);
        edgeIds.insert(edgeIds.end(), inEdgeIds.begin(), inEdgeIds.end());
    }

    // Remove duplicates if any
    std::sort(edgeIds.begin(), edgeIds.end());
    edgeIds.erase(std::unique(edgeIds.begin(), edgeIds.end()), edgeIds.end());

    // Retrieve the actual edges
	auto fetchedEdges = storage_->getEdges(edgeIds);
	// Convert them to a map if we need fast lookups
	std::unordered_map<int64_t, Edge> edgeMap;
	edgeMap.reserve(fetchedEdges.size());
	for (auto &e : fetchedEdges) {
		edgeMap[e.getId()] = e;
	}

    for (auto edgeId : edgeIds) {
        auto it = edgeMap.find(edgeId);
        if (it != edgeMap.end()) {
            result.addEdge(it->second);

            // Optionally include connected nodes
            if (stringParams.find("includeNodes") != stringParams.end()) {
                std::vector<int64_t> neededIds;
                neededIds.push_back(it->second.getFromNodeId());
                neededIds.push_back(it->second.getToNodeId());
                auto connectedNodes = storage_->getNodes(neededIds);
                for (auto &node : connectedNodes) {
                    result.addNode(node);
                }
            }
        }
    }

    return result;
}

} // namespace graph::query