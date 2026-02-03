/**
 * @file test_IntegrationPersistence.cpp
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
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;
using namespace graph;

/**
 * Integration test fixture for data persistence and recovery
 */
class IntegrationPersistenceTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_persist_" + boost::uuids::to_string(uuid) + ".graph");
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath);
	}

	void TearDown() override {
		if (db)
			db->close();
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath);
	}

	query::QueryResult execute(const std::string &query) const { return db->getQueryEngine()->execute(query); }

	std::filesystem::path testDbPath;
	std::unique_ptr<Database> db;
};

/**
 * Test basic data persistence across sessions
 */
TEST_F(IntegrationPersistenceTest, BasicDataPersistence) {
	// Session 1: Create data
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
		(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");
		db->close();
	}

	// Session 2: Verify persistence
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.name");
		EXPECT_EQ(result.rowCount(), 2UL);
	}
}

/**
 * Test relationship persistence
 */
TEST_F(IntegrationPersistenceTest, RelationshipPersistence) {
	// Session 1: Create graph
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		(void) execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");
		(void) execute("CREATE (b:Person {name: 'Bob'})-[:KNOWS]->(c:Person {name: 'Charlie'})");
		db->close();
	}

	// Session 2: Verify relationships
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		auto result = execute("MATCH (a:Person)-[:KNOWS]->(b:Person) RETURN a.name, b.name");
		EXPECT_EQ(result.rowCount(), 2UL);
	}
}

/**
 * Test persistence after updates
 */
TEST_F(IntegrationPersistenceTest, PersistenceAfterUpdates) {
	// Session 1: Create and update
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
		(void) execute("MATCH (n:Person {name: 'Alice'}) SET n.age = 31");
		(void) execute("MATCH (n:Person {name: 'Alice'}) SET n.city = 'NYC'");
		db->close();
	}

	// Session 2: Verify updates
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		auto result = execute("MATCH (n:Person {name: 'Alice'}) RETURN n.age, n.city");
		EXPECT_EQ(result.rowCount(), 1UL);
	}
}

/**
 * Test persistence after deletions
 */
TEST_F(IntegrationPersistenceTest, PersistenceAfterDeletions) {
	// Session 1: Create and delete
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		(void) execute("CREATE (n:Person {name: 'Alice'})");
		(void) execute("CREATE (n:Person {name: 'Bob'})");
		(void) execute("CREATE (n:Person {name: 'Charlie'})");
		(void) execute("MATCH (n:Person {name: 'Bob'}) DELETE n");
		db->close();
	}

	// Session 2: Verify deletion
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.name");
		EXPECT_EQ(result.rowCount(), 2UL);
	}
}

/**
 * Test multiple read/write cycles
 */
TEST_F(IntegrationPersistenceTest, MultipleReadWriteCycles) {
	// Cycle 1: Create initial data
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
		db->close();
	}

	// Cycle 2: Add more data
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");
		db->close();
	}

	// Cycle 3: Update existing data
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		(void) execute("MATCH (n:Person {name: 'Alice'}) SET n.age = 31");
		db->close();
	}

	// Cycle 4: Verify all changes
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		auto result = execute("MATCH (n:Person) RETURN n.name, n.age ORDER BY n.name");
		EXPECT_EQ(result.rowCount(), 2UL);
	}
}

/**
 * Test large dataset persistence
 */
TEST_F(IntegrationPersistenceTest, LargeDatasetPersistence) {

	// Session 1: Create large dataset
	{
		constexpr int nodeCount = 200;
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		for (int i = 0; i < nodeCount; ++i) {
			(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
		}
		db->close();
	}

	// Session 2: Verify large dataset
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		// Verify specific nodes exist
		auto result1 = execute("MATCH (n:Test {id: 0}) RETURN n");
		EXPECT_EQ(result1.rowCount(), 1UL);

		auto result2 = execute("MATCH (n:Test {id: 199}) RETURN n");
		EXPECT_EQ(result2.rowCount(), 1UL);

		auto result3 = execute("MATCH (n:Test {id: 200}) RETURN n");
		EXPECT_EQ(result3.rowCount(), 0UL);
	}
}

/**
 * Test persistence with transactions
 */
TEST_F(IntegrationPersistenceTest, PersistenceWithTransactions) {
	// Session 1: Transactional writes
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();

		auto txn1 = db->beginTransaction();
		(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
		txn1.commit();

		auto txn2 = db->beginTransaction();
		(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");
		txn2.commit();

		db->close();
	}

	// Session 2: Verify transactional data
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.name");
		EXPECT_EQ(result.rowCount(), 2UL);
	}
}

/**
 * Test multiple instance consistency
 */
TEST_F(IntegrationPersistenceTest, MultipleInstancesConsistency) {
	// Create initial data
	{
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		(void) execute("CREATE (n:Person {name: 'Alice'})");
		(void) execute("CREATE (n:Person {name: 'Bob'})");
		db->close();
	}

	// Open, read, close multiple times
	for (int i = 0; i < 3; ++i) {
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.name");
		EXPECT_EQ(result.rowCount(), 2UL);
		db->close();
	}
}
