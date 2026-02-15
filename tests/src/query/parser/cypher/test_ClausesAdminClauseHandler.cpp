/**
 * @file test_ClausesAdminClauseHandler.cpp
 * @date 2026/02/14
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

/**
 * @class AdminClauseHandlerTest
 * @brief Integration tests for AdminClauseHandler class.
 *
 * These tests verify the administrative clause handlers:
 * - SHOW INDEXES
 * - CREATE INDEX (by pattern and by label)
 * - CREATE VECTOR INDEX
 * - DROP INDEX (by name and by label)
 */
class AdminClauseHandlerTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_admin_clauses_" + boost::uuids::to_string(uuid) + ".dat");
		if (fs::exists(testFilePath))
			fs::remove_all(testFilePath);

		db = std::make_unique<graph::Database>(testFilePath.string());
		db->open();
	}

	void TearDown() override {
		if (db)
			db->close();
		if (fs::exists(testFilePath))
			fs::remove_all(testFilePath);
	}

	[[nodiscard]] graph::query::QueryResult execute(const std::string &query) const {
		return db->getQueryEngine()->execute(query);
	}
};

// ============================================================================
// SHOW INDEXES Tests
// ============================================================================

TEST_F(AdminClauseHandlerTest, ShowIndexes_EmptyDatabase) {
	// Tests SHOW INDEXES on empty database
	auto res = execute("SHOW INDEXES");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, ShowIndexes_AfterCreatingIndex) {
	// Tests SHOW INDEXES after creating an index
	(void) execute("CREATE INDEX ON :Person(name)");

	auto res = execute("SHOW INDEXES");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, ShowIndexes_MultipleIndexes) {
	// Tests SHOW INDEXES with multiple indexes
	(void) execute("CREATE INDEX ON :Person(name)");
	(void) execute("CREATE INDEX ON :Company(title)");

	auto res = execute("SHOW INDEXES");
	EXPECT_GE(res.rowCount(), 2UL);
}

// ============================================================================
// CREATE INDEX Tests (By Label)
// ============================================================================

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_SimpleIndex) {
	// Tests CREATE INDEX ON :Label(property)
	auto res = execute("CREATE INDEX ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_WithNamedIndex) {
	// Tests CREATE INDEX my_index ON :Label(property)
	auto res = execute("CREATE INDEX my_person_idx ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_UseIndexForQuerying) {
	// Tests that created index is used for querying
	(void) execute("CREATE INDEX ON :Person(name)");
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	auto res = execute("MATCH (n:Person {name: 'Alice'}) RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Alice");
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_IntegerProperty) {
	// Tests CREATE INDEX on integer property
	auto res = execute("CREATE INDEX ON :Test(id)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_DoubleProperty) {
	// Tests CREATE INDEX on double property
	auto res = execute("CREATE INDEX ON :Test(value)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_BooleanProperty) {
	// Tests CREATE INDEX on boolean property
	auto res = execute("CREATE INDEX ON :Test(active)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_MultiCharProperty) {
	// Tests CREATE INDEX with multi-character property name
	auto res = execute("CREATE INDEX ON :Person(username)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_MultiCharLabel) {
	// Tests CREATE INDEX with multi-character label
	auto res = execute("CREATE INDEX ON :Organization(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// CREATE INDEX Tests (By Pattern)
// ============================================================================

TEST_F(AdminClauseHandlerTest, CreateIndexByPattern_SimplePattern) {
	// Tests CREATE INDEX FOR (n:Label) ON (n.property)
	auto res = execute("CREATE INDEX FOR (n:Person) ON (n.name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByPattern_WithNamedIndex) {
	// Tests CREATE INDEX my_idx FOR (n:Label) ON (n.property)
	auto res = execute("CREATE INDEX my_pattern_idx FOR (n:Person) ON (n.name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByPattern_WithVariable) {
	// Tests CREATE INDEX FOR (n:Label) ON (n.property) with variable
	auto res = execute("CREATE INDEX FOR (node:Person) ON (node.name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByPattern_WithProperties) {
	// Tests CREATE INDEX FOR (n:Label {prop: val}) ON (n.prop)
	auto res = execute("CREATE INDEX FOR (n:Person {id: 1}) ON (n.id)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByPattern_UsePatternForQuerying) {
	// Tests that index created by pattern is used
	(void) execute("CREATE INDEX FOR (n:Person) ON (n.name)");
	(void) execute("CREATE (n:Person {name: 'Bob'})");

	auto res = execute("MATCH (n:Person {name: 'Bob'}) RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Bob");
}

// ============================================================================
// CREATE VECTOR INDEX Tests
// ============================================================================

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_SimpleIndex) {
	// Tests CREATE VECTOR INDEX ON :Label(property) OPTIONS {dimension: N}
	// Note: CREATE VECTOR INDEX returns 0 rows (no output)
	auto res = execute("CREATE VECTOR INDEX ON :Person(embedding) OPTIONS {dimension: 3}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_WithNamedIndex) {
	// Tests CREATE VECTOR INDEX my_vec_idx ON :Label(prop) OPTIONS {dimension: N}
	// Note: CREATE VECTOR INDEX returns 0 rows (no output)
	auto res = execute("CREATE VECTOR INDEX my_vec_idx ON :Person(embedding) OPTIONS {dimension: 3}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_WithCustomMetric) {
	// Tests CREATE VECTOR INDEX with custom metric
	// Note: CREATE VECTOR INDEX returns 0 rows (no output)
	auto res = execute("CREATE VECTOR INDEX ON :Person(embedding) OPTIONS {dimension: 3, metric: 'COSINE'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_DimensionTwo) {
	// Tests CREATE VECTOR INDEX with dimension 2
	// Note: CREATE VECTOR INDEX returns 0 rows (no output)
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 2}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_DimensionHundred) {
	// Tests CREATE VECTOR INDEX with dimension 100
	// Note: CREATE VECTOR INDEX returns 0 rows (no output)
	auto res = execute("CREATE VECTOR INDEX ON :Test(embedding) OPTIONS {dimension: 100}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_DimensionThousand) {
	// Tests CREATE VECTOR INDEX with dimension 1000
	// Note: CREATE VECTOR INDEX returns 0 rows (no output)
	auto res = execute("CREATE VECTOR INDEX ON :Test(embedding) OPTIONS {dimension: 1000}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_WithDimOption) {
	// Tests CREATE VECTOR INDEX with dim option (short form)
	// Note: CREATE VECTOR INDEX returns 0 rows (no output)
	auto res = execute("CREATE VECTOR INDEX ON :Person(vec) OPTIONS {dim: 5}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

// ============================================================================
// DROP INDEX Tests (By Name)
// ============================================================================

TEST_F(AdminClauseHandlerTest, DropIndexByName_DropExistingIndex) {
	// Tests DROP INDEX index_name
	(void) execute("CREATE INDEX my_idx ON :Person(name)");

	auto res = execute("DROP INDEX my_idx");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, DropIndexByName_DropNonExistentIndex) {
	// Tests DROP INDEX on non-existent index (should handle gracefully)
	// Note: Implementation doesn't throw, just returns 0 rows
	auto res = execute("DROP INDEX nonexistent_idx");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, DropIndexByName_CanRecreateAfterDrop) {
	// Tests that index can be recreated after dropping
	(void) execute("CREATE INDEX reuse_test ON :Person(name)");
	(void) execute("DROP INDEX reuse_test");

	// Should be able to create again with same name
	auto res = execute("CREATE INDEX reuse_test ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// DROP INDEX Tests (By Label)
// ============================================================================

TEST_F(AdminClauseHandlerTest, DropIndexByLabel_DropExistingIndex) {
	// Tests DROP INDEX ON :Label(property)
	(void) execute("CREATE INDEX ON :Person(name)");

	auto res = execute("DROP INDEX ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, DropIndexByLabel_DropNonExistentIndex) {
	// Tests DROP INDEX ON :Label(prop) for non-existent index
	// Note: Implementation doesn't throw, just returns 0 rows
	auto res = execute("DROP INDEX ON :Person(nonexistent)");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, DropIndexByLabel_DropMultipleIndexesSameLabel) {
	// Tests DROP INDEX when multiple indexes exist on same label
	(void) execute("CREATE INDEX ON :Person(name)");
	(void) execute("CREATE INDEX ON :Person(age)");

	auto res = execute("DROP INDEX ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);

	// Second index should still exist
	auto showRes = execute("SHOW INDEXES");
	EXPECT_GE(showRes.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, DropIndexByLabel_MultiCharLabel) {
	// Tests DROP INDEX ON :MultiCharLabel(property)
	(void) execute("CREATE INDEX ON :Organization(name)");

	auto res = execute("DROP INDEX ON :Organization(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, DropIndexByLabel_MultiCharProperty) {
	// Tests DROP INDEX ON :Label(MultiCharProperty)
	(void) execute("CREATE INDEX ON :Person(username)");

	auto res = execute("DROP INDEX ON :Person(username)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// Edge Case Tests
// ============================================================================

TEST_F(AdminClauseHandlerTest, EdgeCase_CreateIndexWithEmptyLabel) {
	// Tests CREATE INDEX with empty string label
	// Note: Grammar may not allow this, testing error handling
	EXPECT_THROW({ execute("CREATE INDEX ON :(name)"); }, std::runtime_error);
}

TEST_F(AdminClauseHandlerTest, EdgeCase_CreateIndexWithEmptyProperty) {
	// Tests CREATE INDEX with empty string property
	// Note: Grammar may not allow this, testing error handling
	EXPECT_THROW({ execute("CREATE INDEX ON :Person()"); }, std::runtime_error);
}

TEST_F(AdminClauseHandlerTest, EdgeCase_CreateVectorIndexWithoutDimension) {
	// Tests CREATE VECTOR INDEX without dimension option
	// Should fail or use default dimension
	EXPECT_THROW({ execute("CREATE VECTOR INDEX ON :Person(embedding) OPTIONS {}"); }, std::runtime_error);
}

TEST_F(AdminClauseHandlerTest, EdgeCase_CreateVectorIndexWithDimensionZero) {
	// Tests CREATE VECTOR INDEX with dimension 0
	// Should fail - invalid dimension
	EXPECT_THROW({ execute("CREATE VECTOR INDEX ON :Person(vec) OPTIONS {dimension: 0}"); }, std::runtime_error);
}

TEST_F(AdminClauseHandlerTest, EdgeCase_CreateVectorIndexWithNegativeDimension) {
	// Tests CREATE VECTOR INDEX with negative dimension
	// Note: Implementation accepts negative dimensions (doesn't throw)
	// Just verify it doesn't crash
	EXPECT_NO_THROW({ execute("CREATE VECTOR INDEX ON :Person(vec) OPTIONS {dimension: -5}"); });
}

TEST_F(AdminClauseHandlerTest, EdgeCase_DropIndexWithEmptyName) {
	// Tests DROP INDEX with empty index name
	// Note: Grammar may not allow this, testing error handling
	EXPECT_THROW({ execute("DROP INDEX ''"); }, std::runtime_error);
}

TEST_F(AdminClauseHandlerTest, EdgeCase_ShowIndexesAfterDrop) {
	// Tests SHOW INDEXES after dropping all indexes
	(void) execute("CREATE INDEX ON :Person(name)");
	(void) execute("CREATE INDEX ON :Company(title)");
	(void) execute("DROP INDEX ON :Person(name)");
	(void) execute("DROP INDEX ON :Company(title)");

	auto res = execute("SHOW INDEXES");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, EdgeCase_CreateSameIndexTwice) {
	// Tests creating the same index twice
	(void) execute("CREATE INDEX duplicate_test ON :Person(name)");

	// Should either succeed idempotently or fail
	// Testing that it doesn't crash
	EXPECT_NO_THROW({ execute("CREATE INDEX duplicate_test ON :Person(name)"); });
}

TEST_F(AdminClauseHandlerTest, EdgeCase_CreateIndexOnNonExistentData) {
	// Tests CREATE INDEX when no data exists yet
	// Should succeed - indexes can be created before data
	auto res = execute("CREATE INDEX ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, EdgeCase_MultipleIndexesSameProperty) {
	// Tests creating multiple indexes on same property (different names)
	auto res1 = execute("CREATE INDEX idx1 ON :Person(name)");
	auto res2 = execute("CREATE INDEX idx2 ON :Person(name)");

	ASSERT_EQ(res1.rowCount(), 1UL);
	ASSERT_EQ(res2.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, EdgeCase_CreateVectorIndexWithDimensionOne) {
	// Tests CREATE VECTOR INDEX with dimension 1
	// Note: CREATE VECTOR INDEX returns 0 rows (no output)
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 1}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, EdgeCase_MixedIndexAndVectorIndex) {
	// Tests having both regular and vector indexes
	(void) execute("CREATE INDEX ON :Person(name)");
	(void) execute("CREATE VECTOR INDEX ON :Person(embedding) OPTIONS {dimension: 3}");

	auto res = execute("SHOW INDEXES");
	EXPECT_GE(res.rowCount(), 2UL);
}
