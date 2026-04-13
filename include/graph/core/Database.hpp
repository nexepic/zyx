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
#include <mutex>
#include <string>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/config/SystemConfigManager.hpp"
#include "graph/core/Transaction.hpp"
#include "graph/core/TransactionManager.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/wal/WALManager.hpp"

namespace graph {

	class Database {
	public:
		explicit Database(const std::string &dbPath, storage::OpenMode mode = storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE,
						  size_t cacheSize = 10000);
		~Database();

		void open();
		[[nodiscard]] bool openIfExists();
		void close() const;

		[[nodiscard]] bool exists() const;
		[[nodiscard]] bool isOpen() const;

		Transaction beginTransaction();
		Transaction beginReadOnlyTransaction();

		// Query engine access (lazy-initialized on first call)
		std::shared_ptr<query::QueryEngine> getQueryEngine();

		// Access to storage for advanced operations
		[[nodiscard]] std::shared_ptr<storage::FileStorage> getStorage() const { return storage; }

		// Check if there is an active transaction
		[[nodiscard]] bool hasActiveTransaction() const;

		[[nodiscard]] std::shared_ptr<config::SystemConfigManager> getConfigManager() const { return configManager_; }

		// Access to thread pool for parallel operations (lazy-initialized on first call)
		std::shared_ptr<concurrent::ThreadPool> getThreadPool();

		// Resize the thread pool at runtime
		void setThreadPoolSize(size_t poolSize);

	private:
		std::shared_ptr<storage::FileStorage> storage;
		std::shared_ptr<query::QueryEngine> queryEngine;
		std::string dbPath;
		std::shared_ptr<config::SystemConfigManager> configManager_;
		std::shared_ptr<storage::wal::WALManager> walManager_;
		std::unique_ptr<TransactionManager> transactionManager_;
		std::shared_ptr<concurrent::ThreadPool> threadPool_;

		// Lazy initialization flags
		mutable std::once_flag threadPoolInitFlag_;
		mutable std::once_flag queryEngineInitFlag_;
		mutable std::once_flag walInitFlag_;

		void ensureThreadPool();
		void ensureQueryEngine();
		void ensureWALAndTransactionManager();
	};

} // namespace graph
