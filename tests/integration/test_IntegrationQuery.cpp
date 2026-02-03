/**
 * @file test_IntegrationQuery.cpp
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

#include <gtest/gtest.h>
#include <filesystem>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;
using namespace graph;

/**
 * Integration test fixture for query execution
 */
class IntegrationQueryTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_query_" + boost::uuids::to_string(uuid) + ".graph");
		if (fs::exists(testDbPath)) fs::remove_all(testDbPath);
		db = std::make_unique<Database>(testDbPath);
		db->open();
	}

	void TearDown() override {
		if (db) db->close();
		if (fs::exists(testDbPath)) fs::remove_all(testDbPath);
	}

	void setupSocialGraph() {
		execute("CREATE (a:Person {name: 'Alice', age: 30})");
		execute("CREATE (b:Person {name: 'Bob', age: 25})");
		execute("CREATE (c:Person {name: 'Charlie', age: 35})");
		execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");
		execute("CREATE (b:Person {name: 'Bob'})-[:KNOWS]->(c:Person {name: 'Charlie'})");
	}

	graph::query::QueryResult execute(const std::string &query) {
		return db->getQueryEngine()->execute(query);
	}

	std::string testDbPath;
	std::unique_ptr<Database> db;
};

/**
 * Test simple MATCH query
 */
TEST_F(IntegrationQueryTest, SimpleMatchQuery) {
	execute("CREATE (n:Person {name: 'Alice', age: 30})");

	auto result = execute("MATCH (n:Person) RETURN n.name, n.age");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test MATCH with WHERE clause
 */
TEST_F(IntegrationQueryTest, MatchWithWhere) {
	execute("CREATE (n:Person {name: 'Alice', age: 30, active: true})");
	execute("CREATE (n:Person {name: 'Bob', age: 25, active: false})");

	auto result = execute("MATCH (n:Person) WHERE n.active = true RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test MATCH with relationships
 */
TEST_F(IntegrationQueryTest, MatchWithRelationships) {
	execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");

	auto result = execute("MATCH (a:Person)-[:KNOWS]->(b:Person) RETURN a.name, b.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test variable length traversal
 */
TEST_F(IntegrationQueryTest, VariableLengthTraversal) {
	setupSocialGraph();

	auto result = execute("MATCH (a:Person {name: 'Alice'})-[:KNOWS*1..2]->(b:Person) RETURN DISTINCT b.name");
	EXPECT_GT(result.rowCount(), 0UL);
}

/**
 * Test ORDER BY clause
 */
TEST_F(IntegrationQueryTest, OrderByQuery) {
	execute("CREATE (n:Person {name: 'Charlie', age: 35})");
	execute("CREATE (n:Person {name: 'Alice', age: 30})");
	execute("CREATE (n:Person {name: 'Bob', age: 25})");

	auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.age");
	EXPECT_EQ(result.rowCount(), 3UL);
}

/**
 * Test LIMIT clause
 */
TEST_F(IntegrationQueryTest, LimitQuery) {
	execute("CREATE (n:Person {name: 'Alice', age: 30})");
	execute("CREATE (n:Person {name: 'Bob', age: 25})");
	execute("CREATE (n:Person {name: 'Charlie', age: 35})");

	auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.name LIMIT 2");
	EXPECT_EQ(result.rowCount(), 2UL);
}

/**
 * Test combined query with multiple clauses
 */
TEST_F(IntegrationQueryTest, CombinedQuery) {
	setupSocialGraph();

	auto result = execute(
		"MATCH (p:Person) "
		"WHERE p.age > 25 "
		"RETURN p.name, p.age "
		"ORDER BY p.age DESC "
		"LIMIT 2"
	);
	EXPECT_GT(result.rowCount(), 0UL);
	EXPECT_LE(result.rowCount(), 2UL);
}

/**
 * Test CREATE and RETURN
 */
TEST_F(IntegrationQueryTest, CreateAndReturn) {
	auto result = execute("CREATE (n:Person {name: 'Alice'}) RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test SET clause
 */
TEST_F(IntegrationQueryTest, SetClause) {
	execute("CREATE (n:Person {name: 'Alice', age: 30})");
	execute("MATCH (n:Person {name: 'Alice'}) SET n.age = 31");

	auto result = execute("MATCH (n:Person {name: 'Alice'}) RETURN n.age");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test DELETE clause
 */
TEST_F(IntegrationQueryTest, DeleteClause) {
	execute("CREATE (n:Person {name: 'Alice'})");
	execute("CREATE (n:Person {name: 'Bob'})");
	execute("MATCH (n:Person {name: 'Alice'}) DELETE n");

	auto result = execute("MATCH (n:Person) RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test MERGE query
 */
TEST_F(IntegrationQueryTest, MergeQuery) {
	execute("MERGE (n:Person {name: 'Alice'}) ON CREATE SET n.age = 30");
	execute("MERGE (n:Person {name: 'Alice'}) ON MATCH SET n.age = 31");

	auto result = execute("MATCH (n:Person {name: 'Alice'}) RETURN n.age");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test query with multiple MATCH patterns
 */
TEST_F(IntegrationQueryTest, MultipleMatchPatterns) {
	setupSocialGraph();

	auto result = execute(
		"MATCH (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person), "
		"(b:Person)-[:KNOWS]->(c:Person) "
		"RETURN a.name, b.name, c.name"
	);
	EXPECT_GT(result.rowCount(), 0UL);
}
