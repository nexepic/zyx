/**
 * @file QueryEngine.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/QueryEngine.hpp"
#include "graph/query/QueryExecutor.hpp"
#include "graph/query/QueryPlanner.hpp"

namespace graph::query {

	QueryEngine::QueryEngine(std::shared_ptr<storage::FileStorage> storage) : storage_(std::move(storage)) {
		indexManager_ = std::make_shared<indexes::IndexManager>(storage_);
		indexManager_->initialize();

		queryPlanner_ = std::make_unique<QueryPlanner>(indexManager_);
		queryExecutor_ = std::make_unique<QueryExecutor>(indexManager_, storage_->getDataManager());
		traversalQuery_ = std::make_shared<TraversalQuery>(storage_->getDataManager());
	}

	QueryEngine::~QueryEngine() = default;

	bool QueryEngine::buildIndexes(const std::string& entityType) const {
		if (entityType != "node" && entityType != "edge") return false;
		return indexManager_->buildIndexes(entityType);
	}

	bool QueryEngine::buildPropertyIndex(const std::string& entityType, const std::string& key) const {
		if (entityType != "node" && entityType != "edge") return false;
		return indexManager_->buildPropertyIndex(entityType, key);
	}

	bool QueryEngine::dropIndex(const std::string& entityType, const std::string& indexType, const std::string& key) const {
		if (entityType != "node" && entityType != "edge") return false;
		return indexManager_->dropIndex(entityType, indexType, key);
	}

	std::vector<std::pair<std::string, std::string>> QueryEngine::listIndexes(const std::string& entityType) const {
		if (entityType != "node" && entityType != "edge") return {};
		return indexManager_->listIndexes(entityType);
	}

	// --- Node Query API ---

	QueryResult QueryEngine::findNodesByLabel(const std::string &label) const {
		auto plan = queryPlanner_->createPlanForNodeLabelQuery(label);
		return queryExecutor_->execute(plan);
	}

	QueryResult QueryEngine::findNodesByProperty(const std::string &key, const std::string &value) const {
		auto plan = queryPlanner_->createPlanForNodePropertyQuery(key, value);
		return queryExecutor_->execute(plan);
	}

	QueryResult QueryEngine::findNodesByLabelAndProperty(const std::string &label, const std::string &key,
														 const std::string &value) const {
		auto plan = queryPlanner_->createPlanForNodeLabelAndPropertyQuery(label, key, value);
		return queryExecutor_->execute(plan);
	}

	// --- Edge Query API ---

	QueryResult QueryEngine::findEdgesByLabel(const std::string& label) const {
		auto plan = queryPlanner_->createPlanForEdgeLabelQuery(label);
		return queryExecutor_->execute(plan);
	}

	QueryResult QueryEngine::findEdgesByProperty(const std::string& key, const std::string& value) const {
		auto plan = queryPlanner_->createPlanForEdgePropertyQuery(key, value);
		return queryExecutor_->execute(plan);
	}

	QueryResult QueryEngine::findConnectedNodes(uint64_t nodeId, const std::string &edgeLabel,
												const std::string &direction, const std::string &nodeLabel) const {
		// Create a traversal plan
		auto plan = queryPlanner_->createPlanForConnectedNodesQuery(nodeId, nodeLabel, edgeLabel, direction);
		return queryExecutor_->execute(plan);
	}

	QueryResult QueryEngine::findShortestPath(int64_t startNodeId, int64_t endNodeId, int maxDepth,
											  const std::string &direction) const {
		auto plan = queryPlanner_->createPlanForTraversalShortestPath(startNodeId, endNodeId, maxDepth, direction);
		return queryExecutor_->execute(plan);
	}

	void QueryEngine::breadthFirstTraversal(int64_t startNodeId, const std::function<bool(const Node &, int)> &visitFn,
											int maxDepth, const std::string &direction) const {
		traversalQuery_->breadthFirstTraversal(startNodeId, visitFn, maxDepth, direction);
	}

} // namespace graph::query
