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

		auto label = plan.getParameter<std::string>("label");
		if (!label)
			return result;

		// Use the new index manager interface for nodes
		auto nodeIds = indexManager_->findNodeIdsByLabel(*label);

		unifiedQueryView_->mergeDirtyEntities<Node>(nodeIds,
													[&](const Node &node) { return node.getLabel() == *label; });

		if (!nodeIds.empty()) {
			for (const auto &node: dataManager_->getNodeBatch(nodeIds)) {
				result.addNode(node);
			}
		}
		return result;
	}

	QueryResult QueryExecutor::executeNodePropertyScan(const QueryPlan &plan) const {
		QueryResult result;
		auto key = plan.getParameter<std::string>("key");
		auto value = plan.getParameter<PropertyValue>("value");

		if (!key || !value) {
			return result;
		}

		auto nodeIds = indexManager_->findNodeIdsByProperty(*key, *value);

		// The mergeDirtyEntities lambda now needs to compare PropertyValues
		unifiedQueryView_->mergeDirtyEntities<Node>(nodeIds, [&](const Node &node) {
			auto properties = dataManager_->getNodeProperties(node.getId());
			auto propIt = properties.find(*key);
			if (propIt != properties.end()) {
				return property_utils::checkPropertyMatch(propIt->second, *value);
			}
			return false;
		});

		if (!nodeIds.empty()) {
			for (const auto &node: dataManager_->getNodeBatch(nodeIds)) {
				result.addNode(node);
			}
		}
		return result;
	}

	QueryResult QueryExecutor::executeNodeLabelPropertyScan(const QueryPlan &plan) const {
		QueryResult result;

		auto label = plan.getParameter<std::string>("label");
		auto key = plan.getParameter<std::string>("key");
		auto value = plan.getParameter<PropertyValue>("value");

		if (!label || !key || !value)
			return result;

		auto labelNodeIds = indexManager_->findNodeIdsByLabel(*label);
		auto propertyNodeIds = indexManager_->findNodeIdsByProperty(*key, *value);

		// Intersect results for efficiency
		std::vector<int64_t> initialIds;
		std::unordered_set<int64_t> propertySet(propertyNodeIds.begin(), propertyNodeIds.end());
		for (int64_t id: labelNodeIds) {
			if (propertySet.contains(id)) {
				initialIds.push_back(id);
			}
		}

		unifiedQueryView_->mergeDirtyEntities<Node>(initialIds, [&](const Node &node) {
			auto properties = dataManager_->getNodeProperties(node.getId());
			auto propIt = properties.find(*key);
			return node.getLabel() == *label && propIt != properties.end() &&
				   property_utils::checkPropertyMatch(propIt->second, *value);
		});

		if (!initialIds.empty()) {
			for (const auto &node: dataManager_->getNodeBatch(initialIds)) {
				result.addNode(node);
			}
		}
		return result;
	}

	QueryResult QueryExecutor::executeEdgeLabelScan(const QueryPlan &plan) const {
		QueryResult result;

		auto label = plan.getParameter<std::string>("label");
		if (!label)
			return result;

		// Use the new index manager interface for edges
		auto edgeIds = indexManager_->findEdgeIdsByLabel(*label);

		unifiedQueryView_->mergeDirtyEntities<Edge>(edgeIds,
													[&](const Edge &edge) { return edge.getLabel() == *label; });

		if (!edgeIds.empty()) {
			for (const auto &edge: dataManager_->getEdgeBatch(edgeIds)) {
				result.addEdge(edge);
			}
		}
		return result;
	}

	QueryResult QueryExecutor::executeEdgePropertyScan(const QueryPlan &plan) const {
		QueryResult result;

		auto key = plan.getParameter<std::string>("key");
		auto value = plan.getParameter<PropertyValue>("value");
		if (!key || !value)
			return result;

		auto edgeIds = indexManager_->findEdgeIdsByProperty(*key, *value);

		unifiedQueryView_->mergeDirtyEntities<Edge>(edgeIds, [&](const Edge &edge) {
			auto properties = dataManager_->getEdgeProperties(edge.getId());
			auto propIt = properties.find(*key);
			if (propIt != properties.end()) {
				return property_utils::checkPropertyMatch(propIt->second, *value);
			}
			return false;
		});

		if (!edgeIds.empty()) {
			for (const auto &edge: dataManager_->getEdgeBatch(edgeIds)) {
				result.addEdge(edge);
			}
		}
		return result;
	}

	QueryResult QueryExecutor::executeTraversalConnectedNodes(const QueryPlan &plan) const {
		QueryResult result;

		auto startNodeId = plan.getParameter<int64_t>("startNodeId");
		if (!startNodeId)
			return result;

		auto direction = plan.getParameter<std::string>("direction");
		std::string directionStr = direction ? *direction : "both";

		auto edgeLabel = plan.getParameter<std::string>("edgeLabel");
		std::string edgeLabelStr = edgeLabel ? *edgeLabel : "";

		auto nodeLabel = plan.getParameter<std::string>("nodeLabel");
		std::string nodeLabelStr = nodeLabel ? *nodeLabel : "";

		// TODO: Create traversalQuery once
		// Create TraversalQuery and execute traversal
		const auto traversalQuery = std::make_shared<TraversalQuery>(dataManager_);

		auto nodes = traversalQuery->findConnectedNodes(*startNodeId, directionStr, edgeLabelStr, nodeLabelStr);
		for (const auto &node: nodes) {
			result.addNode(node);
		}

		return result;
	}

	QueryResult QueryExecutor::executeTraversalShortestPath(const QueryPlan &plan) const {
		QueryResult result;

		auto startNodeId = plan.getParameter<int64_t>("startNodeId");
		auto endNodeId = plan.getParameter<int64_t>("endNodeId");
		auto maxDepth = plan.getParameter<int64_t>("maxDepth");
		auto direction = plan.getParameter<std::string>("direction");

		// The check for existence is also very readable.
		if (!startNodeId || !endNodeId || !maxDepth || !direction) {
			// Here you could log which parameter was missing for better debugging.
			std::cerr << "Error: Missing required parameters for shortest path query." << std::endl;
			return result;
		}

		// Create TraversalQuery and execute shortest path
		auto traversalQuery = std::make_shared<TraversalQuery>(dataManager_);

		auto pathNodes = traversalQuery->findShortestPath(*startNodeId, *endNodeId, *direction);
		for (const auto &node: pathNodes) {
			result.addNode(node);
		}

		return result;
	}

	QueryResult QueryExecutor::executeFullNodeLabelScan(const QueryPlan &plan) const {
		QueryResult result;

		auto label = plan.getParameter<std::string>("label");
		if (!label)
			return result;

		// Get maximum node ID to determine scan range
		int64_t maxNodeId = dataManager_->getIdAllocator()->getCurrentMaxNodeId();
		constexpr int64_t BATCH_SIZE = 1000; // Process in batches for efficiency

		for (int64_t startId = 1; startId <= maxNodeId; startId += BATCH_SIZE) {
			int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxNodeId);
			auto nodes = dataManager_->getNodesInRange(startId, endId);

			for (auto &node: nodes) {
				if (node.getLabel() == *label) {
					result.addNode(node);
				}
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executeFullNodePropertyScan(const QueryPlan &plan) const {
		QueryResult result;

		auto key = plan.getParameter<std::string>("key");
		auto value = plan.getParameter<PropertyValue>("value");
		if (!key || !value)
			return result;

		int64_t maxNodeId = dataManager_->getIdAllocator()->getCurrentMaxNodeId();
		constexpr int64_t BATCH_SIZE = 1000;

		for (int64_t startId = 1; startId <= maxNodeId; startId += BATCH_SIZE) {
			int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxNodeId);
			auto nodes = dataManager_->getNodesInRange(startId, endId);

			for (auto &node: nodes) {
				auto properties = dataManager_->getNodeProperties(node.getId());
				auto propIt = properties.find(*key);

				if (propIt != properties.end() && property_utils::checkPropertyMatch(propIt->second, *value)) {
					result.addNode(node);
				}
			}
		}
		return result;
	}

	QueryResult QueryExecutor::executeFullNodeLabelPropertyScan(const QueryPlan &plan) const {
		QueryResult result;

		auto label = plan.getParameter<std::string>("label");
		auto key = plan.getParameter<std::string>("key");
		auto value = plan.getParameter<PropertyValue>("value");

		if (!label || !key || !value)
			return result;

		// Get maximum node ID to determine scan range
		int64_t maxNodeId = dataManager_->getIdAllocator()->getCurrentMaxNodeId();
		constexpr int64_t BATCH_SIZE = 1000; // Process in batches for efficiency

		for (int64_t startId = 1; startId <= maxNodeId; startId += BATCH_SIZE) {
			int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxNodeId);
			auto nodes = dataManager_->getNodesInRange(startId, endId);

			for (auto &node: nodes) {
				// Check label first (cheaper operation)
				if (node.getLabel() == *label) {
					auto properties = dataManager_->getNodeProperties(node.getId());
					auto propIt = properties.find(*key);

					if (propIt != properties.end() && property_utils::checkPropertyMatch(propIt->second, *value)) {
						result.addNode(node);
					}
				}
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executeFullEdgeLabelScan(const QueryPlan &plan) const {
		QueryResult result;

		auto label = plan.getParameter<std::string>("label");
		if (!label)
			return result;

		// Get the highest possible edge ID to define the scan range.
		const int64_t maxEdgeId = dataManager_->getIdAllocator()->getCurrentMaxEdgeId();
		// TODO: Fix hardcoded batch size
		constexpr int64_t BATCH_SIZE = 1000;

		for (int64_t startId = 1; startId <= maxEdgeId; startId += BATCH_SIZE) {
			const int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxEdgeId);
			auto edges = dataManager_->getEdgesInRange(startId, endId);

			for (auto &edge: edges) {
				// Apply the label filter.
				if (edge.isActive() && edge.getLabel() == *label) {
					result.addEdge(edge);
				}
			}
		}

		return result;
	}

	QueryResult QueryExecutor::executeFullEdgePropertyScan(const QueryPlan &plan) const {
		QueryResult result;

		auto key = plan.getParameter<std::string>("key");
		auto value = plan.getParameter<PropertyValue>("value");
		if (!key || !value)
			return result;

		// Get the highest possible edge ID to define the scan range.
		const int64_t maxEdgeId = dataManager_->getIdAllocator()->getCurrentMaxEdgeId();
		constexpr int64_t BATCH_SIZE = 1000;

		for (int64_t startId = 1; startId <= maxEdgeId; startId += BATCH_SIZE) {
			const int64_t endId = std::min<int64_t>(startId + BATCH_SIZE - 1, maxEdgeId);
			auto edges = dataManager_->getEdgesInRange(startId, endId);

			for (auto &edge: edges) {
				if (!edge.isActive()) {
					continue;
				}

				auto properties = dataManager_->getEdgeProperties(edge.getId());
				auto propIt = properties.find(*key);

				if (propIt != properties.end() && property_utils::checkPropertyMatch(propIt->second, *value)) {
					result.addEdge(edge);
				}
			}
		}

		return result;
	}

} // namespace graph::query
