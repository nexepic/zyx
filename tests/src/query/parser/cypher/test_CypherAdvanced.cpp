/**
 * @file test_CypherAdvanced.cpp
 * @author Unit Test Coverage
 * @date 2026/02/14
 *
 * @copyright Copyright (c) 2026 Nexepic
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
#include <string>
#include "graph/core/Database.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;

class CypherAdvancedTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_cypher_adv_" + boost::uuids::to_string(uuid) + ".dat");
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
// Tests for PropertyValueEvaluator coverage
// ============================================================================

TEST_F(CypherAdvancedTest, PropertyValue_IntegerZeroInCreate) {
	(void) execute("CREATE (n:Test {value: 0})");
	(void) execute("CREATE (n:Test {value: 100})");

	auto res = execute("MATCH (n:Test) WHERE n.value = 0 RETURN n.value");
	EXPECT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "0");
}

TEST_F(CypherAdvancedTest, PropertyValue_LargeIntegerInWhere) {
	(void) execute("CREATE (n:Test {id: 1, value: 1000000})");
	(void) execute("CREATE (n:Test {id: 2, value: 1000000})");
	(void) execute("CREATE (n:Test {id: 3, value: 1000000})");

	auto res = execute("MATCH (n:Test) WHERE n.value >= 1000000 RETURN n.id");
	EXPECT_EQ(res.rowCount(), 3UL);
}

TEST_F(CypherAdvancedTest, PropertyValues_BooleanInMatch) {
	(void) execute("CREATE (n:Test {flag: true})");
	(void) execute("CREATE (n:Test {flag: false})");

	auto res = execute("MATCH (n:Test) WHERE n.flag = true RETURN n.flag");
	EXPECT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.flag").toString(), "true");
}

TEST_F(CypherAdvancedTest, Pattern_MultiplePropertiesInMatch) {
	(void) execute("CREATE (n:Test {p1: 1, p2: 2, p3: 3})");

	auto res = execute("MATCH (n:Test) WHERE n.p1 = 1 AND n.p2 = 2 AND n.p3 = 3 RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// Tests for PatternBuilder coverage
// ============================================================================

TEST_F(CypherAdvancedTest, Pattern_EdgeWithLabelAndDirection) {
	auto res = execute("CREATE (a)-[:REL_LEFT]->(b:Person) RETURN a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherAdvancedTest, Pattern_PathWithMultipleEdges) {
	auto res = execute("CREATE (a:Person)-[:KNOWS]->(b:Person)-[:KNOWS]->(c:Person) RETURN a, b, c");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// Tests for ExpressionBuilder coverage
// ============================================================================

TEST_F(CypherAdvancedTest, Expression_ScientificNotationInCreate) {
	// Test double literals with 'e' notation
	auto res = execute("CREATE (n:Test {val: 1.5e10}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	// The value is stored as double and returned in scientific notation
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("val").toString(), "1.5e+10");
}

TEST_F(CypherAdvancedTest, Expression_DecimalNotationInCreate) {
	// Test double literals with decimal point
	auto res = execute("CREATE (n:Test {val: 3.14}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("val").toString(), "3.14");
}

TEST_F(CypherAdvancedTest, Expression_NullLiteralInCreate) {
	// Test NULL literal
	auto res = execute("CREATE (n:Test {val: null}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("val").getType(), graph::PropertyType::NULL_TYPE);
}

// ============================================================================
// Tests for PatternBuilder SET clause coverage
// ============================================================================

TEST_F(CypherAdvancedTest, SetClause_DoubleValueWithDecimal) {
	// Test SET with double value (decimal point)
	(void) execute("CREATE (n:SetTest {id: 1})");
	(void) execute("MATCH (n:SetTest {id: 1}) SET n.value = 2.5");

	auto res = execute("MATCH (n:SetTest {id: 1}) RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "2.5");
}

TEST_F(CypherAdvancedTest, SetClause_FalseLowercaseLiteral) {
	// Test SET with false (lowercase) literal
	(void) execute("CREATE (n:SetTest {id: 2})");
	(void) execute("MATCH (n:SetTest {id: 2}) SET n.flag = false");

	auto res = execute("MATCH (n:SetTest {id: 2}) RETURN n.flag");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.flag").toString(), "false");
}

TEST_F(CypherAdvancedTest, SetClause_NegativeIntegerValue) {
	// Test SET with negative integer value
	(void) execute("CREATE (n:SetTest {id: 3})");
	(void) execute("MATCH (n:SetTest {id: 3}) SET n.value = -42");

	auto res = execute("MATCH (n:SetTest {id: 3}) RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "-42");
}

// ============================================================================
// Tests for ReadingClauseHandler UNWIND coverage (ExpressionBuilder)
// ============================================================================

TEST_F(CypherAdvancedTest, Unwind_IntegerList) {
	// Test UNWIND with integer list
	auto res = execute("UNWIND [1, 2, 3] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "1");
	EXPECT_EQ(res.getRows()[1].at("x").toString(), "2");
	EXPECT_EQ(res.getRows()[2].at("x").toString(), "3");
}

TEST_F(CypherAdvancedTest, Unwind_MixedTypeList) {
	// Test UNWIND with mixed type list
	auto res = execute("UNWIND [1, 'two', true] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "1");
	EXPECT_EQ(res.getRows()[1].at("x").toString(), "two");
	EXPECT_EQ(res.getRows()[2].at("x").toString(), "true");
}

TEST_F(CypherAdvancedTest, Unwind_DoubleList) {
	// Test UNWIND with double values
	auto res = execute("UNWIND [1.5, 2.7, 3.14] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "1.5");
	EXPECT_EQ(res.getRows()[1].at("x").toString(), "2.7");
	EXPECT_EQ(res.getRows()[2].at("x").toString(), "3.14");
}

TEST_F(CypherAdvancedTest, Unwind_SingleItemList) {
	// Test UNWIND with single item
	auto res = execute("UNWIND [42] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "42");
}

TEST_F(CypherAdvancedTest, Unwind_NullInList) {
	// Test UNWIND with NULL value in list
	auto res = execute("UNWIND [null] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].at("x").isPrimitive());
	EXPECT_EQ(res.getRows()[0].at("x").asPrimitive().getType(), graph::PropertyType::NULL_TYPE);
}

TEST_F(CypherAdvancedTest, Unwind_IntegerListValues) {
	// Test UNWIND with integer list values (not floats)
	auto res = execute("UNWIND [1, 2, 3, 4, 5] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 5UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "1");
	EXPECT_EQ(res.getRows()[4].at("x").toString(), "5");
}

TEST_F(CypherAdvancedTest, Unwind_StringInList) {
	// Test UNWIND with string values in list
	auto res = execute("UNWIND ['a', 'b', 'c'] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "a");
	EXPECT_EQ(res.getRows()[1].at("x").toString(), "b");
	EXPECT_EQ(res.getRows()[2].at("x").toString(), "c");
}
