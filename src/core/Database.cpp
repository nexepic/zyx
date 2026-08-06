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
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"

namespace graph {

	Database::Database(const std::string &dbPath, storage::OpenMode mode, size_t cacheSize) :
		dbPath(dbPath), openMode_(mode), cacheSize_(cacheSize) {}

	Database::~Database() {
		try {
			close();
		} catch (...) {
			// Destructors must not throw. The relocatable backend retains recovery state.
		}
	}

	bool Database::isOpen() const { return backend_ && backend_->isOpen(); }

	std::shared_ptr<storage::LogicalGraphWriteBackend> Database::getLogicalGraphWriteBackend() const {
		return std::dynamic_pointer_cast<storage::LogicalGraphWriteBackend>(backend_);
	}

	std::shared_ptr<storage::LogicalGraphTransactionalBackend>
	Database::getLogicalGraphTransactionalBackend() const {
		return std::dynamic_pointer_cast<storage::LogicalGraphTransactionalBackend>(backend_);
	}

	void Database::open() {
		if (isOpen()) {
			return;
		}

		debug::ScopedPerfTimer totalTimer("db.open.total");
		// Public Database creation is exclusively native relocatable.
		{
			debug::ScopedPerfTimer storageTimer("db.open.storage");
			if (!backend_) {
				storage::StorageBackendSelection selection =
						storage::StorageBackendFactory::create(dbPath, cacheSize_, openMode_);
				backend_ = std::move(selection.backend);
				relocatableStorage_ = std::move(selection.relocatable_storage);
				if (!relocatableStorage_) {
					throw std::logic_error("public Database requires a native relocatable storage backend");
				}
			}
			backend_->open();
		}

		// ThreadPool and QueryEngine are deferred to first use.
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

	void Database::flush() const {
		if (!isOpen()) {
			return;
		}

		backend_->flush();
	}

	void Database::close() const {
		if (!isOpen()) {
			return;
		}

		debug::ScopedPerfTimer totalTimer("db.close.total");
		debug::ScopedPerfTimer storageTimer("db.close.storage");
		backend_->close();
	}

	bool Database::exists() const { return std::filesystem::exists(dbPath); }

	Transaction Database::beginTransaction() {
		if (!isOpen()) {
			open();
		}
		return beginLogicalTransaction(false);
	}

	Transaction Database::beginBulkTransaction() {
		if (!isOpen()) {
			open();
		}
		return beginLogicalTransaction(false);
	}

	Transaction Database::beginReadOnlyTransaction() {
		if (!isOpen()) {
			open();
		}
		return beginLogicalTransaction(true);
	}

	bool Database::hasActiveTransaction() const {
		return activeLogicalWriteTransactions_->load(std::memory_order_acquire) != 0U;
	}

	Transaction Database::beginLogicalTransaction(bool readOnly) {
		auto logicalTransactions = getLogicalGraphTransactionalBackend();
		if (!logicalTransactions) {
			throw std::logic_error("database backend does not provide logical transactions");
		}
		const uint64_t transactionId = nextLogicalTransactionId_.fetch_add(1U, std::memory_order_relaxed);
		return Transaction(transactionId, logicalTransactions->beginLogicalGraphTransaction(readOnly),
						   activeLogicalWriteTransactions_);
	}

	void Database::setThreadPoolSize(size_t poolSize) {
		configuredThreadPoolSize_ = poolSize;

		// If the pool has not been lazily initialized yet, defer construction so
		// ensureThreadPool() can wire the explicitly configured size exactly once.
		if (!threadPool_) {
			return;
		}

		threadPool_ = std::make_shared<concurrent::ThreadPool>(poolSize);

		if (queryEngine) {
			queryEngine->setThreadPool(threadPool_.get());
		}
	}

	// --- Lazy initialization methods ---

	void Database::ensureThreadPool() {
		std::call_once(threadPoolInitFlag_, [this]() {
			debug::ScopedPerfTimer timer("db.ensure_thread_pool");
			if (configuredThreadPoolSize_.has_value()) {
				threadPool_ = std::make_shared<concurrent::ThreadPool>(*configuredThreadPoolSize_);
			} else {
				// Preserve ThreadPool's auto-detection policy rather than converting
				// the detected value into an indistinguishable manual override.
				threadPool_ = std::make_shared<concurrent::ThreadPool>();
			}
		});
	}

	void Database::ensureQueryEngine() {
		std::call_once(queryEngineInitFlag_, [this]() {
			debug::ScopedPerfTimer timer("db.ensure_query_engine");
			ensureThreadPool();
			if (relocatableStorage_) {
				query::LogicalReadExecutionOptions options;
				std::error_code error;
				const std::filesystem::path databaseDirectory =
						std::filesystem::absolute(std::filesystem::path(dbPath).parent_path(), error);
				if (error || databaseDirectory.empty()) {
					throw std::runtime_error("relocatable database cannot resolve its LOAD CSV import directory");
				}
				options.csv_import_root = databaseDirectory;
				queryEngine = std::make_shared<query::QueryEngine>(
						std::static_pointer_cast<storage::LogicalGraphReadBackend>(relocatableStorage_),
						std::static_pointer_cast<storage::LogicalGraphWriteBackend>(relocatableStorage_), std::move(options));
			} else {
				throw std::logic_error("Database has no open storage backend for query execution");
			}
			queryEngine->setThreadPool(threadPool_.get());
		});
	}

	std::shared_ptr<query::QueryEngine> Database::getQueryEngine() {
		if (!isOpen())
			return nullptr;
		ensureQueryEngine();
		return queryEngine;
	}

	std::shared_ptr<concurrent::ThreadPool> Database::getThreadPool() {
		if (!isOpen())
			return nullptr;
		ensureThreadPool();
		return threadPool_;
	}

} // namespace graph
