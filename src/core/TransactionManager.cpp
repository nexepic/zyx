/**
 * @file TransactionManager.cpp
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

#include "graph/core/TransactionManager.hpp"
#include "graph/core/Database.hpp"
#include <chrono>
#include <iostream>
#include <thread>

namespace graph {

// ============================================================================
// Constructor
// ============================================================================

TransactionManager::TransactionManager(std::shared_ptr<storage::wal::WALManager> walManager,
                                       const Config& config)
    : walManager_(std::move(walManager))
    , config_(config) {
}

// ============================================================================
// Transaction Lifecycle
// ============================================================================

std::unique_ptr<Transaction> TransactionManager::beginTransaction() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Allocate transaction ID
    uint64_t txnId = nextTransactionId_.fetch_add(1);

    // Create snapshot for MVCC
    uint64_t snapshotId = createSnapshot();

    // Create transaction
    // Note: Using new to access private constructor
    auto transaction = std::unique_ptr<Transaction>(
        new Transaction(*reinterpret_cast<const Database*>(this), txnId, snapshotId));

    // Track active transaction
    TransactionInfo info;
    info.snapshotId = snapshotId;
    activeTransactions_[txnId] = std::move(info);

    // Log transaction begin to WAL
    walManager_->beginTransaction();

    return transaction;
}

void TransactionManager::commit(Transaction* transaction) {
    if (!transaction || !transaction->isActive()) {
        throw std::runtime_error("Invalid transaction state for commit");
    }

    // Execute with retry logic if enabled
    bool success = executeWithRetry([this, transaction]() {
        // Validate conflicts
        if (!validateConflict(transaction)) {
            return false;  // Conflict detected
        }

        // Apply WriteSet to WAL
        applyWriteSetToWAL(transaction);

        // Commit transaction in WAL
        walManager_->commitTransaction(transaction->getTransactionId());

        return true;
    }, config_.maxRetries);

    if (!success) {
        // Max retries exceeded, rollback
        rollback(transaction);
        throw std::runtime_error("Transaction commit failed: max retries exceeded due to conflicts");
    }

    // Mark transaction as committed
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transaction->state_ = Transaction::State::COMMITTED;

        // Remove from active transactions
        activeTransactions_.erase(transaction->getTransactionId());

        // Update entity versions
        for (const auto& [entityId, entry] : transaction->getWriteSet()) {
            entityVersion_[entityId] = transaction->getTransactionId();
        }
    }
}

void TransactionManager::rollback(Transaction* transaction) {
    if (!transaction) {
        return;
    }

    // Call transaction's rollback method
    transaction->rollback();

    // Log rollback to WAL
    walManager_->rollbackTransaction(transaction->getTransactionId());

    // Remove from active transactions
    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeTransactions_.erase(transaction->getTransactionId());
    }
}

// ============================================================================
// Conflict Detection
// ============================================================================

bool TransactionManager::validateConflict(const Transaction* transaction) {
    if (!transaction) {
        return false;
    }

    // Check write-write conflicts
    if (detectWriteWriteConflict(transaction)) {
        std::cout << "Write-write conflict detected in transaction "
                  << transaction->getTransactionId() << "\n";
        return false;
    }

    // Check read-write conflicts
    if (detectReadWriteConflict(transaction)) {
        std::cout << "Read-write conflict detected in transaction "
                  << transaction->getTransactionId() << "\n";
        return false;
    }

    return true;  // No conflicts
}

bool TransactionManager::detectWriteWriteConflict(const Transaction* transaction) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t txnId = transaction->getTransactionId();

    // Check each entity in WriteSet
    for (const auto& [entityId, entry] : transaction->getWriteSet()) {
        // Check if another transaction modified this entity
        auto it = entityVersion_.find(entityId);
        if (it != entityVersion_.end() && it->second != txnId) {
            // Entity was modified by another transaction
            return true;
        }

        // Check active transactions
        for (const auto& [otherTxnId, info] : activeTransactions_) {
            if (otherTxnId == txnId) {
                continue;  // Skip self
            }

            // Check if other transaction also wrote this entity
            if (info.writeSet.count(entityId) > 0) {
                return true;  // Write-write conflict
            }
        }
    }

    return false;  // No write-write conflicts
}

bool TransactionManager::detectReadWriteConflict(const Transaction* transaction) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t txnId = transaction->getTransactionId();

    // Check each entity in ReadSet
    for (int64_t entityId : transaction->getReadSet()) {
        // Check if another transaction modified this entity
        if (isModifiedByOtherTransaction(entityId, txnId)) {
            return true;  // Read-write conflict
        }
    }

    return false;  // No read-write conflicts
}

bool TransactionManager::isModifiedByOtherTransaction(int64_t entityId, uint64_t excludeTxnId) const {
    // Check active transactions
    for (const auto& [txnId, info] : activeTransactions_) {
        if (txnId == excludeTxnId) {
            continue;  // Skip self
        }

        // Check if other transaction wrote this entity
        if (info.writeSet.count(entityId) > 0) {
            return true;
        }
    }

    // Check committed entity versions
    auto it = entityVersion_.find(entityId);
    if (it != entityVersion_.end() && it->second != excludeTxnId) {
        return true;
    }

    return false;
}

// ============================================================================
// Snapshot Management (MVCC)
// ============================================================================

uint64_t TransactionManager::getCurrentSnapshotId() const {
    return currentSnapshotId_.load();
}

uint64_t TransactionManager::createSnapshot() {
    // Increment snapshot ID
    return currentSnapshotId_.fetch_add(1);
}

// ============================================================================
// State Queries
// ============================================================================

size_t TransactionManager::getActiveTransactionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeTransactions_.size();
}

// ============================================================================
// Internal Implementation
// ============================================================================

void TransactionManager::applyWriteSetToWAL(const Transaction* transaction) {
    if (!transaction) {
        return;
    }

    // Apply each WriteSet entry to WAL
    for (const auto& [entityId, entry] : transaction->getWriteSet()) {
        switch (entry.type) {
            case Transaction::WriteType::CREATE_NODE:
                if (entry.node.has_value()) {
                    walManager_->logNodeInsert(*entry.node);
                }
                break;

            case Transaction::WriteType::CREATE_EDGE:
                if (entry.edge.has_value()) {
                    walManager_->logEdgeInsert(*entry.edge);
                }
                break;

            case Transaction::WriteType::DELETE_NODE:
                walManager_->logNodeDelete(entityId);
                break;

            case Transaction::WriteType::DELETE_EDGE:
                walManager_->logEdgeDelete(entityId);
                break;

            case Transaction::WriteType::UPDATE_PROPERTY:
                // Property updates are logged as part of node/edge updates
                // For now, we log the full entity
                // TODO: Optimize to log only changed properties
                break;
        }
    }
}

template<typename Func>
bool TransactionManager::executeWithRetry(Func&& func, uint64_t maxRetries) {
    uint64_t attempt = 0;

    while (attempt < maxRetries) {
        if (func()) {
            return true;  // Success
        }

        // Conflict detected, retry
        attempt++;

        if (attempt < maxRetries) {
            // Exponential backoff
            uint64_t delayMs = config_.retryDelayMs * (1 << attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
    }

    return false;  // Max retries exceeded
}

} // namespace graph
