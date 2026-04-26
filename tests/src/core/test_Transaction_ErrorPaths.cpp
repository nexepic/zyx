/**
 * @file test_Transaction_ErrorPaths.cpp
 * @brief Tests targeting uncovered branches in Transaction.cpp:
 *        - Destructor catch(...) block (line 33-37)
 *        - Move assignment with active transaction rollback failure
 */

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include "graph/core/Database.hpp"
#include "graph/core/Transaction.hpp"

namespace fs = std::filesystem;

class TransactionErrorPathTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_txn_err_" + boost::uuids::to_string(uuid) + ".zyx");
		fs::remove_all(testDbPath);
	}

	void TearDown() override {
		std::error_code ec;
		fs::remove_all(testDbPath, ec);
		fs::remove(testDbPath.string() + "-wal", ec);
	}

	fs::path testDbPath;
};

// ============================================================================
// Destructor auto-rollback after database close
// When storage is closed before the transaction is destroyed, the destructor's
// rollback call may fail. The catch(...) block in the destructor should handle
// this gracefully by releasing locks even if rollback throws.
// ============================================================================

TEST_F(TransactionErrorPathTest, DestructorHandlesClosedStorage) {
	// Create database and begin transaction
	auto db = std::make_unique<graph::Database>(testDbPath.string());
	db->open();

	// Use a raw pointer to the storage so we can close it explicitly
	auto storage = db->getStorage();
	auto txnMgr = std::make_unique<graph::TransactionManager>(storage, nullptr);

	auto txn = txnMgr->begin();
	EXPECT_TRUE(txn.isActive());

	// Close the database storage while transaction is still active
	db->close();

	// Now let txn go out of scope. The destructor calls rollbackTransaction,
	// which checks storage_->isOpen() and should handle the closed state.
	// This exercises the rollback path when storage is closed.
}

// ============================================================================
// Move assignment: self-assignment is a no-op
// ============================================================================

TEST_F(TransactionErrorPathTest, MoveAssignmentSelfAssignment) {
	auto db = std::make_unique<graph::Database>(testDbPath.string());
	db->open();

	auto txn = db->beginTransaction();
	uint64_t id = txn.getId();
	EXPECT_TRUE(txn.isActive());

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
	txn = std::move(txn);
#pragma clang diagnostic pop

	EXPECT_TRUE(txn.isActive());
	EXPECT_EQ(txn.getId(), id);
	txn.commit();
}

// ============================================================================
// Move construction from committed transaction
// ============================================================================

TEST_F(TransactionErrorPathTest, MoveConstructFromCommitted) {
	auto db = std::make_unique<graph::Database>(testDbPath.string());
	db->open();

	auto txn1 = db->beginTransaction();
	txn1.commit();
	EXPECT_FALSE(txn1.isActive());

	// Move construction from committed transaction
	auto txn2 = std::move(txn1);
	// txn2 now holds the committed state
	EXPECT_FALSE(txn2.isActive());
	EXPECT_EQ(txn2.getState(), graph::Transaction::TxnState::TXN_COMMITTED);
}

// ============================================================================
// Move assignment: target is committed, source is active
// This exercises the false branch of (state_ == TXN_ACTIVE) in operator=
// ============================================================================

TEST_F(TransactionErrorPathTest, MoveAssignCommittedFromActive) {
	auto db = std::make_unique<graph::Database>(testDbPath.string());
	db->open();

	auto txn1 = db->beginTransaction();
	txn1.commit();
	EXPECT_FALSE(txn1.isActive());

	auto txn2 = db->beginTransaction();
	uint64_t id2 = txn2.getId();
	EXPECT_TRUE(txn2.isActive());

	// Move-assign active txn2 into committed txn1
	txn1 = std::move(txn2);
	EXPECT_TRUE(txn1.isActive());
	EXPECT_EQ(txn1.getId(), id2);
	txn1.commit();
}

// ============================================================================
// Move assignment: target is rolled back, source is active
// ============================================================================

TEST_F(TransactionErrorPathTest, MoveAssignRolledBackFromActive) {
	auto db = std::make_unique<graph::Database>(testDbPath.string());
	db->open();

	auto txn1 = db->beginTransaction();
	txn1.rollback();
	EXPECT_FALSE(txn1.isActive());
	EXPECT_EQ(txn1.getState(), graph::Transaction::TxnState::TXN_ROLLED_BACK);

	auto txn2 = db->beginTransaction();
	uint64_t id2 = txn2.getId();
	EXPECT_TRUE(txn2.isActive());

	// Move-assign active txn2 into rolled-back txn1
	txn1 = std::move(txn2);
	EXPECT_TRUE(txn1.isActive());
	EXPECT_EQ(txn1.getId(), id2);
	txn1.commit();
}

// ============================================================================
// Read-only transaction: commit and rollback
// ============================================================================

TEST_F(TransactionErrorPathTest, ReadOnlyDoubleCommitThrows) {
	auto db = std::make_unique<graph::Database>(testDbPath.string());
	db->open();

	auto txn = db->beginReadOnlyTransaction();
	EXPECT_TRUE(txn.isReadOnly());
	txn.commit();
	EXPECT_THROW(txn.commit(), std::runtime_error);
}

TEST_F(TransactionErrorPathTest, ReadOnlyDoubleRollbackThrows) {
	auto db = std::make_unique<graph::Database>(testDbPath.string());
	db->open();

	auto txn = db->beginReadOnlyTransaction();
	EXPECT_TRUE(txn.isReadOnly());
	txn.rollback();
	EXPECT_THROW(txn.rollback(), std::runtime_error);
}

// ============================================================================
// Move assignment: target is ACTIVE (triggers rollback in operator=)
// This exercises the true branch of (state_ == TXN_ACTIVE && manager_)
// inside operator=, which was previously uncovered.
// ============================================================================

TEST_F(TransactionErrorPathTest, MoveAssignActiveTargetTriggersRollback) {
	auto db = std::make_unique<graph::Database>(testDbPath.string());
	db->open();

	auto storage = db->getStorage();

	// Use two separate TransactionManagers so we can have two active txns
	// without deadlocking on the shared_mutex.
	graph::TransactionManager txnMgr1(storage, nullptr);
	graph::TransactionManager txnMgr2(storage, nullptr);

	// txn1 is active (will be the move-assign target)
	auto txn1 = txnMgr1.begin();
	EXPECT_TRUE(txn1.isActive());

	// txn2 is active (will be the move-assign source)
	auto txn2 = txnMgr2.begin();
	EXPECT_TRUE(txn2.isActive());
	uint64_t id2 = txn2.getId();

	// Move-assign txn2 into txn1: txn1 is ACTIVE so operator= rolls it back first
	txn1 = std::move(txn2);
	EXPECT_TRUE(txn1.isActive());
	EXPECT_EQ(txn1.getId(), id2);
	txnMgr2.commitTransaction(txn1);
}

// ============================================================================
// Destructor auto-rollback of read-only transaction
// Tests the destructor path where state_ == TXN_ACTIVE and the transaction
// is read-only — exercises the readOnly branch in rollbackTransaction
// ============================================================================

TEST_F(TransactionErrorPathTest, DestructorAutoRollbackReadOnly) {
	auto db = std::make_unique<graph::Database>(testDbPath.string());
	db->open();

	{
		auto txn = db->beginReadOnlyTransaction();
		EXPECT_TRUE(txn.isActive());
		EXPECT_TRUE(txn.isReadOnly());
		// Let txn go out of scope without commit or rollback
		// Destructor should auto-rollback
	}

	// Verify database is still usable
	auto txn2 = db->beginTransaction();
	EXPECT_TRUE(txn2.isActive());
	txn2.commit();
}
