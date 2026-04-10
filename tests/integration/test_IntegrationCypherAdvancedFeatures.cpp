/**
 * @file test_IntegrationCypherAdvancedFeatures.cpp
 *
 * Integration tests for advanced Cypher features:
 * - collect() returning LIST (bug fix)
 * - collect(DISTINCT)
 * - =~ regex operator
 * - IN with dynamic lists
 * - Path functions: length()
 * - Map projections
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;
using namespace graph;

class IntegrationCypherAdvancedTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_advanced_" + boost::uuids::to_string(uuid) + ".graph");
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
	}

	query::QueryResult execute(const std::string &query) const { return db->getQueryEngine()->execute(query); }

	std::string val(const query::QueryResult &r, const std::string &col, size_t row = 0) const {
		return r.getRows().at(row).at(col).toString();
	}

	std::filesystem::path testDbPath;
	std::unique_ptr<Database> db;
};

// ============================================================================
// collect() returns LIST (bug fix)
// ============================================================================

TEST_F(IntegrationCypherAdvancedTest, Collect_ReturnsListType) {
	execute("CREATE (:Person {name: 'Alice'})");
	execute("CREATE (:Person {name: 'Bob'})");
	execute("CREATE (:Person {name: 'Charlie'})");
	auto result = execute("MATCH (n:Person) RETURN collect(n.name) AS names");
	ASSERT_EQ(result.rowCount(), 1UL);
	auto namesStr = val(result, "names");
	EXPECT_TRUE(namesStr.find("Alice") != std::string::npos);
	EXPECT_TRUE(namesStr.find("Bob") != std::string::npos);
	EXPECT_TRUE(namesStr.find("Charlie") != std::string::npos);
}

TEST_F(IntegrationCypherAdvancedTest, Collect_EmptyResult) {
	auto result = execute("MATCH (n:Nonexistent) RETURN collect(n.name) AS names");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "names"), "[]");
}

TEST_F(IntegrationCypherAdvancedTest, Collect_SingleValue) {
	execute("CREATE (:Solo {val: 42})");
	auto result = execute("MATCH (n:Solo) RETURN collect(n.val) AS vals");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "vals"), "[42]");
}

// ============================================================================
// collect(DISTINCT)
// ============================================================================

TEST_F(IntegrationCypherAdvancedTest, CollectDistinct_DeduplicatesValues) {
	execute("CREATE (:Tag {name: 'X'})");
	execute("CREATE (:Tag {name: 'X'})");
	execute("CREATE (:Tag {name: 'Y'})");
	execute("CREATE (:Tag {name: 'Y'})");
	execute("CREATE (:Tag {name: 'Z'})");
	auto result = execute("MATCH (t:Tag) RETURN collect(DISTINCT t.name) AS tags");
	ASSERT_EQ(result.rowCount(), 1UL);
	auto tagsStr = val(result, "tags");
	EXPECT_TRUE(tagsStr.find("X") != std::string::npos);
	EXPECT_TRUE(tagsStr.find("Y") != std::string::npos);
	EXPECT_TRUE(tagsStr.find("Z") != std::string::npos);
}

TEST_F(IntegrationCypherAdvancedTest, CollectDistinct_Empty) {
	auto result = execute("MATCH (n:Nothing) RETURN collect(DISTINCT n.x) AS vals");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "vals"), "[]");
}

// ============================================================================
// =~ regex operator
// ============================================================================

TEST_F(IntegrationCypherAdvancedTest, Regex_BasicMatch) {
	execute("CREATE (:Person {name: 'Alice'})");
	execute("CREATE (:Person {name: 'Anna'})");
	execute("CREATE (:Person {name: 'Bob'})");
	auto result = execute("MATCH (n:Person) WHERE n.name =~ 'A.*' RETURN n.name AS name ORDER BY name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "name", 1), "Anna");
}

TEST_F(IntegrationCypherAdvancedTest, Regex_NoMatch) {
	execute("CREATE (:Person {name: 'Alice'})");
	auto result = execute("MATCH (n:Person) WHERE n.name =~ 'Z.*' RETURN n.name AS name");
	EXPECT_EQ(result.rowCount(), 0UL);
}

TEST_F(IntegrationCypherAdvancedTest, Regex_ComplexPattern) {
	execute("CREATE (:Item {code: 'AB-123'})");
	execute("CREATE (:Item {code: 'CD-456'})");
	execute("CREATE (:Item {code: 'invalid'})");
	auto result = execute("MATCH (i:Item) WHERE i.code =~ '[A-Z][A-Z]-[0-9][0-9][0-9]' RETURN i.code AS code ORDER BY code");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "code", 0), "AB-123");
	EXPECT_EQ(val(result, "code", 1), "CD-456");
}

TEST_F(IntegrationCypherAdvancedTest, Regex_FullStringMatch) {
	execute("CREATE (:Person {name: 'Alice'})");
	// =~ does full string match, so 'lic' should NOT match 'Alice'
	auto result = execute("MATCH (n:Person) WHERE n.name =~ 'lic' RETURN n.name AS name");
	EXPECT_EQ(result.rowCount(), 0UL);
}

// ============================================================================
// IN with lists
// ============================================================================

TEST_F(IntegrationCypherAdvancedTest, InLiteral_WithListLiteral) {
	execute("CREATE (:Person {name: 'Alice'})");
	execute("CREATE (:Person {name: 'Bob'})");
	execute("CREATE (:Person {name: 'Charlie'})");
	auto result = execute("MATCH (n:Person) WHERE n.name IN ['Alice', 'Charlie'] RETURN n.name AS name ORDER BY name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "name", 1), "Charlie");
}

TEST_F(IntegrationCypherAdvancedTest, InLiteral_EmptyList) {
	execute("CREATE (:Person {name: 'Alice'})");
	auto result = execute("MATCH (n:Person) WHERE n.name IN [] RETURN n.name AS name");
	EXPECT_EQ(result.rowCount(), 0UL);
}

// ============================================================================
// Path functions: length()
// ============================================================================

TEST_F(IntegrationCypherAdvancedTest, LengthFunction_String) {
	auto result = execute("RETURN length('hello') AS len");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "len"), "5");
}

TEST_F(IntegrationCypherAdvancedTest, LengthFunction_List) {
	auto result = execute("RETURN length([1, 2, 3]) AS len");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "len"), "3");
}

TEST_F(IntegrationCypherAdvancedTest, LengthFunction_EmptyString) {
	auto result = execute("RETURN length('') AS len");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "len"), "0");
}

TEST_F(IntegrationCypherAdvancedTest, LengthFunction_EmptyList) {
	auto result = execute("RETURN length([]) AS len");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "len"), "0");
}

// ============================================================================
// Map projections
// ============================================================================

TEST_F(IntegrationCypherAdvancedTest, MapProjection_PropertySelector) {
	execute("CREATE (:Person {name: 'Alice', age: 30, city: 'NYC'})");
	auto result = execute("MATCH (n:Person) RETURN n {.name, .age} AS proj");
	ASSERT_EQ(result.rowCount(), 1UL);
	auto projStr = val(result, "proj");
	EXPECT_TRUE(projStr.find("name") != std::string::npos);
	EXPECT_TRUE(projStr.find("Alice") != std::string::npos);
	EXPECT_TRUE(projStr.find("age") != std::string::npos);
}

TEST_F(IntegrationCypherAdvancedTest, MapProjection_ComputedValue) {
	execute("CREATE (:Person {name: 'Alice', age: 30})");
	auto result = execute("MATCH (n:Person) RETURN n {.name, doubleAge: n.age * 2} AS proj");
	ASSERT_EQ(result.rowCount(), 1UL);
	auto projStr = val(result, "proj");
	EXPECT_TRUE(projStr.find("name") != std::string::npos);
	EXPECT_TRUE(projStr.find("doubleAge") != std::string::npos);
}

TEST_F(IntegrationCypherAdvancedTest, MapProjection_AllProperties) {
	execute("CREATE (:Person {name: 'Alice', age: 30})");
	auto result = execute("MATCH (n:Person) RETURN n {.*} AS proj");
	ASSERT_EQ(result.rowCount(), 1UL);
	auto projStr = val(result, "proj");
	EXPECT_TRUE(projStr.find("name") != std::string::npos);
	EXPECT_TRUE(projStr.find("age") != std::string::npos);
}
