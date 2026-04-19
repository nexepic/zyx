/**
 * @file Transaction.hpp
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

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace graph::storage {
	class FileStorage;
	struct CommittedSnapshot;
}

namespace graph {

	class TransactionManager;

	class Transaction {
	public:
		enum class TxnState { TXN_ACTIVE, TXN_COMMITTED, TXN_ROLLED_BACK };

		struct TxnOperation {
			enum OpType : uint8_t { OP_ADD, OP_UPDATE, OP_DELETE };
			OpType opType;
			uint8_t entityType; // graph::EntityType value
			int64_t entityId;
		};

		~Transaction();

		// Move-only
		Transaction(Transaction &&other) noexcept;
		Transaction &operator=(Transaction &&other) noexcept;
		Transaction(const Transaction &) = delete;
		Transaction &operator=(const Transaction &) = delete;

		void commit();
		void rollback();

		[[nodiscard]] uint64_t getId() const { return txnId_; }
		[[nodiscard]] TxnState getState() const { return state_; }
		[[nodiscard]] bool isActive() const { return state_ == TxnState::TXN_ACTIVE; }
		[[nodiscard]] bool isReadOnly() const { return readOnly_; }

		[[nodiscard]] const storage::CommittedSnapshot *getSnapshot() const { return snapshot_.get(); }

	private:
		friend class TransactionManager;
		Transaction(uint64_t txnId, TransactionManager &mgr, std::shared_ptr<storage::FileStorage> storage);

		uint64_t txnId_ = 0;
		TxnState state_ = TxnState::TXN_ACTIVE;
		bool readOnly_ = false;
		TransactionManager *manager_ = nullptr; // Non-owning
		std::shared_ptr<storage::FileStorage> storage_;

		// Shared lock held for read-only transaction's lifetime (RAII)
		std::shared_lock<std::shared_mutex> readLock_;

		// Write lock held for write transaction's lifetime (RAII, moves with Transaction)
		std::unique_lock<std::shared_mutex> writeLock_;

		// Snapshot for read-only transactions (immutable once acquired)
		std::shared_ptr<storage::CommittedSnapshot> snapshot_;
	};

} // namespace graph
