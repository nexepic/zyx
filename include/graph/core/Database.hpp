/**
 * @file Database.hpp
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
#include <string>

#include "graph/config/SystemConfigManager.hpp"
#include "graph/core/Transaction.hpp"
#include "graph/core/TransactionManager.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/wal/WALManager.hpp"

namespace graph {

/**
 * @brief Metrix Graph Database
 *
 * Main entry point for database operations. Provides ACID transactions via WAL,
 * crash recovery, and query engine integration.
 *
 * Features:
 * - Automatic crash recovery on open()
 * - Automatic checkpoint on close()
 * - ACID transactions with MVCC and OCC
 * - Query engine for Cypher queries
 * - Configuration management
 *
 * Lifecycle:
 * 1. Construct Database object
 * 2. Call open() - performs crash recovery if needed
 * 3. Execute operations (queries, transactions)
 * 4. Call close() - performs checkpoint
 */
class Database {
public:
	/**
	 * @brief Construct database
	 * @param dbPath Path to database file
	 * @param mode Open mode (create if not exists, open existing, etc.)
	 * @param cacheSize Size of cache in number of entities
	 */
	explicit Database(const std::string& dbPath,
	                  storage::OpenMode mode = storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE,
	                  size_t cacheSize = 10000);

	~Database();

	// ========================================================================
	// Lifecycle Management
	// ========================================================================

	/**
	 * @brief Open database
	 *
	 * Process:
	 * 1. Initialize FileStorage
	 * 2. Initialize WALManager
	 * 3. Initialize TransactionManager
	 * 4. Perform crash recovery if WAL exists
	 * 5. Initialize QueryEngine
	 *
	 * @throws std::runtime_error if database cannot be opened
	 */
	void open();

	/**
	 * @brief Open database if it exists
	 * @return true if database opened successfully
	 */
	[[nodiscard]] bool openIfExists();

	/**
	 * @brief Close database
	 *
	 * Process:
	 * 1. Perform checkpoint to apply WAL to main database
	 * 2. Close WAL
	 * 3. Close FileStorage
	 * 4. Cleanup resources
	 */
	void close();

	/**
	 * @brief Check if database file exists
	 * @return true if database file exists
	 */
	[[nodiscard]] bool exists() const;

	/**
	 * @brief Check if database is open
	 * @return true if database is open
	 */
	[[nodiscard]] bool isOpen() const;

	// ========================================================================
	// Transaction Management
	// ========================================================================

	/**
	 * @brief Begin a new transaction
	 * @return Transaction object
	 *
	 * Creates transaction with:
	 * - Unique transaction ID
	 * - MVCC snapshot
	 * - ACID guarantees
	 *
	 * @throws std::runtime_error if database is not open
	 */
	Transaction beginTransaction();

	// ========================================================================
	// Query Engine Access
	// ========================================================================

	/**
	 * @brief Get query engine
	 * @return Query engine for Cypher queries
	 */
	std::shared_ptr<query::QueryEngine> getQueryEngine() { return queryEngine; }

	// ========================================================================
	// Storage Access (Advanced)
	// ========================================================================

	/**
	 * @brief Get file storage
	 * @return File storage for low-level operations
	 */
	[[nodiscard]] std::shared_ptr<storage::FileStorage> getStorage() const { return storage; }

	/**
	 * @brief Get WAL manager
	 * @return WAL manager for transaction logging
	 */
	[[nodiscard]] std::shared_ptr<storage::wal::WALManager> getWALManager() const { return walManager_; }

	/**
	 * @brief Get transaction manager
	 * @return Transaction manager for transaction coordination
	 */
	[[nodiscard]] std::shared_ptr<TransactionManager> getTransactionManager() const {
		return transactionManager_;
	}

private:
	// ========================================================================
	// Member Variables
	// ========================================================================

	// Core components
	std::shared_ptr<storage::FileStorage> storage;
	std::shared_ptr<storage::wal::WALManager> walManager_;
	std::shared_ptr<TransactionManager> transactionManager_;
	std::shared_ptr<query::QueryEngine> queryEngine;
	std::shared_ptr<config::SystemConfigManager> configManager_;

	// Configuration
	std::string dbPath_;
	storage::OpenMode openMode_;
	size_t cacheSize_;

	// State
	bool isOpen_ = false;
};

} // namespace graph
