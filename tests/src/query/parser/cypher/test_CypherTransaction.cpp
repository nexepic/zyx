/**
 * @file test_CypherTransaction.cpp
 * @date 2026/3/26
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

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "zyx/zyx.hpp"

namespace fs = std::filesystem;

// ============================================================================
// Functional Tests: Cypher Transaction via zyx::Database public API
// ============================================================================

class CypherTransactionTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_cypher_txn_" + boost::uuids::to_string(uuid) + ".graph");
		fs::remove_all(testDbPath);

		db = std::make_unique<zyx::Database>(testDbPath.string());
		db->open();
	}

	void TearDown() override {
		db.reset();
		fs::remove_all(testDbPath);
		fs::remove(testDbPath.string() + "-wal");
	}

	fs::path testDbPath;
	std::unique_ptr<zyx::Database> db;
};

// --- Grammar Parse Tests ---

TEST_F(CypherTransactionTest, BeginParsesSuccessfully) {
	auto res = db->execute("BEGIN");
	EXPECT_TRUE(res.isSuccess()) << res.getError();
}

TEST_F(CypherTransactionTest, BeginTransactionParsesSuccessfully) {
	auto res = db->execute("BEGIN TRANSACTION");
	EXPECT_TRUE(res.isSuccess()) << res.getError();
	// Clean up the transaction
	(void) db->execute("ROLLBACK");
}

TEST_F(CypherTransactionTest, CommitParsesAfterBegin) {
	(void) db->execute("BEGIN");
	auto res = db->execute("COMMIT");
	EXPECT_TRUE(res.isSuccess()) << res.getError();
}

TEST_F(CypherTransactionTest, RollbackParsesAfterBegin) {
	(void) db->execute("BEGIN");
	auto res = db->execute("ROLLBACK");
	EXPECT_TRUE(res.isSuccess()) << res.getError();
}

TEST_F(CypherTransactionTest, CaseInsensitiveParsing) {
	auto res = db->execute("begin transaction");
	EXPECT_TRUE(res.isSuccess()) << res.getError();
	(void) db->execute("rollback");
}

// --- Functional Tests ---

TEST_F(CypherTransactionTest, BeginCommit_PersistsData) {
	auto beginRes = db->execute("BEGIN");
	EXPECT_TRUE(beginRes.isSuccess()) << beginRes.getError();

	(void) db->execute("CREATE (n:TxnTest {name: 'Alice'})");

	auto commitRes = db->execute("COMMIT");
	EXPECT_TRUE(commitRes.isSuccess()) << commitRes.getError();

	// Verify data persists
	auto queryRes = db->execute("MATCH (n:TxnTest) RETURN n.name");
	EXPECT_TRUE(queryRes.isSuccess());
	EXPECT_TRUE(queryRes.hasNext());
}

TEST_F(CypherTransactionTest, BeginRollback_RevertsData) {
	auto beginRes = db->execute("BEGIN");
	EXPECT_TRUE(beginRes.isSuccess()) << beginRes.getError();

	(void) db->execute("CREATE (n:TxnTest {name: 'Bob'})");

	auto rollbackRes = db->execute("ROLLBACK");
	EXPECT_TRUE(rollbackRes.isSuccess()) << rollbackRes.getError();

	// Verify data was rolled back
	auto queryRes = db->execute("MATCH (n:TxnTest) RETURN n.name");
	EXPECT_TRUE(queryRes.isSuccess());
	EXPECT_FALSE(queryRes.hasNext());
}

TEST_F(CypherTransactionTest, BeginWithinBegin_Error) {
	auto res1 = db->execute("BEGIN");
	EXPECT_TRUE(res1.isSuccess());

	auto res2 = db->execute("BEGIN");
	EXPECT_FALSE(res2.isSuccess());
	EXPECT_NE(res2.getError().find("already active"), std::string::npos);

	(void) db->execute("ROLLBACK");
}

TEST_F(CypherTransactionTest, CommitWithoutBegin_Error) {
	auto res = db->execute("COMMIT");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_NE(res.getError().find("No active transaction"), std::string::npos);
}

TEST_F(CypherTransactionTest, RollbackWithoutBegin_Error) {
	auto res = db->execute("ROLLBACK");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_NE(res.getError().find("No active transaction"), std::string::npos);
}

TEST_F(CypherTransactionTest, MultipleQueriesInTransaction) {
	(void) db->execute("BEGIN");

	(void) db->execute("CREATE (n:Multi {id: 1})");
	(void) db->execute("CREATE (n:Multi {id: 2})");
	(void) db->execute("CREATE (n:Multi {id: 3})");

	(void) db->execute("COMMIT");

	auto queryRes = db->execute("MATCH (n:Multi) RETURN n.id");
	EXPECT_TRUE(queryRes.isSuccess());

	int count = 0;
	while (queryRes.hasNext()) {
		queryRes.next();
		count++;
	}
	EXPECT_EQ(count, 3);
}

TEST_F(CypherTransactionTest, HasActiveTransactionReflectsCypherTxn) {
	EXPECT_FALSE(db->hasActiveTransaction());

	(void) db->execute("BEGIN");
	EXPECT_TRUE(db->hasActiveTransaction());

	(void) db->execute("COMMIT");
	EXPECT_FALSE(db->hasActiveTransaction());
}
