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
		queryPlanner_ = std::make_unique<QueryPlanner>(indexManager_);
		queryExecutor_ = std::make_unique<QueryExecutor>(indexManager_, storage_);
		traversalQuery_ = std::make_shared<TraversalQuery>(storage_->getDataManager());
	}

	QueryEngine::~QueryEngine() = default;

	bool QueryEngine::buildLabelIndex() {
		return indexManager_->buildLabelIndex();
	}

	bool QueryEngine::buildPropertyIndex(const std::string& key) {
		return indexManager_->buildPropertyIndex(key);
	}

	bool QueryEngine::dropIndex(const std::string& indexType, const std::string& key) {
		return indexManager_->dropIndex(indexType, key);
	}

	std::vector<std::pair<std::string, std::string>> QueryEngine::listIndexes() {
		return indexManager_->listIndexes();
	}

	QueryResult QueryEngine::findNodesByLabel(const std::string &label) {
		auto plan = queryPlanner_->createPlanForLabelQuery(label);
		return queryExecutor_->execute(plan);
	}

	QueryResult QueryEngine::findNodesByProperty(const std::string &key, const std::string &value) {
		auto plan = queryPlanner_->createPlanForPropertyQuery(key, value);
		return queryExecutor_->execute(plan);
	}

	QueryResult QueryEngine::findNodesByLabelAndProperty(const std::string &label, const std::string &key,
														 const std::string &value) {
		auto plan = queryPlanner_->createPlanForLabelAndPropertyQuery(label, key, value);
		return queryExecutor_->execute(plan);
	}

	QueryResult QueryEngine::findNodesByPropertyRange(const std::string &key, double minValue, double maxValue) {
		auto plan = queryPlanner_->createPlanForPropertyRangeQuery(key, minValue, maxValue);
		return queryExecutor_->execute(plan);
	}

	QueryResult QueryEngine::findNodesByTextSearch(const std::string &key, const std::string &searchText) {
		auto plan = queryPlanner_->createPlanForTextSearchQuery(key, searchText);
		return queryExecutor_->execute(plan);
	}

	QueryResult QueryEngine::findRelationships(uint64_t nodeId, const std::string &edgeLabel) {
		auto plan = queryPlanner_->createPlanForRelationshipQuery(nodeId, edgeLabel);
		return queryExecutor_->execute(plan);
	}

	QueryResult QueryEngine::findConnectedNodes(uint64_t nodeId, const std::string &edgeLabel,
												const std::string &direction) {
		auto plan = queryPlanner_->createPlanForConnectedNodesQuery(nodeId, edgeLabel, direction);
		return queryExecutor_->execute(plan);
	}

	QueryResult QueryEngine::findConnectedNodesTraversal(int64_t startNodeId, const std::string &direction,
														 const std::string &edgeLabel, const std::string &nodeLabel) {
		auto nodes = traversalQuery_->findConnectedNodes(startNodeId, direction, edgeLabel, nodeLabel);
		return nodesToQueryResult(nodes);
	}

	QueryResult QueryEngine::findShortestPath(int64_t startNodeId, int64_t endNodeId, int maxDepth,
											  const std::string &direction) {
		auto pathNodes = traversalQuery_->findShortestPath(startNodeId, endNodeId, maxDepth, direction);
		return nodesToQueryResult(pathNodes);
	}

	void QueryEngine::breadthFirstTraversal(int64_t startNodeId, const std::function<bool(const Node &, int)> &visitFn,
											int maxDepth, const std::string &direction) {
		traversalQuery_->breadthFirstTraversal(startNodeId, visitFn, maxDepth, direction);
	}

	// Helper method to convert vector of Nodes to QueryResult
	QueryResult QueryEngine::nodesToQueryResult(const std::vector<Node> &nodes) {
		QueryResult result;
		for (const auto &node: nodes) {
			result.addNode(node);
		}
		return result;
	}

	void QueryEngine::rebuildIndexes() { indexManager_->buildIndexes(); }

} // namespace graph::query
