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
		// The switch now includes new operations for edges.
		switch (plan.getType()) {
			// Node Operations
			case QueryPlan::OperationType::NODE_LABEL_SCAN:
				return executeNodeLabelScan(plan);
			case QueryPlan::OperationType::NODE_PROPERTY_SCAN:
				return executeNodePropertyScan(plan);
			case QueryPlan::OperationType::NODE_LABEL_PROPERTY_SCAN:
				return executeNodeLabelPropertyScan(plan);
				// Edge Operations
			case QueryPlan::OperationType::EDGE_LABEL_SCAN:
				return executeEdgeLabelScan(plan);
			case QueryPlan::OperationType::EDGE_PROPERTY_SCAN:
				return executeEdgePropertyScan(plan);
				// Full Scans (assuming these remain largely the same)
			case QueryPlan::OperationType::FULL_NODE_LABEL_SCAN:
				return executeFullNodeLabelScan(plan);
			case QueryPlan::OperationType::FULL_NODE_PROPERTY_SCAN:
				return executeFullNodePropertyScan(plan);
			case QueryPlan::OperationType::FULL_NODE_LABEL_PROPERTY_SCAN:
				return executeFullNodeLabelPropertyScan(plan);
			case QueryPlan::OperationType::FULL_EDGE_LABEL_SCAN:
				return executeFullEdgeLabelScan(plan);
			case QueryPlan::OperationType::FULL_EDGE_PROPERTY_SCAN:
				return executeFullEdgePropertyScan(plan);
			default:
				return {};
		}
	}

	QueryResult QueryExecutor::executeNodeLabelScan(const QueryPlan &plan) const {
		QueryResult result;
		const auto &params = plan.getStringParams();
		if (!params.contains("label")) return result;
		const std::string &label = params.at("label");

		// Use the new index manager interface for nodes
		auto nodeIds = indexManager_->findNodeIdsByLabel(label);

		unifiedQueryView_->mergeDirtyEntities<Node>(nodeIds,
			[&](const Node &node) { return node.getLabel() == label; }
		);

		if (!nodeIds.empty()) {
			for (const auto &node : dataManager_->getNodeBatch(nodeIds)) {
				result.addNode(node);
			}
		}
		return result;
	}

	QueryResult QueryExecutor::executeNodePropertyScan(const QueryPlan &plan) const {
		QueryResult result;
		const auto &params = plan.getStringParams();
		if (!params.contains("key") || !params.contains("value")) return result;

		const std::string &key = params.at("key");
		const std::string &value = params.at("value");

		auto nodeIds = indexManager_->findNodeIdsByProperty(key, value);

		// ... unified view logic remains the same ...
		unifiedQueryView_->mergeDirtyEntities<Node>(nodeIds,
			[&](const Node &node) {
				auto properties = dataManager_->getNodeProperties(node.getId());
				auto propIt = properties.find(key);
				if (propIt != properties.end() && std::holds_alternative<std::string>(propIt->second)) {
					return std::get<std::string>(propIt->second) == value;
				}
				return false;
			}
		);

		if (!nodeIds.empty()) {
			for (const auto &node : dataManager_->getNodeBatch(nodeIds)) {
				result.addNode(node);
			}
		}
		return result;
	}

	QueryResult QueryExecutor::executeNodeLabelPropertyScan(const QueryPlan &plan) const {
		QueryResult result;
		const auto& params = plan.getStringParams();
		if (!params.contains("label") || !params.contains("key") || !params.contains("value")) return result;

		const std::string &label = params.at("label");
		const std::string &key = params.at("key");
		const std::string &value = params.at("value");

		auto labelNodeIds = indexManager_->findNodeIdsByLabel(label);
		auto propertyNodeIds = indexManager_->findNodeIdsByProperty(key, value);

		// Intersect results for efficiency
		std::vector<int64_t> initialIds;
		std::unordered_set propertySet(propertyNodeIds.begin(), propertyNodeIds.end());
		for (int64_t id : labelNodeIds) {
			if (propertySet.contains(id)) {
				initialIds.push_back(id);
			}
		}

		// ... unified view and data fetching logic remains the same ...
		if (!initialIds.empty()) {
			for (const auto& node : dataManager_->getNodeBatch(initialIds)) {
				result.addNode(node);
			}
		}
		return result;
	}

	QueryResult QueryExecutor::executeEdgeLabelScan(const QueryPlan& plan) const {
		QueryResult result;
		const auto& params = plan.getStringParams();
		if (!params.contains("label")) return result;
		const std::string& label = params.at("label");

		// Use the new index manager interface for edges
		auto edgeIds = indexManager_->findEdgeIdsByLabel(label);

		unifiedQueryView_->mergeDirtyEntities<Edge>(edgeIds,
			[&](const Edge& edge) { return edge.getLabel() == label; }
		);

		if (!edgeIds.empty()) {
			for (const auto& edge : dataManager_->getEdgeBatch(edgeIds)) {
				result.addEdge(edge);
			}
		}
		return result;
	}

	QueryResult QueryExecutor::executeEdgePropertyScan(const QueryPlan& plan) const {
		QueryResult result;
		const auto& params = plan.getStringParams();
		if (!params.contains("key") || !params.contains("value")) return result;
		const std::string& key = params.at("key");
		const std::string& value = params.at("value");

		// Use the new index manager interface for edges
		auto edgeIds = indexManager_->findEdgeIdsByProperty(key, value);

		unifiedQueryView_->mergeDirtyEntities<Edge>(edgeIds,
			[&](const Edge& edge) {
				auto properties = dataManager_->getEdgeProperties(edge.getId());
				auto propIt = properties.find(key);
				if (propIt != properties.end() && std::holds_alternative<std::string>(propIt->second)) {
					return std::get<std::string>(propIt->second) == value;
				}
				return false;
			}
		);

		if (!edgeIds.empty()) {
			for (const auto& edge : dataManager_->getEdgeBatch(edgeIds)) {
				result.addEdge(edge);
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

	QueryResult QueryExecutor::executeFullEdgeLabelScan(const QueryPlan& plan) const {
		QueryResult result;

		// Extract the label from the query plan.
		const auto& params = plan.getStringParams();
		if (!params.contains("label")) {
			return result; // Invalid plan.
		}
		const std::string& label = params.at("label");

		// Get the highest possible edge ID to define the scan range.
		const int64_t maxEdgeId = dataManager_->getIdAllocator()->getCurrentMaxEdgeId();
		// TODO: Fix hardcoded batch size
		constexpr int64_t BATCH_SIZE = 1000;

		for (int64_t startId = 1; startId <= maxEdgeId; startId += BATCH_SIZE) {
			const int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxEdgeId);
			auto edges = dataManager_->getEdgesInRange(startId, endId);

			for (auto& edge : edges) {
				// Apply the label filter.
				if (edge.isActive() && edge.getLabel() == label) {
					result.addEdge(edge);
				}
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executeFullEdgePropertyScan(const QueryPlan& plan) const {
		QueryResult result;

		// Extract key and value from the query plan.
		const auto& params = plan.getStringParams();
		if (!params.contains("key") || !params.contains("value")) {
			return result; // Invalid plan.
		}
		const std::string& key = params.at("key");
		const std::string& value = params.at("value");

		// Get the highest possible edge ID to define the scan range.
		const int64_t maxEdgeId = dataManager_->getIdAllocator()->getCurrentMaxEdgeId();
		constexpr int64_t BATCH_SIZE = 1000;

		for (int64_t startId = 1; startId <= maxEdgeId; startId += BATCH_SIZE) {
			const int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxEdgeId);
			auto edges = dataManager_->getEdgesInRange(startId, endId);

			for (auto& edge : edges) {
				if (!edge.isActive()) {
					continue;
				}

				// Fetch properties for the current edge. This is the expensive part of the operation.
				auto properties = dataManager_->getEdgeProperties(edge.getId());
				auto propIt = properties.find(key);

				// Check if the property exists and its value matches.
				// Note: This currently only handles string-to-string comparison.
				if (propIt != properties.end() &&
					std::holds_alternative<std::string>(propIt->second) &&
					std::get<std::string>(propIt->second) == value)
				{
					result.addEdge(edge);
				}
			}
		}

		return result;
	}

} // namespace graph::query
