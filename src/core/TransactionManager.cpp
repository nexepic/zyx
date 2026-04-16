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
#include <thread>
#include "graph/core/Transaction.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/PersistenceManager.hpp"
#include "graph/storage/SnapshotManager.hpp"
#include "graph/storage/wal/WALManager.hpp"

namespace graph {

	TransactionManager::TransactionManager(std::shared_ptr<storage::FileStorage> storage,
										   std::shared_ptr<storage::wal::WALManager> walManager) :
		storage_(std::move(storage)), walManager_(std::move(walManager)),
		snapshotManager_(std::make_unique<storage::SnapshotManager>()) {}

	TransactionManager::~TransactionManager() = default;

	Transaction TransactionManager::begin() { return begin(std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultTxnTimeout)); }

	Transaction TransactionManager::beginReadOnly() {
		return beginReadOnly(std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultTxnTimeout));
	}

	Transaction TransactionManager::beginReadOnly(std::chrono::milliseconds timeout) {
		// Acquire shared lock — multiple readers can hold this concurrently
		std::shared_lock<std::shared_mutex> lock(rwMutex_, std::defer_lock);

#ifdef __EMSCRIPTEN__
		(void)timeout;
		lock.lock(); // Single-threaded: always succeeds immediately
#else
		auto deadline = std::chrono::steady_clock::now() + timeout;
		while (!lock.try_lock()) {
			if (std::chrono::steady_clock::now() >= deadline) {
				throw std::runtime_error("Read-only transaction begin timed out: a write transaction is blocking");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
#endif

		uint64_t txnId = nextTxnId_.fetch_add(1);

		// Acquire an immutable snapshot of committed state
		auto snapshot = snapshotManager_->acquireSnapshot();

		// Set up thread-local snapshot for DataManager reads
		auto dm = storage_->getDataManager();
		dm->setCurrentSnapshot(snapshot.get());

		// Mark DataManager's thread-local read-only guard
		dm->setReadOnlyMode(true);

		Transaction txn(txnId, *this, storage_);
		txn.readOnly_ = true;
		txn.readLock_ = std::move(lock);
		txn.snapshot_ = std::move(snapshot);
		return txn;
	}

	Transaction TransactionManager::begin(std::chrono::milliseconds timeout) {
		// Acquire exclusive lock — blocks until all readers and writers finish
		std::unique_lock<std::shared_mutex> lock(rwMutex_, std::defer_lock);

#ifdef __EMSCRIPTEN__
		(void)timeout;
		lock.lock();  // Single-threaded: always succeeds immediately
#else
		// Try to acquire with timeout
		auto deadline = std::chrono::steady_clock::now() + timeout;
		while (!lock.try_lock()) {
			if (std::chrono::steady_clock::now() >= deadline) {
				throw std::runtime_error("Transaction begin timed out: another transaction is active");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
#endif

		uint64_t txnId = nextTxnId_.fetch_add(1);

		// Write WAL begin record
		if (walManager_ && walManager_->isOpen()) {
			walManager_->writeBegin(txnId);
		}

		// Set up DataManager transaction context (flag + txnId, no pointer)
		auto dm = storage_->getDataManager();
		dm->setActiveTransaction(txnId);

		// Suppress auto-flush during active transaction
		dm->getPersistenceManager()->setTransactionActive(true);

		activeWriteTxn_ = true;

		Transaction txn(txnId, *this, storage_);
		txn.writeLock_ = std::move(lock);
		return txn;
	}

	void TransactionManager::commitTransaction(Transaction &txn) {
		using Clock = std::chrono::steady_clock;

		if (txn.getState() != Transaction::TxnState::TXN_ACTIVE) {
			return;
		}

		// Read-only transactions: release snapshot and shared lock
		if (txn.isReadOnly()) {
			auto dm = storage_->getDataManager();
			dm->clearCurrentSnapshot();
			dm->setReadOnlyMode(false);
			txn.snapshot_.reset();
			txn.state_ = Transaction::TxnState::TXN_COMMITTED;
			// shared_lock released via RAII when txn.readLock_ is destroyed/moved
			txn.readLock_ = {};
			return;
		}

		// Write WAL commit record (writeCommit internally flushes buffer + fsync via group commit)
		if (walManager_ && walManager_->isOpen()) {
			auto walStart = Clock::now();
			walManager_->writeCommit(txn.getId());
			debug::PerfTrace::addDuration(
					"wal.commit_sync",
					static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
												 walStart)
												 .count()));
		}

		// Capture dirty state into snapshot BEFORE save flushes and clears registries.
		// Read-only transactions that start after this commit will use this snapshot
		// to see the committed state even though data is now on disk.
		auto dm = storage_->getDataManager();
		auto newSnapshot = dm->getPersistenceManager()->captureCommittedSnapshot();

		// Persist dirty data to main DB file
		auto saveStart = Clock::now();
		storage_->save();
		debug::PerfTrace::addDuration(
				"txn.save", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
												 saveStart)
											  .count()));

		// Checkpoint WAL if size exceeds threshold (data is now in main DB)
		if (walManager_ && walManager_->isOpen() && walManager_->shouldCheckpoint()) {
			auto checkpointStart = Clock::now();
			walManager_->checkpoint();
			debug::PerfTrace::addDuration(
					"checkpoint",
					static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
												 checkpointStart)
												 .count()));
		}

		// Publish the populated snapshot for future readers
		snapshotManager_->publishSnapshot(std::move(newSnapshot));

		// Clear DataManager transaction context
		dm->clearActiveTransaction();
		dm->getPersistenceManager()->setTransactionActive(false);

		// Update transaction state
		txn.state_ = Transaction::TxnState::TXN_COMMITTED;
		activeWriteTxn_ = false;

		// Release exclusive lock via RAII
		if (txn.writeLock_.owns_lock()) {
			txn.writeLock_.unlock();
		}
	}

	void TransactionManager::rollbackTransaction(Transaction &txn) {
		if (txn.getState() != Transaction::TxnState::TXN_ACTIVE) {
			return;
		}

		// Read-only transactions: release snapshot and shared lock
		if (txn.isReadOnly()) {
			auto dm = storage_->getDataManager();
			if (dm) {
				dm->clearCurrentSnapshot();
				dm->setReadOnlyMode(false);
			}
			txn.snapshot_.reset();
			txn.state_ = Transaction::TxnState::TXN_ROLLED_BACK;
			txn.readLock_ = {};
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
				dm->getPersistenceManager()->setTransactionActive(false);
			}

			// Write WAL rollback record
			if (walManager_ && walManager_->isOpen()) {
				walManager_->writeRollback(txn.getId());
			}
		}

		// Update transaction state
		txn.state_ = Transaction::TxnState::TXN_ROLLED_BACK;
		activeWriteTxn_ = false;

		// Release exclusive lock via RAII
		if (txn.writeLock_.owns_lock()) {
			txn.writeLock_.unlock();
		}
	}

	bool TransactionManager::hasActiveTransaction() const { return activeWriteTxn_.load(std::memory_order_acquire); }

	void TransactionManager::setWALManager(std::shared_ptr<storage::wal::WALManager> walManager) {
		walManager_ = std::move(walManager);
	}

} // namespace graph
