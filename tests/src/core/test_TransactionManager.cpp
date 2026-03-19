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
