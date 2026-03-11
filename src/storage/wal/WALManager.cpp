/**
 * @file WALManager.cpp
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

#include "graph/storage/wal/WALManager.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace graph::storage::wal {

// ============================================================================
// Constructor and Destructor
// ============================================================================

WALManager::WALManager(const WALConfig& config)
    : config_(config), walPath_(getWALPath()) {
    // Reserve buffer space
    buffer_.reserve(config_.maxBufferSize);
}

WALManager::~WALManager() {
    if (initialized_) {
        close();
    }
}

// ============================================================================
// Lifecycle Management
// ============================================================================

void WALManager::initialize() {
    std::lock_guard<std::mutex> lock(walMutex_);

    if (initialized_) {
        return;  // Already initialized
    }

    // Open WAL file in append mode (create if doesn't exist)
    walFile_.open(walPath_, std::ios::binary | std::ios::in | std::ios::out);

    if (!walFile_) {
        // File doesn't exist, create it
        walFile_.open(walPath_, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!walFile_) {
            throw std::runtime_error("Failed to create WAL file: " + walPath_);
        }
    }

    // Get current WAL file size
    walFile_.seekg(0, std::ios::end);
    currentWALSize_ = walFile_.tellg();
    walFile_.seekg(0, std::ios::beg);

    initialized_ = true;
}

void WALManager::close() {
    std::lock_guard<std::mutex> lock(walMutex_);

    if (!initialized_) {
        return;
    }

    // Flush any buffered records
    if (bufferSize_ > 0) {
        flushImpl(true);
    }

    // Force checkpoint before closing
    checkpoint();

    // Close WAL file
    if (walFile_.is_open()) {
        walFile_.close();
    }

    initialized_ = false;
}

// ============================================================================
// Write Operations
// ============================================================================

uint64_t WALManager::logNodeInsert(const graph::Node& node) {
    auto record = std::make_unique<NodeRecord>(WALRecordType::NODE_INSERT, node);
    return writeRecord(std::move(record));
}

uint64_t WALManager::logNodeUpdate(const graph::Node& node) {
    auto record = std::make_unique<NodeRecord>(WALRecordType::NODE_UPDATE, node);
    return writeRecord(std::move(record));
}

uint64_t WALManager::logNodeDelete(uint64_t nodeId) {
    graph::Node node;
    node.setId(nodeId);
    auto record = std::make_unique<NodeRecord>(WALRecordType::NODE_DELETE, node);
    return writeRecord(std::move(record));
}

uint64_t WALManager::logEdgeInsert(const graph::Edge& edge) {
    auto record = std::make_unique<EdgeRecord>(WALRecordType::EDGE_INSERT, edge);
    return writeRecord(std::move(record));
}

uint64_t WALManager::logEdgeUpdate(const graph::Edge& edge) {
    auto record = std::make_unique<EdgeRecord>(WALRecordType::EDGE_UPDATE, edge);
    return writeRecord(std::move(record));
}

uint64_t WALManager::logEdgeDelete(uint64_t edgeId) {
    graph::Edge edge;
    edge.setId(edgeId);
    auto record = std::make_unique<EdgeRecord>(WALRecordType::EDGE_DELETE, edge);
    return writeRecord(std::move(record));
}

// ============================================================================
// Transaction Support
// ============================================================================

uint64_t WALManager::beginTransaction() {
    uint64_t txnId = nextTxnId_++;

    // Create transaction begin record
    auto record = std::make_unique<TransactionRecord>(WALRecordType::TRANSACTION_BEGIN, txnId);
    writeRecord(std::move(record));

    // Track transaction state
    transactions_[txnId].state = TransactionState::ACTIVE;

    return txnId;
}

void WALManager::commitTransaction(uint64_t txnId) {
    // Create transaction commit record
    auto record = std::make_unique<TransactionRecord>(WALRecordType::TRANSACTION_COMMIT, txnId);
    writeRecord(std::move(record));

    // Update transaction state
    auto it = transactions_.find(txnId);
    if (it != transactions_.end()) {
        it->second.state = TransactionState::COMMITTED;
    }

    // Force flush on commit for durability
    flush();

    // Check if WAL is large enough to trigger auto-checkpoint
    if (currentWALSize_ >= config_.checkpointThreshold) {
        checkpoint();
    }
}

void WALManager::rollbackTransaction(uint64_t txnId) {
    // Create transaction rollback record
    auto record = std::make_unique<TransactionRecord>(WALRecordType::TRANSACTION_ROLLBACK, txnId);
    writeRecord(std::move(record));

    // Update transaction state
    auto it = transactions_.find(txnId);
    if (it != transactions_.end()) {
        it->second.state = TransactionState::ROLLED_BACK;
    }
}

// ============================================================================
// Flush and Checkpoint
// ============================================================================

void WALManager::flush() {
    std::lock_guard<std::mutex> lock(walMutex_);
    flushImpl(true);
}

void WALManager::flushImpl(bool sync) {
    if (bufferSize_ == 0) {
        return;  // Nothing to flush
    }

    // Write buffer to WAL file
    walFile_.seekp(0, std::ios::end);  // Append to end
    walFile_.write(reinterpret_cast<const char*>(buffer_.data()), bufferSize_);

    if (!walFile_) {
        throw std::runtime_error("Failed to write to WAL file");
    }

    // Update WAL size
    currentWALSize_ += bufferSize_;

    // Clear buffer
    buffer_.clear();
    bufferSize_ = 0;

    // Sync to disk if requested
    if (sync) {
        walFile_.flush();
        #ifdef __unix__
            fsync(walFile_.fd());
        #endif
    }
}

uint64_t WALManager::checkpoint() {
    std::lock_guard<std::mutex> lock(walMutex_);

    // Flush any buffered records first
    if (bufferSize_ > 0) {
        flushImpl(true);
    }

    // Read all WAL records
    auto records = readAllRecords();

    if (records.empty()) {
        return lastCheckpointLSN_.load();  // Nothing to checkpoint
    }

    // Apply committed transaction records to main database
    // Note: The applyFunc is provided by the caller via performRecovery
    // For checkpoint, we just need to track the last applied LSN

    uint64_t maxLSN = 0;
    for (const auto& record : records) {
        if (record->getLSN() > maxLSN) {
            maxLSN = record->getLSN();
        }
    }

    // Update checkpoint LSN
    lastCheckpointLSN_.store(maxLSN);

    // Truncate WAL file
    truncateWAL();

    // Clear old transaction states
    transactions_.clear();

    return maxLSN;
}

// ============================================================================
// Recovery
// ============================================================================

bool WALManager::needsRecovery() const {
    std::lock_guard<std::mutex> lock(walMutex_);

    if (!initialized_) {
        return false;
    }

    // Check if WAL file exists and has content
    return fs::exists(walPath_) && currentWALSize_ > 0 &&
           lastCheckpointLSN_ < currentLSN_;
}

void WALManager::performRecovery(std::function<void(const WALRecord*)> applyFunc) {
    std::lock_guard<std::mutex> lock(walMutex_);

    // Read all WAL records
    auto records = readAllRecords();

    if (records.empty()) {
        return;  // Nothing to recover
    }

    std::cout << "Starting WAL recovery with " << records.size() << " records...\n";

    // Track active transactions
    std::unordered_set<uint64_t> activeTransactions;

    // First pass: identify committed transactions
    for (const auto& record : records) {
        if (record->getType() == WALRecordType::TRANSACTION_BEGIN) {
            activeTransactions.insert(record->getTransactionId());
        } else if (record->getType() == WALRecordType::TRANSACTION_COMMIT) {
            activeTransactions.erase(record->getTransactionId());
            transactions_[record->getTransactionId()].state = TransactionState::COMMITTED;
        } else if (record->getType() == WALRecordType::TRANSACTION_ROLLBACK) {
            activeTransactions.erase(record->getTransactionId());
            transactions_[record->getTransactionId()].state = TransactionState::ROLLED_BACK;
        }
    }

    // Second pass: apply committed transaction records
    size_t appliedCount = 0;
    for (const auto& record : records) {
        uint64_t txnId = record->getTransactionId();

        // Skip records from uncommitted or rolled back transactions
        if (txnId != 0) {
            auto it = transactions_.find(txnId);
            if (it != transactions_.end() &&
                it->second.state != TransactionState::COMMITTED) {
                continue;
            }
            if (activeTransactions.count(txnId) > 0) {
                continue;  // Transaction was active but never committed
            }
        }

        // Apply data operations
        switch (record->getType()) {
            case WALRecordType::NODE_INSERT:
            case WALRecordType::NODE_UPDATE:
            case WALRecordType::NODE_DELETE:
            case WALRecordType::EDGE_INSERT:
            case WALRecordType::EDGE_UPDATE:
            case WALRecordType::EDGE_DELETE:
                applyFunc(record.get());
                appliedCount++;
                break;
            default:
                break;  // Skip transaction control records
        }

        // Update current LSN
        if (record->getLSN() > currentLSN_) {
            currentLSN_.store(record->getLSN());
        }
    }

    std::cout << "WAL recovery complete. Applied " << appliedCount << " records.\n";

    // Checkpoint after recovery to clean WAL
    truncateWAL();
}

// ============================================================================
// Internal Implementation
// ============================================================================

uint64_t WALManager::writeRecord(std::unique_ptr<WALRecord> record) {
    std::lock_guard<std::mutex> lock(walMutex_);

    if (!initialized_) {
        throw std::runtime_error("WALManager not initialized");
    }

    // Assign LSN
    uint64_t lsn = currentLSN_.fetch_add(1);
    record->setLSN(lsn);

    // Serialize record to buffer
    size_t recordSize = record->getSize();
    size_t newBufferSize = bufferSize_ + recordSize;

    // Check if we need to flush first
    if (newBufferSize > config_.maxBufferSize) {
        flushImpl(false);  // Flush without sync (batch sync)
    }

    // Serialize record to temporary buffer
    std::vector<uint8_t> recordBuffer(recordSize);
    record->serialize(recordBuffer.data());

    // Append to main buffer
    buffer_.insert(buffer_.end(), recordBuffer.begin(), recordBuffer.end());
    bufferSize_ += recordSize;

    return lsn;
}

std::vector<std::unique_ptr<WALRecord>> WALManager::readAllRecords() const {
    std::vector<std::unique_ptr<WALRecord>> records;

    if (!walFile_.is_open()) {
        return records;
    }

    // Seek to beginning of WAL file
    walFile_.seekg(0, std::ios::beg);

    // Read all records
    while (walFile_) {
        try {
            auto record = WALRecord::createFromStream(walFile_);
            if (record) {
                records.push_back(std::move(record));
            }
        } catch (const std::exception& e) {
            // End of file or corrupted record
            break;
        }
    }

    return records;
}

void WALManager::truncateWAL() {
    // Close and reopen WAL file to truncate it
    if (walFile_.is_open()) {
        walFile_.close();
    }

    // Truncate file
    walFile_.open(walPath_, std::ios::binary | std::ios::out | std::ios::trunc);
    walFile_.close();

    // Reopen for append
    walFile_.open(walPath_, std::ios::binary | std::ios::in | std::ios::out);

    currentWALSize_ = 0;
}

std::string WALManager::getWALPath() const {
    // Return <dbPath>.wal
    return config_.dbPath + ".wal";
}

} // namespace graph::storage::wal
