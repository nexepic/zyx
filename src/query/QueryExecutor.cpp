/**
 * @file QueryExecutor.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/QueryExecutor.hpp"
#include <algorithm>
#include <graph/query/strategies/TraversalQuery.hpp>

namespace graph::query {

	QueryExecutor::QueryExecutor(std::shared_ptr<indexes::IndexManager> indexManager,
								 std::shared_ptr<storage::DataManager> dataManager) :
		indexManager_(std::move(indexManager)), dataManager_(std::move(dataManager)) {
		unifiedQueryView_ = std::make_unique<UnifiedQueryView>(dataManager_);
	}

	QueryResult QueryExecutor::execute(const QueryPlan &plan) const {
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
			case QueryPlan::OperationType::TRAVERSAL_CONNECTED_NODES:
				return executeTraversalConnectedNodes(plan);
			case QueryPlan::OperationType::TRAVERSAL_SHORTEST_PATH:
				return executeTraversalShortestPath(plan);
			case QueryPlan::OperationType::FULL_NODE_SCAN:
				return executeFullNodeScan(plan);
			case QueryPlan::OperationType::FULL_NODE_LABEL_SCAN:
				return executeFullNodeLabelScan(plan);
			case QueryPlan::OperationType::FULL_NODE_PROPERTY_SCAN:
				return executeFullNodePropertyScan(plan);
			case QueryPlan::OperationType::FULL_NODE_LABEL_PROPERTY_SCAN:
				return executeFullNodeLabelPropertyScan(plan);
			case QueryPlan::OperationType::FULL_RELATIONSHIP_SCAN:
				return executeFullRelationshipScan(plan);
			default:
				// Unknown operation type
				return {};
		}
	}

	QueryResult QueryExecutor::executeLabelScan(const QueryPlan &plan) const {
		QueryResult result;

		// Get parameters
		const auto &params = plan.getStringParams();
		if (!params.contains("label")) {
		    return result;
		}

		const std::string &label = params.at("label");

		// Find nodes by label using the index
		auto nodeIds = indexManager_->findNodeIdsByLabel(label);

		unifiedQueryView_->mergeDirtyEntities<Node>(nodeIds,
			[&](const Node &node) { return node.getLabel() == label; }
		);

		// Retrieve the actual nodes
		if (!nodeIds.empty()) {
			auto fetchedNodes = dataManager_->getNodeBatch(nodeIds);
			for (const auto &node: fetchedNodes) {
				result.addNode(node);
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executePropertyScan(const QueryPlan &plan) const {
		QueryResult result;

		// Get parameters
		const auto &params = plan.getStringParams();
		auto keyIt = params.find("key");
		auto valueIt = params.find("value");

		if (keyIt == params.end() || valueIt == params.end()) {
			return result; // Missing parameters
		}

		const std::string &key = keyIt->second;
		const std::string &value = valueIt->second;

		// Find nodes by property using the index
		auto nodeIds = indexManager_->findNodeIdsByProperty(key, value);

		unifiedQueryView_->mergeDirtyEntities<Node>(nodeIds,
			[&](const Node &node) {
				// To check properties, we must ask the DataManager, as properties are linked.
				auto properties = dataManager_->getNodeProperties(node.getId());
				auto propIt = properties.find(key);
				if (propIt != properties.end() && std::holds_alternative<std::string>(propIt->second)) {
					return std::get<std::string>(propIt->second) == value;
				}
				return false;
			}
		);

		// Retrieve the actual nodes
		if (!nodeIds.empty()) {
			auto fetchedNodes = dataManager_->getNodeBatch(nodeIds);
			for (const auto &node: fetchedNodes) {
				result.addNode(node);
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executeLabelPropertyScan(const QueryPlan &plan) const {
		QueryResult result;

		// Get parameters
		const auto &params = plan.getStringParams();
		const std::string &label = params.at("label");
		const std::string &key = params.at("key");
		const std::string &value = params.at("value");

		// Find nodes by label and property using the indexes
		// Use the smaller set for iteration and filtering
		auto labelNodeIds = indexManager_->findNodeIdsByLabel(label);
		auto propertyNodeIds = indexManager_->findNodeIdsByProperty(key, value);

		// Intersect the results from the two indexes first for efficiency.
		std::vector<int64_t> initialIds;
		std::unordered_set propertySet(propertyNodeIds.begin(), propertyNodeIds.end());
		for (int64_t id : labelNodeIds) {
			if (propertySet.contains(id)) {
				initialIds.push_back(id);
			}
		}

		unifiedQueryView_->mergeDirtyEntities<Node>(initialIds,
			[&](const Node &node) {
				if (node.getLabel() != label) return false;

				auto properties = dataManager_->getNodeProperties(node.getId());
				auto propIt = properties.find(key);
				if (propIt != properties.end() && std::holds_alternative<std::string>(propIt->second)) {
					return std::get<std::string>(propIt->second) == value;
				}
				return false;
			}
		);

		// Retrieve the actual nodes
		if (!initialIds.empty()) {
			auto fetchedNodes = dataManager_->getNodeBatch(initialIds);
			for (const auto &node: fetchedNodes) {
				result.addNode(node);
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executePropertyRangeScan(const QueryPlan &plan) const {
		QueryResult result;

		// Get parameters
		const auto &stringParams = plan.getStringParams();
		const auto &doubleParams = plan.getDoubleParams();

		auto keyIt = stringParams.find("key");
		auto minIt = doubleParams.find("minValue");
		auto maxIt = doubleParams.find("maxValue");

		if (keyIt == stringParams.end() || minIt == doubleParams.end() || maxIt == doubleParams.end()) {
			return result; // Missing parameters
		}

		const std::string &key = keyIt->second;
		double minValue = minIt->second;
		double maxValue = maxIt->second;

		// Find nodes by property range using the index
		auto nodeIds = indexManager_->findNodeIdsByPropertyRange(key, minValue, maxValue);

		// Retrieve the actual nodes
		auto fetchedNodes = dataManager_->getNodeBatch(nodeIds);
		for (auto &node: fetchedNodes) {
			result.addNode(node);
		}

		return result;
	}

	QueryResult QueryExecutor::executeTextSearch(const QueryPlan &plan) const {
		QueryResult result;

		// Get parameters
		const auto &params = plan.getStringParams();
		auto keyIt = params.find("key");
		auto searchTextIt = params.find("searchText");

		if (keyIt == params.end() || searchTextIt == params.end()) {
			return result; // Missing parameters
		}

		const std::string &key = keyIt->second;
		const std::string &searchText = searchTextIt->second;

		// Find nodes by text search using the index
		auto nodeIds = indexManager_->findNodeIdsByTextSearch(key, searchText);

		// Retrieve the actual nodes
		auto fetchedNodes = dataManager_->getNodeBatch(nodeIds);
		for (auto &node: fetchedNodes) {
			result.addNode(node);
		}

		return result;
	}

	QueryResult QueryExecutor::executeRelationshipScan(const QueryPlan &plan) const {
		QueryResult result;

		// Get parameters
		const auto &stringParams = plan.getStringParams();
		const auto &int64Params = plan.getInt64Params();

		auto nodeIdIt = int64Params.find("nodeId");
		auto labelIt = stringParams.find("label");
		auto directionIt = stringParams.find("direction");

		if (nodeIdIt == int64Params.end()) {
			return result; // Missing node ID
		}

		int64_t nodeId = nodeIdIt->second;
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
		std::ranges::sort(edgeIds);
		edgeIds.erase(std::ranges::unique(edgeIds).begin(), edgeIds.end());

		// Retrieve the actual edges
		auto fetchedEdges = dataManager_->getEdgeBatch(edgeIds);
		// Convert them to a map if we need fast lookups
		std::unordered_map<int64_t, Edge> edgeMap;
		edgeMap.reserve(fetchedEdges.size());
		for (auto &e: fetchedEdges) {
			edgeMap[e.getId()] = e;
		}

		for (auto edgeId: edgeIds) {
			auto it = edgeMap.find(edgeId);
			if (it != edgeMap.end()) {
				result.addEdge(it->second);

				// Optionally include connected nodes
				if (stringParams.contains("includeNodes")) {
					std::vector<int64_t> neededIds;
					neededIds.push_back(it->second.getSourceNodeId());
					neededIds.push_back(it->second.getTargetNodeId());
					auto connectedNodes = dataManager_->getNodeBatch(neededIds);
					for (auto &node: connectedNodes) {
						result.addNode(node);
					}
				}
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executeTraversalConnectedNodes(const QueryPlan &plan) const {
		QueryResult result;

		// Get parameters
		const auto &stringParams = plan.getStringParams();
		const auto &uint64Params = plan.getInt64Params();

		auto nodeIdIt = uint64Params.find("startNodeId");
		if (nodeIdIt == uint64Params.end()) {
			return result; // Missing node ID
		}

		const auto startNodeId = static_cast<int64_t>(nodeIdIt->second);

		std::string direction = "both";
		auto directionIt = stringParams.find("direction");
		if (directionIt != stringParams.end()) {
			direction = directionIt->second;
		}

		std::string edgeLabel;
		if (const auto edgeLabelIt = stringParams.find("edgeLabel"); edgeLabelIt != stringParams.end()) {
			edgeLabel = edgeLabelIt->second;
		}

		std::string nodeLabel;
		if (const auto nodeLabelIt = stringParams.find("nodeLabel"); nodeLabelIt != stringParams.end()) {
			nodeLabel = nodeLabelIt->second;
		}

		// TODO: Create traversalQuery once
		// Create TraversalQuery and execute traversal
		const auto traversalQuery = std::make_shared<TraversalQuery>(dataManager_);

		for (const auto nodes = traversalQuery->findConnectedNodes(startNodeId, direction, edgeLabel, nodeLabel);
			 const auto &node: nodes) {
			result.addNode(node);
		}

		return result;
	}

	QueryResult QueryExecutor::executeTraversalShortestPath(const QueryPlan &plan) const {
		QueryResult result;

		// Get parameters
		const auto &stringParams = plan.getStringParams();
		const auto &uint64Params = plan.getInt64Params();
		const auto &doubleParams = plan.getDoubleParams();

		auto startNodeIdIt = uint64Params.find("startNodeId");
		auto endNodeIdIt = uint64Params.find("endNodeId");

		if (startNodeIdIt == uint64Params.end() || endNodeIdIt == uint64Params.end()) {
			return result; // Missing node IDs
		}

		auto startNodeId = static_cast<int64_t>(startNodeIdIt->second);
		auto endNodeId = static_cast<int64_t>(endNodeIdIt->second);

		auto maxDepthIt = doubleParams.find("maxDepth");
		// TODO: maxDepth is not used in this implementation, but can be used for limiting the search depth
		int maxDepth = (maxDepthIt != doubleParams.end()) ? static_cast<int>(maxDepthIt->second) : 10;

		std::string direction = "both";
		auto directionIt = stringParams.find("direction");
		if (directionIt != stringParams.end()) {
			direction = directionIt->second;
		}

		// Create TraversalQuery and execute shortest path
		auto traversalQuery = std::make_shared<TraversalQuery>(dataManager_);

		auto pathNodes = traversalQuery->findShortestPath(startNodeId, endNodeId, direction);
		for (const auto &node: pathNodes) {
			result.addNode(node);
		}

		return result;
	}

	QueryResult QueryExecutor::executeFullNodeScan(const QueryPlan &plan) const {
		QueryResult result;

		// Get maximum node ID to determine scan range
		int64_t maxNodeId = dataManager_->getIdAllocator()->getCurrentMaxNodeId();
		constexpr int64_t BATCH_SIZE = 1000; // Process in batches for efficiency

		for (int64_t startId = 1; startId <= maxNodeId; startId += BATCH_SIZE) {
			int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxNodeId);
			auto nodes = dataManager_->getNodesInRange(startId, endId);

			for (auto &node: nodes) {
				result.addNode(node);
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executeFullNodeLabelScan(const QueryPlan &plan) const {
		QueryResult result;

		// Get parameters
		const auto &params = plan.getStringParams();
		if (!params.contains("label")) {
		    return result;
		}
		const std::string &label = params.at("label");

		// Get maximum node ID to determine scan range
		int64_t maxNodeId = dataManager_->getIdAllocator()->getCurrentMaxNodeId();
		constexpr int64_t BATCH_SIZE = 1000; // Process in batches for efficiency

		for (int64_t startId = 1; startId <= maxNodeId; startId += BATCH_SIZE) {
			int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxNodeId);
			auto nodes = dataManager_->getNodesInRange(startId, endId);

			for (auto &node: nodes) {
				if (node.getLabel() == label) {
					result.addNode(node);
				}
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executeFullNodePropertyScan(const QueryPlan &plan) const {
		QueryResult result;

		// Get parameters
		const auto &params = plan.getStringParams();
		auto keyIt = params.find("key");
		auto valueIt = params.find("value");

		if (keyIt == params.end() || valueIt == params.end()) {
			return result; // Missing parameters
		}

		const std::string &key = keyIt->second;
		const std::string &value = valueIt->second;

		// Get maximum node ID to determine scan range
		int64_t maxNodeId = dataManager_->getIdAllocator()->getCurrentMaxNodeId();
		constexpr int64_t BATCH_SIZE = 1000; // Process in batches for efficiency

		for (int64_t startId = 1; startId <= maxNodeId; startId += BATCH_SIZE) {
			int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxNodeId);
			auto nodes = dataManager_->getNodesInRange(startId, endId);

			for (auto &node: nodes) {
				auto properties = dataManager_->getNodeProperties(node.getId());
				auto propIt = properties.find(key);

				if (propIt != properties.end() && (std::holds_alternative<std::string>(propIt->second) &&
												   std::get<std::string>(propIt->second) == value)) {
					result.addNode(node);
				}
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executeFullNodeLabelPropertyScan(const QueryPlan &plan) const {
		QueryResult result;

		// Get parameters
		const auto &params = plan.getStringParams();
		auto labelIt = params.find("label");
		auto keyIt = params.find("key");
		auto valueIt = params.find("value");

		if (labelIt == params.end() || keyIt == params.end() || valueIt == params.end()) {
			return result; // Missing parameters
		}

		const std::string &label = labelIt->second;
		const std::string &key = keyIt->second;
		const std::string &value = valueIt->second;

		// Get maximum node ID to determine scan range
		int64_t maxNodeId = dataManager_->getIdAllocator()->getCurrentMaxNodeId();
		constexpr int64_t BATCH_SIZE = 1000; // Process in batches for efficiency

		for (int64_t startId = 1; startId <= maxNodeId; startId += BATCH_SIZE) {
			int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxNodeId);
			auto nodes = dataManager_->getNodesInRange(startId, endId);

			for (auto &node: nodes) {
				// Check label first (cheaper operation)
				if (node.getLabel() == label) {
					auto properties = dataManager_->getNodeProperties(node.getId());
					auto propIt = properties.find(key);

					if (propIt != properties.end() && (std::holds_alternative<std::string>(propIt->second) &&
													   std::get<std::string>(propIt->second) == value)) {
						result.addNode(node);
					}
				}
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executeFullRelationshipScan(const QueryPlan &plan) const {
		QueryResult result;

		// Get parameters
		const auto &stringParams = plan.getStringParams();
		const auto &uint64Params = plan.getInt64Params();

		auto nodeIdIt = uint64Params.find("nodeId");
		auto labelIt = stringParams.find("label");
		auto directionIt = stringParams.find("direction");

		// Early return if we can't find the node ID
		if (nodeIdIt == uint64Params.end()) {
			return result;
		}

		uint64_t nodeId = nodeIdIt->second;
		std::string label = (labelIt != stringParams.end()) ? labelIt->second : "";
		std::string direction = (directionIt != stringParams.end()) ? directionIt->second : "outgoing";

		// Get maximum edge ID to determine scan range
		const int64_t maxEdgeId = dataManager_->getIdAllocator()->getCurrentMaxEdgeId();
		// TODO: Fix hardcoded batch size
		constexpr int64_t BATCH_SIZE = 1000; // Process in batches for efficiency

		for (int64_t startId = 1; startId <= maxEdgeId; startId += BATCH_SIZE) {
			const int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxEdgeId);

			for (auto edges = dataManager_->getEdgesInRange(startId, endId); auto &edge: edges) {
				bool directionMatch = false;

				// Check direction and connected node
				directionMatch = ((direction == "outgoing" || direction == "both") &&
								  edge.getSourceNodeId() == static_cast<int64_t>(nodeId)) ||
								 ((direction == "incoming" || direction == "both") &&
								  edge.getTargetNodeId() == static_cast<int64_t>(nodeId));

				// Check label if specified

				if (const bool labelMatch = label.empty() || edge.getLabel() == label; directionMatch && labelMatch) {
					result.addEdge(edge);

					// Optionally include connected nodes
					if (stringParams.contains("includeNodes")) {
						std::vector<int64_t> neededIds;
						neededIds.push_back(edge.getSourceNodeId());
						neededIds.push_back(edge.getTargetNodeId());
						auto connectedNodes = dataManager_->getNodeBatch(neededIds);
						for (auto &node: connectedNodes) {
							result.addNode(node);
						}
					}
				}
			}
		}

		return result;
	}

} // namespace graph::query
