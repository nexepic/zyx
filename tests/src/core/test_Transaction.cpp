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
