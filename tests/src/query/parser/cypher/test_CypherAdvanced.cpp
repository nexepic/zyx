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

TEST_F(CypherAdvancedTest, Unwind_ListWithNonLiteralFallback) {
	// Test UNWIND with non-literal items (uses text fallback)
	auto res = execute("UNWIND [1, n.prop, 3] AS x RETURN x");
	// Should return 3 items with text fallback for non-literals
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "1");
	EXPECT_EQ(res.getRows()[1].at("x").toString(), "n.prop");
	EXPECT_EQ(res.getRows()[2].at("x").toString(), "3");
}

TEST_F(CypherAdvancedTest, Unwind_ScientificNotationInList) {
	// Test UNWIND with scientific notation
	auto res = execute("UNWIND [1.5e10, 2.0E5, -3.0e-5] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);
}

TEST_F(CypherAdvancedTest, Unwind_EmptyList) {
	// Test UNWIND with empty list
	auto res = execute("UNWIND [] AS x RETURN x");
	EXPECT_EQ(res.rowCount(), 0UL);
}

TEST_F(CypherAdvancedTest, Unwind_ListWithNull) {
	// Test UNWIND with null in list
	auto res = execute("UNWIND [1, null, 3] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);
}

// ============================================================================
// Tests for PatternBuilder SET clause with various value types
// ============================================================================

TEST_F(CypherAdvancedTest, SetClause_ScientificNotationLowerE) {
	// Test SET with scientific notation (lowercase e)
	(void) execute("CREATE (n:SetSci1 {id: 1})");
	(void) execute("MATCH (n:SetSci1 {id: 1}) SET n.value = 1.5e10");

	auto res = execute("MATCH (n:SetSci1 {id: 1}) RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	// Value should be stored as double
	EXPECT_FALSE(res.getRows()[0].at("n.value").toString().empty());
}

TEST_F(CypherAdvancedTest, SetClause_ScientificNotationUpperE) {
	// Test SET with scientific notation (uppercase E)
	(void) execute("CREATE (n:SetSci2 {id: 2})");
	(void) execute("MATCH (n:SetSci2 {id: 2}) SET n.value = 2.0E5");

	auto res = execute("MATCH (n:SetSci2 {id: 2}) RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherAdvancedTest, SetClause_ScientificNotationNegativeExp) {
	// Test SET with scientific notation (negative exponent)
	(void) execute("CREATE (n:SetSci3 {id: 3})");
	(void) execute("MATCH (n:SetSci3 {id: 3}) SET n.value = 1.5e-5");

	auto res = execute("MATCH (n:SetSci3 {id: 3}) RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherAdvancedTest, SetClause_NegativeDouble) {
	// Test SET with negative double
	(void) execute("CREATE (n:SetSci4 {id: 4})");
	(void) execute("MATCH (n:SetSci4 {id: 4}) SET n.value = -3.14");

	auto res = execute("MATCH (n:SetSci4 {id: 4}) RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "-3.14");
}

TEST_F(CypherAdvancedTest, SetClause_NegativeInteger) {
	// Test SET with negative integer
	(void) execute("CREATE (n:SetSci5 {id: 5})");
	(void) execute("MATCH (n:SetSci5 {id: 5}) SET n.value = -42");

	auto res = execute("MATCH (n:SetSci5 {id: 5}) RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "-42");
}

TEST_F(CypherAdvancedTest, SetClause_LargeInteger) {
	// Test SET with large integer
	(void) execute("CREATE (n:SetSci6 {id: 6})");
	(void) execute("MATCH (n:SetSci6 {id: 6}) SET n.value = 10000000000");

	auto res = execute("MATCH (n:SetSci6 {id: 6}) RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherAdvancedTest, SetClause_ZeroValue) {
	// Test SET with zero value
	(void) execute("CREATE (n:SetSci7 {id: 7})");
	(void) execute("MATCH (n:SetSci7 {id: 7}) SET n.value = 0");

	auto res = execute("MATCH (n:SetSci7 {id: 7}) RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "0");
}

TEST_F(CypherAdvancedTest, SetClause_MultiplePropertiesMixedTypes) {
	// Test SET with multiple properties of different types
	(void) execute("CREATE (n:SetMulti {id: 1})");
	(void) execute("MATCH (n:SetMulti {id: 1}) SET n.str = 'text', n.num = 42, n.dbl = 3.14, n.flag = true");

	auto res = execute("MATCH (n:SetMulti {id: 1}) RETURN n.str, n.num, n.dbl, n.flag");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.str").toString(), "text");
	EXPECT_EQ(res.getRows()[0].at("n.num").toString(), "42");
	EXPECT_EQ(res.getRows()[0].at("n.dbl").toString(), "3.14");
	EXPECT_EQ(res.getRows()[0].at("n.flag").toString(), "true");
}

// ============================================================================
// Tests for Variable-Length Relationships (PatternBuilder Coverage)
// ============================================================================

TEST_F(CypherAdvancedTest, VarLengthRangeBoth) {
	// Test variable-length relationship with range *1..2
	(void) execute("CREATE (a:VarLen)-[:KNOWS]->(b:VarLen)-[:KNOWS]->(c:VarLen)");

	// Should match paths of length 1 and 2
	auto res = execute("MATCH (a:VarLen)-[:KNOWS*1..2]->(b) RETURN a, b");
	EXPECT_GE(res.rowCount(), 2UL); // At least a->b and a->b->c
}

TEST_F(CypherAdvancedTest, VarLengthExactHops) {
	// Test variable-length relationship with exact hops *2
	(void) execute("CREATE (a:VarLen2)-[:KNOWS]->(b:VarLen2)-[:KNOWS]->(c:VarLen2)");

	// Should only match paths of exactly 2 hops
	auto res = execute("MATCH (a:VarLen2)-[:KNOWS*2]->(b) RETURN a, b");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherAdvancedTest, VarLengthMaxOnly) {
	// Test variable-length relationship with max only *..3
	(void) execute("CREATE (a:VarLen3)-[:KNOWS]->(b:VarLen3)-[:KNOWS]->(c:VarLen3)");

	// Should match paths of 1-3 hops
	auto res = execute("MATCH (a:VarLen3)-[:KNOWS*..3]->(b) RETURN a, b");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(CypherAdvancedTest, VarLengthMinOnly) {
	// Test variable-length relationship with min only *2..
	(void) execute("CREATE (a:VarLen4)-[:KNOWS]->(b:VarLen4)-[:KNOWS]->(c:VarLen4)");

	// Should match paths of 2+ hops
	auto res = execute("MATCH (a:VarLen4)-[:KNOWS*2..]->(b) RETURN a, b");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(CypherAdvancedTest, MultiplePatternPartsCartesian) {
	// Test multiple pattern parts (cartesian product)
	(void) execute("CREATE (a:Person)");
	(void) execute("CREATE (b:Company)");

	// Should return cartesian product
	auto res = execute("MATCH (a:Person), (b:Company) RETURN a, b");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherAdvancedTest, TargetNodeLabelFilter) {
	// Test target node label filtering
	(void) execute("CREATE (a:Source)-[:WORKS_AT]->(b:Company)");
	(void) execute("CREATE (a:Source)-[:WORKS_AT]->(c:Person)");

	// Should only return Company targets
	auto res = execute("MATCH (a:Source)-[:WORKS_AT]->(b:Company) RETURN b");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherAdvancedTest, TargetNodePropertyFilter) {
	// Test target node property filtering
	(void) execute("CREATE (a:Source2)-[:LINK]->(b:Target {name: 'TechCorp'})");
	(void) execute("CREATE (a:Source2)-[:LINK]->(c:Target {name: 'OtherCorp'})");

	// Should filter by property
	auto res = execute("MATCH (a:Source2)-[:LINK]->(b:Target {name: 'TechCorp'}) RETURN b");
	EXPECT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("b").asNode().getProperties().at("name").toString(), "TechCorp");
}

TEST_F(CypherAdvancedTest, EdgePropertyFilterFixedLength) {
	// Test edge property filtering (fixed-length only)
	(void) execute("CREATE (a:EF1)-[r:KNOWS {since: 2020}]->(b:EF1)");
	(void) execute("CREATE (a:EF1)-[r:KNOWS {since: 2021}]->(c:EF1)");

	// Should filter by edge property
	auto res = execute("MATCH (a:EF1)-[r:KNOWS {since: 2020}]->(b) RETURN a, b, r.since");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherAdvancedTest, EdgePropertyFilterVarLengthIgnored) {
	// Test that edge properties are ignored in variable-length relationships
	(void) execute("CREATE (a:EF2)-[:KNOWS]->(b:EF2)-[:KNOWS]->(c:EF2)");

	// Variable-length with edge property - property should be ignored
	auto res = execute("MATCH (a:EF2)-[:KNOWS*1..2 {weight: 1.5}]->(b) RETURN a, b");
	// Should still match because properties are ignored in var-length
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(CypherAdvancedTest, PatternChainWithMultipleHops) {
	// Test pattern chain with multiple relationship hops
	(void) execute("CREATE (a:Chain)-[:LINK1]->(b:Chain)-[:LINK2]->(c:Chain)-[:LINK3]->(d:Chain)");

	auto res = execute("MATCH (a:Chain)-[:LINK1]->(b)-[:LINK2]->(c)-[:LINK3]->(d) RETURN a, b, c, d");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherAdvancedTest, BidirectionalTraversal) {
	// Test bidirectional traversal in pattern
	(void) execute("CREATE (a:Bi)-[:LINK]->(b:Bi)");
	(void) execute("CREATE (c:Bi)-[:LINK]->(b:Bi)");

	// Match incoming to b
	auto res = execute("MATCH (b:Bi)<-[:LINK]-(a:Bi) RETURN a");
	EXPECT_EQ(res.rowCount(), 2UL);
}

// ============================================================================
// Additional Tests for ExpressionBuilder Coverage
// ============================================================================

TEST_F(CypherAdvancedTest, WhereClause_LessThanOrEqualWithDoubles) {
	// Test <= with double values
	(void) execute("CREATE (n:WCLE {value: 3.14})");
	(void) execute("CREATE (n:WCLE {value: 2.71})");

	auto res = execute("MATCH (n:WCLE) WHERE n.value <= 3.14 RETURN n");
	EXPECT_EQ(res.rowCount(), 2UL);
}

TEST_F(CypherAdvancedTest, WhereClause_GreaterThanOrEqualWithDoubles) {
	// Test >= with double values
	(void) execute("CREATE (n:WCGE {value: 3.14})");
	(void) execute("CREATE (n:WCGE {value: 2.71})");

	auto res = execute("MATCH (n:WCGE) WHERE n.value >= 3.0 RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// Additional Tests for PropertyValueEvaluator Coverage
// ============================================================================

TEST_F(CypherAdvancedTest, CreateWithVector_DoublePrecision) {
	// Test vector creation with high precision doubles
	(void) execute("CREATE VECTOR INDEX idx_hp ON :HighPrec(vec) OPTIONS {dim: 3}");

	auto res = execute("CREATE (n:HighPrec {vec: [0.123456789, 0.987654321, 0.456789123]}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherAdvancedTest, CreateWithVector_NegativeValues) {
	// Test vector creation with negative values
	(void) execute("CREATE VECTOR INDEX idx_neg ON :NegVec(vec) OPTIONS {dim: 3}");

	auto res = execute("CREATE (n:NegVec {vec: [-1.0, -2.5, -3.14]}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherAdvancedTest, CreateWithVector_ZeroValues) {
	// Test vector creation with zero values
	(void) execute("CREATE VECTOR INDEX idx_zero ON :ZeroVec(vec) OPTIONS {dim: 3}");

	auto res = execute("CREATE (n:ZeroVec {vec: [0.0, 0, 0.0]}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}
