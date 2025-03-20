/**
 * @file QueryEngine.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

#include "graph/storage/FileStorage.h"
#include "graph/core/Node.h"
#include "graph/core/Edge.h"
#include "indexes/IndexManager.h"
#include "QueryResult.h"

namespace graph::query {

class QueryPlanner;
class QueryExecutor;

class QueryEngine {
public:
    explicit QueryEngine(std::shared_ptr<storage::FileStorage> storage);
    ~QueryEngine();

    // Initialize the query engine and build indexes
    bool initialize();

    // Query nodes by label
    QueryResult findNodesByLabel(const std::string& label);

    // Query nodes by property key and value
    QueryResult findNodesByProperty(const std::string& key, const std::string& value);

    // Combined query - nodes with specific label AND property
    QueryResult findNodesByLabelAndProperty(const std::string& label,
                                         const std::string& key,
                                         const std::string& value);

    // Range query for numeric properties
    QueryResult findNodesByPropertyRange(const std::string& key,
                                      double minValue,
                                      double maxValue);

    // Text search within properties (partial matching)
    QueryResult findNodesByTextSearch(const std::string& key,
                                   const std::string& searchText);

    // Find relationships between nodes
    QueryResult findRelationships(uint64_t nodeId,
                               const std::string& edgeLabel = "");

    // Find nodes connected to a specified node
    QueryResult findConnectedNodes(uint64_t nodeId,
                                const std::string& edgeLabel = "",
                                const std::string& direction = "outgoing");

    // Rebuild indexes - call when data has changed significantly
    void rebuildIndexes();

private:
    std::shared_ptr<storage::FileStorage> storage_;
    std::shared_ptr<indexes::IndexManager> indexManager_;
    std::unique_ptr<QueryPlanner> queryPlanner_;
    std::unique_ptr<QueryExecutor> queryExecutor_;
};

} // namespace graph::query