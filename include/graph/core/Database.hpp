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

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Transaction.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/storage/StorageBackend.hpp"
#include "graph/storage/StorageBackendFactory.hpp"

namespace graph::storage {
	class RelocatableStorageBackend;
}

namespace graph {

	class Database {
	public:
		explicit Database(const std::string &dbPath,
						  storage::OpenMode mode = storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE,
						  size_t cacheSize = 64U * 1024U * 1024U);
		~Database();

		void open();
		[[nodiscard]] bool openIfExists();
		void flush() const;
		void close() const;

		[[nodiscard]] bool exists() const;
		[[nodiscard]] bool isOpen() const;

		Transaction beginTransaction();
		// Logical write transaction. Publication is deferred until commit.
		Transaction beginBulkTransaction();
		Transaction beginReadOnlyTransaction();

		// Query engine access (lazy-initialized on first call)
		std::shared_ptr<query::QueryEngine> getQueryEngine();

		// The generic backend preserves the format boundary. It is the only
		// storage accessor available to the native public database.
		[[nodiscard]] std::shared_ptr<storage::StorageBackend> getStorageBackend() const { return backend_; }
		// The logical write adapter keeps direct writes transaction-scoped without
		// exposing physical storage internals through the public database API.
		[[nodiscard]] std::shared_ptr<storage::LogicalGraphWriteBackend> getLogicalGraphWriteBackend() const;
		[[nodiscard]] std::shared_ptr<storage::LogicalGraphTransactionalBackend>
		getLogicalGraphTransactionalBackend() const;
		[[nodiscard]] bool usesLogicalBackend() const { return static_cast<bool>(relocatableStorage_); }

		// Check if there is an active transaction
		[[nodiscard]] bool hasActiveTransaction() const;

		[[nodiscard]] const std::string &getPath() const { return dbPath; }

		// Access to thread pool for parallel operations (lazy-initialized on first call)
		std::shared_ptr<concurrent::ThreadPool> getThreadPool();

		// Resize the thread pool at runtime
		void setThreadPoolSize(size_t poolSize);

	private:
		std::shared_ptr<storage::StorageBackend> backend_;
		std::shared_ptr<storage::RelocatableStorageBackend> relocatableStorage_;
		std::shared_ptr<query::QueryEngine> queryEngine;
		std::string dbPath;
		storage::OpenMode openMode_;
		size_t cacheSize_;
		std::atomic<uint64_t> nextLogicalTransactionId_{1U};
		std::shared_ptr<std::atomic<size_t>> activeLogicalWriteTransactions_ =
				std::make_shared<std::atomic<size_t>>(0U);
		std::shared_ptr<concurrent::ThreadPool> threadPool_;
		std::optional<size_t> configuredThreadPoolSize_;

		// Lazy initialization flags
		mutable std::once_flag threadPoolInitFlag_;
		mutable std::once_flag queryEngineInitFlag_;

		void ensureThreadPool();
		void ensureQueryEngine();
		Transaction beginLogicalTransaction(bool readOnly);
	};

} // namespace graph
