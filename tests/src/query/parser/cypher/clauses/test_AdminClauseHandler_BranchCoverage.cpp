/**
 * @file test_AdminClauseHandler_BranchCoverage.cpp
 * @brief Additional branch coverage tests for AdminClauseHandler
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
 * @class AdminClauseHandlerBranchCoverageTest
 * @brief Additional branch coverage tests for AdminClauseHandler
 */
class AdminClauseHandlerBranchCoverageTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_admin_branch_" + boost::uuids::to_string(uuid) + ".dat");
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
// Additional tests for better branch coverage
// ============================================================================

TEST_F(AdminClauseHandlerBranchCoverageTest, CreateIndexByPattern_WithName) {
	// Covers True branch at line 43
	auto res = execute("CREATE INDEX my_idx FOR (n:Person) ON (n.name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerBranchCoverageTest, CreateIndexByLabel_WithName) {
	// Covers True branch at line 68
	auto res = execute("CREATE INDEX test_idx ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerBranchCoverageTest, CreateVectorIndex_WithName) {
	// Covers True branch at line 82
	auto res = execute("CREATE VECTOR INDEX vec_idx ON :Person(embedding) OPTIONS {dimension: 3}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerBranchCoverageTest, CreateVectorIndex_EmptyOptionsMap) {
	// Tests OPTIONS with only dimension - this hits line 94 False branch when keys is empty
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 1}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

// ============================================================================
// Tests for dimension/metric key variations
// ============================================================================

TEST_F(AdminClauseHandlerBranchCoverageTest, CreateVectorIndex_DimKey) {
	// Tests "dim" key instead of "dimension"
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dim: 10}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerBranchCoverageTest, CreateVectorIndex_MetricOnly) {
	// Tests with metric only (but dimension is required, so this might throw)
	// Actually, dimension is required, so this should throw
	EXPECT_THROW({
		execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {metric: 'L2'}");
	}, std::runtime_error);
}

TEST_F(AdminClauseHandlerBranchCoverageTest, CreateVectorIndex_DimensionFirst) {
	// Tests dimension before metric in OPTIONS
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 5, metric: 'L2'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerBranchCoverageTest, CreateVectorIndex_WithDimAndMetric) {
	// Tests using 'dim' key with metric
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dim: 5, metric: 'COSINE'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerBranchCoverageTest, CreateVectorIndex_MultipleOtherOptions) {
	// Tests OPTIONS with dimension and multiple other keys
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, other1: 1, other2: 2}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

// ============================================================================
// Create index variations
// ============================================================================

TEST_F(AdminClauseHandlerBranchCoverageTest, CreateIndexByPattern_WithVariableAndProperties) {
	// Tests pattern with properties
	auto res = execute("CREATE INDEX FOR (n:Person {id: 1}) ON (n.id)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerBranchCoverageTest, CreateIndexByLabel_MultipleIndicesSameLabel) {
	// Tests multiple indexes on same label
	auto res1 = execute("CREATE INDEX ON :Person(name)");
	ASSERT_EQ(res1.rowCount(), 1UL);

	auto res2 = execute("CREATE INDEX ON :Person(age)");
	ASSERT_EQ(res2.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerBranchCoverageTest, CreateVectorIndex_MultipleDifferentSizes) {
	// Tests multiple vector indexes with different dimensions
	auto res1 = execute("CREATE VECTOR INDEX ON :Person(vec1) OPTIONS {dimension: 3}");
	ASSERT_EQ(res1.rowCount(), 0UL);

	auto res2 = execute("CREATE VECTOR INDEX ON :Person(vec2) OPTIONS {dimension: 128}");
	ASSERT_EQ(res2.rowCount(), 0UL);

	auto res3 = execute("CREATE VECTOR INDEX ON :Person(vec3) OPTIONS {dimension: 512}");
	ASSERT_EQ(res3.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerBranchCoverageTest, CreateVectorIndex_DifferentMetrics) {
	// Tests different metric types
	auto res1 = execute("CREATE VECTOR INDEX ON :Person(vec1) OPTIONS {dimension: 3, metric: 'L2'}");
	ASSERT_EQ(res1.rowCount(), 0UL);

	auto res2 = execute("CREATE VECTOR INDEX ON :Person(vec2) OPTIONS {dimension: 3, metric: 'COSINE'}");
	ASSERT_EQ(res2.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerBranchCoverageTest, CreateVectorIndex_WithBothDimForms) {
	// Tests using both 'dimension' and 'dim' forms in different queries
	auto res1 = execute("CREATE VECTOR INDEX ON :Test(vec1) OPTIONS {dimension: 5}");
	ASSERT_EQ(res1.rowCount(), 0UL);

	auto res2 = execute("CREATE VECTOR INDEX ON :Test(vec2) OPTIONS {dim: 10}");
	ASSERT_EQ(res2.rowCount(), 0UL);
}
