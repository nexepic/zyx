/**
 * @file test_Transaction.cpp
 * @author Nexepic
 * @date 2025/1/29
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

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include "graph/core/Database.hpp"
#include "graph/core/Transaction.hpp"

namespace fs = std::filesystem;

class TransactionTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_txn_" + boost::uuids::to_string(uuid) + ".graph");
		fs::remove_all(testDbPath);
	}

	void TearDown() override { fs::remove_all(testDbPath); }

	fs::path testDbPath;
};

TEST_F(TransactionTest, BeginTransactionReturnsActiveTransaction) {
	graph::Database db(testDbPath.string());
	db.open();
	ASSERT_TRUE(db.isOpen());

	auto txn = db.beginTransaction();
	EXPECT_TRUE(txn.isActive());
	EXPECT_EQ(txn.getState(), graph::Transaction::TxnState::TXN_ACTIVE);
	EXPECT_GT(txn.getId(), 0UL);
	txn.commit();
}

TEST_F(TransactionTest, CommitChangesState) {
	graph::Database db(testDbPath.string());
	db.open();

	auto txn = db.beginTransaction();
	EXPECT_TRUE(txn.isActive());

	EXPECT_NO_THROW(txn.commit());
	EXPECT_FALSE(txn.isActive());
	EXPECT_EQ(txn.getState(), graph::Transaction::TxnState::TXN_COMMITTED);
}

TEST_F(TransactionTest, RollbackChangesState) {
	graph::Database db(testDbPath.string());
	db.open();

	auto txn = db.beginTransaction();
	EXPECT_TRUE(txn.isActive());

	EXPECT_NO_THROW(txn.rollback());
	EXPECT_FALSE(txn.isActive());
	EXPECT_EQ(txn.getState(), graph::Transaction::TxnState::TXN_ROLLED_BACK);
}

TEST_F(TransactionTest, DoubleCommitThrows) {
	graph::Database db(testDbPath.string());
	db.open();

	auto txn = db.beginTransaction();
	txn.commit();
	EXPECT_THROW(txn.commit(), std::runtime_error);
}

TEST_F(TransactionTest, DoubleRollbackThrows) {
	graph::Database db(testDbPath.string());
	db.open();

	auto txn = db.beginTransaction();
	txn.rollback();
	EXPECT_THROW(txn.rollback(), std::runtime_error);
}

TEST_F(TransactionTest, CommitAfterRollbackThrows) {
	graph::Database db(testDbPath.string());
	db.open();

	auto txn = db.beginTransaction();
	txn.rollback();
	EXPECT_THROW(txn.commit(), std::runtime_error);
}

TEST_F(TransactionTest, AutoRollbackOnScopeExit) {
	graph::Database db(testDbPath.string());
	db.open();

	{
		auto txn = db.beginTransaction();
		EXPECT_TRUE(txn.isActive());
		// Destructor auto-rolls back
	}

	// Should be able to start a new transaction
	auto txn2 = db.beginTransaction();
	EXPECT_TRUE(txn2.isActive());
	txn2.commit();
}

TEST_F(TransactionTest, MoveSemantics) {
	graph::Database db(testDbPath.string());
	db.open();

	auto txn1 = db.beginTransaction();
	uint64_t id = txn1.getId();

	// Move construction
	auto txn2 = std::move(txn1);
	EXPECT_TRUE(txn2.isActive());
	EXPECT_EQ(txn2.getId(), id);
	EXPECT_FALSE(txn1.isActive()); // NOLINT - intentionally checking moved-from state

	txn2.commit();
}

TEST_F(TransactionTest, SequentialTransactions) {
	graph::Database db(testDbPath.string());
	db.open();

	for (int i = 0; i < 5; ++i) {
		auto txn = db.beginTransaction();
		EXPECT_TRUE(txn.isActive());
		txn.commit();
	}

	EXPECT_TRUE(db.isOpen());
}

TEST_F(TransactionTest, MixedCommitAndRollback) {
	graph::Database db(testDbPath.string());
	db.open();

	{
		auto txn = db.beginTransaction();
		txn.commit();
	}
	{
		auto txn = db.beginTransaction();
		txn.rollback();
	}
	{
		auto txn = db.beginTransaction();
		txn.commit();
	}
	{
		auto txn = db.beginTransaction();
		txn.rollback();
	}

	EXPECT_TRUE(db.isOpen());
}

TEST_F(TransactionTest, RecordOperations) {
	graph::Database db(testDbPath.string());
	db.open();

	auto txn = db.beginTransaction();

	// Manually record operations
	txn.recordOperation({graph::Transaction::TxnOperation::OP_ADD, 0, 1});
	txn.recordOperation({graph::Transaction::TxnOperation::OP_UPDATE, 0, 1});
	txn.recordOperation({graph::Transaction::TxnOperation::OP_DELETE, 0, 1});

	EXPECT_EQ(txn.getOperations().size(), 3UL);
	EXPECT_EQ(txn.getOperations()[0].opType, graph::Transaction::TxnOperation::OP_ADD);
	EXPECT_EQ(txn.getOperations()[1].opType, graph::Transaction::TxnOperation::OP_UPDATE);
	EXPECT_EQ(txn.getOperations()[2].opType, graph::Transaction::TxnOperation::OP_DELETE);

	txn.rollback();
}

TEST_F(TransactionTest, HasActiveTransaction) {
	graph::Database db(testDbPath.string());
	db.open();

	EXPECT_FALSE(db.hasActiveTransaction());

	auto txn = db.beginTransaction();
	EXPECT_TRUE(db.hasActiveTransaction());

	txn.commit();
	EXPECT_FALSE(db.hasActiveTransaction());
}

// ============================================================================
// Branch Coverage Tests: Move assignment operator (lines 46-66)
// ============================================================================

TEST_F(TransactionTest, MoveAssignmentToDefaultConstructedLikeTransaction) {
	graph::Database db(testDbPath.string());
	db.open();

	// Create first transaction, commit it (not active)
	auto txn1 = db.beginTransaction();
	txn1.commit();

	// Create second transaction (active)
	auto txn2 = db.beginTransaction();
	uint64_t id2 = txn2.getId();
	EXPECT_TRUE(txn2.isActive());

	// Move-assign txn2 into txn1: exercises operator=(Transaction&&)
	// txn1 is not active (committed), so the "if active, rollback" check inside
	// operator= will take the false branch (state_ != TXN_ACTIVE).
	txn1 = std::move(txn2);

	EXPECT_TRUE(txn1.isActive());
	EXPECT_EQ(txn1.getId(), id2);
	EXPECT_FALSE(txn2.isActive()); // NOLINT - intentionally checking moved-from state

	txn1.commit();
}

TEST_F(TransactionTest, MoveAssignmentFromActiveToActive) {
	// Cover: operator=(&&) line 49 True branch: state_ == TXN_ACTIVE && manager_
	// We need txn1 to be ACTIVE when we move-assign txn2 into it.
	// Since we use single-writer lock, we need two separate TransactionManagers
	// each with their own database to have two active transactions.

	// Create two separate databases with separate TransactionManagers
	boost::uuids::uuid uuid2 = boost::uuids::random_generator()();
	auto testDbPath2 = fs::temp_directory_path() / ("test_txn_move2_" + boost::uuids::to_string(uuid2) + ".graph");
	fs::remove_all(testDbPath2);

	graph::Database db1(testDbPath.string());
	db1.open();
	graph::Database db2(testDbPath2.string());
	db2.open();

	// txn1 is active from db1
	auto txn1 = db1.beginTransaction();
	EXPECT_TRUE(txn1.isActive());

	// txn2 is active from db2
	auto txn2 = db2.beginTransaction();
	uint64_t id2 = txn2.getId();
	EXPECT_TRUE(txn2.isActive());

	// Move-assign txn2 into txn1
	// This should trigger auto-rollback of txn1 (active -> rolled back)
	// then take ownership of txn2's state
	txn1 = std::move(txn2);

	EXPECT_TRUE(txn1.isActive());
	EXPECT_EQ(txn1.getId(), id2);
	EXPECT_FALSE(txn2.isActive()); // NOLINT - checking moved-from state

	txn1.commit();

	// db1's transaction was auto-rolled-back via move-assign, so we can begin another
	auto txn3 = db1.beginTransaction();
	txn3.commit();

	db1.close();
	db2.close();
	fs::remove_all(testDbPath2);
	fs::remove(testDbPath2.string() + "-wal");
}

TEST_F(TransactionTest, MoveAssignmentSelfAssignment) {
	// Cover: operator=(&&) line 47 False branch: this == &other (self-assignment)
	graph::Database db(testDbPath.string());
	db.open();

	auto txn = db.beginTransaction();
	uint64_t id = txn.getId();
	EXPECT_TRUE(txn.isActive());

	// Self-assignment should be a no-op
	txn = std::move(txn); // NOLINT - intentionally testing self-assignment

	EXPECT_TRUE(txn.isActive());
	EXPECT_EQ(txn.getId(), id);

	txn.commit();
}
