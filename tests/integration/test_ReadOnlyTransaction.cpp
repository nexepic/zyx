/**
 * @file test_ReadOnlyTransaction.cpp
 * @brief End-to-end integration tests for read-only transaction access control.
 *
 * Tests the 3-layer defense:
 * 1. Plan layer: QueryEngine rejects write plans under EM_READ_ONLY
 * 2. API layer: zyx::Transaction::isReadOnly() / graph::Transaction read-only
 * 3. Storage layer: DataManager::guardReadOnly() prevents writes
 *
 * Licensed under the Apache License, Version 2.0
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

// ============================================================================
// Integration tests via graph::Database (internal API)
// ============================================================================

class ReadOnlyTransactionInternalTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_ro_" + boost::uuids::to_string(uuid) + ".zyx");
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath);
		db = std::make_unique<graph::Database>(testDbPath.string());
		db->open();

		// Seed some data
		auto qe = db->getQueryEngine();
		auto txn = db->beginTransaction();
		qe->execute("CREATE (a:Person {name: 'Alice', age: 30})");
		qe->execute("CREATE (b:Person {name: 'Bob', age: 25})");
		qe->execute("MATCH (a:Person {name: 'Alice'}) MATCH (b:Person {name: 'Bob'}) CREATE (a)-[:KNOWS]->(b)");
		txn.commit();
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

	graph::query::QueryResult execute(const std::string &query) const {
		return db->getQueryEngine()->execute(query);
	}

	fs::path testDbPath;
	std::unique_ptr<graph::Database> db;
};

TEST_F(ReadOnlyTransactionInternalTest, ReadOnlyTxnAllowsRead) {
	auto txn = db->beginReadOnlyTransaction();
	auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.name");
	EXPECT_EQ(result.rowCount(), 2UL);
	txn.commit();
}

TEST_F(ReadOnlyTransactionInternalTest, ReadOnlyTxnBlocksCreate) {
	auto txn = db->beginReadOnlyTransaction();
	EXPECT_THROW(execute("CREATE (n:Person {name: 'Charlie'})"), std::runtime_error);
	txn.rollback();
}

TEST_F(ReadOnlyTransactionInternalTest, ReadOnlyTxnBlocksSet) {
	auto txn = db->beginReadOnlyTransaction();
	EXPECT_THROW(execute("MATCH (n:Person {name: 'Alice'}) SET n.age = 99"), std::runtime_error);
	txn.rollback();
}

TEST_F(ReadOnlyTransactionInternalTest, ReadOnlyTxnBlocksDelete) {
	auto txn = db->beginReadOnlyTransaction();
	EXPECT_THROW(execute("MATCH (n:Person {name: 'Bob'}) DELETE n"), std::runtime_error);
	txn.rollback();
}

TEST_F(ReadOnlyTransactionInternalTest, ReadOnlyTxnBlocksMerge) {
	auto txn = db->beginReadOnlyTransaction();
	EXPECT_THROW(execute("MERGE (n:Person {name: 'Dave'})"), std::runtime_error);
	txn.rollback();
}

TEST_F(ReadOnlyTransactionInternalTest, DataUnchangedAfterBlockedWrite) {
	// Attempt a write in a read-only transaction
	{
		auto txn = db->beginReadOnlyTransaction();
		EXPECT_THROW(execute("CREATE (n:Person {name: 'Ghost'})"), std::runtime_error);
		txn.rollback();
	}

	// Verify data is unchanged
	auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.name");
	EXPECT_EQ(result.rowCount(), 2UL);
}

TEST_F(ReadOnlyTransactionInternalTest, ReadWriteTxnStillWorks) {
	// A normal transaction should still allow writes
	auto txn = db->beginTransaction();
	auto result = execute("CREATE (n:Person {name: 'Charlie'}) RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
	txn.commit();

	auto verifyResult = execute("MATCH (n:Person) RETURN n.name");
	EXPECT_EQ(verifyResult.rowCount(), 3UL);
}

// ============================================================================
// Integration tests via zyx::Database (public API)
// ============================================================================

class ReadOnlyTransactionPublicTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_ro_pub_" + boost::uuids::to_string(uuid) + ".zyx");
		fs::remove_all(testDbPath);

		db = std::make_unique<zyx::Database>(testDbPath.string());
		db->open();

		// Seed data
		auto res = db->execute("CREATE (a:Person {name: 'Alice', age: 30})");
		ASSERT_TRUE(res.isSuccess());
		res = db->execute("CREATE (b:Person {name: 'Bob', age: 25})");
		ASSERT_TRUE(res.isSuccess());
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

TEST_F(ReadOnlyTransactionPublicTest, IsReadOnlyFlag) {
	auto txn = db->beginReadOnlyTransaction();
	EXPECT_TRUE(txn.isReadOnly());
	txn.commit();
}

TEST_F(ReadOnlyTransactionPublicTest, NormalTxnIsNotReadOnly) {
	auto txn = db->beginTransaction();
	EXPECT_FALSE(txn.isReadOnly());
	txn.rollback();
}

TEST_F(ReadOnlyTransactionPublicTest, ReadOnlyTxnAllowsRead) {
	auto txn = db->beginReadOnlyTransaction();
	auto res = txn.execute("MATCH (n:Person) RETURN n.name ORDER BY n.name");
	EXPECT_TRUE(res.isSuccess());

	int count = 0;
	while (res.hasNext()) {
		res.next();
		count++;
	}
	EXPECT_EQ(count, 2);
	txn.commit();
}

TEST_F(ReadOnlyTransactionPublicTest, ReadOnlyTxnBlocksCreate) {
	auto txn = db->beginReadOnlyTransaction();
	auto res = txn.execute("CREATE (n:Person {name: 'Charlie'})");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_FALSE(res.getError().empty());
	txn.rollback();
}

TEST_F(ReadOnlyTransactionPublicTest, ReadOnlyTxnBlocksSet) {
	auto txn = db->beginReadOnlyTransaction();
	auto res = txn.execute("MATCH (n:Person {name: 'Alice'}) SET n.age = 99");
	EXPECT_FALSE(res.isSuccess());
	txn.rollback();
}

TEST_F(ReadOnlyTransactionPublicTest, ReadOnlyTxnBlocksDelete) {
	auto txn = db->beginReadOnlyTransaction();
	auto res = txn.execute("MATCH (n:Person {name: 'Bob'}) DELETE n");
	EXPECT_FALSE(res.isSuccess());
	txn.rollback();
}

TEST_F(ReadOnlyTransactionPublicTest, DataUnchangedAfterBlockedWrite) {
	{
		auto txn = db->beginReadOnlyTransaction();
		auto res = txn.execute("CREATE (n:Person {name: 'Ghost'})");
		EXPECT_FALSE(res.isSuccess());
		txn.rollback();
	}

	// Verify only original data exists
	auto res = db->execute("MATCH (n:Person) RETURN n.name");
	EXPECT_TRUE(res.isSuccess());
	int count = 0;
	while (res.hasNext()) {
		res.next();
		count++;
	}
	EXPECT_EQ(count, 2);
}

TEST_F(ReadOnlyTransactionPublicTest, SequentialReadOnlyThenWriteTxn) {
	// Read-only transaction first
	{
		auto txn = db->beginReadOnlyTransaction();
		auto res = txn.execute("MATCH (n:Person) RETURN n.name");
		EXPECT_TRUE(res.isSuccess());
		txn.commit();
	}

	// Then a normal write transaction
	{
		auto txn = db->beginTransaction();
		auto res = txn.execute("CREATE (n:Person {name: 'Charlie'})");
		EXPECT_TRUE(res.isSuccess());
		txn.commit();
	}

	// Verify
	auto res = db->execute("MATCH (n:Person) RETURN n.name");
	EXPECT_TRUE(res.isSuccess());
	int count = 0;
	while (res.hasNext()) {
		res.next();
		count++;
	}
	EXPECT_EQ(count, 3);
}
