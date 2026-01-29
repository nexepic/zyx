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
#include <filesystem>
#include "graph/core/Database.hpp"
#include "graph/core/Transaction.hpp"
#include "graph/storage/FileStorage.hpp"

class TransactionTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Create a temporary database path
		testDbPath = "test_transaction_db.graph";
		// Clean up any existing test database
		std::filesystem::remove_all(testDbPath);
	}

	void TearDown() override {
		// Clean up test database
		std::filesystem::remove_all(testDbPath);
	}

	std::string testDbPath;
};

TEST_F(TransactionTest, ConstructorInitializesStorage) {
	// Create a database and verify it can be used to create a transaction
	graph::Database db(testDbPath);
	db.open();

	ASSERT_TRUE(db.isOpen());

	// Create a transaction - this should initialize the transaction and begin a write
	graph::Transaction txn = db.beginTransaction();

	// Transaction should have been created successfully
	EXPECT_TRUE(db.isOpen());
}

TEST_F(TransactionTest, TransactionWithExistingDatabase) {
	// Create and open a database
	graph::Database db(testDbPath);
	db.open();
	ASSERT_TRUE(db.isOpen());

	// Create multiple transactions in sequence
	{
		graph::Transaction txn1 = db.beginTransaction();
		EXPECT_TRUE(db.isOpen());
		txn1.commit();
	}

	{
		graph::Transaction txn2 = db.beginTransaction();
		EXPECT_TRUE(db.isOpen());
		txn2.commit();
	}
}

TEST_F(TransactionTest, TransactionCommit) {
	graph::Database db(testDbPath);
	db.open();

	graph::Transaction txn = db.beginTransaction();

	// Commit should complete without throwing
	EXPECT_NO_THROW(txn.commit());

	// Database should still be open after commit
	EXPECT_TRUE(db.isOpen());
}

TEST_F(TransactionTest, TransactionRollback) {
	graph::Database db(testDbPath);
	db.open();

	// Rollback is a static method, but we create a transaction first
	graph::Transaction txn = db.beginTransaction();

	// Rollback should complete without throwing
	EXPECT_NO_THROW(graph::Transaction::rollback());

	// Database should still be open after rollback
	EXPECT_TRUE(db.isOpen());
}

TEST_F(TransactionTest, TransactionCommitAndRollbackSequence) {
	graph::Database db(testDbPath);
	db.open();

	// Commit a transaction
	{
		graph::Transaction txn1 = db.beginTransaction();
		EXPECT_NO_THROW(txn1.commit());
	}

	// Rollback a transaction
	{
		graph::Transaction txn2 = db.beginTransaction();
		EXPECT_NO_THROW(graph::Transaction::rollback());
	}

	// Commit another transaction
	{
		graph::Transaction txn3 = db.beginTransaction();
		EXPECT_NO_THROW(txn3.commit());
	}

	EXPECT_TRUE(db.isOpen());
}

TEST_F(TransactionTest, MultipleTransactionsInSequence) {
	graph::Database db(testDbPath);
	db.open();

	// Create multiple transactions in sequence
	for (int i = 0; i < 5; ++i) {
		graph::Transaction txn = db.beginTransaction();
		EXPECT_NO_THROW(txn.commit());
	}

	EXPECT_TRUE(db.isOpen());
}

TEST_F(TransactionTest, MultipleRollbacksInSequence) {
	graph::Database db(testDbPath);
	db.open();

	// Create multiple transactions with rollback in sequence
	for (int i = 0; i < 5; ++i) {
		graph::Transaction txn = db.beginTransaction();
		EXPECT_NO_THROW(graph::Transaction::rollback());
	}

	EXPECT_TRUE(db.isOpen());
}

TEST_F(TransactionTest, MixedCommitAndRollback) {
	graph::Database db(testDbPath);
	db.open();

	// Mix of commits and rollbacks
	{
		graph::Transaction txn1 = db.beginTransaction();
		EXPECT_NO_THROW(txn1.commit());
	}

	{
		graph::Transaction txn2 = db.beginTransaction();
		EXPECT_NO_THROW(graph::Transaction::rollback());
	}

	{
		graph::Transaction txn3 = db.beginTransaction();
		EXPECT_NO_THROW(txn3.commit());
	}

	{
		graph::Transaction txn4 = db.beginTransaction();
		EXPECT_NO_THROW(graph::Transaction::rollback());
	}

	EXPECT_TRUE(db.isOpen());
}
