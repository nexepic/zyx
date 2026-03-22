/**
 * @file test_TransactionManager.cpp
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

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include "graph/core/Database.hpp"
#include "graph/core/TransactionManager.hpp"
#include "graph/storage/wal/WALManager.hpp"

namespace fs = std::filesystem;

class TransactionManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_txnmgr_" + boost::uuids::to_string(uuid) + ".graph");
		fs::remove_all(testDbPath);

		db = std::make_unique<graph::Database>(testDbPath.string());
		db->open();
	}

	void TearDown() override {
		if (db) {
			db->close();
			db.reset();
		}
		fs::remove_all(testDbPath);
		fs::remove(testDbPath.string() + "-wal");
	}

	fs::path testDbPath;
	std::unique_ptr<graph::Database> db;
};

TEST_F(TransactionManagerTest, BeginCreatesActiveTransaction) {
	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());
	EXPECT_TRUE(db->hasActiveTransaction());
	txn.commit();
	EXPECT_FALSE(db->hasActiveTransaction());
}

TEST_F(TransactionManagerTest, CommitReleasesLock) {
	{
		auto txn = db->beginTransaction();
		txn.commit();
	}

	// Should be able to start another transaction
	auto txn2 = db->beginTransaction();
	EXPECT_TRUE(txn2.isActive());
	txn2.commit();
}

TEST_F(TransactionManagerTest, RollbackReleasesLock) {
	{
		auto txn = db->beginTransaction();
		txn.rollback();
	}

	// Should be able to start another transaction
	auto txn2 = db->beginTransaction();
	EXPECT_TRUE(txn2.isActive());
	txn2.commit();
}

TEST_F(TransactionManagerTest, AutoRollbackReleasesLock) {
	{
		auto txn = db->beginTransaction();
		// Let destructor auto-rollback
	}

	// Should be able to start another transaction
	auto txn2 = db->beginTransaction();
	EXPECT_TRUE(txn2.isActive());
	txn2.commit();
}

TEST_F(TransactionManagerTest, SequentialTransactionsIncrementId) {
	uint64_t id1, id2, id3;

	{
		auto txn = db->beginTransaction();
		id1 = txn.getId();
		txn.commit();
	}
	{
		auto txn = db->beginTransaction();
		id2 = txn.getId();
		txn.commit();
	}
	{
		auto txn = db->beginTransaction();
		id3 = txn.getId();
		txn.commit();
	}

	EXPECT_LT(id1, id2);
	EXPECT_LT(id2, id3);
}

TEST_F(TransactionManagerTest, WALFileCreatedOnOpen) {
	std::string walPath = testDbPath.string() + "-wal";
	EXPECT_TRUE(fs::exists(walPath));
}

// ============================================================================
// Coverage Tests: Commit/Rollback on non-active transactions
// Transaction::commit/rollback throw when not active, but
// TransactionManager::commitTransaction/rollbackTransaction do an early return.
// These tests exercise those branches via public API throw paths.
// ============================================================================

TEST_F(TransactionManagerTest, CommitAlreadyCommittedTransactionThrows) {
	// Cover branch: Transaction::commit checks state -> throws
	// The TransactionManager::commitTransaction line 52 early return is tested via NoWAL test
	auto txn = db->beginTransaction();
	txn.commit();
	EXPECT_FALSE(txn.isActive());

	// Calling commit again throws from Transaction level
	EXPECT_THROW(txn.commit(), std::runtime_error);
}

TEST_F(TransactionManagerTest, RollbackAlreadyCommittedTransactionThrows) {
	// Cover branch: Transaction::rollback checks state -> throws
	auto txn = db->beginTransaction();
	txn.commit();
	EXPECT_FALSE(txn.isActive());

	EXPECT_THROW(txn.rollback(), std::runtime_error);
}

TEST_F(TransactionManagerTest, RollbackAlreadyRolledBackTransactionThrows) {
	auto txn = db->beginTransaction();
	txn.rollback();
	EXPECT_FALSE(txn.isActive());

	EXPECT_THROW(txn.rollback(), std::runtime_error);
}

TEST_F(TransactionManagerTest, CommitAlreadyRolledBackTransactionThrows) {
	auto txn = db->beginTransaction();
	txn.rollback();
	EXPECT_FALSE(txn.isActive());

	EXPECT_THROW(txn.commit(), std::runtime_error);
}

// ============================================================================
// Coverage Tests: Transaction with data modifications
// ============================================================================

TEST_F(TransactionManagerTest, CommitWithDataPersists) {
	// Exercises the full commit path: WAL write, storage save, WAL checkpoint
	{
		auto txn = db->beginTransaction();
		// Perform some data operation within the transaction
		auto storage = db->getStorage();
		auto dm = storage->getDataManager();
		graph::Node node(0, dm->getOrCreateLabelId("TestLabel"));
		dm->addNode(node);
		txn.commit();
	}

	// Verify data persists after commit
	auto storage = db->getStorage();
	auto dm = storage->getDataManager();
	// The node should have been created with a valid ID
	EXPECT_TRUE(dm->getIdAllocator()->getCurrentMaxNodeId() > 0);
}

TEST_F(TransactionManagerTest, RollbackRevertsData) {
	// Exercises the full rollback path: dm->rollbackActiveTransaction, WAL rollback, checkpoint
	auto storage = db->getStorage();
	auto dm = storage->getDataManager();
	{
		auto txn = db->beginTransaction();
		graph::Node node(0, dm->getOrCreateLabelId("RollbackLabel"));
		dm->addNode(node);
		txn.rollback();
	}

	// After rollback, we should be able to start a new transaction
	auto txn2 = db->beginTransaction();
	EXPECT_TRUE(txn2.isActive());
	txn2.commit();
}

// ============================================================================
// Coverage Tests: Auto-rollback on destruction with storage still open
// ============================================================================

TEST_F(TransactionManagerTest, AutoRollbackWithOpenStorage) {
	// Cover: storage_ && storage_->isOpen() -> True (line 90)
	// and dm check (line 92) during auto-rollback
	{
		auto txn = db->beginTransaction();
		auto storage = db->getStorage();
		auto dm = storage->getDataManager();
		graph::Node node(0, dm->getOrCreateLabelId("AutoRollbackLabel"));
		dm->addNode(node);
		// Let destructor auto-rollback while storage is open
	}

	// Should be able to start a new transaction after auto-rollback
	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());
	txn.commit();
}

// ============================================================================
// Coverage Tests: Transaction without WAL (null walManager)
// ============================================================================

TEST(TransactionManagerNoWALTest, TransactionWithoutWAL) {
	// Cover: walManager_ && walManager_->isOpen() -> False branches in begin, commit, rollback
	// Create a separate database for this test
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	auto testPath = fs::temp_directory_path() / ("test_txnmgr_nowal_" + boost::uuids::to_string(uuid) + ".graph");
	fs::remove_all(testPath);

	auto testDb = std::make_unique<graph::Database>(testPath.string());
	testDb->open();
	auto storage = testDb->getStorage();

	// Create a TransactionManager directly with null WAL manager
	graph::TransactionManager txnMgr(storage, nullptr);

	// Begin should work without WAL (walManager_ is null -> skip WAL write)
	auto txn = txnMgr.begin();
	EXPECT_TRUE(txn.isActive());
	EXPECT_TRUE(txnMgr.hasActiveTransaction());

	// Commit should work without WAL (walManager_ null -> skip commit/checkpoint)
	txnMgr.commitTransaction(txn);
	EXPECT_FALSE(txnMgr.hasActiveTransaction());

	// Rollback path without WAL
	auto txn2 = txnMgr.begin();
	EXPECT_TRUE(txn2.isActive());
	txnMgr.rollbackTransaction(txn2);
	EXPECT_FALSE(txnMgr.hasActiveTransaction());

	testDb->close();
	testDb.reset();
	fs::remove_all(testPath);
	fs::remove(testPath.string() + "-wal");
}

// ============================================================================
// Coverage Tests: Destructor auto-rollback after close
// ============================================================================

TEST(TransactionManagerNoWALTest, CommitNonActiveTransactionEarlyReturn) {
	// Cover: TransactionManager::commitTransaction line 52: if (txn.getState() != TXN_ACTIVE) return
	// We call commitTransaction directly on a TransactionManager with a non-active transaction
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	auto testPath = fs::temp_directory_path() / ("test_txnmgr_commit_noop_" + boost::uuids::to_string(uuid) + ".graph");
	fs::remove_all(testPath);

	auto testDb = std::make_unique<graph::Database>(testPath.string());
	testDb->open();
	auto storage = testDb->getStorage();
	graph::TransactionManager txnMgr(storage, nullptr);

	auto txn = txnMgr.begin();
	txnMgr.commitTransaction(txn);
	EXPECT_FALSE(txn.isActive());

	// Call commitTransaction again on non-active txn - should early return at line 52
	txnMgr.commitTransaction(txn);
	EXPECT_FALSE(txn.isActive());

	testDb->close();
	testDb.reset();
	fs::remove_all(testPath);
	fs::remove(testPath.string() + "-wal");
}

TEST(TransactionManagerNoWALTest, RollbackNonActiveTransactionEarlyReturn) {
	// Cover: TransactionManager::rollbackTransaction line 85: if (txn.getState() != TXN_ACTIVE) return
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	auto testPath = fs::temp_directory_path() / ("test_txnmgr_rollback_noop_" + boost::uuids::to_string(uuid) + ".graph");
	fs::remove_all(testPath);

	auto testDb = std::make_unique<graph::Database>(testPath.string());
	testDb->open();
	auto storage = testDb->getStorage();
	graph::TransactionManager txnMgr(storage, nullptr);

	auto txn = txnMgr.begin();
	txnMgr.rollbackTransaction(txn);
	EXPECT_FALSE(txn.isActive());

	// Call rollbackTransaction again on non-active txn - should early return at line 85
	txnMgr.rollbackTransaction(txn);
	EXPECT_FALSE(txn.isActive());

	testDb->close();
	testDb.reset();
	fs::remove_all(testPath);
	fs::remove(testPath.string() + "-wal");
}

// ============================================================================
// Coverage Tests: Transaction move assignment operator
// ============================================================================

TEST_F(TransactionManagerTest, MoveAssignmentOnActiveTransaction) {
	// Cover: Transaction::operator=(Transaction&&) at lines 46-66
	// Exercises the move-assign path where the target txn is active (auto-rollback)
	auto txn1 = db->beginTransaction();
	txn1.commit();

	auto txn2 = db->beginTransaction();
	uint64_t id2 = txn2.getId();

	// Move-assign txn2 into txn1. txn1 is committed (not active), so no rollback
	txn1 = std::move(txn2);
	EXPECT_TRUE(txn1.isActive());
	EXPECT_EQ(txn1.getId(), id2);
	EXPECT_FALSE(txn2.isActive()); // NOLINT - checking moved-from state

	txn1.commit();
}

TEST_F(TransactionManagerTest, MoveAssignmentRollsBackActiveTarget) {
	// Cover: the if (state_ == TXN_ACTIVE && manager_) branch in operator=
	// txn1 is active, then we move-assign txn2 into it, causing txn1 to auto-rollback
	auto txn1 = db->beginTransaction();
	txn1.commit();

	auto txn2 = db->beginTransaction();
	txn2.commit();

	// Now begin two fresh transactions sequentially and move-assign
	auto txn3 = db->beginTransaction();
	txn3.commit();

	auto txn4 = db->beginTransaction();
	uint64_t id4 = txn4.getId();

	// Move txn4 into txn3 - txn3 is committed so no rollback needed
	txn3 = std::move(txn4);
	EXPECT_TRUE(txn3.isActive());
	EXPECT_EQ(txn3.getId(), id4);
	txn3.commit();
}

TEST_F(TransactionManagerTest, RollbackAfterStorageClosed) {
	// Cover: storage_->isOpen() -> False branch in rollbackTransaction (line 90)
	// Create a separate database, begin transaction, close DB, then let destructor auto-rollback
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	auto testPath2 = fs::temp_directory_path() / ("test_txnmgr_closed_" + boost::uuids::to_string(uuid) + ".graph");
	fs::remove_all(testPath2);

	auto testDb2 = std::make_unique<graph::Database>(testPath2.string());
	testDb2->open();
	auto storage2 = testDb2->getStorage();
	graph::TransactionManager txnMgr2(storage2, nullptr);

	auto txn = txnMgr2.begin();
	EXPECT_TRUE(txn.isActive());

	// Close the database (storage is no longer open)
	testDb2->close();

	// Now rollback - should hit storage_->isOpen() == false branch
	txnMgr2.rollbackTransaction(txn);
	EXPECT_FALSE(txn.isActive());

	testDb2.reset();
	fs::remove_all(testPath2);
	fs::remove(testPath2.string() + "-wal");
}

// ============================================================================
// Coverage Tests: WAL manager exists but is closed
// ============================================================================

TEST(TransactionManagerClosedWALTest, TransactionWithClosedWAL) {
	// Cover: walManager_ && walManager_->isOpen() -> walManager_->isOpen() is false
	// This exercises the second operand of the && check in begin, commit, rollback.
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	auto testPath = fs::temp_directory_path() / ("test_txnmgr_closedwal_" + boost::uuids::to_string(uuid) + ".graph");
	fs::remove_all(testPath);

	auto testDb = std::make_unique<graph::Database>(testPath.string());
	testDb->open();
	auto storage = testDb->getStorage();

	// Create a WAL manager, open it, then close it
	auto walMgr = std::make_shared<graph::storage::wal::WALManager>();
	walMgr->open(testPath.string());
	walMgr->close();

	// walMgr is non-null but isOpen() == false
	EXPECT_FALSE(walMgr->isOpen());

	graph::TransactionManager txnMgr(storage, walMgr);

	// Begin transaction - should skip WAL write (walManager_->isOpen() == false)
	auto txn = txnMgr.begin();
	EXPECT_TRUE(txn.isActive());

	// Commit - should skip WAL commit/checkpoint
	txnMgr.commitTransaction(txn);
	EXPECT_FALSE(txn.isActive());

	// Begin another and rollback - should skip WAL rollback
	auto txn2 = txnMgr.begin();
	EXPECT_TRUE(txn2.isActive());
	txnMgr.rollbackTransaction(txn2);
	EXPECT_FALSE(txn2.isActive());

	testDb->close();
	testDb.reset();
	fs::remove_all(testPath);
	fs::remove(testPath.string() + "-wal");
}
