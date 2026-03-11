/**
 * @file Database.cpp
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

#include "graph/core/Database.hpp"
#include "graph/storage/wal/WALApplier.hpp"
#include <filesystem>
#include <iostream>

namespace graph {

// ============================================================================
// Constructor and Destructor
// ============================================================================

Database::Database(const std::string& dbPath, storage::OpenMode mode, size_t cacheSize)
    : dbPath_(dbPath)
    , openMode_(mode)
    , cacheSize_(cacheSize) {

    // Initialize FileStorage
    storage = std::make_shared<storage::FileStorage>(dbPath, cacheSize, mode);
}

Database::~Database() {
    close();
}

// ============================================================================
// Lifecycle Management
// ============================================================================

void Database::open() {
    if (isOpen()) {
        std::cout << "Database already open, skipping initialization.\n";
		return;
	}

    std::cout << "Opening database: " << dbPath_ << "\n";

    // Step 1: Open FileStorage
    std::cout << "Initializing FileStorage...\n";
    storage->open();
    storage->initializeComponents();

    // Step 2: Initialize WALManager
    std::cout << "Initializing WALManager...\n";
    storage::wal::WALConfig walConfig;
    walConfig.dbPath = dbPath_;
    walConfig.maxBufferSize = 1024 * 1024;  // 1MB buffer
    walConfig.checkpointThreshold = 10 * 1024 * 1024;  // 10MB checkpoint threshold
    walConfig.syncOnCommit = true;  // Fsync on every commit for durability

    walManager_ = std::make_shared<storage::wal::WALManager>(walConfig);
    walManager_->initialize();

    // Step 3: Check if crash recovery is needed
    if (walManager_->needsRecovery()) {
        std::cout << "Crash recovery needed! Performing WAL recovery...\n";

        // Create WALApplier to apply records
        auto walApplier = std::make_unique<storage::wal::WALApplier>(storage);

        // Perform recovery
        walManager_->performRecovery([this, &walApplier](const storage::wal::WALRecord* record) {
            // Apply record via WALApplier
            walApplier->applyRecord(record);
        });

        std::cout << "Crash recovery complete.\n";
    } else {
        std::cout << "No recovery needed (clean shutdown).\n";
    }

    // Step 4: Initialize TransactionManager
    std::cout << "Initializing TransactionManager...\n";
    transactionManager_ = std::make_shared<TransactionManager>(walManager_);

    // Step 5: Set up WAL apply callback for future checkpoints
    auto walApplier = std::make_unique<storage::wal::WALApplier>(storage);
    walManager_->setApplyCallback(walApplier->createApplyCallback());

    // Step 6: Initialize configuration manager
    std::cout << "Initializing SystemConfigManager...\n";
    configManager_ = std::make_shared<config::SystemConfigManager>(storage->getSystemStateManager(), storage);
    configManager_->loadAndApplyAll();
    storage->getDataManager()->registerObserver(configManager_);

    // Step 7: Initialize QueryEngine
    std::cout << "Initializing QueryEngine...\n";
    queryEngine = std::make_shared<query::QueryEngine>(storage);

    isOpen_ = true;
    std::cout << "Database opened successfully.\n";
}

bool Database::openIfExists() {
    if (isOpen()) {
		return true;
	}
	if (!exists()) {
		return false;
	}

	try {
		open();
		return isOpen();
	} catch (const std::exception& e) {
		std::cerr << "Failed to open database: " << e.what() << "\n";
		return false;
	}
}

void Database::close() {
    if (!isOpen()) {
		return;
	}

    std::cout << "Closing database: " << dbPath_ << "\n";

    try {
        // Step 1: Perform checkpoint to apply all WAL records to main database
        std::cout << "Performing final checkpoint...\n";
        uint64_t checkpointLSN = walManager_->checkpoint();
        std::cout << "Checkpoint complete. Last applied LSN: " << checkpointLSN << "\n";

        // Step 2: Close WAL
        std::cout << "Closing WAL...\n";
        walManager_->close();
        walManager_.reset();

        // Step 3: Close FileStorage
        std::cout << "Closing FileStorage...\n";
        storage->close();

        // Step 4: Cleanup
        queryEngine.reset();
        transactionManager_.reset();
        configManager_.reset();

        isOpen_ = false;
        std::cout << "Database closed successfully.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error during database close: " << e.what() << "\n";
        // Attempt cleanup even on error
        isOpen_ = false;
        throw;
    }
}

bool Database::exists() const {
    return std::filesystem::exists(dbPath_);
}

bool Database::isOpen() const {
    return isOpen_ && storage && storage->isOpen();
}

// ============================================================================
// Transaction Management
// ============================================================================

Transaction Database::beginTransaction() {
    if (!isOpen()) {
        throw std::runtime_error("Database is not open. Call open() first.");
    }

    std::cout << "Beginning transaction...\n";

    // Delegate to TransactionManager
    return transactionManager_->beginTransaction();
}

} // namespace graph
