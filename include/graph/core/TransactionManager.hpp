/**
 * @file TransactionManager.hpp
 * @date 2026/3/19
 *
 * @copyright Copyright (c) 2026 Nexepic
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

namespace graph::storage {
	class FileStorage;
	namespace wal {
		class WALManager;
	}
} // namespace graph::storage

namespace graph {

	class Transaction;

	class TransactionManager {
	public:
		TransactionManager(std::shared_ptr<storage::FileStorage> storage,
						   std::shared_ptr<storage::wal::WALManager> walManager);
		~TransactionManager() = default;

		Transaction begin();
		void commitTransaction(Transaction &txn);
		void rollbackTransaction(Transaction &txn);
		[[nodiscard]] bool hasActiveTransaction() const;

	private:
		friend class Transaction;
		std::mutex writeMutex_;
		std::unique_lock<std::mutex> writeLock_;
		std::atomic<uint64_t> nextTxnId_{1};
		bool hasActive_ = false;
		std::shared_ptr<storage::FileStorage> storage_;
		std::shared_ptr<storage::wal::WALManager> walManager_;
	};

} // namespace graph
