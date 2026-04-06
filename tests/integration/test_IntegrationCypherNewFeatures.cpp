/**
 * @file test_IntegrationCypherNewFeatures.cpp
 * @date 2026/03/22
 *
 * Integration tests for newly implemented Cypher features:
 * - REDUCE function
 * - UNWIND nested properties
 * - EXISTS pattern expressions
 * - Pattern comprehensions
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;
using namespace graph;

class IntegrationCypherNewFeaturesTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_new_features_" + boost::uuids::to_string(uuid) + ".graph");
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
	}

	query::QueryResult execute(const std::string &query) const { return db->getQueryEngine()->execute(query); }

	std::string val(const query::QueryResult &r, const std::string &col) const {
		return r.getRows().at(0).at(col).toString();
	}

	std::filesystem::path testDbPath;
	std::unique_ptr<Database> db;
};

// ============================================================================
// REDUCE Function Tests
// ============================================================================

TEST_F(IntegrationCypherNewFeaturesTest, Reduce_SumIntegers) {
	auto result = execute("RETURN reduce(total = 0, x IN [1, 2, 3] | total + x) AS sum");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "sum"), "6");
}

TEST_F(IntegrationCypherNewFeaturesTest, Reduce_EmptyList) {
	auto result = execute("RETURN reduce(total = 0, x IN [] | total + x) AS sum");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "sum"), "0");
}

TEST_F(IntegrationCypherNewFeaturesTest, Reduce_StringConcatenation) {
	auto result = execute("RETURN reduce(s = '', x IN ['a', 'b', 'c'] | s + x) AS concat");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "concat"), "abc");
}

TEST_F(IntegrationCypherNewFeaturesTest, Reduce_WithRange) {
	auto result = execute("RETURN reduce(total = 0, x IN range(1, 5) | total + x) AS sum");
	ASSERT_EQ(result.rowCount(), 1UL);
	// 1+2+3+4+5 = 15
	EXPECT_EQ(val(result, "sum"), "15");
}

// ============================================================================
// UNWIND Nested Properties Tests
// ============================================================================

TEST_F(IntegrationCypherNewFeaturesTest, Unwind_NestedProperty) {
	(void) execute("CREATE (n:ListNode {name: 'test', items: [1, 2, 3]})");
	auto result = execute("MATCH (n:ListNode) UNWIND n.items AS x RETURN x");
	EXPECT_EQ(result.rowCount(), 3UL);
}

TEST_F(IntegrationCypherNewFeaturesTest, Unwind_NestedStringProperty) {
	(void) execute("CREATE (n:TagNode {tags: ['red', 'green', 'blue']})");
	auto result = execute("MATCH (n:TagNode) UNWIND n.tags AS t RETURN t");
	EXPECT_EQ(result.rowCount(), 3UL);
}

// ============================================================================
// EXISTS Pattern Expression Tests
// ============================================================================

TEST_F(IntegrationCypherNewFeaturesTest, Exists_MatchingPattern) {
	(void) execute("CREATE (a:ExPerson {name: 'Alice'})-[:KNOWS]->(b:ExPerson {name: 'Bob'})");
	auto result = execute("MATCH (n:ExPerson) WHERE exists((n)-[:KNOWS]->()) RETURN n.name");
	EXPECT_GE(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "n.name"), "Alice");
}

TEST_F(IntegrationCypherNewFeaturesTest, Exists_NoMatchingPattern) {
	(void) execute("CREATE (a:ExPerson2 {name: 'Alice'})-[:KNOWS]->(b:ExPerson2 {name: 'Bob'})");
	auto result = execute("MATCH (n:ExPerson2) WHERE exists((n)-[:LIKES]->()) RETURN n.name");
	EXPECT_EQ(result.rowCount(), 0UL);
}

TEST_F(IntegrationCypherNewFeaturesTest, Exists_IncomingPattern) {
	(void) execute("CREATE (a:ExPerson3 {name: 'Alice'})-[:KNOWS]->(b:ExPerson3 {name: 'Bob'})");
	auto result = execute("MATCH (n:ExPerson3) WHERE exists((n)<-[:KNOWS]-()) RETURN n.name");
	EXPECT_GE(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "n.name"), "Bob");
}

// ============================================================================
// Pattern Comprehension Tests
// ============================================================================

TEST_F(IntegrationCypherNewFeaturesTest, PatternComprehension_CollectNames) {
	(void) execute("CREATE (a:PCPerson {name: 'Alice'})-[:KNOWS]->(b:PCPerson {name: 'Bob'})");
	auto result = execute("MATCH (n:PCPerson {name: 'Alice'}) RETURN [(n)-[:KNOWS]->(m) | m.name] AS friends");
	EXPECT_GE(result.rowCount(), 1UL);
}

TEST_F(IntegrationCypherNewFeaturesTest, PatternComprehension_EmptyResult) {
	(void) execute("CREATE (a:PCPerson2 {name: 'Alone'})");
	auto result = execute("MATCH (n:PCPerson2 {name: 'Alone'}) RETURN [(n)-[:KNOWS]->(m) | m.name] AS friends");
	EXPECT_EQ(result.rowCount(), 1UL);
	// Should return a row with an empty list or similar
	EXPECT_FALSE(result.isEmpty());
}
