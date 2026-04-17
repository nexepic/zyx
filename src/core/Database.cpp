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
#include <filesystem>
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/VectorIndexManager.hpp"
#include "graph/storage/wal/WALManager.hpp"
#include "graph/storage/wal/WALRecovery.hpp"

namespace graph {

	Database::Database(const std::string &dbPath, storage::OpenMode mode, size_t cacheSize) : dbPath(dbPath) {
		storage = std::make_shared<storage::FileStorage>(dbPath, cacheSize, mode);
	}

	Database::~Database() { close(); }

	bool Database::isOpen() const {
		// The database is open if the storage is initialized and its file is open.
		return storage && storage->isOpen();
	}

	void Database::open() {
		if (isOpen()) {
			return;
		}

		// Essential initialization — always needed at open()
		storage->open();
		configManager_ = std::make_shared<config::SystemConfigManager>(storage->getSystemStateManager(), storage);
		configManager_->loadAndApplyAll();
		storage->getDataManager()->registerObserver(configManager_);

		// ThreadPool, QueryEngine, WALManager, TransactionManager are deferred
		// to first use via ensure*() methods with std::call_once.
	}

	bool Database::openIfExists() {
		if (isOpen())
			return true;
		if (!exists())
			return false;

		try {
			open();
			return isOpen(); // Verify it's actually open
		} catch (...) {
			return false;
		}
	}

	void Database::close() const {
		if (!isOpen()) {
			return;
		}

		// Close WAL manager (only if it was initialized)
		if (walManager_) {
			walManager_->close();
		}

		storage->close();
	}

	bool Database::exists() const { return std::filesystem::exists(dbPath); }

	Transaction Database::beginTransaction() {
		if (!isOpen()) {
			open();
		}

		// Ensure transaction manager is ready (Phase 1: opens WAL only if recovery needed)
		const_cast<Database *>(this)->ensureWALAndTransactionManager();
		// Ensure WAL file exists for write durability (Phase 2: creates WAL if not yet open)
		const_cast<Database *>(this)->ensureWALForWrites();

		// Create transaction via TransactionManager
		// TransactionManager::begin() acquires the write lock, writes WAL begin,
		// and sets DataManager's transaction context (flag + txnId).
		return transactionManager_->begin();
	}

	Transaction Database::beginReadOnlyTransaction() {
		if (!isOpen()) {
			open();
		}

		// Ensure WAL and transaction manager are ready
		const_cast<Database *>(this)->ensureWALAndTransactionManager();

		// Create read-only transaction via TransactionManager
		return transactionManager_->beginReadOnly();
	}

	bool Database::hasActiveTransaction() const {
		return transactionManager_ && transactionManager_->hasActiveTransaction();
	}

	void Database::setThreadPoolSize(size_t poolSize) {
		threadPool_ = std::make_shared<concurrent::ThreadPool>(poolSize);

		// Re-wire the new pool to all subsystems
		if (storage)
			storage->setThreadPool(threadPool_.get());
		if (queryEngine) {
			queryEngine->setThreadPool(threadPool_.get());
			auto vim = queryEngine->getIndexManager()->getVectorIndexManager();
			if (vim)
				vim->setThreadPool(threadPool_.get());
		}
	}

	// --- Lazy initialization methods ---

	void Database::ensureThreadPool() {
		std::call_once(threadPoolInitFlag_, [this]() {
			size_t poolSize = configManager_->getThreadPoolSize();
			threadPool_ = std::make_shared<concurrent::ThreadPool>(poolSize);
			storage->setThreadPool(threadPool_.get());
		});
	}

	void Database::ensureQueryEngine() {
		std::call_once(queryEngineInitFlag_, [this]() {
			ensureThreadPool();
			queryEngine = std::make_shared<query::QueryEngine>(storage);
			queryEngine->setThreadPool(threadPool_.get());
			queryEngine->setConfigManager(configManager_.get());

			// Wire thread pool to vector index subsystem
			auto vim = queryEngine->getIndexManager()->getVectorIndexManager();
			if (vim)
				vim->setThreadPool(threadPool_.get());
		});
	}

	void Database::ensureWALAndTransactionManager() {
		std::call_once(walInitFlag_, [this]() {
			ensureThreadPool();

			// Only open WAL if a WAL file already exists on disk (crash recovery).
			// For clean databases with only read-only operations (e.g., WASM Playground),
			// we skip WAL creation entirely — TransactionManager handles nullptr safely.
			std::string walFilePath = dbPath + "-wal";
			if (std::filesystem::exists(walFilePath)) {
				walManager_ = std::make_shared<storage::wal::WALManager>();
				walManager_->open(dbPath);

				// WAL Recovery: replay committed transactions that weren't flushed to DB file.
				if (walManager_->needsRecovery()) {
					storage::wal::WALRecovery recovery(
							*walManager_,
							storage->getStorageWriter(),
							storage->getIDAllocators(),
							storage->getSegmentTracker(),
							storage->getStorageIO(),
							storage->getDataManager(),
							storage->getFileHeaderManager());
					recovery.recover();
				}

				storage->getDataManager()->setWALManager(walManager_.get());
			}

			// Always create TransactionManager (walManager_ may be nullptr for pure read-only paths)
			transactionManager_ = std::make_unique<TransactionManager>(storage, walManager_);
		});
	}

	void Database::ensureWALForWrites() {
		std::call_once(walWriteInitFlag_, [this]() {
			// If WAL was already created during recovery in ensureWALAndTransactionManager(), skip
			if (walManager_) {
				return;
			}

			// Create WAL file for the first write transaction
			walManager_ = std::make_shared<storage::wal::WALManager>();
			walManager_->open(dbPath);

			// Wire WAL into TransactionManager for future WAL record writes
			transactionManager_->setWALManager(walManager_);

			// Wire WAL into DataManager for per-entity WAL recording during mutations
			storage->getDataManager()->setWALManager(walManager_.get());
		});
	}

	std::shared_ptr<query::QueryEngine> Database::getQueryEngine() {
		if (!isOpen()) return nullptr;
		ensureQueryEngine();
		return queryEngine;
	}

	std::shared_ptr<concurrent::ThreadPool> Database::getThreadPool() {
		if (!isOpen()) return nullptr;
		ensureThreadPool();
		return threadPool_;
	}

} // namespace graph
