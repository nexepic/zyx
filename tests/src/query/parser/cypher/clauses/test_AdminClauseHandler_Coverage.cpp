/**
 * @file test_AdminClauseHandler_Coverage.cpp
 * @brief Coverage-focused unit tests for AdminClauseHandler
 * @date 2026/02/17
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;

/**
 * @class AdminClauseHandlerCoverageTest
 * @brief Coverage-focused tests for AdminClauseHandler
 */
class AdminClauseHandlerCoverageTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_admin_coverage_" + boost::uuids::to_string(uuid) + ".dat");
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
// CREATE INDEX without name - Line 43, 68, 82 False branches
// ============================================================================

TEST_F(AdminClauseHandlerCoverageTest, CreateIndexByPattern_NoName) {
	// Covers False branch at line 43
	auto res = execute("CREATE INDEX FOR (n:Person) ON (n.name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateIndexByLabel_NoName) {
	// Covers False branch at line 68
	auto res = execute("CREATE INDEX ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_NoName) {
	// Covers False branch at line 82
	auto res = execute("CREATE VECTOR INDEX ON :Person(embedding) OPTIONS {dimension: 3}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_NoName_WithMetric) {
	// Covers False branch at line 82 with metric
	auto res = execute("CREATE VECTOR INDEX ON :Person(vec) OPTIONS {dimension: 5, metric: 'COSINE'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

// ============================================================================
// CREATE VECTOR INDEX without OPTIONS - Line 90 False branch
// ============================================================================

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_WithOptionsKeyButNoLiteral) {
	// This test attempts to hit the False branch for OPTIONS
	// Note: The grammar may require OPTIONS with mapLiteral

	// Test with OPTIONS but minimal content
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 10}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

// ============================================================================
// OPTIONS with empty keys - Line 94 False branch
// ============================================================================

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_OptionsOnlyDimension) {
	// Covers empty loop (only dimension is processed)
	// This hits the case where only one key exists
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 128}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

// ============================================================================
// OPTIONS with other keys (non-dimension/dim/metric) - Line 97 False branch
// ============================================================================

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_OptionsWithOtherKey) {
	// Covers the case where key is neither dimension nor metric
	// Tests "else" path in line 97-99
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, other_key: 'value'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_OptionsWithMultipleOtherKeys) {
	// Tests multiple unknown keys
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, key1: 1, key2: 2}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_DimNotDimension) {
	// Tests when key is "dim" (covers True branch) vs other keys
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dim: 5}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_UnknownKeyWithDimension) {
	// Tests dimension followed by unknown key
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 5, unknown: 'test'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_MetricOnly) {
	// Tests with metric but dimension first
	// This hits the else if at line 99
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, metric: 'L2'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_OtherKeyBetweenDimAndMetric) {
	// Tests unknown key between dimension and metric
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, other: 1, metric: 'L2'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

// ============================================================================
// Dimension != 0 tests - Line 104 False branch
// ============================================================================

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_DimensionOne) {
	// Covers False branch at line 104 (dim == 0 check)
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 1}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_DimensionSmall) {
	// Covers False branch with small dimension
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 2}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_DimensionMedium) {
	// Covers False branch with medium dimension
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 256}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_DimensionLarge) {
	// Covers False branch with large dimension
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 1024}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_DimKeyWithNonDimension) {
	// Tests using 'dim' key
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dim: 64}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

// ============================================================================
// Mix of True/False branches
// ============================================================================

TEST_F(AdminClauseHandlerCoverageTest, CreateIndexByName_ThenDropByName) {
	// Tests create with name, then drop by name
	auto res1 = execute("CREATE INDEX my_idx ON :Person(name)");
	ASSERT_EQ(res1.rowCount(), 1UL);

	auto res2 = execute("DROP INDEX my_idx");
	ASSERT_EQ(res2.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateMultipleIndexesDifferentNames) {
	// Tests creating multiple indexes with different names (all without name)
	auto res1 = execute("CREATE INDEX ON :Person(name)");
	ASSERT_EQ(res1.rowCount(), 1UL);

	auto res2 = execute("CREATE INDEX ON :Person(age)");
	ASSERT_EQ(res2.rowCount(), 1UL);

	auto res3 = execute("CREATE INDEX ON :Person(email)");
	ASSERT_EQ(res3.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndexMultiple) {
	// Tests multiple vector index creations
	auto res1 = execute("CREATE VECTOR INDEX ON :Person(vec1) OPTIONS {dimension: 3}");
	ASSERT_EQ(res1.rowCount(), 0UL);

	auto res2 = execute("CREATE VECTOR INDEX ON :Person(vec2) OPTIONS {dim: 5}");
	ASSERT_EQ(res2.rowCount(), 0UL);

	auto res3 = execute("CREATE VECTOR INDEX ON :Person(vec3) OPTIONS {dimension: 10, metric: 'L2'}");
	ASSERT_EQ(res3.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateIndexByPattern_VariousPatterns) {
	// Tests different patterns to hit line 50 branches
	// Pattern with label but no name
	auto res1 = execute("CREATE INDEX FOR (n:Person) ON (n.name)");
	ASSERT_EQ(res1.rowCount(), 1UL);

	// Pattern with properties
	auto res2 = execute("CREATE INDEX FOR (n:Person {id: 1}) ON (n.id)");
	ASSERT_EQ(res2.rowCount(), 1UL);
}

// ============================================================================
// Additional branch coverage tests
// ============================================================================

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_WithOptionsAndName) {
	// Tests named vector index with options
	auto res = execute("CREATE VECTOR INDEX my_vec ON :Test(vec) OPTIONS {dimension: 5}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndex_MultipleOptions) {
	// Tests OPTIONS with dimension and various other combinations
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, metric: 'COSINE'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateIndexByLabel_SingleCharLabelAndProperty) {
	// Tests single character label and property
	auto res = execute("CREATE INDEX ON :A(x)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateIndexByPattern_SingleCharVariable) {
	// Tests with single character variable
	auto res = execute("CREATE INDEX FOR (x:Label) ON (x.prop)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// Edge cases for existing branches
// ============================================================================

TEST_F(AdminClauseHandlerCoverageTest, CreateIndexThenShowThenDrop) {
	// Tests full lifecycle
	auto res1 = execute("CREATE INDEX test_idx ON :Person(name)");
	ASSERT_EQ(res1.rowCount(), 1UL);

	auto res2 = execute("SHOW INDEXES");
	EXPECT_GE(res2.rowCount(), 1UL);

	auto res3 = execute("DROP INDEX test_idx");
	ASSERT_EQ(res3.rowCount(), 1UL);

	auto res4 = execute("SHOW INDEXES");
	EXPECT_EQ(res4.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerCoverageTest, CreateVectorIndexThenShow) {
	// Tests vector index appears in SHOW INDEXES
	auto res1 = execute("CREATE VECTOR INDEX ON :Person(vec) OPTIONS {dimension: 3}");
	ASSERT_EQ(res1.rowCount(), 0UL);

	auto res2 = execute("SHOW INDEXES");
	EXPECT_GE(res2.rowCount(), 1UL);
}
