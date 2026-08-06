/**
 * @file LegacyPhysicalStorageTestHarness.hpp
 * @brief Test-only owner for the isolated legacy physical storage format.
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
 */

#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "graph/config/SystemConfigManager.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/core/TransactionManager.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/state/SystemStateManager.hpp"
#include "graph/storage/wal/WALManager.hpp"
#include "graph/storage/wal/WALRecovery.hpp"

namespace graph::storage::testing {

	/**
	 * Keeps legacy physical test coverage out of the native public Database
	 * boundary. It owns FileStorage and lazily constructs its direct legacy
	 * QueryEngine when a physical query or index test needs one.
	 */
	class LegacyPhysicalStorageTestHarness final {
	public:
		explicit LegacyPhysicalStorageTestHarness(
				std::string path, OpenMode mode = OpenMode::OPEN_CREATE_OR_OPEN_FILE,
				size_t cacheSize = 10'000U) :
			path_(std::move(path)), mode_(mode), cacheSize_(cacheSize) {}

		~LegacyPhysicalStorageTestHarness() {
			try {
				close();
			} catch (...) {
				// Google Test teardown must not throw while preserving its failure.
			}
		}

		LegacyPhysicalStorageTestHarness(const LegacyPhysicalStorageTestHarness &) = delete;
		LegacyPhysicalStorageTestHarness &operator=(const LegacyPhysicalStorageTestHarness &) = delete;

		void open() {
			if (storage_ && storage_->isOpen()) {
				return;
			}
			if (!storage_) {
				storage_ = std::make_shared<FileStorage>(path_, cacheSize_, mode_);
			}
			storage_->open();
		}

		void close() {
			if (storage_ && storage_->isOpen()) {
				// Preserve the legacy database checkpoint boundary before closing WAL.
				if (walManager_) {
					storage_->flushOrThrow();
					if (transactionManager_) {
						transactionManager_->markStorageCheckpointed();
					}
					walManager_->close(wal::WALManager::CloseMode::WCM_REMOVE_FILE,
									   wal::WALManager::CloseSyncMode::WSM_FLUSH_ONLY);
				}
				storage_->close();
			}
			queryEngine_.reset();
			indexManager_.reset();
			// Keep the legacy transaction owner alive through a closed-storage
			// rollback, just as the historical Database did.
		}

		[[nodiscard]] std::shared_ptr<FileStorage> storage() const { return storage_; }
		[[nodiscard]] bool isOpen() const { return storage_ && storage_->isOpen(); }

		void flush() {
			if (storage_ && storage_->isOpen()) {
				storage_->flush();
			}
		}

		[[nodiscard]] std::shared_ptr<query::indexes::IndexManager> indexManager() {
			return queryEngine()->getIndexManager();
		}

		[[nodiscard]] std::shared_ptr<query::QueryEngine> queryEngine() {
			if (!storage_ || !storage_->isOpen()) {
				throw std::logic_error("legacy physical storage must be open before creating a query engine");
			}
			if (!queryEngine_) {
				if (!configManager_) {
					// The physical engine reads query limits from the legacy state store.
					// Keep the manager alive for the raw pointer retained by QueryEngine.
					systemStateManager_ = std::make_shared<state::SystemStateManager>(storage_->getDataManager());
					configManager_ = std::make_shared<config::SystemConfigManager>(systemStateManager_, storage_);
					storage_->getDataManager()->registerObserver(configManager_);
				}
				queryEngine_ = std::make_shared<query::QueryEngine>(storage_);
				queryEngine_->setConfigManager(configManager_.get());
				configManager_->loadAndApplyAll();
			}
			indexManager_ = queryEngine_->getIndexManager();
			return queryEngine_;
		}

		Transaction beginTransaction() {
			ensureTransactionManager();
			ensureWALForWrites();
			return transactionManager_->begin();
		}

		Transaction beginBulkTransaction() {
			ensureTransactionManager();
			ensureWALForWrites();
			return transactionManager_->beginBulk();
		}

		Transaction beginReadOnlyTransaction() {
			ensureTransactionManager();
			return transactionManager_->beginReadOnly();
		}

		[[nodiscard]] bool hasActiveTransaction() const {
			return transactionManager_ && transactionManager_->hasActiveTransaction();
		}

	private:
		void ensureTransactionManager() {
			if (transactionManager_) {
				return;
			}
			if (!storage_ || !storage_->isOpen()) {
				throw std::logic_error("legacy physical storage must be open before starting a transaction");
			}

			const std::string walPath = path_ + "-wal";
			if (std::filesystem::exists(walPath)) {
				walManager_ = std::make_shared<wal::WALManager>();
				walManager_->open(path_);
				if (walManager_->needsRecovery()) {
					wal::WALRecovery recovery(*walManager_, storage_->getStorageWriter(), storage_->getIDAllocators(),
								storage_->getSegmentTracker(), storage_->getStorageIO(),
								storage_->getDataManager(), storage_->getFileHeaderManager());
					recovery.recover();
				}
				storage_->getDataManager()->setWALManager(walManager_.get());
			}
			transactionManager_ = std::make_unique<TransactionManager>(storage_, walManager_);
		}

		void ensureWALForWrites() {
			if (walManager_) {
				return;
			}
			walManager_ = std::make_shared<wal::WALManager>();
			walManager_->open(path_);
			transactionManager_->setWALManager(walManager_);
			storage_->getDataManager()->setWALManager(walManager_.get());
		}

		std::string path_;
		OpenMode mode_;
		size_t cacheSize_;
		std::shared_ptr<FileStorage> storage_;
		std::shared_ptr<query::indexes::IndexManager> indexManager_;
		std::shared_ptr<state::SystemStateManager> systemStateManager_;
		std::shared_ptr<config::SystemConfigManager> configManager_;
		std::shared_ptr<query::QueryEngine> queryEngine_;
		std::shared_ptr<wal::WALManager> walManager_;
		std::unique_ptr<TransactionManager> transactionManager_;
	};

} // namespace graph::storage::testing
