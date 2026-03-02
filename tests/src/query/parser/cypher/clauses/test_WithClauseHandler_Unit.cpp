/**
 * @file test_WithClauseHandler_Unit.cpp
 * @brief Unit tests for WithClauseHandler to verify WITH clause functionality
 * @date 2026/03/02
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
 * @class WithClauseHandlerUnitTest
 * @brief Unit tests for WithClauseHandler functionality
 */
class WithClauseHandlerUnitTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_with_unit_" + boost::uuids::to_string(uuid) + ".dat");
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
// Basic WITH Clause Tests
// ============================================================================

TEST_F(WithClauseHandlerUnitTest, With_Basic) {
	// Test basic WITH clause
	(void)execute("CREATE (n:Test {value: 100})");
	auto res = execute("MATCH (n:Test) WITH n RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL) << "Basic WITH clause should return 1 row";
}

TEST_F(WithClauseHandlerUnitTest, With_WithProperty) {
	// Test WITH with property access
	(void)execute("CREATE (n:Test {value: 100})");
	auto res = execute("MATCH (n:Test) WITH n.value AS v RETURN v");
	EXPECT_EQ(res.rowCount(), 1UL);
	if (res.rowCount() >= 1) {
		EXPECT_EQ(res.getRows()[0].at("v").toString(), "100");
	}
}

TEST_F(WithClauseHandlerUnitTest, With_WithWhere) {
	// Test WITH clause with WHERE filter
	(void)execute("CREATE (n:Test {value: 10})");
	(void)execute("CREATE (n:Test {value: 20})");
	auto res = execute("MATCH (n:Test) WITH n WHERE n.value > 15 RETURN n.value");
	EXPECT_EQ(res.rowCount(), 1UL) << "WITH WHERE should filter to 1 row";
}

TEST_F(WithClauseHandlerUnitTest, With_Chained) {
	// Test chained WITH clauses
	(void)execute("CREATE (n:Test {value: 100})");
	auto res = execute("MATCH (n:Test) WITH n.value AS v WITH v * 2 AS doubled RETURN doubled");
	EXPECT_EQ(res.rowCount(), 1UL);
	if (res.rowCount() >= 1) {
		EXPECT_EQ(res.getRows()[0].at("doubled").toString(), "200");
	}
}
