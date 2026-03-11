/**
 * @file WALManager.hpp
 * @author Nexepic
 * @date 2025/3/24
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

#include <atomic>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "WALRecordImpl.hpp"

namespace graph::storage::wal {

/**
 * @brief Configuration for Write-Ahead Log manager
 *
 * Designed for embedded graph databases (similar to SQLite).
 * No background threads, single WAL file, predictable lifecycle.
 */
struct WALConfig {
    // Path to database file (WAL will be <dbPath>.wal)
    std::string dbPath = "./metrix.db";

    // Maximum buffer size before auto-flush (1MB default)
    size_t maxBufferSize = 1024 * 1024;

    // WAL size threshold for auto-checkpoint (10MB default)
    size_t checkpointThreshold = 10 * 1024 * 1024;

    // Force fsync on every transaction commit
    bool syncOnCommit = true;
};

/**
 * @brief Write-Ahead Log Manager for embedded graph database
 *
 * Key features:
 * - Single WAL file (<dbPath>.wal)
 * - No background threads (suitable for embedded use)
 * - Buffer management for efficient writes
 * - Checkpoint applies WAL to main database
 * - Crash recovery on database open
 *
 * Lifecycle:
 * 1. Open database → Check if WAL exists → Perform recovery if needed
 * 2. Transaction commit → Write to WAL → fsync → Apply to main storage
 * 3. Close database → Force checkpoint → Truncate WAL
 */
class WALManager {
public:
    /**
     * @brief Construct WAL manager
     * @param config Configuration settings
     */
    explicit WALManager(const WALConfig& config);
    ~WALManager();

    // =========================================================================
    // Lifecycle Management
    // =========================================================================

    /**
     * @brief Initialize WAL system
     *
     * Creates WAL file if needed, performs recovery if WAL exists from crash.
     * Must be called before any other operations.
     */
    void initialize();

    /**
     * @brief Close WAL system
     *
     * Flushes all buffered records, forces checkpoint, and closes WAL file.
     * Called automatically by destructor.
     */
    void close();

    // =========================================================================
    // Write Operations
    // =========================================================================

    /**
     * @brief Log node insert operation
     * @param node Node to insert
     * @return Log Sequence Number (LSN)
     */
    uint64_t logNodeInsert(const graph::Node& node);

    /**
     * @brief Log node update operation
     * @param node Node with updated data
     * @return Log Sequence Number (LSN)
     */
    uint64_t logNodeUpdate(const graph::Node& node);

    /**
     * @brief Log node delete operation
     * @param nodeId ID of node to delete
     * @return Log Sequence Number (LSN)
     */
    uint64_t logNodeDelete(uint64_t nodeId);

    /**
     * @brief Log edge insert operation
     * @param edge Edge to insert
     * @return Log Sequence Number (LSN)
     */
    uint64_t logEdgeInsert(const graph::Edge& edge);

    /**
     * @brief Log edge update operation
     * @param edge Edge with updated data
     * @return Log Sequence Number (LSN)
     */
    uint64_t logEdgeUpdate(const graph::Edge& edge);

    /**
     * @brief Log edge delete operation
     * @param edgeId ID of edge to delete
     * @return Log Sequence Number (LSN)
     */
    uint64_t logEdgeDelete(uint64_t edgeId);

    // =========================================================================
    // Transaction Support
    // =========================================================================

    /**
     * @brief Begin a new transaction
     * @return Transaction ID
     */
    uint64_t beginTransaction();

    /**
     * @brief Commit transaction
     * @param txnId Transaction ID
     *
     * Forces WAL flush to ensure durability.
     */
    void commitTransaction(uint64_t txnId);

    /**
     * @brief Rollback transaction
     * @param txnId Transaction ID
     *
     * Marks transaction as rolled back in WAL (for recovery).
     */
    void rollbackTransaction(uint64_t txnId);

    // =========================================================================
    // Flush and Checkpoint
    // =========================================================================

    /**
     * @brief Flush buffer to disk
     *
     * Writes all buffered records to WAL file and calls fsync.
     * Called automatically on transaction commit.
     */
    void flush();

    /**
     * @brief Create checkpoint
     *
     * Applies all committed WAL records to main database file,
     * then truncates WAL. This is the critical operation that
     * makes WAL changes persistent in the main database.
     *
     * @return Last applied LSN
     */
    uint64_t checkpoint();

    // =========================================================================
    // Recovery
    // =========================================================================

    /**
     * @brief Check if recovery is needed
     * @return true if WAL file exists and has unapplied records
     */
    [[nodiscard]] bool needsRecovery() const;

    /**
     * @brief Perform crash recovery
     * @param applyFunc Function to apply each WAL record to main storage
     *
     * Reads WAL file, identifies committed transactions,
     * and applies their records to main database.
     */
    void performRecovery(std::function<void(const WALRecord*)> applyFunc);

    // =========================================================================
    // State Queries
    // =========================================================================

    /**
     * @brief Get current Log Sequence Number
     * @return Current LSN (next LSN to be assigned)
     */
    [[nodiscard]] uint64_t getCurrentLSN() const { return currentLSN_.load(); }

    /**
     * @brief Get last checkpoint LSN
     * @return LSN of last checkpoint
     */
    [[nodiscard]] uint64_t getLastCheckpointLSN() const { return lastCheckpointLSN_.load(); }

    /**
     * @brief Get current WAL file size
     * @return Size in bytes
     */
    [[nodiscard]] size_t getWALSize() const { return currentWALSize_.load(); }

private:
    // =========================================================================
    // Internal Implementation
    // =========================================================================

    /**
     * @brief Write a record to WAL
     * @param record Record to write (takes ownership)
     * @return Assigned LSN
     */
    uint64_t writeRecord(std::unique_ptr<WALRecord> record);

    /**
     * @brief Internal flush implementation
     * @param sync Whether to call fsync after write
     */
    void flushImpl(bool sync);

    /**
     * @brief Read all records from WAL file
     * @return Vector of WAL records
     */
    std::vector<std::unique_ptr<WALRecord>> readAllRecords() const;

    /**
     * @brief Truncate WAL file after checkpoint
     */
    void truncateWAL();

    /**
     * @brief Get WAL file path from database path
     * @return WAL file path (<dbPath>.wal)
     */
    [[nodiscard]] std::string getWALPath() const;

    // =========================================================================
    // Member Variables
    // =========================================================================

    // Configuration
    WALConfig config_;

    // WAL file
    std::string walPath_;
    std::fstream walFile_;

    // Buffer management (no background thread)
    std::vector<uint8_t> buffer_;
    size_t bufferSize_ = 0;

    // LSN tracking
    std::atomic<uint64_t> currentLSN_{1};       // Next LSN to assign
    std::atomic<uint64_t> lastCheckpointLSN_{0}; // Last checkpoint LSN
    std::atomic<size_t> currentWALSize_{0};     // Current WAL file size

    // Transaction tracking for recovery
    struct TransactionState {
        enum State { ACTIVE, COMMITTED, ROLLED_BACK };
        State state = ACTIVE;
    };
    std::unordered_map<uint64_t, TransactionState> transactions_;

    // Thread safety
    mutable std::mutex walMutex_;

    // Transaction ID generator
    std::atomic<uint64_t> nextTxnId_{1};

    // Initialization state
    bool initialized_ = false;
};

} // namespace graph::storage::wal
