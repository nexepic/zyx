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

	Transaction TransactionManager::beginBulk() {
		return beginBulk(std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultTxnTimeout));
	}

	Transaction TransactionManager::beginReadOnly() {
		return beginReadOnly(std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultTxnTimeout));
	}

	void TransactionManager::ensureReadSnapshotCurrent() {
		if (!dirtySnapshotPending_.load(std::memory_order_acquire)) {
			return;
		}

		std::lock_guard lock(snapshotPublishMutex_);
		if (!dirtySnapshotPending_.load(std::memory_order_relaxed)) { // ZYX_COV_EXCL_LINE: double-check race guard; single-thread tests cannot interleave the publisher.
			return;
		}

		auto dm = storage_->getDataManager();
		if (dm->hasUnsavedChanges()) { // ZYX_COV_EXCL_LINE: dirty snapshot pending is set only while committed overlay changes remain unsaved.
			debug::ScopedPerfTimer timer("txn.snapshot_materialize");
			snapshotManager_->publishSnapshot(dm->captureCommittedSnapshot());
		} else {
			snapshotManager_->publishCleanSnapshot();
		}
		dirtySnapshotPending_.store(false, std::memory_order_release);
	}

	Transaction TransactionManager::beginReadOnly(std::chrono::milliseconds timeout) {
		// Acquire shared lock — multiple readers can hold this concurrently
#ifdef __EMSCRIPTEN__
		(void)timeout;
		while (!rwLock_.try_lock_read()) {} // Single-threaded: always succeeds immediately
#else
		auto deadline = std::chrono::steady_clock::now() + timeout;
		while (!rwLock_.try_lock_read()) {
			if (std::chrono::steady_clock::now() >= deadline) {
				throw std::runtime_error("Read-only transaction begin timed out: a write transaction is blocking");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
#endif
		concurrent::ReadLockGuard lock(rwLock_);

		uint64_t txnId = nextTxnId_.fetch_add(1);

		ensureReadSnapshotCurrent();

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
		return beginWithPolicy(timeout, Transaction::CheckpointPolicy::TCP_AUTO);
	}

	Transaction TransactionManager::beginBulk(std::chrono::milliseconds timeout) {
		return beginWithPolicy(timeout, Transaction::CheckpointPolicy::TCP_DEFER_CHECKPOINT);
	}

	Transaction TransactionManager::beginWithPolicy(std::chrono::milliseconds timeout,
													Transaction::CheckpointPolicy policy) {
		// Acquire exclusive lock — blocks until all readers and writers finish
#ifdef __EMSCRIPTEN__
		(void)timeout;
		while (!rwLock_.try_lock_write()) {} // Single-threaded: always succeeds immediately
#else
		// Try to acquire with timeout
		auto deadline = std::chrono::steady_clock::now() + timeout;
		while (!rwLock_.try_lock_write()) {
			if (std::chrono::steady_clock::now() >= deadline) {
				throw std::runtime_error("Transaction begin timed out: another transaction is active");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
#endif
		concurrent::WriteLockGuard lock(rwLock_);

		uint64_t txnId = nextTxnId_.fetch_add(1);

		// Write WAL begin record
		const bool walOpen = walManager_ && walManager_->isOpen();
		if (walOpen) {
			walManager_->writeBegin(txnId);
		}

		// Set up DataManager transaction context (flag + txnId, no pointer)
		auto dm = storage_->getDataManager();
		dm->setActiveTransaction(txnId);

		// Suppress auto-flush during active transaction
		dm->getPersistenceManager()->setTransactionActive(true);

		activeWriteTxn_ = true;

		Transaction txn(txnId, *this, storage_);
		txn.checkpointPolicy_ = policy;
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
			txn.readLock_.reset();
			return;
		}

		auto dm = storage_->getDataManager();

		// Flush all transaction-local entity records before the commit marker.
		// Once writeCommit() fsyncs, recovery can replay these final states even
		// if the main DB checkpoint is deferred.
		const bool walOpen = walManager_ && walManager_->isOpen();
		const bool hasWalRecords = dm->hasTransactionWALRecordsToFlush();
		const bool mustCommitWal = hasWalRecords || dm->hasUnsavedChanges();
		if (walOpen && mustCommitWal) {
			dm->flushTransactionWALRecords();
			auto walStart = Clock::now();
			walManager_->writeCommit(txn.getId());
			debug::PerfTrace::addDuration(
					"wal.commit_sync",
					static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
												 walStart)
												 .count()));
		}

		bool checkpointed = false;

		// Checkpoint only when the WAL crosses its size threshold. Small write
		// commits stay WAL-first and are materialized by a later checkpoint or
		// clean close, avoiding a main DB fsync per transaction.
		if (!walOpen) {
			auto saveStart = Clock::now();
			storage_->save();
			debug::PerfTrace::addDuration(
					"txn.save", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
															Clock::now() - saveStart)
															.count()));
			checkpointed = true;
		} else if (txn.getCheckpointPolicy() == Transaction::CheckpointPolicy::TCP_AUTO &&
				   walManager_->shouldCheckpoint()) {
			auto checkpointStart = Clock::now();
			auto saveStart = Clock::now();
			storage_->save();
			debug::PerfTrace::addDuration(
					"txn.save", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
															Clock::now() - saveStart)
															.count()));
			walManager_->checkpoint();
			debug::PerfTrace::addDuration(
					"checkpoint",
					static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
												 checkpointStart)
												 .count()));
			checkpointed = true;
		}

		if (checkpointed || !dm->hasUnsavedChanges()) {
			// Main DB state is current; readers can rely on disk plus an empty overlay.
			snapshotManager_->publishCleanSnapshot();
			dirtySnapshotPending_.store(false, std::memory_order_release);
		} else {
			// The committed overlay may be large during bulk loads. Defer the
			// immutable snapshot copy until a read-only transaction actually
			// needs it; schema builds and explicit checkpoints can then proceed
			// without paying an avoidable full dirty-map copy.
			dirtySnapshotPending_.store(true, std::memory_order_release);
		}

		// Clear DataManager transaction context
		dm->clearActiveTransaction();
		dm->getPersistenceManager()->setTransactionActive(false);

		// Update transaction state
		txn.state_ = Transaction::TxnState::TXN_COMMITTED;
		activeWriteTxn_ = false;

		// Release exclusive lock
		txn.writeLock_.unlock();
	}

	void TransactionManager::rollbackTransaction(Transaction &txn) {
		if (txn.getState() != Transaction::TxnState::TXN_ACTIVE) {
			return;
		}

		// Read-only transactions: release snapshot and shared lock
		if (txn.isReadOnly()) {
			auto dm = storage_->getDataManager();
			dm->clearCurrentSnapshot();
			dm->setReadOnlyMode(false);
			txn.snapshot_.reset();
			txn.state_ = Transaction::TxnState::TXN_ROLLED_BACK;
			txn.readLock_.reset();
			return;
		}

		// Check if storage is still open (may have been closed before auto-rollback)
		if (storage_->isOpen()) {
			auto dm = storage_->getDataManager();
			// Rollback: undo all changes from this transaction
			dm->rollbackActiveTransaction();

			// Clear DataManager transaction context
			dm->clearActiveTransaction();
			dm->getPersistenceManager()->setTransactionActive(false);

			// Write WAL rollback record
			if (walManager_ && walManager_->isOpen()) {
				walManager_->writeRollback(txn.getId());
			}
		}

		// Update transaction state
		txn.state_ = Transaction::TxnState::TXN_ROLLED_BACK;
		activeWriteTxn_ = false;

		// Release exclusive lock
		txn.writeLock_.unlock();
	}

	bool TransactionManager::hasActiveTransaction() const { return activeWriteTxn_.load(std::memory_order_acquire); }

	void TransactionManager::setWALManager(std::shared_ptr<storage::wal::WALManager> walManager) {
		walManager_ = std::move(walManager);
	}

	void TransactionManager::markStorageCheckpointed() {
		if (!storage_ || !storage_->isOpen()) { // ZYX_COV_EXCL_LINE: Database owns an opened storage when it marks a checkpoint.
			return;
		}
		auto dm = storage_->getDataManager();
		if (!dm || dm->hasUnsavedChanges()) { // ZYX_COV_EXCL_LINE: checkpoint notification is issued immediately after a successful clean flush.
			return;
		}

		std::lock_guard lock(snapshotPublishMutex_);
		snapshotManager_->publishCleanSnapshot();
		dirtySnapshotPending_.store(false, std::memory_order_release);
	}

} // namespace graph
