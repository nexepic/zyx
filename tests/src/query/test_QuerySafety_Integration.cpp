/**
 * @file test_QuerySafety_Integration.cpp
 * @brief Integration tests for QueryGuard timeout, memory limits, and var-length depth clamping.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/core/Transaction.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"
#include "graph/storage/state/SystemStateManager.hpp"

namespace fs = std::filesystem;

class QuerySafetyTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_safety_" + boost::uuids::to_string(uuid) + ".graph");
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath);
		db = std::make_unique<graph::Database>(testDbPath.string());
		db->open();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath, ec);
		fs::remove(testDbPath.string() + "-wal", ec);
	}

	void setConfig(const std::string &key, int64_t value) {
		auto stateManager = std::make_shared<graph::storage::state::SystemStateManager>(
			db->getStorage()->getDataManager());
		stateManager->set<int64_t>(graph::storage::state::keys::SYS_CONFIG, key, value);
	}

	graph::query::QueryResult execute(const std::string &query) const {
		return db->getQueryEngine()->execute(query);
	}

	fs::path testDbPath;
	std::unique_ptr<graph::Database> db;
};

// Build a linear chain: n0 -> n1 -> n2 -> ... -> nN
void buildLinearChain(graph::Database &db, int length) {
	auto qe = db.getQueryEngine();
	auto txn = db.beginTransaction();

	qe->execute("CREATE (n:Start {idx: 0})");
	for (int i = 1; i <= length; ++i) {
		qe->execute("CREATE (n:Node {idx: " + std::to_string(i) + "})");
	}
	for (int i = 0; i < length; ++i) {
		qe->execute("MATCH (a {idx: " + std::to_string(i) + "}) "
		            "MATCH (b {idx: " + std::to_string(i + 1) + "}) "
		            "CREATE (a)-[:NEXT]->(b)");
	}
	txn.commit();
}

// Build a cycle: n0 -> n1 -> n2 -> n0
void buildCyclicGraph(graph::Database &db) {
	auto qe = db.getQueryEngine();
	auto txn = db.beginTransaction();

	qe->execute("CREATE (a:Node {name: 'A'})");
	qe->execute("CREATE (b:Node {name: 'B'})");
	qe->execute("CREATE (c:Node {name: 'C'})");
	qe->execute("MATCH (a:Node {name: 'A'}) MATCH (b:Node {name: 'B'}) CREATE (a)-[:LINK]->(b)");
	qe->execute("MATCH (b:Node {name: 'B'}) MATCH (c:Node {name: 'C'}) CREATE (b)-[:LINK]->(c)");
	qe->execute("MATCH (c:Node {name: 'C'}) MATCH (a:Node {name: 'A'}) CREATE (c)-[:LINK]->(a)");
	txn.commit();
}

TEST_F(QuerySafetyTest, QueryTimeout_LongRunningQueryThrows) {
	// Set very short timeout (1ms)
	setConfig(graph::storage::state::keys::Config::QUERY_TIMEOUT_MS, 1);

	// Create enough nodes so that a cartesian product generates significant work
	{
		auto qe = db->getQueryEngine();
		auto txn = db->beginTransaction();
		for (int i = 0; i < 100; ++i) {
			qe->execute("CREATE (n:Item {idx: " + std::to_string(i) + "})");
		}
		txn.commit();
	}

	auto txn = db->beginReadOnlyTransaction();
	// Cartesian product of 100 x 100 x 100 = 1M combinations — triggers timeout
	EXPECT_THROW(
		execute("MATCH (a:Item),(b:Item),(c:Item) RETURN count(*)"),
		graph::query::QueryTimeoutException
	);
	txn.rollback();
}

TEST_F(QuerySafetyTest, QueryTimeout_NormalQuerySucceeds) {
	// Default timeout (30s) — simple query should succeed
	auto qe = db->getQueryEngine();
	auto txn = db->beginTransaction();
	qe->execute("CREATE (n:Person {name: 'Test'})");
	txn.commit();

	auto txn2 = db->beginReadOnlyTransaction();
	auto result = execute("MATCH (n:Person) RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
	txn2.commit();
}

TEST_F(QuerySafetyTest, VarLengthDepthClamp_ExceedsConfigLimit) {
	// Set max depth to 3
	setConfig(graph::storage::state::keys::Config::QUERY_MAX_VAR_LENGTH_DEPTH, 3);

	buildLinearChain(*db, 10);

	auto txn = db->beginReadOnlyTransaction();
	// Query asks for *1..100 but should be clamped to depth 3
	auto result = execute("MATCH (a:Start)-[*1..100]->(b) RETURN count(*)");
	// With 10-node chain and depth clamped to 3, we should get exactly 3 results
	EXPECT_EQ(result.rowCount(), 1UL); // count(*) returns 1 row
	auto row = result.getRows()[0];
	auto countVal = row.at("count(*)");
	EXPECT_TRUE(countVal.isPrimitive());
	auto pv = countVal.asPrimitive();
	EXPECT_EQ(pv.getType(), graph::PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(pv.getVariant()), 3);
	txn.commit();
}

TEST_F(QuerySafetyTest, VarLengthIterativeDFS_NoStackOverflow) {
	// Build a deep chain and traverse it — should not crash
	buildLinearChain(*db, 50);

	auto txn = db->beginReadOnlyTransaction();
	auto result = execute("MATCH (a:Start)-[*1..50]->(b) RETURN count(*)");
	EXPECT_EQ(result.rowCount(), 1UL);
	auto row = result.getRows()[0];
	auto countVal = row.at("count(*)");
	EXPECT_TRUE(countVal.isPrimitive());
	EXPECT_GE(std::get<int64_t>(countVal.asPrimitive().getVariant()), 1);
	txn.commit();
}

TEST_F(QuerySafetyTest, VarLengthIterativeDFS_CycleDetection) {
	buildCyclicGraph(*db);

	auto txn = db->beginReadOnlyTransaction();
	// This should terminate (cycle detection prevents infinite loop)
	auto result = execute("MATCH (a:Node {name: 'A'})-[*1..10]->(b) RETURN count(*)");
	EXPECT_EQ(result.rowCount(), 1UL);
	auto row = result.getRows()[0];
	auto countVal = row.at("count(*)");
	EXPECT_TRUE(countVal.isPrimitive());
	// In a 3-node cycle with cycle detection, we should get exactly 2 results
	// (B at depth 1, C at depth 2 — then A is visited so cycle stops)
	EXPECT_EQ(std::get<int64_t>(countVal.asPrimitive().getVariant()), 2);
	txn.commit();
}

TEST_F(QuerySafetyTest, MemoryLimit_NormalQuerySucceeds) {
	// Set generous memory limit (10 GB)
	setConfig(graph::storage::state::keys::Config::QUERY_MAX_MEMORY_MB, 10000);

	auto qe = db->getQueryEngine();
	auto txn = db->beginTransaction();
	qe->execute("CREATE (n:Person {name: 'Test'})");
	txn.commit();

	auto txn2 = db->beginReadOnlyTransaction();
	auto result = execute("MATCH (n:Person) RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
	txn2.commit();
}
