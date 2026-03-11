/**
 * @file Transaction.cpp
 * @author Nexepic
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/core/Transaction.hpp"
#include "graph/core/Database.hpp"
#include <stdexcept>

namespace graph {

// ============================================================================
// Constructor
// ============================================================================

Transaction::Transaction(const Database& db, uint64_t transactionId, uint64_t snapshotId)
    : db_(db)
    , transactionId_(transactionId)
    , startSnapshotId_(snapshotId)
    , state_(State::ACTIVE) {
}

// ============================================================================
// Graph Operations
// ============================================================================

Node Transaction::createNode(const std::string& label) {
    if (!isActive()) {
        throw std::runtime_error("Transaction is not active");
    }

    // TODO: Implement node creation with ID allocation
    // For now, create a temporary node
    Node node;
    node.setLabel(label);

    // Record in WriteSet
    WriteEntry entry;
    entry.type = WriteType::CREATE_NODE;
    entry.entityId = node.getId();
    entry.node = node;
    writeSet_[node.getId()] = entry;

    // Cache locally
    localNodes_[node.getId()] = node;

    return node;
}

Edge Transaction::createEdge(int64_t from, int64_t to, const std::string& label) {
    if (!isActive()) {
        throw std::runtime_error("Transaction is not active");
    }

    // Validate source and target nodes exist
    auto fromNode = getNode(from);
    auto toNode = getNode(to);

    if (!fromNode.has_value()) {
        throw std::runtime_error("Source node does not exist: " + std::to_string(from));
    }

    if (!toNode.has_value()) {
        throw std::runtime_error("Target node does not exist: " + std::to_string(to));
    }

    // TODO: Implement edge creation with ID allocation
    // For now, create a temporary edge
    Edge edge;
    edge.setFromNodeId(from);
    edge.setToNodeId(to);
    edge.setLabel(label);

    // Record in WriteSet
    WriteEntry entry;
    entry.type = WriteType::CREATE_EDGE;
    entry.entityId = edge.getId();
    entry.edge = edge;
    writeSet_[edge.getId()] = entry;

    // Cache locally
    localEdges_[edge.getId()] = edge;

    return edge;
}

void Transaction::deleteNode(int64_t nodeId) {
    if (!isActive()) {
        throw std::runtime_error("Transaction is not active");
    }

    // Check if node exists
    auto node = getNode(nodeId);
    if (!node.has_value()) {
        throw std::runtime_error("Node does not exist: " + std::to_string(nodeId));
    }

    // Record in WriteSet
    WriteEntry entry;
    entry.type = WriteType::DELETE_NODE;
    entry.entityId = nodeId;
    writeSet_[nodeId] = entry;

    // Remove from local cache
    localNodes_.erase(nodeId);
}

void Transaction::deleteEdge(int64_t edgeId) {
    if (!isActive()) {
        throw std::runtime_error("Transaction is not active");
    }

    // Check if edge exists
    auto edge = getEdge(edgeId);
    if (!edge.has_value()) {
        throw std::runtime_error("Edge does not exist: " + std::to_string(edgeId));
    }

    // Record in WriteSet
    WriteEntry entry;
    entry.type = WriteType::DELETE_EDGE;
    entry.entityId = edgeId;
    writeSet_[edgeId] = entry;

    // Remove from local cache
    localEdges_.erase(edgeId);
}

// ============================================================================
// Property Operations
// ============================================================================

void Transaction::setNodeProperty(int64_t nodeId, const std::string& key,
                                 const PropertyValue& value) {
    if (!isActive()) {
        throw std::runtime_error("Transaction is not active");
    }

    // Check if node exists
    auto node = getNode(nodeId);
    if (!node.has_value()) {
        throw std::runtime_error("Node does not exist: " + std::to_string(nodeId));
    }

    // Update local cache
    localNodes_[nodeId].setProperty(key, value);

    // Record in WriteSet
    WriteEntry entry;
    entry.type = WriteType::UPDATE_PROPERTY;
    entry.entityId = nodeId;
    entry.key = key;
    entry.value = value;
    writeSet_[nodeId] = entry;
}

void Transaction::setEdgeProperty(int64_t edgeId, const std::string& key,
                                 const PropertyValue& value) {
    if (!isActive()) {
        throw std::runtime_error("Transaction is not active");
    }

    // Check if edge exists
    auto edge = getEdge(edgeId);
    if (!edge.has_value()) {
        throw std::runtime_error("Edge does not exist: " + std::to_string(edgeId));
    }

    // Update local cache
    localEdges_[edgeId].setProperty(key, value);

    // Record in WriteSet
    WriteEntry entry;
    entry.type = WriteType::UPDATE_PROPERTY;
    entry.entityId = edgeId;
    entry.key = key;
    entry.value = value;
    writeSet_[edgeId] = entry;
}

// ============================================================================
// Query Operations (with MVCC snapshot)
// ============================================================================

std::optional<Node> Transaction::getNode(int64_t nodeId) {
    if (!isActive()) {
        throw std::runtime_error("Transaction is not active");
    }

    // Check local cache first (includes modifications)
    auto localIt = localNodes_.find(nodeId);
    if (localIt != localNodes_.end()) {
        readSet_.insert(nodeId);
        return localIt->second;
    }

    // TODO: Query from storage with MVCC snapshot visibility check
    // For now, return empty
    readSet_.insert(nodeId);
    return std::nullopt;
}

std::optional<Edge> Transaction::getEdge(int64_t edgeId) {
    if (!isActive()) {
        throw std::runtime_error("Transaction is not active");
    }

    // Check local cache first (includes modifications)
    auto localIt = localEdges_.find(edgeId);
    if (localIt != localEdges_.end()) {
        readSet_.insert(edgeId);
        return localIt->second;
    }

    // TODO: Query from storage with MVCC snapshot visibility check
    // For now, return empty
    readSet_.insert(edgeId);
    return std::nullopt;
}

// ============================================================================
// Traversal Operations (with revalidation)
// ============================================================================

std::vector<Edge> Transaction::getOutgoingEdges(int64_t nodeId) {
    if (!isActive()) {
        throw std::runtime_error("Transaction is not active");
    }

    std::vector<Edge> result;

    // TODO: Query outgoing edges from storage
    // For each edge, revalidate that target node still exists

    // Revalidation: Check if target node still exists
    // This prevents traversing to deleted nodes
    for (const auto& edge : result) {
        auto targetNode = getNode(edge.getToNodeId());
        if (!targetNode.has_value()) {
            // Target node was deleted, skip this edge
            continue;
        }
    }

    return result;
}

std::vector<Edge> Transaction::getIncomingEdges(int64_t nodeId) {
    if (!isActive()) {
        throw std::runtime_error("Transaction is not active");
    }

    std::vector<Edge> result;

    // TODO: Query incoming edges from storage
    // For each edge, revalidate that source node still exists

    // Revalidation: Check if source node still exists
    for (const auto& edge : result) {
        auto sourceNode = getNode(edge.getFromNodeId());
        if (!sourceNode.has_value()) {
            // Source node was deleted, skip this edge
            continue;
        }
    }

    return result;
}

// ============================================================================
// Transaction Control
// ============================================================================

void Transaction::commit() {
    if (!isActive()) {
        throw std::runtime_error("Transaction is not active");
    }

    // Transition to COMMITTING state
    state_ = State::COMMITTING;

    // TODO: Validate conflicts with TransactionManager
    // TODO: Apply WriteSet to WAL
    // TODO: Mark transaction as committed

    state_ = State::COMMITTED;
}

void Transaction::rollback() {
    if (!isActive()) {
        throw std::runtime_error("Transaction is not active");
    }

    // Discard all changes
    writeSet_.clear();
    readSet_.clear();
    localNodes_.clear();
    localEdges_.clear();

    state_ = State::ROLLED_BACK;
}

} // namespace graph
