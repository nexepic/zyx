/**
 * @file test_IntegrationDatabase.cpp
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
 * Integration test fixture for database lifecycle tests
 */
class IntegrationDatabaseTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_db_" + boost::uuids::to_string(uuid) + ".graph");
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
 * Test complete database lifecycle
 */
TEST_F(IntegrationDatabaseTest, CompleteDatabaseLifecycle) {
	// Create data and close
	execute("CREATE (n:Person {name: 'Alice'})");
	db->close();
	EXPECT_FALSE(db->isOpen());

	// Reopen and verify data
	db = std::make_unique<Database>(testDbPath);
	db->open();
	ASSERT_TRUE(db->isOpen());

	auto result = execute("MATCH (n:Person) RETURN n");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test multiple open/close cycles
 */
TEST_F(IntegrationDatabaseTest, MultipleOpenCloseCycles) {
	// First cycle
	db->close();
	EXPECT_FALSE(db->isOpen());
	db->open();
	EXPECT_TRUE(db->isOpen());

	// Second cycle
	db->close();
	db->open();
	EXPECT_TRUE(db->isOpen());
}

/**
 * Test data persistence across sessions
 */
TEST_F(IntegrationDatabaseTest, PersistenceAcrossSessions) {
	// Session 1: Create data
	execute("CREATE (n:Person {name: 'Alice', age: 30})");
	execute("CREATE (n:Person {name: 'Bob', age: 25})");
	db->close();

	// Session 2: Verify persistence
	db = std::make_unique<Database>(testDbPath);
	db->open();

	auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.name");
	EXPECT_EQ(result.rowCount(), 2UL);
}

/**
 * Test database with large dataset
 */
TEST_F(IntegrationDatabaseTest, LargeDataset) {
	const int nodeCount = 100;
	for (int i = 0; i < nodeCount; ++i) {
		execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	// Verify nodes were created by checking for specific nodes
	auto result1 = execute("MATCH (n:Test {id: 0}) RETURN n");
	EXPECT_EQ(result1.rowCount(), 1UL);

	auto result2 = execute("MATCH (n:Test {id: 99}) RETURN n");
	EXPECT_EQ(result2.rowCount(), 1UL);

	auto result3 = execute("MATCH (n:Test {id: 100}) RETURN n");
	EXPECT_EQ(result3.rowCount(), 0UL);
}

/**
 * Test database file size growth
 */
TEST_F(IntegrationDatabaseTest, DatabaseFileSizeGrowth) {
	// Initial file size
	db->close();
	auto initialSize = fs::file_size(testDbPath);
	EXPECT_GT(initialSize, 0);

	// Add data and check size growth
	db->open();
	for (int i = 0; i < 50; ++i) {
		execute("CREATE (n:Test {id: " + std::to_string(i) + ", data: 'test data'})");
	}
	db->close();

	auto newSize = fs::file_size(testDbPath);
	EXPECT_GT(newSize, initialSize);
}
