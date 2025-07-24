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
								 std::shared_ptr<storage::FileStorage> storage) :
		indexManager_(std::move(indexManager)), storage_(std::move(storage)) {}

	QueryResult QueryExecutor::execute(const QueryPlan &plan) {
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

	QueryResult QueryExecutor::executeLabelScan(const QueryPlan &plan) {
		QueryResult result;

		// Get parameters
		const auto &params = plan.getStringParams();
		auto it = params.find("label");
		if (it == params.end()) {
			return result; // No label specified
		}

		const std::string &label = it->second;

		// Find nodes by label using the index
		auto nodeIds = indexManager_->findNodeIdsByLabel(label);

		// Retrieve the actual nodes
		auto fetchedNodes = storage_->getNodes(nodeIds);
		for (auto &node: fetchedNodes) {
			// node.getId() can be checked if needed
			result.addNode(node);
		}

		return result;
	}

	QueryResult QueryExecutor::executePropertyScan(const QueryPlan &plan) {
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

		// Retrieve the actual nodes
		auto fetchedNodes = storage_->getNodes(nodeIds);
		for (auto &node: fetchedNodes) {
			result.addNode(node);
		}

		return result;
	}

	QueryResult QueryExecutor::executeLabelPropertyScan(const QueryPlan &plan) {
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

		// Find nodes by label and property using the indexes
		// Use the smaller set for iteration and filtering
		auto labelNodeIds = indexManager_->findNodeIdsByLabel(label);
		auto propertyNodeIds = indexManager_->findNodeIdsByProperty(key, value);

		// Use set intersection approach for optimization
		if (labelNodeIds.size() > propertyNodeIds.size()) {
			std::swap(labelNodeIds, propertyNodeIds);
		}

		std::vector<int64_t> intersection;
		for (int64_t nodeId: labelNodeIds) {
			if (std::find(propertyNodeIds.begin(), propertyNodeIds.end(), nodeId) != propertyNodeIds.end()) {
				intersection.push_back(nodeId);
			}
		}

		// Retrieve the actual nodes
		auto fetchedNodes = storage_->getNodes(intersection);
		for (auto &node: fetchedNodes) {
			result.addNode(node);
		}

		return result;
	}

	QueryResult QueryExecutor::executePropertyRangeScan(const QueryPlan &plan) {
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
		auto fetchedNodes = storage_->getNodes(nodeIds);
		for (auto &node: fetchedNodes) {
			result.addNode(node);
		}

		return result;
	}

	QueryResult QueryExecutor::executeTextSearch(const QueryPlan &plan) {
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
		auto fetchedNodes = storage_->getNodes(nodeIds);
		for (auto &node: fetchedNodes) {
			result.addNode(node);
		}

		return result;
	}

	QueryResult QueryExecutor::executeRelationshipScan(const QueryPlan &plan) {
		QueryResult result;

		// Get parameters
		const auto &stringParams = plan.getStringParams();
		const auto &uint64Params = plan.getUint64Params();

		auto nodeIdIt = uint64Params.find("nodeId");
		auto labelIt = stringParams.find("label");
		auto directionIt = stringParams.find("direction");

		if (nodeIdIt == uint64Params.end()) {
			return result; // Missing node ID
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
		for (auto &e: fetchedEdges) {
			edgeMap[e.getId()] = e;
		}

		for (auto edgeId: edgeIds) {
			auto it = edgeMap.find(edgeId);
			if (it != edgeMap.end()) {
				result.addEdge(it->second);

				// Optionally include connected nodes
				if (stringParams.find("includeNodes") != stringParams.end()) {
					std::vector<int64_t> neededIds;
					neededIds.push_back(it->second.getSourceNodeId());
					neededIds.push_back(it->second.getTargetNodeId());
					auto connectedNodes = storage_->getNodes(neededIds);
					for (auto &node: connectedNodes) {
						result.addNode(node);
					}
				}
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executeTraversalConnectedNodes(const QueryPlan &plan) {
		QueryResult result;

		// Get parameters
		const auto &stringParams = plan.getStringParams();
		const auto &uint64Params = plan.getUint64Params();

		auto nodeIdIt = uint64Params.find("startNodeId");
		if (nodeIdIt == uint64Params.end()) {
			return result; // Missing node ID
		}

		int64_t startNodeId = static_cast<int64_t>(nodeIdIt->second);

		std::string direction = "both";
		auto directionIt = stringParams.find("direction");
		if (directionIt != stringParams.end()) {
			direction = directionIt->second;
		}

		std::string edgeLabel = "";
		auto edgeLabelIt = stringParams.find("edgeLabel");
		if (edgeLabelIt != stringParams.end()) {
			edgeLabel = edgeLabelIt->second;
		}

		std::string nodeLabel = "";
		auto nodeLabelIt = stringParams.find("nodeLabel");
		if (nodeLabelIt != stringParams.end()) {
			nodeLabel = nodeLabelIt->second;
		}

		// Create TraversalQuery and execute traversal
		auto dataManager = storage_->getDataManager();
		auto traversalQuery = std::make_shared<TraversalQuery>(dataManager);

		auto nodes = traversalQuery->findConnectedNodes(startNodeId, direction, edgeLabel, nodeLabel);
		for (const auto &node: nodes) {
			result.addNode(node);
		}

		return result;
	}

	QueryResult QueryExecutor::executeTraversalShortestPath(const QueryPlan &plan) {
		QueryResult result;

		// Get parameters
		const auto &stringParams = plan.getStringParams();
		const auto &uint64Params = plan.getUint64Params();
		const auto &doubleParams = plan.getDoubleParams();

		auto startNodeIdIt = uint64Params.find("startNodeId");
		auto endNodeIdIt = uint64Params.find("endNodeId");

		if (startNodeIdIt == uint64Params.end() || endNodeIdIt == uint64Params.end()) {
			return result; // Missing node IDs
		}

		auto startNodeId = static_cast<int64_t>(startNodeIdIt->second);
		auto endNodeId = static_cast<int64_t>(endNodeIdIt->second);

		int maxDepth = 10; // Default
		auto maxDepthIt = doubleParams.find("maxDepth");
		if (maxDepthIt != doubleParams.end()) {
			maxDepth = static_cast<int>(maxDepthIt->second);
		}

		std::string direction = "both";
		auto directionIt = stringParams.find("direction");
		if (directionIt != stringParams.end()) {
			direction = directionIt->second;
		}

		// Create TraversalQuery and execute shortest path
		auto dataManager = storage_->getDataManager();
		auto traversalQuery = std::make_shared<TraversalQuery>(dataManager);

		auto pathNodes = traversalQuery->findShortestPath(startNodeId, endNodeId, direction);
		for (const auto &node: pathNodes) {
			result.addNode(node);
		}

		return result;
	}

	QueryResult QueryExecutor::executeFullNodeScan(const QueryPlan &plan) {
		QueryResult result;

		// Get maximum node ID to determine scan range
		int64_t maxNodeId = storage_->getDataManager()->getIdAllocator()->getCurrentMaxNodeId();
		constexpr int64_t BATCH_SIZE = 1000; // Process in batches for efficiency

		for (int64_t startId = 1; startId <= maxNodeId; startId += BATCH_SIZE) {
			int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxNodeId);
			auto nodes = storage_->getDataManager()->getNodesInRange(startId, endId);

			for (auto &node: nodes) {
				result.addNode(node);
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executeFullNodeLabelScan(const QueryPlan &plan) {
		QueryResult result;

		// Get parameters
		const auto &params = plan.getStringParams();
		auto it = params.find("label");
		if (it == params.end()) {
			return result; // No label specified
		}

		const std::string &label = it->second;

		// Get maximum node ID to determine scan range
		int64_t maxNodeId = storage_->getDataManager()->getIdAllocator()->getCurrentMaxNodeId();
		constexpr int64_t BATCH_SIZE = 1000; // Process in batches for efficiency

		for (int64_t startId = 1; startId <= maxNodeId; startId += BATCH_SIZE) {
			int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxNodeId);
			auto nodes = storage_->getDataManager()->getNodesInRange(startId, endId);

			for (auto &node: nodes) {
				if (node.getLabel() == label) {
					result.addNode(node);
				}
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executeFullNodePropertyScan(const QueryPlan &plan) {
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
		int64_t maxNodeId = storage_->getDataManager()->getIdAllocator()->getCurrentMaxNodeId();
		constexpr int64_t BATCH_SIZE = 1000; // Process in batches for efficiency

		for (int64_t startId = 1; startId <= maxNodeId; startId += BATCH_SIZE) {
			int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxNodeId);
			auto nodes = storage_->getDataManager()->getNodesInRange(startId, endId);

			for (auto &node: nodes) {
				auto properties = storage_->getDataManager()->getNodeProperties(node.getId());
				auto propIt = properties.find(key);

				if (propIt != properties.end() && (std::holds_alternative<std::string>(propIt->second) &&
												   std::get<std::string>(propIt->second) == value)) {
					result.addNode(node);
				}
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executeFullNodeLabelPropertyScan(const QueryPlan &plan) {
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
		int64_t maxNodeId = storage_->getDataManager()->getIdAllocator()->getCurrentMaxNodeId();
		constexpr int64_t BATCH_SIZE = 1000; // Process in batches for efficiency

		for (int64_t startId = 1; startId <= maxNodeId; startId += BATCH_SIZE) {
			int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxNodeId);
			auto nodes = storage_->getDataManager()->getNodesInRange(startId, endId);

			for (auto &node: nodes) {
				// Check label first (cheaper operation)
				if (node.getLabel() == label) {
					auto properties = storage_->getDataManager()->getNodeProperties(node.getId());
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

	QueryResult QueryExecutor::executeFullRelationshipScan(const QueryPlan &plan) {
		QueryResult result;

		// Get parameters
		const auto &stringParams = plan.getStringParams();
		const auto &uint64Params = plan.getUint64Params();

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
		int64_t maxEdgeId = storage_->getDataManager()->getIdAllocator()->getCurrentMaxEdgeId();
		constexpr int64_t BATCH_SIZE = 1000; // Process in batches for efficiency

		for (int64_t startId = 1; startId <= maxEdgeId; startId += BATCH_SIZE) {
			int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxEdgeId);
			auto edges = storage_->getDataManager()->getEdgesInRange(startId, endId);

			for (auto &edge: edges) {
				bool directionMatch = false;

				// Check direction and connected node
				if ((direction == "outgoing" || direction == "both") &&
					edge.getSourceNodeId() == static_cast<int64_t>(nodeId)) {
					directionMatch = true;
				} else if ((direction == "incoming" || direction == "both") &&
						   edge.getTargetNodeId() == static_cast<int64_t>(nodeId)) {
					directionMatch = true;
				}

				// Check label if specified
				bool labelMatch = label.empty() || edge.getLabel() == label;

				if (directionMatch && labelMatch) {
					result.addEdge(edge);

					// Optionally include connected nodes
					if (stringParams.find("includeNodes") != stringParams.end()) {
						std::vector<int64_t> neededIds;
						neededIds.push_back(edge.getSourceNodeId());
						neededIds.push_back(edge.getTargetNodeId());
						auto connectedNodes = storage_->getNodes(neededIds);
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
