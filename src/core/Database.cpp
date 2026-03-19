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

		storage->open();
		configManager_ = std::make_shared<config::SystemConfigManager>(storage->getSystemStateManager(), storage);
		configManager_->loadAndApplyAll();
		storage->getDataManager()->registerObserver(configManager_);
		queryEngine = std::make_shared<query::QueryEngine>(storage);

		// Initialize WAL manager
		walManager_ = std::make_shared<storage::wal::WALManager>();
		walManager_->open(dbPath);

		// If WAL needs recovery, replay committed transactions
		if (walManager_->needsRecovery()) {
			// For now, just checkpoint (discard incomplete WAL data)
			// Full replay would deserialize entities and apply them
			walManager_->checkpoint();
		}

		// Initialize transaction manager
		transactionManager_ = std::make_unique<TransactionManager>(storage, walManager_);

		// Set WAL manager reference in DataManager
		storage->getDataManager()->setWALManager(walManager_.get());
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

		// Close WAL manager
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

		// Flush any pending changes to ensure clean start
		storage->flush();

		// Create transaction via TransactionManager
		// TransactionManager::begin() acquires the write lock, writes WAL begin,
		// and sets DataManager's transaction context (flag + txnId).
		return transactionManager_->begin();
	}

	bool Database::hasActiveTransaction() const {
		return transactionManager_ && transactionManager_->hasActiveTransaction();
	}

} // namespace graph
