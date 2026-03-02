/**
 * @file test_AdminClauseHandler_Unit.cpp
 * @brief Unit tests for AdminClauseHandler coverage gaps
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
 * @class AdminClauseHandlerUnitTest
 * @brief Unit tests for AdminClauseHandler edge cases
 */
class AdminClauseHandlerUnitTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_admin_unit_" + boost::uuids::to_string(uuid) + ".dat");
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
// CREATE VECTOR INDEX - OPTIONS Edge Cases
// ============================================================================

TEST_F(AdminClauseHandlerUnitTest, CreateVectorIndex_WithDimensionOnly) {
	// Tests CREATE VECTOR INDEX with OPTIONS containing only dimension
	// This covers the else-if branch for metric check
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 5}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerUnitTest, CreateVectorIndex_WithUnknownOptionKey) {
	// Tests CREATE VECTOR INDEX with OPTIONS containing unknown keys
	// This should be handled gracefully - unknown keys are ignored
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, unknown_key: 'value'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerUnitTest, CreateVectorIndex_WithMultipleUnknownKeys) {
	// Tests CREATE VECTOR INDEX with multiple unknown option keys
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, key1: 1, key2: 2}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerUnitTest, CreateVectorIndex_MetricL2) {
	// Tests CREATE VECTOR INDEX with L2 metric (default)
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, metric: 'L2'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerUnitTest, CreateVectorIndex_MetricCOSINE) {
	// Tests CREATE VECTOR INDEX with COSINE metric
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, metric: 'COSINE'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerUnitTest, CreateVectorIndex_MetricInnerProduct) {
	// Tests CREATE VECTOR INDEX with Inner Product metric
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, metric: 'INNER_PRODUCT'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

// ============================================================================
// CREATE INDEX - Edge Cases
// ============================================================================

TEST_F(AdminClauseHandlerUnitTest, CreateIndexByPattern_SingleCharLabel) {
	// Tests CREATE INDEX with single character label
	auto res = execute("CREATE INDEX FOR (n:A) ON (n.prop)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerUnitTest, CreateIndexByPattern_SingleCharProperty) {
	// Tests CREATE INDEX with single character property
	auto res = execute("CREATE INDEX FOR (n:Label) ON (n.x)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerUnitTest, CreateIndexByLabel_SingleCharLabel) {
	// Tests CREATE INDEX ON :X(prop) with single character label
	auto res = execute("CREATE INDEX ON :A(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerUnitTest, CreateIndexByLabel_SingleCharProperty) {
	// Tests CREATE INDEX ON :Label(x) with single character property
	auto res = execute("CREATE INDEX ON :Label(x)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerUnitTest, CreateIndexByLabel_LongIndexName) {
	// Tests CREATE INDEX with very long index name
	std::string longName(100, 'a'); // 100 character name
	auto res = execute("CREATE INDEX " + longName + " ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// DROP INDEX - Edge Cases
// ============================================================================

TEST_F(AdminClauseHandlerUnitTest, DropIndexByName_LongIndexName) {
	// Tests DROP INDEX with very long index name
	std::string longName(50, 'x'); // 50 character name
	(void) execute("CREATE INDEX " + longName + " ON :Person(name)");
	auto res = execute("DROP INDEX " + longName);
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerUnitTest, DropIndexByLabel_SingleCharLabel) {
	// Tests DROP INDEX ON :A(prop) with single character label
	(void) execute("CREATE INDEX ON :A(name)");
	auto res = execute("DROP INDEX ON :A(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerUnitTest, DropIndexByLabel_SingleCharProperty) {
	// Tests DROP INDEX ON :Label(x) with single character property
	(void) execute("CREATE INDEX ON :Label(x)");
	auto res = execute("DROP INDEX ON :Label(x)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// SHOW INDEXES - Edge Cases
// ============================================================================

TEST_F(AdminClauseHandlerUnitTest, ShowIndexes_AfterCreateDrop) {
	// Tests SHOW INDEXES after create and drop operations
	(void) execute("CREATE INDEX ON :Person(name)");
	(void) execute("DROP INDEX ON :Person(name)");
	auto res = execute("SHOW INDEXES");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerUnitTest, ShowIndexes_AfterMultipleOperations) {
	// Tests SHOW INDEXES after multiple create/drop operations
	(void) execute("CREATE INDEX idx1 ON :Person(name)");
	(void) execute("CREATE INDEX idx2 ON :Person(age)");
	(void) execute("DROP INDEX idx1");
	(void) execute("CREATE INDEX idx3 ON :Person(email)");

	auto res = execute("SHOW INDEXES");
	EXPECT_GE(res.rowCount(), 2UL);
}

// ============================================================================
// Index Name Edge Cases
// ============================================================================

TEST_F(AdminClauseHandlerUnitTest, CreateIndex_WithUnderscoreName) {
	// Tests CREATE INDEX with underscore in name
	auto res = execute("CREATE INDEX my_test_idx ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerUnitTest, CreateIndex_WithNumbersInName) {
	// Tests CREATE INDEX with numbers in name
	auto res = execute("CREATE INDEX idx123 ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerUnitTest, CreateIndex_WithMixedCaseName) {
	// Tests CREATE INDEX with mixed case name
	auto res = execute("CREATE INDEX MyIndexName ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// Vector Index OPTIONS - Various Dimension Values
// ============================================================================

TEST_F(AdminClauseHandlerUnitTest, CreateVectorIndex_DimensionLargeValue) {
	// Tests CREATE VECTOR INDEX with large dimension value
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 10000}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerUnitTest, CreateVectorIndex_DimKeyShortForm) {
	// Tests CREATE VECTOR INDEX using 'dim' instead of 'dimension'
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dim: 128}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerUnitTest, CreateVectorIndex_OptionsOrderReversed) {
	// Tests CREATE VECTOR INDEX with metric before dimension
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {metric: 'L2', dimension: 5}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

// ============================================================================
// Error Cases - Lines 52, 103-104
// ============================================================================

TEST_F(AdminClauseHandlerUnitTest, CreateIndexByPattern_NoLabelError) {
	// Tests error path at line 52: CREATE INDEX pattern without label
	// Covers the else branch that throws runtime_error
	EXPECT_THROW({
		execute("CREATE INDEX FOR (n) ON (n.prop)");
	}, std::runtime_error);
}

TEST_F(AdminClauseHandlerUnitTest, CreateVectorIndex_NoDimensionError) {
	// Tests error path at line 103-104: Missing dimension in OPTIONS
	// Covers the "if (dim == 0)" check and throw
	EXPECT_THROW({
		execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {metric: 'L2'}");
	}, std::runtime_error);
}

TEST_F(AdminClauseHandlerUnitTest, CreateVectorIndex_NoOptionsError) {
	// Tests error path when OPTIONS is missing entirely
	// dim remains 0, triggering the error at line 103-104
	EXPECT_THROW({
		execute("CREATE VECTOR INDEX ON :Test(vec)");
	}, std::runtime_error);
}

TEST_F(AdminClauseHandlerUnitTest, CreateVectorIndex_EmptyOptionsError) {
	// Tests error path with empty OPTIONS map
	// No dimension provided, so dim stays 0
	EXPECT_THROW({
		execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {}");
	}, std::runtime_error);
}
