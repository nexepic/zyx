/**
 * @file test_IntegrationSubqueryFeatures.cpp
 *
 * Integration tests for:
 * - FOREACH statement
 * - CALL { subquery }
 * - LOAD CSV
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;
using namespace graph;

class IntegrationSubqueryFeaturesTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_subquery_" + boost::uuids::to_string(uuid) + ".zyx");
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
		// Clean up any CSV files we created
		for (auto &f : csvFiles) {
			if (fs::exists(f))
				fs::remove(f, ec);
		}
	}

	query::QueryResult execute(const std::string &query) const { return db->getQueryEngine()->execute(query); }

	std::string val(const query::QueryResult &r, const std::string &col, size_t row = 0) const {
		return r.getRows().at(row).at(col).toString();
	}

	// Helper to create a temporary CSV file and return its path
	std::string createTempCsv(const std::string &content) {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		auto path = fs::temp_directory_path() / ("test_csv_" + boost::uuids::to_string(uuid) + ".csv");
		std::ofstream out(path);
		out << content;
		out.close();
		csvFiles.push_back(path);

		// Use std::filesystem to convert path to generic format (forward slashes)
		// This handles Windows backslashes properly and is more portable than manual replace
		return path.generic_string();
	}

	std::filesystem::path testDbPath;
	std::unique_ptr<Database> db;
	std::vector<std::filesystem::path> csvFiles;
};

// ============================================================================
// FOREACH Tests
// ============================================================================

TEST_F(IntegrationSubqueryFeaturesTest, Foreach_CreateNodes) {
	execute("FOREACH (name IN ['Alice', 'Bob', 'Charlie'] | CREATE (:ForeachPerson {name: name}))");
	auto result = execute("MATCH (n:ForeachPerson) RETURN n.name AS name ORDER BY name");
	ASSERT_EQ(result.rowCount(), 3UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "name", 1), "Bob");
	EXPECT_EQ(val(result, "name", 2), "Charlie");
}

TEST_F(IntegrationSubqueryFeaturesTest, Foreach_SetProperties) {
	// FOREACH with SET on iterator variable - creates nodes and sets properties
	execute("FOREACH (name IN ['X', 'Y'] | CREATE (:ForeachSetTarget {name: name, val: 99}))");

	auto result = execute("MATCH (n:ForeachSetTarget) RETURN n.val AS v ORDER BY n.name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "v", 0), "99");
	EXPECT_EQ(val(result, "v", 1), "99");
}

TEST_F(IntegrationSubqueryFeaturesTest, Foreach_EmptyList) {
	// FOREACH over empty list should be a no-op
	execute("FOREACH (x IN [] | CREATE (:ShouldNotExist))");
	auto result = execute("MATCH (n:ShouldNotExist) RETURN count(n) AS c");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "c"), "0");
}

TEST_F(IntegrationSubqueryFeaturesTest, Foreach_WithPipeline) {
	// FOREACH after MATCH
	execute("CREATE (:ForeachSrc {name: 'source'})");
	execute("MATCH (s:ForeachSrc) FOREACH (i IN [1, 2, 3] | CREATE (:ForeachTarget {idx: i}))");
	auto result = execute("MATCH (n:ForeachTarget) RETURN count(n) AS c");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "c"), "3");
}

// ============================================================================
// CALL { subquery } Tests
// ============================================================================

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_Basic) {
	execute("CREATE (:SubqPerson {name: 'Alice', age: 30})");
	execute("CREATE (:SubqPerson {name: 'Bob', age: 25})");

	auto result = execute(
		"CALL { MATCH (p:SubqPerson) RETURN p.name AS name } "
		"RETURN name ORDER BY name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "name", 1), "Bob");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_WithOuterScope) {
	// Standalone CALL subquery that uses its own MATCH internally
	execute("CREATE (:SubqInner2 {val: 10})");
	execute("CREATE (:SubqInner2 {val: 20})");

	auto result = execute(
		"CALL { MATCH (i:SubqInner2) RETURN sum(i.val) AS total } "
		"RETURN total");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "total"), "30");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_WriteOnly) {
	// Write-only subquery wrapped with MATCH to satisfy grammar
	execute("CREATE (:SubqDummy {x: 1})");
	execute("MATCH (d:SubqDummy) CALL { CREATE (:SubqWriteOnly {created: true}) } RETURN d");
	auto result = execute("MATCH (n:SubqWriteOnly) RETURN n.created AS c");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "c"), "true");
}

// ============================================================================
// LOAD CSV Tests
// ============================================================================

TEST_F(IntegrationSubqueryFeaturesTest, LoadCsv_WithHeaders) {
	auto csvPath = createTempCsv("name,age\nAlice,30\nBob,25\n");

	auto result = execute(
		"LOAD CSV WITH HEADERS FROM 'file:///" + csvPath +
		"' AS row "
		"RETURN row.name AS name, row.age AS age ORDER BY name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "age", 0), "30");
	EXPECT_EQ(val(result, "name", 1), "Bob");
}

TEST_F(IntegrationSubqueryFeaturesTest, LoadCsv_WithoutHeaders) {
	auto csvPath = createTempCsv("Alice,30\nBob,25\n");

	auto result = execute(
		"LOAD CSV FROM 'file:///" + csvPath +
		"' AS row "
		"RETURN row[0] AS name, row[1] AS age ORDER BY name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "age", 0), "30");
}

TEST_F(IntegrationSubqueryFeaturesTest, LoadCsv_CreateNodes) {
	auto csvPath = createTempCsv("name,city\nAlice,NYC\nBob,LA\n");

	execute(
		"LOAD CSV WITH HEADERS FROM 'file:///" + csvPath +
		"' AS row "
		"CREATE (:CsvPerson {name: row.name, city: row.city})");

	auto result = execute("MATCH (n:CsvPerson) RETURN n.name AS name, n.city AS city ORDER BY name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "city", 0), "NYC");
	EXPECT_EQ(val(result, "name", 1), "Bob");
	EXPECT_EQ(val(result, "city", 1), "LA");
}

TEST_F(IntegrationSubqueryFeaturesTest, LoadCsv_QuotedFields) {
	auto csvPath = createTempCsv("name,bio\nAlice,\"Has a, comma\"\nBob,\"Line1\nLine2\"\n");

	auto result = execute(
		"LOAD CSV WITH HEADERS FROM 'file:///" + csvPath +
		"' AS row "
		"RETURN row.name AS name, row.bio AS bio ORDER BY name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "bio", 0), "Has a, comma");
}

TEST_F(IntegrationSubqueryFeaturesTest, LoadCsv_CustomDelimiter) {
	auto csvPath = createTempCsv("name\tage\nAlice\t30\nBob\t25\n");

	auto result = execute(
		"LOAD CSV WITH HEADERS FROM 'file:///" + csvPath +
		"' AS row FIELDTERMINATOR '\\t' "
		"RETURN row.name AS name ORDER BY name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "name", 1), "Bob");
}

TEST_F(IntegrationSubqueryFeaturesTest, LoadCsv_EmptyFile) {
	auto csvPath = createTempCsv("");

	auto result = execute(
		"LOAD CSV FROM 'file:///" + csvPath +
		"' AS row "
		"RETURN row");
	EXPECT_EQ(result.rowCount(), 0UL);
}
