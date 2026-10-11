/**
 * @file test_CypherListLiterals.cpp
 * @date 2026/1/4
 *
 * Comprehensive tests for list literal support in Cypher queries
 */

#include "QueryTestFixture.hpp"

using namespace graph;

class CypherListLiteralsTest : public QueryTestFixture {};

// ============================================================================
// Simple List Literals
// ============================================================================

TEST_F(CypherListLiteralsTest, ReturnSimpleList) {
	auto result = execute("RETURN [1, 2, 3] AS list");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, ReturnEmptyList) {
	auto result = execute("RETURN [] AS empty");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, ReturnStringList) {
	auto result = execute("RETURN ['a', 'b', 'c'] AS strings");
	EXPECT_EQ(result.rowCount(), 1UL);
}

// ============================================================================
// Mixed-Type Lists
// ============================================================================

TEST_F(CypherListLiteralsTest, ReturnMixedTypeList) {
	auto result = execute("RETURN [1, 'text', true, 3.14] AS mixed");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, ReturnListWithNull) {
	auto result = execute("RETURN [1, null, 2] AS withNull");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, ReturnBooleanList) {
	auto result = execute("RETURN [true, false, true] AS bools");
	EXPECT_EQ(result.rowCount(), 1UL);
}

// ============================================================================
// Nested Lists
// ============================================================================

TEST_F(CypherListLiteralsTest, ReturnNestedList) {
	auto result = execute("RETURN [[1, 2], [3, 4]] AS nested");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, ReturnDeeplyNestedList) {
	auto result = execute("RETURN [[[1, 2]], [[3, 4]]] AS deep");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, ReturnMixedNestedList) {
	auto result = execute("RETURN [[1, 'a'], [true, 2.5]] AS mixedNested");
	EXPECT_EQ(result.rowCount(), 1UL);
}

// ============================================================================
// List Literals in Node Properties
// ============================================================================

TEST_F(CypherListLiteralsTest, ListInNodeProperties) {
	(void)execute("CREATE (n:Test {items: [1, 2, 3]})");
	auto result = execute("MATCH (n:Test) RETURN n.items AS items");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, MixedListInNodeProperties) {
	(void)execute("CREATE (n:Test {data: [1, 'text', true, null]})");
	auto result = execute("MATCH (n:Test) RETURN n.data AS data");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, NestedListInNodeProperties) {
	(void)execute("CREATE (n:Test {matrix: [[1, 2], [3, 4]]})");
	auto result = execute("MATCH (n:Test) RETURN n.matrix AS matrix");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, PersistsNestedHeterogeneousListPropertiesAcrossReopen) {
	(void)execute(
			"CREATE (:PersistedList {value: [[1, 'one', null], [true, 2.5, ['nested', 7]]]})");

	db->close();
	db = std::make_unique<graph::Database>(testFilePath.string());
	db->open();

	const auto result = execute("MATCH (n:PersistedList) RETURN n.value AS value");
	ASSERT_EQ(result.rowCount(), 1UL);
	const PropertyValue expected(std::vector<PropertyValue>{
				PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t{1}), PropertyValue("one"), PropertyValue()}),
				PropertyValue(std::vector<PropertyValue>{
						PropertyValue(true), PropertyValue(2.5),
						PropertyValue(std::vector<PropertyValue>{PropertyValue("nested"), PropertyValue(int64_t{7})})})});
	EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), expected);
}

TEST_F(CypherListLiteralsTest, PersistsNestedMapAndListPropertiesAcrossReopen) {
	(void)execute(
			"CREATE (:PersistedMap {value: {name: 'root', enabled: true, "
			"children: [{id: 1}, {id: 2, tags: ['x', null]}]}})");

	db->close();
	db = std::make_unique<graph::Database>(testFilePath.string());
	db->open();

	const auto result = execute("MATCH (n:PersistedMap) RETURN n.value AS value");
	ASSERT_EQ(result.rowCount(), 1UL);
	const PropertyValue expected(PropertyValue::MapType{
				{"name", PropertyValue("root")},
				{"enabled", PropertyValue(true)},
				{"children", PropertyValue(std::vector<PropertyValue>{
								 PropertyValue(PropertyValue::MapType{{"id", PropertyValue(int64_t{1})}}),
								 PropertyValue(PropertyValue::MapType{
										 {"id", PropertyValue(int64_t{2})},
										 {"tags", PropertyValue(std::vector<PropertyValue>{PropertyValue("x"), PropertyValue()})}})})}});
	EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), expected);
}

TEST_F(CypherListLiteralsTest, MultipleNodesWithLists) {
	(void)execute("CREATE (n1:Test {vals: [1, 2]}), (n2:Test {vals: [3, 4]})");
	auto result = execute("MATCH (n:Test) RETURN n.vals AS vals ORDER BY vals[0]");
	EXPECT_EQ(result.rowCount(), 2UL);
}

// ============================================================================
// List Slicing in RETURN
// ============================================================================

TEST_F(CypherListLiteralsTest, ListSlicingInReturn) {
	auto result = execute("RETURN [1, 2, 3, 4, 5][1..3] AS sliced");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(result.getRows().front().at("sliced").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(2), PropertyValue(3)}));
}

TEST_F(CypherListLiteralsTest, ListSlicingSingleIndex) {
	auto result = execute("RETURN [1, 2, 3][0] AS first");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(result.getRows().front().at("first").asPrimitive(), PropertyValue(1));
}

TEST_F(CypherListLiteralsTest, ListSlicingNegativeIndex) {
	auto result = execute("RETURN [1, 2, 3][-1] AS last");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(result.getRows().front().at("last").asPrimitive(), PropertyValue(3));
}

TEST_F(CypherListLiteralsTest, ListSlicingOmitStart) {
	auto result = execute("RETURN [1, 2, 3, 4, 5][..3] AS firstThree");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, ListSlicingOmitEnd) {
	auto result = execute("RETURN [1, 2, 3, 4, 5][2..] AS fromTwo");
	EXPECT_EQ(result.rowCount(), 1UL);
}

// ============================================================================
// List Operations in WHERE Clause
// ============================================================================

TEST_F(CypherListLiteralsTest, ListInWhereClause) {
	(void)execute("CREATE (n:Test {nums: [1, 2, 3, 4, 5]})");
	auto result = execute("MATCH (n:Test) WHERE n.nums[0] = 1 RETURN n");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, ListComparisonInWhere) {
	(void)execute("CREATE (n1:Test {vals: [1, 2]}), (n2:Test {vals: [3, 4]})");
	auto result = execute("MATCH (n:Test) WHERE n.vals[0] = 1 RETURN n");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, NegativeIndexInWhere) {
	(void)execute("CREATE (n:Test {items: [10, 20, 30]})");
	auto result = execute("MATCH (n:Test) WHERE n.items[-1] = 30 RETURN n");
	EXPECT_EQ(result.rowCount(), 1UL);
}

// ============================================================================
// List Slicing on Node Properties
// ============================================================================

TEST_F(CypherListLiteralsTest, SliceNodeProperty) {
	(void)execute("CREATE (n:Test {nums: [1, 2, 3, 4, 5]})");
	auto result = execute("MATCH (n:Test) RETURN n.nums[1..3] AS sliced");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, IndexNodeProperty) {
	(void)execute("CREATE (n:Test {tags: ['a', 'b', 'c']})");
	auto result = execute("MATCH (n:Test) RETURN n.tags[1] AS secondTag");
	EXPECT_EQ(result.rowCount(), 1UL);
}

// ============================================================================
// Complex Queries with Lists
// ============================================================================

TEST_F(CypherListLiteralsTest, ListLiteralInArithmetic) {
	auto result = execute("RETURN [1, 2, 3][0] + 10 AS sum");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, ListLiteralInComparison) {
	auto result = execute("RETURN [1, 2, 3][0] = 1 AS isEqual");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, MultipleListsInReturn) {
	auto result = execute("RETURN [1, 2] AS a, [3, 4] AS b");
	EXPECT_EQ(result.rowCount(), 1UL);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(CypherListLiteralsTest, SingleElementList) {
	auto result = execute("RETURN [42] AS single");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, OutOfBoundsIndex) {
	auto result = execute("RETURN [1, 2, 3][10] AS outOfBounds");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(result.getRows().front().at("outOfBounds").asPrimitive(), PropertyValue());
}

TEST_F(CypherListLiteralsTest, NegativeOutOfBounds) {
	auto result = execute("RETURN [1, 2, 3][-10] AS negOutOfBounds");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, EmptySlice) {
	auto result = execute("RETURN [1, 2, 3][5..10] AS emptySlice");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, LargeList) {
	auto result = execute("RETURN [1, 2, 3, 4, 5, 6, 7, 8, 9, 10] AS large");
	EXPECT_EQ(result.rowCount(), 1UL);
}

// ============================================================================
// List Comprehensions (FILTER mode)
// ============================================================================

TEST_F(CypherListLiteralsTest, FilterComprehension) {
	auto result = execute("RETURN [x IN [1, 2, 3, 4, 5] WHERE x % 2 = 0] AS evens");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(result.getRows().front().at("evens").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(2), PropertyValue(4)}));
}

TEST_F(CypherListLiteralsTest, FilterWithNulls) {
	auto result = execute("RETURN [x IN [1, null, 2, null, 3] WHERE x IS NOT NULL] AS filtered");
	EXPECT_EQ(result.rowCount(), 1UL);
}

// ============================================================================
// List Comprehensions (EXTRACT mode)
// ============================================================================

TEST_F(CypherListLiteralsTest, ExtractComprehension) {
	auto result = execute("RETURN [x IN [1, 2, 3] | x * 2] AS doubled");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(result.getRows().front().at("doubled").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(2), PropertyValue(4), PropertyValue(6)}));
}

TEST_F(CypherListLiteralsTest, ExtractWithProperty) {
	(void)execute("CREATE (n:Test {values: [1, 2, 3]})");
	auto result = execute("MATCH (n:Test) RETURN [x IN n.values | x * 10] AS tens");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(CypherListLiteralsTest, ExtractNestedComprehension) {
	auto result = execute("RETURN [x IN [[1, 2], [3, 4]] | x[0]] AS firsts");
	EXPECT_EQ(result.rowCount(), 1UL);
}

// ============================================================================
// List Comprehensions (Combined FILTER + EXTRACT)
// ============================================================================

TEST_F(CypherListLiteralsTest, FilterAndExtract) {
	auto result = execute("RETURN [x IN [1, 2, 3, 4, 5] WHERE x % 2 = 0 | x * 10] AS result");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(result.getRows().front().at("result").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(20), PropertyValue(40)}));
}

TEST_F(CypherListLiteralsTest, ComplexComprehension) {
	auto result = execute("RETURN [x IN [1, 2, 3, 4, 5] WHERE x > 2 | x ^ 2] AS squares");
	EXPECT_EQ(result.rowCount(), 1UL);
}
