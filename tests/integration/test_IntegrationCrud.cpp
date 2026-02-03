/**
 * @file test_IntegrationCrud.cpp
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
 * Integration test fixture for CRUD operations
 */
class IntegrationCrudTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_crud_" + boost::uuids::to_string(uuid) + ".graph");
		if (fs::exists(testDbPath)) fs::remove_all(testDbPath);
		db = std::make_unique<Database>(testDbPath);
		db->open();
	}

	void TearDown() override {
		if (db) db->close();
		if (fs::exists(testDbPath)) fs::remove_all(testDbPath);
	}

	graph::query::QueryResult execute(const std::string &query) {
		return db->getQueryEngine()->execute(query);
	}

	std::string testDbPath;
	std::unique_ptr<Database> db;
};

/**
 * Test CREATE operation
 */
TEST_F(IntegrationCrudTest, CreateSingleNode) {
	execute("CREATE (n:Person {name: 'Alice', age: 30})");

	auto result = execute("MATCH (n:Person {name: 'Alice'}) RETURN n.name, n.age");
	ASSERT_EQ(result.rowCount(), 1UL);
}

/**
 * Test CREATE multiple nodes
 */
TEST_F(IntegrationCrudTest, CreateMultipleNodes) {
	execute("CREATE (n:Person {name: 'Alice'})");
	execute("CREATE (n:Person {name: 'Bob'})");
	execute("CREATE (n:Person {name: 'Charlie'})");

	auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.name");
	EXPECT_EQ(result.rowCount(), 3UL);
}

/**
 * Test CREATE with relationships
 */
TEST_F(IntegrationCrudTest, CreateWithRelationships) {
	execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");

	auto result = execute("MATCH (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person) RETURN b.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test READ operation
 */
TEST_F(IntegrationCrudTest, ReadWithFilter) {
	execute("CREATE (n:Person {name: 'Alice', age: 30, active: true})");
	execute("CREATE (n:Person {name: 'Bob', age: 25, active: false})");
	execute("CREATE (n:Person {name: 'Charlie', age: 35, active: true})");

	auto result = execute("MATCH (n:Person) WHERE n.active = true RETURN n.name");
	EXPECT_EQ(result.rowCount(), 2UL);
}

/**
 * Test READ relationships
 */
TEST_F(IntegrationCrudTest, ReadRelationships) {
	execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");
	execute("CREATE (b:Person {name: 'Bob'})-[:KNOWS]->(c:Person {name: 'Charlie'})");

	auto result = execute("MATCH (a:Person)-[:KNOWS]->(b:Person) RETURN a.name, b.name");
	EXPECT_EQ(result.rowCount(), 2UL);
}

/**
 * Test UPDATE operation
 */
TEST_F(IntegrationCrudTest, UpdateNodeProperties) {
	execute("CREATE (n:Person {name: 'Alice', age: 30})");
	execute("MATCH (n:Person {name: 'Alice'}) SET n.age = 31");

	auto result = execute("MATCH (n:Person {name: 'Alice'}) RETURN n.age");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test UPDATE add new properties
 */
TEST_F(IntegrationCrudTest, AddNewProperties) {
	execute("CREATE (n:Person {name: 'Alice'})");
	execute("MATCH (n:Person {name: 'Alice'}) SET n.age = 30, n.city = 'NYC'");

	auto result = execute("MATCH (n:Person {name: 'Alice'}) RETURN n.age, n.city");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test DELETE operation
 */
TEST_F(IntegrationCrudTest, DeleteSingleNode) {
	execute("CREATE (n:Person {name: 'Alice'})");

	auto beforeResult = execute("MATCH (n:Person {name: 'Alice'}) RETURN n");
	EXPECT_EQ(beforeResult.rowCount(), 1UL);

	execute("MATCH (n:Person {name: 'Alice'}) DELETE n");

	auto afterResult = execute("MATCH (n:Person {name: 'Alice'}) RETURN n");
	EXPECT_EQ(afterResult.rowCount(), 0UL);
}

/**
 * Test DELETE with filter
 */
TEST_F(IntegrationCrudTest, DeleteWithFilter) {
	execute("CREATE (n:Person {name: 'Alice', active: false})");
	execute("CREATE (n:Person {name: 'Bob', active: true})");
	execute("CREATE (n:Person {name: 'Charlie', active: false})");

	execute("MATCH (n:Person) WHERE n.active = false DELETE n");

	auto result = execute("MATCH (n:Person) RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test complete CRUD workflow
 */
TEST_F(IntegrationCrudTest, CompleteCrudWorkflow) {
	// CREATE
	execute("CREATE (n:Product {id: 1, name: 'Laptop', price: 1000})");

	// READ
	auto read1 = execute("MATCH (n:Product {id: 1}) RETURN n.name");
	ASSERT_EQ(read1.rowCount(), 1UL);

	// UPDATE
	execute("MATCH (n:Product {id: 1}) SET n.price = 900");

	// READ
	auto read2 = execute("MATCH (n:Product {id: 1}) RETURN n.price");
	ASSERT_EQ(read2.rowCount(), 1UL);

	// DELETE
	execute("MATCH (n:Product {id: 1}) DELETE n");

	// READ
	auto read3 = execute("MATCH (n:Product {id: 1}) RETURN n");
	EXPECT_EQ(read3.rowCount(), 0UL);
}

/**
 * Test MERGE operation
 */
TEST_F(IntegrationCrudTest, MergeOperation) {
	// First MERGE creates
	execute("MERGE (n:Person {name: 'Alice'}) SET n.age = 30");

	// Second MERGE matches
	execute("MERGE (n:Person {name: 'Alice'}) SET n.age = 31");

	auto result = execute("MATCH (n:Person {name: 'Alice'}) RETURN n.age");
	EXPECT_EQ(result.rowCount(), 1UL);

	// Verify only one node exists
	auto countResult = execute("MATCH (n:Person {name: 'Alice'}) RETURN count(n)");
	EXPECT_EQ(countResult.rowCount(), 1UL);
}
