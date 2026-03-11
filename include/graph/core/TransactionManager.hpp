/**
 * @file TransactionManager.hpp
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

#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "graph/core/Transaction.hpp"
#include "graph/storage/wal/WALManager.hpp"

namespace graph {

/**
 * @brief Transaction Manager with OCC and MVCC support
 *
 * Features:
 * - Transaction lifecycle management
 * - OCC conflict detection (write-write, read-write)
 * - MVCC snapshot management
 * - Transaction ID allocation
 *
 * Concurrency Model:
 * - Optimistic: No locks during transaction execution
 * - Conflict detection at commit time
 * - Automatic retry on conflict (configurable)
 *
 * Isolation Level:
 * - Read Committed: See only committed data
 * - MVCC snapshots: Each transaction gets consistent view
 */
class TransactionManager {
public:
    /**
     * @brief Configuration for transaction manager
     */
    struct Config {
        uint64_t maxRetries = 3;                    // Max retry attempts on conflict
        uint64_t retryDelayMs = 100;                // Delay between retries (ms)
        bool enableAutoRetry = true;                // Automatically retry on conflict
    };

    /**
     * @brief Construct transaction manager
     * @param walManager WAL manager for logging
     * @param config Configuration settings
     */
    explicit TransactionManager(std::shared_ptr<storage::wal::WALManager> walManager,
                                const Config& config = Config{});
    ~TransactionManager() = default;

    // =========================================================================
    // Transaction Lifecycle
    // =========================================================================

    /**
     * @brief Begin a new transaction
     * @return Unique pointer to transaction
     *
     * Creates transaction with:
     * - Unique transaction ID
     * - MVCC snapshot ID
     * - ACTIVE state
     */
    [[nodiscard]] std::unique_ptr<Transaction> beginTransaction();

    /**
     * @brief Commit transaction
     * @param transaction Transaction to commit
     *
     * Process:
     * 1. Validate conflicts (OCC)
     * 2. If conflict and auto-retry enabled: rollback and retry
     * 3. If no conflict: apply WriteSet to WAL
     * 4. Flush WAL
     * 5. Mark transaction as committed
     *
     * @throws std::runtime_error if validation fails or max retries exceeded
     */
    void commit(Transaction* transaction);

    /**
     * @brief Rollback transaction
     * @param transaction Transaction to rollback
     *
     * Discards all changes and marks transaction as rolled back.
     */
    void rollback(Transaction* transaction);

    // =========================================================================
    // Conflict Detection
    // =========================================================================

    /**
     * @brief Validate transaction for conflicts
     * @param transaction Transaction to validate
     * @return true if no conflicts, false otherwise
     *
     * Checks:
     * - Write-write conflicts: Did another transaction modify same entities?
     * - Read-write conflicts: Did another transaction modify entities we read?
     */
    [[nodiscard]] bool validateConflict(const Transaction* transaction);

    // =========================================================================
    // Snapshot Management (MVCC)
    // =========================================================================

    /**
     * @brief Get current snapshot ID
     * @return Current snapshot ID representing latest committed state
     */
    [[nodiscard]] uint64_t getCurrentSnapshotId() const;

    /**
     * @brief Create new snapshot
     * @return New snapshot ID
     *
     * Called when beginning a transaction to establish MVCC view.
     */
    [[nodiscard]] uint64_t createSnapshot();

    // =========================================================================
    // State Queries
    // =========================================================================

    /**
     * @brief Get number of active transactions
     * @return Count of transactions in ACTIVE state
     */
    [[nodiscard]] size_t getActiveTransactionCount() const;

    /**
     * @brief Get configuration
     * @return Current configuration
     */
    [[nodiscard]] const Config& getConfig() const { return config_; }

    /**
     * @brief Get WAL manager
     * @return WAL manager
     */
    [[nodiscard]] std::shared_ptr<storage::wal::WALManager> getWALManager() const {
        return walManager_;
    }

private:
    // =========================================================================
    // Internal Implementation
    // =========================================================================

    /**
     * @brief Detect write-write conflicts
     * @param transaction Transaction to check
     * @return true if conflicts detected
     */
    [[nodiscard]] bool detectWriteWriteConflict(const Transaction* transaction);

    /**
     * @brief Detect read-write conflicts
     * @param transaction Transaction to check
     * @return true if conflicts detected
     */
    [[nodiscard]] bool detectReadWriteConflict(const Transaction* transaction);

    /**
     * @brief Check if entity was modified by another transaction
     * @param entityId Entity ID to check
     * @param excludeTxnId Transaction ID to exclude (current transaction)
     * @return true if entity was modified
     */
    [[nodiscard]] bool isModifiedByOtherTransaction(int64_t entityId,
                                                     uint64_t excludeTxnId) const;

    /**
     * @brief Apply transaction WriteSet to WAL
     * @param transaction Transaction with WriteSet to apply
     */
    void applyWriteSetToWAL(const Transaction* transaction);

    /**
     * @brief Execute transaction with retry logic
     * @param func Function to execute (returns true on success)
     * @param maxRetries Maximum retry attempts
     * @return true if succeeded, false otherwise
     */
    template<typename Func>
    bool executeWithRetry(Func&& func, uint64_t maxRetries);

    // =========================================================================
    // Member Variables
    // =========================================================================

    // WAL manager for logging
    std::shared_ptr<storage::wal::WALManager> walManager_;

    // Configuration
    Config config_;

    // Transaction ID allocation
    std::atomic<uint64_t> nextTransactionId_{1};

    // Snapshot ID allocation (MVCC)
    std::atomic<uint64_t> currentSnapshotId_{0};

    // Active transaction tracking
    struct TransactionInfo {
        uint64_t snapshotId;
        std::unordered_set<int64_t> writeSet;  // Entities modified
        std::unordered_set<int64_t> readSet;   // Entities read
    };
    std::unordered_map<uint64_t, TransactionInfo> activeTransactions_;

    // Global write tracking for conflict detection
    // Tracks all entities modified by any transaction
    std::unordered_map<int64_t, uint64_t> entityVersion_;

    // Thread safety
    mutable std::mutex mutex_;
};

} // namespace graph
