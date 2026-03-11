/**
 * @file Transaction.hpp
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

#pragma once

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include "Edge.hpp"
#include "Node.hpp"
#include "graph/storage/FileStorage.hpp"
#include "metrix/value.hpp"

namespace graph {

class Database;
class TransactionManager;

/**
 * @brief Transaction with MVCC snapshot isolation and OCC conflict detection
 *
 * Features:
 * - WriteSet: Track all modifications (create, update, delete)
 * - ReadSet: Track reads for conflict detection
 * - MVCC snapshot: Read consistent view as of transaction start
 * - OCC: Optimistic concurrency control with conflict detection at commit
 *
 * Lifecycle:
 * 1. begin() → ACTIVE
 * 2. execute operations → ACTIVE
 * 3. commit() → COMMITTING → validate conflicts → COMMITTED or ROLLED_BACK
 * 4. rollback() → ROLLED_BACK
 */
class Transaction {
public:
    /**
     * @brief Transaction state
     */
    enum class State {
        ACTIVE,       // Transaction is active, can execute operations
        COMMITTING,   // In the process of committing (validating conflicts)
        COMMITTED,    // Successfully committed
        ROLLED_BACK   // Rolled back
    };

    /**
     * @brief WriteSet entry types
     */
    enum class WriteType {
        CREATE_NODE,
        CREATE_EDGE,
        DELETE_NODE,
        DELETE_EDGE,
        UPDATE_PROPERTY
    };

    /**
     * @brief WriteSet entry
     */
    struct WriteEntry {
        WriteType type;
        int64_t entityId;        // Node or Edge ID
        std::string key;         // For property updates
        PropertyValue value;     // For property updates

        // For create operations
        std::optional<Node> node;
        std::optional<Edge> edge;
    };

    // ========================================================================
    // Graph Operations
    // ========================================================================

    /**
     * @brief Create a new node
     * @param label Node label
     * @return Created node
     */
    [[nodiscard]] Node createNode(const std::string& label);

    /**
     * @brief Create a new edge
     * @param from Source node ID
     * @param to Target node ID
     * @param label Edge label
     * @return Created edge
     */
    [[nodiscard]] Edge createEdge(int64_t from, int64_t to, const std::string& label);

    /**
     * @brief Delete a node
     * @param nodeId Node ID to delete
     */
    void deleteNode(int64_t nodeId);

    /**
     * @brief Delete an edge
     * @param edgeId Edge ID to delete
     */
    void deleteEdge(int64_t edgeId);

    // ========================================================================
    // Property Operations
    // ========================================================================

    /**
     * @brief Set node property
     * @param nodeId Node ID
     * @param key Property key
     * @param value Property value
     */
    void setNodeProperty(int64_t nodeId, const std::string& key, const PropertyValue& value);

    /**
     * @brief Set edge property
     * @param edgeId Edge ID
     * @param key Property key
     * @param value Property value
     */
    void setEdgeProperty(int64_t edgeId, const std::string& key, const PropertyValue& value);

    // ========================================================================
    // Query Operations (with MVCC snapshot)
    // ========================================================================

    /**
     * @brief Get node by ID (uses MVCC snapshot)
     * @param nodeId Node ID
     * @return Node if found and visible in snapshot
     */
    [[nodiscard]] std::optional<Node> getNode(int64_t nodeId);

    /**
     * @brief Get edge by ID (uses MVCC snapshot)
     * @param edgeId Edge ID
     * @return Edge if found and visible in snapshot
     */
    [[nodiscard]] std::optional<Edge> getEdge(int64_t edgeId);

    // ========================================================================
    // Traversal Operations (with revalidation)
    // ========================================================================

    /**
     * @brief Get outgoing edges from a node (with revalidation)
     * @param nodeId Source node ID
     * @return List of outgoing edges (only to nodes that still exist)
     *
     * Note: Each edge is revalidated to ensure the target node still exists.
     * This prevents traversing to deleted nodes during transaction execution.
     */
    [[nodiscard]] std::vector<Edge> getOutgoingEdges(int64_t nodeId);

    /**
     * @brief Get incoming edges to a node (with revalidation)
     * @param nodeId Target node ID
     * @return List of incoming edges (only from nodes that still exist)
     */
    [[nodiscard]] std::vector<Edge> getIncomingEdges(int64_t nodeId);

    // ========================================================================
    // Transaction Control
    // ========================================================================

    /**
     * @brief Commit transaction
     *
     * Process:
     * 1. Validate conflicts (write-write, read-write)
     * 2. If no conflicts, apply WriteSet to WAL
     * 3. Flush WAL
     * 4. Mark transaction as committed
     *
     * @throws std::runtime_error if conflict detected or transaction already completed
     */
    void commit();

    /**
     * @brief Rollback transaction
     *
     * Discards all changes and marks transaction as rolled back.
     */
    void rollback();

    // ========================================================================
    // State Queries
    // ========================================================================

    /**
     * @brief Get transaction state
     * @return Current state
     */
    [[nodiscard]] State getState() const { return state_; }

    /**
     * @brief Get transaction ID
     * @return Unique transaction ID
     */
    [[nodiscard]] uint64_t getTransactionId() const { return transactionId_; }

    /**
     * @brief Get snapshot ID (MVCC)
     * @return Snapshot ID representing transaction's view
     */
    [[nodiscard]] uint64_t getSnapshotId() const { return startSnapshotId_; }

    /**
     * @brief Check if transaction is active
     * @return true if transaction is active
     */
    [[nodiscard]] bool isActive() const { return state_ == State::ACTIVE; }

    // ========================================================================
    // Internal Access (for TransactionManager)
    // ========================================================================

    /**
     * @brief Get WriteSet (for conflict detection)
     * @return Const reference to WriteSet
     */
    [[nodiscard]] const std::unordered_map<int64_t, WriteEntry>& getWriteSet() const { return writeSet_; }

    /**
     * @brief Get ReadSet (for conflict detection)
     * @return Const reference to ReadSet
     */
    [[nodiscard]] const std::unordered_set<int64_t>& getReadSet() const { return readSet_; }

private:
    // Allow TransactionManager to access private members
    friend class TransactionManager;

    /**
     * @brief Construct transaction (called by TransactionManager)
     * @param db Database reference
     * @param transactionId Transaction ID
     * @param snapshotId MVCC snapshot ID
     */
    Transaction(const Database& db, uint64_t transactionId, uint64_t snapshotId);

    // ========================================================================
    // Member Variables
    // ========================================================================

    const Database& db_;               // Database reference
    uint64_t transactionId_;           // Unique transaction ID
    uint64_t startSnapshotId_;         // MVCC snapshot ID
    State state_;                      // Transaction state

    // WriteSet: All modifications made by this transaction
    std::unordered_map<int64_t, WriteEntry> writeSet_;

    // ReadSet: Entities read by this transaction (for conflict detection)
    std::unordered_set<int64_t> readSet_;

    // Node/Edge cache for local modifications
    std::unordered_map<int64_t, Node> localNodes_;
    std::unordered_map<int64_t, Edge> localEdges_;
};

} // namespace graph
