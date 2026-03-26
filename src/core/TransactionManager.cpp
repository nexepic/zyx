/**
 * @file TransactionManager.cpp
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

#include "graph/core/TransactionManager.hpp"
#include <chrono>
#include <cstdint>
#include "graph/core/Transaction.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/wal/WALManager.hpp"

namespace graph {

	TransactionManager::TransactionManager(std::shared_ptr<storage::FileStorage> storage,
										   std::shared_ptr<storage::wal::WALManager> walManager) :
		storage_(std::move(storage)), walManager_(std::move(walManager)) {}

	Transaction TransactionManager::begin() { return begin(std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultTxnTimeout)); }

	Transaction TransactionManager::begin(std::chrono::milliseconds timeout) {
		// Acquire single-writer lock with timeout using a local lock first
		// to avoid releasing the existing writeLock_ before acquiring the new one
		std::unique_lock<std::timed_mutex> lock(writeMutex_, std::defer_lock);
		if (!lock.try_lock_for(timeout)) {
			throw std::runtime_error("Transaction begin timed out: another transaction is active");
		}
		writeLock_ = std::move(lock);

		uint64_t txnId = nextTxnId_.fetch_add(1);

		// Write WAL begin record
		if (walManager_ && walManager_->isOpen()) {
			walManager_->writeBegin(txnId);
		}

		// Set up DataManager transaction context (flag + txnId, no pointer)
		auto dm = storage_->getDataManager();
		dm->setActiveTransaction(txnId);

		hasActive_ = true;

		return Transaction(txnId, *this, storage_);
	}

	void TransactionManager::commitTransaction(Transaction &txn) {
		using Clock = std::chrono::steady_clock;

		if (txn.getState() != Transaction::TxnState::TXN_ACTIVE) {
			return;
		}

		// Write WAL commit record and sync
		if (walManager_ && walManager_->isOpen()) {
			auto walStart = Clock::now();
			walManager_->writeCommit(txn.getId());
			walManager_->sync();
			debug::PerfTrace::addDuration(
					"wal.commit_sync",
					static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
												 walStart)
												 .count()));
		}

		// Persist dirty data to main DB file
		auto saveStart = Clock::now();
		storage_->save();
		debug::PerfTrace::addDuration(
				"txn.save", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
												 saveStart)
											  .count()));

		// Checkpoint WAL (truncate - data is now in main DB)
		if (walManager_ && walManager_->isOpen()) {
			auto checkpointStart = Clock::now();
			walManager_->checkpoint();
			debug::PerfTrace::addDuration(
					"checkpoint",
					static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
												 checkpointStart)
												 .count()));
		}

		// Clear DataManager transaction context
		auto dm = storage_->getDataManager();
		dm->clearActiveTransaction();

		// Update transaction state
		txn.state_ = Transaction::TxnState::TXN_COMMITTED;
		hasActive_ = false;

		// Release single-writer lock via RAII
		if (writeLock_.owns_lock()) {
			writeLock_.unlock();
		}
	}

	void TransactionManager::rollbackTransaction(Transaction &txn) {
		if (txn.getState() != Transaction::TxnState::TXN_ACTIVE) {
			return;
		}

		// Check if storage is still open (may have been closed before auto-rollback)
		if (storage_ && storage_->isOpen()) {
			auto dm = storage_->getDataManager();
			if (dm) {
				// Rollback: undo all changes from this transaction
				dm->rollbackActiveTransaction();

				// Clear DataManager transaction context
				dm->clearActiveTransaction();
			}

			// Write WAL rollback record
			if (walManager_ && walManager_->isOpen()) {
				walManager_->writeRollback(txn.getId());
				walManager_->checkpoint();
			}
		}

		// Update transaction state
		txn.state_ = Transaction::TxnState::TXN_ROLLED_BACK;
		hasActive_ = false;

		// Release single-writer lock via RAII
		if (writeLock_.owns_lock()) {
			writeLock_.unlock();
		}
	}

	bool TransactionManager::hasActiveTransaction() const { return hasActive_.load(std::memory_order_acquire); }

} // namespace graph
