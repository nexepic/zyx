/**
 * @file test_IntegrationTransaction.cpp
 * @date 2026/02/03
 *
 * @copyright Copyright (c) 2026
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

#include "graph/core/Database.hpp"
#include "graph/core/Transaction.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "zyx/zyx.hpp"

namespace fs = std::filesystem;
using namespace graph;

/**
 * Integration test fixture for transaction ACID properties
 */
class IntegrationTransactionTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_txn_" + boost::uuids::to_string(uuid) + ".zyx");
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath);
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
	}

	void TearDown() override {
		if (db)
			db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath, ec);
		fs::remove(testDbPath.string() + "-wal", ec);
	}

	query::QueryResult execute(const std::string &query) const { return db->getQueryEngine()->execute(query); }

	std::filesystem::path testDbPath;
	std::unique_ptr<Database> db;
};

/**
 * Test Atomicity: Commit persists all changes
 */
TEST_F(IntegrationTransactionTest, Atomicity_Commit) {
	auto txn = db->beginTransaction();
	(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
	(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");
	txn.commit();

	auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.name");
	EXPECT_EQ(result.rowCount(), 2UL);
}

/**
 * Test Atomicity: Rollback reverts all changes
 */
TEST_F(IntegrationTransactionTest, Atomicity_Rollback) {
	// Pre-existing data
	{
		auto txn = db->beginTransaction();
		(void) execute("CREATE (n:Person {name: 'Charlie', age: 35})");
		txn.commit();
	}

	auto beforeResult = execute("MATCH (n:Person) RETURN n.name");
	ASSERT_EQ(beforeResult.rowCount(), 1UL);

	// Transaction that will be rolled back
	{
		auto txn = db->beginTransaction();
		(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
		(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");
		txn.rollback();
	}

	// After rollback, only Charlie should remain
	auto afterResult = execute("MATCH (n:Person) RETURN n.name ORDER BY n.name");
	EXPECT_EQ(afterResult.rowCount(), 1UL);
}

/**
 * Test Consistency: Relationships remain consistent
 */
TEST_F(IntegrationTransactionTest, Consistency_AfterCommit) {
	auto txn = db->beginTransaction();
	(void) execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");
	(void) execute("CREATE (b:Person {name: 'Bob'})-[:KNOWS]->(c:Person {name: 'Charlie'})");
	txn.commit();

	// Verify Alice knows Bob
	auto result1 = execute("MATCH (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'}) RETURN b.name");
	EXPECT_EQ(result1.rowCount(), 1UL);

	// Verify Bob knows Charlie
	auto result2 = execute("MATCH (a:Person {name: 'Bob'})-[:KNOWS]->(b:Person {name: 'Charlie'}) RETURN b.name");
	EXPECT_EQ(result2.rowCount(), 1UL);
}

/**
 * Test Isolation: Sequential transactions don't interfere
 */
TEST_F(IntegrationTransactionTest, Isolation_SequentialTransactions) {
	{
		auto txn1 = db->beginTransaction();
		(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
		txn1.commit();
	}

	{
		auto txn2 = db->beginTransaction();
		(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");
		txn2.commit();
	}

	auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.name");
	EXPECT_EQ(result.rowCount(), 2UL);
}

/**
 * Test Durability: Data persists after database reopen
 */
TEST_F(IntegrationTransactionTest, Durability_PersistAfterReopen) {
	{
		auto txn = db->beginTransaction();
		(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
		(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");
		txn.commit();
		db->close();
	}

	{
		Database db2(testDbPath.string());
		db2.open();
		auto queryEngine2 = db2.getQueryEngine();
		auto result = queryEngine2->execute("MATCH (n:Person) RETURN n.name ORDER BY n.name");
		EXPECT_EQ(result.rowCount(), 2UL);
		db2.close();
	}
}

/**
 * Test transaction with CREATE operations
 */
TEST_F(IntegrationTransactionTest, TransactionWithCreate) {
	auto txn = db->beginTransaction();
	(void) execute("CREATE (n:Person {name: 'Alice'})");
	(void) execute("CREATE (n:Person {name: 'Bob'})");
	(void) execute("CREATE (n:Person {name: 'Charlie'})");
	txn.commit();

	auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.name");
	EXPECT_EQ(result.rowCount(), 3UL);
}

/**
 * Test transaction with UPDATE operations
 */
TEST_F(IntegrationTransactionTest, TransactionWithUpdate) {
	{
		auto txn = db->beginTransaction();
		(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
		(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");
		txn.commit();
	}

	{
		auto txn = db->beginTransaction();
		(void) execute("MATCH (n:Person {name: 'Alice'}) SET n.age = 35");
		txn.commit();
	}

	auto result = execute("MATCH (n:Person) WHERE n.age > 30 RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test transaction with DELETE operations
 */
TEST_F(IntegrationTransactionTest, TransactionWithDelete) {
	{
		auto txn = db->beginTransaction();
		(void) execute("CREATE (n:Person {name: 'Alice', active: false})");
		(void) execute("CREATE (n:Person {name: 'Bob', active: false})");
		(void) execute("CREATE (n:Person {name: 'Charlie', active: true})");
		txn.commit();
	}

	{
		auto txn = db->beginTransaction();
		(void) execute("MATCH (n:Person) WHERE n.active = false DELETE n");
		txn.commit();
	}

	auto result = execute("MATCH (n:Person) RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test transaction with relationships
 */
TEST_F(IntegrationTransactionTest, TransactionWithRelationships) {
	auto txn = db->beginTransaction();
	(void) execute("CREATE (a:Person {name: 'Alice'})");
	(void) execute("CREATE (b:Person {name: 'Bob'})");
	(void) execute("MATCH (a:Person {name: 'Alice'}) MATCH (b:Person {name: 'Bob'}) CREATE (a)-[:KNOWS]->(b)");
	txn.commit();

	auto result = execute("MATCH (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person) RETURN b.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test multiple sequential transactions
 */
TEST_F(IntegrationTransactionTest, MultipleSequentialTransactions) {
	{
		auto txn1 = db->beginTransaction();
		(void) execute("CREATE (n:Person {name: 'Alice'})");
		txn1.commit();
	}

	{
		auto txn2 = db->beginTransaction();
		(void) execute("CREATE (n:Person {name: 'Bob'})");
		txn2.commit();
	}

	{
		auto txn3 = db->beginTransaction();
		(void) execute("CREATE (n:Person {name: 'Charlie'})");
		txn3.commit();
	}

	auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.name");
	EXPECT_EQ(result.rowCount(), 3UL);
}

/**
 * Test auto-rollback on scope exit (destructor)
 */
TEST_F(IntegrationTransactionTest, AutoRollbackOnScopeExit) {
	// Pre-existing data
	{
		auto txn = db->beginTransaction();
		(void) execute("CREATE (n:Person {name: 'Alice'})");
		txn.commit();
	}

	// This transaction should be auto-rolled back
	{
		auto txn = db->beginTransaction();
		(void) execute("CREATE (n:Person {name: 'Bob'})");
		// No commit() - destructor auto-rollback
	}

	// Only Alice should exist
	auto result = execute("MATCH (n:Person) RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test transaction commit then query
 */
TEST_F(IntegrationTransactionTest, CommitThenQuery) {
	auto txn = db->beginTransaction();
	(void) execute("CREATE (n:Animal {name: 'Dog', legs: 4})");
	(void) execute("CREATE (n:Animal {name: 'Cat', legs: 4})");
	(void) execute("CREATE (n:Animal {name: 'Spider', legs: 8})");
	txn.commit();

	auto result = execute("MATCH (n:Animal) WHERE n.legs = 4 RETURN n.name ORDER BY n.name");
	EXPECT_EQ(result.rowCount(), 2UL);
}

// ============================================================================
// Bulk API Transaction Tests (via zyx::Database public API)
// ============================================================================

class BulkApiTransactionTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_bulk_txn_" + boost::uuids::to_string(uuid) + ".zyx");
		fs::remove_all(testDbPath);

		db = std::make_unique<zyx::Database>(testDbPath.string());
		db->open();
	}

	void TearDown() override {
		db.reset();
		std::error_code ec;
		fs::remove_all(testDbPath, ec);
		fs::remove(testDbPath.string() + "-wal", ec);
	}

	fs::path testDbPath;
	std::unique_ptr<zyx::Database> db;
};

TEST_F(BulkApiTransactionTest, BulkApiWithImplicitTransaction) {
	// createNodes without active txn should auto-commit
	std::vector<std::unordered_map<std::string, zyx::Value>> batch;
	for (int i = 0; i < 5; ++i) {
		batch.push_back({{"id", static_cast<int64_t>(i)}});
	}
	db->createNodes("BulkTest", batch);

	auto res = db->execute("MATCH (n:BulkTest) RETURN n.id");
	EXPECT_TRUE(res.isSuccess());
	int count = 0;
	while (res.hasNext()) {
		res.next();
		count++;
	}
	EXPECT_EQ(count, 5);
}

TEST_F(BulkApiTransactionTest, BulkApiWithExplicitTransaction) {
	// Begin explicit transaction, use bulk API, commit
	auto txn = db->beginTransaction();

	int64_t nodeId = db->createNode("BulkTest", {{"name", std::string("Alice")}});
	EXPECT_GT(nodeId, 0);

	txn.commit();

	auto res = db->execute("MATCH (n:BulkTest) RETURN n.name");
	EXPECT_TRUE(res.isSuccess());
	EXPECT_TRUE(res.hasNext());
}

TEST_F(BulkApiTransactionTest, BulkApiRollback) {
	// Begin explicit transaction, use bulk API, rollback
	auto txn = db->beginTransaction();

	(void) db->createNode("BulkTest", {{"name", std::string("Bob")}});

	txn.rollback();

	auto res = db->execute("MATCH (n:BulkTest) RETURN n.name");
	EXPECT_TRUE(res.isSuccess());
	EXPECT_FALSE(res.hasNext());
}
