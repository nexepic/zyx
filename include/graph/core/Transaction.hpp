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

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace graph::storage {
	class StorageBackend;
	class LogicalGraphTransaction;
	class LogicalGraphTransactionOverlay;
}

namespace graph::query {
	class QueryEngine;
	class QueryResult;
	namespace gds {
		class LogicalProjectionCatalog;
	}
}

namespace graph {

	class Transaction {
	public:
		enum class TxnState { TXN_ACTIVE, TXN_COMMITTED, TXN_ROLLED_BACK };

		~Transaction();

		// Move-only
		Transaction(Transaction &&other) noexcept;
		Transaction &operator=(Transaction &&other) noexcept;
		Transaction(const Transaction &) = delete;
		Transaction &operator=(const Transaction &) = delete;

		void commit();
		void rollback();

		// Executes through this transaction's explicitly owned logical overlay.
		// Database-scoped QueryEngine execution remains an independent operation.
		[[nodiscard]] query::QueryResult execute(query::QueryEngine &engine, const std::string &query);

		[[nodiscard]] uint64_t getId() const { return txnId_; }
		[[nodiscard]] TxnState getState() const { return state_; }
		[[nodiscard]] bool isActive() const { return state_ == TxnState::TXN_ACTIVE; }
		[[nodiscard]] bool isReadOnly() const { return readOnly_; }

	private:
		friend class Database;
		Transaction(uint64_t txnId, std::unique_ptr<storage::LogicalGraphTransaction> logicalTransaction,
					std::shared_ptr<std::atomic<size_t>> activeWriteTransactionCount,
					std::shared_ptr<storage::StorageBackend> backendLifetime);

		void finishLogicalTransaction() noexcept;
		void rollbackActiveLogicalTransactionNoexcept() noexcept;

		uint64_t txnId_ = 0;
		TxnState state_ = TxnState::TXN_ACTIVE;
		bool readOnly_ = false;

		// Declare this before the transaction implementation so reverse member
		// destruction keeps the backend alive through its final rollback/release.
		std::shared_ptr<storage::StorageBackend> backendLifetime_;
		// Native logical transactions own their snapshot and a read-your-writes
		// overlay. API transaction calls route to the overlay explicitly, so an
		// ordinary Database query cannot inherit another transaction's state.
		std::unique_ptr<storage::LogicalGraphTransaction> logicalTransaction_;
		std::unique_ptr<storage::LogicalGraphTransactionOverlay> logicalOverlay_;
		// GDS projections are derived, transaction-local state. They must never
		// enter the Database-owned catalog until an explicit durable ownership
		// model exists, so commit and rollback both retire this catalog with the
		// transaction object.
		std::unique_ptr<query::gds::LogicalProjectionCatalog> logicalProjectionCatalog_;
		std::shared_ptr<std::atomic<size_t>> activeLogicalWriteTransactionCount_;
		bool logicalWriteTransactionCounted_ = false;
	};

} // namespace graph
