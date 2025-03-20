/**
 * @file QueryEngine.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/query/QueryEngine.h"
#include "graph/core/query/QueryPlanner.h"
#include "graph/core/query/QueryExecutor.h"

namespace graph::query {

QueryEngine::QueryEngine(std::shared_ptr<storage::FileStorage> storage)
    : storage_(std::move(storage)) {
    indexManager_ = std::make_shared<indexes::IndexManager>(storage_);
    queryPlanner_ = std::make_unique<QueryPlanner>(indexManager_);
    queryExecutor_ = std::make_unique<QueryExecutor>(indexManager_, storage_);
}

QueryEngine::~QueryEngine() = default;

bool QueryEngine::initialize() {
    return indexManager_->buildIndexes();
}

QueryResult QueryEngine::findNodesByLabel(const std::string& label) {
    auto plan = queryPlanner_->createPlanForLabelQuery(label);
    return queryExecutor_->execute(plan);
}

QueryResult QueryEngine::findNodesByProperty(const std::string& key, const std::string& value) {
    auto plan = queryPlanner_->createPlanForPropertyQuery(key, value);
    return queryExecutor_->execute(plan);
}

QueryResult QueryEngine::findNodesByLabelAndProperty(const std::string& label,
                                                  const std::string& key,
                                                  const std::string& value) {
    auto plan = queryPlanner_->createPlanForLabelAndPropertyQuery(label, key, value);
    return queryExecutor_->execute(plan);
}

QueryResult QueryEngine::findNodesByPropertyRange(const std::string& key,
                                               double minValue,
                                               double maxValue) {
    auto plan = queryPlanner_->createPlanForPropertyRangeQuery(key, minValue, maxValue);
    return queryExecutor_->execute(plan);
}

QueryResult QueryEngine::findNodesByTextSearch(const std::string& key,
                                            const std::string& searchText) {
    auto plan = queryPlanner_->createPlanForTextSearchQuery(key, searchText);
    return queryExecutor_->execute(plan);
}

QueryResult QueryEngine::findRelationships(uint64_t nodeId,
                                        const std::string& edgeLabel) {
    auto plan = queryPlanner_->createPlanForRelationshipQuery(nodeId, edgeLabel);
    return queryExecutor_->execute(plan);
}

QueryResult QueryEngine::findConnectedNodes(uint64_t nodeId,
                                         const std::string& edgeLabel,
                                         const std::string& direction) {
    auto plan = queryPlanner_->createPlanForConnectedNodesQuery(nodeId, edgeLabel, direction);
    return queryExecutor_->execute(plan);
}

void QueryEngine::rebuildIndexes() {
    indexManager_->buildIndexes();
}

} // namespace graph::query