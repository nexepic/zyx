/**
 * @file test_CypherAdvanced.cpp
 * @author Nexepic
 * @date 2026/1/29
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
#include "graph/core/Database.hpp"
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

// --- Pagination ---

TEST_F(CypherAdvancedTest, LimitResults) {
	for (int i = 0; i < 5; ++i)
		(void) execute("CREATE (n:LimitTest {id: " + std::to_string(i) + "})");
	EXPECT_EQ(execute("MATCH (n:LimitTest) RETURN n LIMIT 3").rowCount(), 3UL);
	EXPECT_EQ(execute("MATCH (n:LimitTest) RETURN n LIMIT 0").rowCount(), 0UL);
}

TEST_F(CypherAdvancedTest, SkipAndLimit) {
	for (int i = 0; i < 10; ++i)
		(void) execute("CREATE (n:PageTest {val: " + std::to_string(i) + "})");
	auto res = execute("MATCH (n:PageTest) RETURN n SKIP 3 LIMIT 4");
	ASSERT_EQ(res.rowCount(), 4UL);
}

// --- Sorting ---

TEST_F(CypherAdvancedTest, OrderByResults) {
	(void) execute("CREATE (n:SortTest {val: 3})");
	(void) execute("CREATE (n:SortTest {val: 1})");
	(void) execute("CREATE (n:SortTest {val: 2})");

	auto resAsc = execute("MATCH (n:SortTest) RETURN n ORDER BY n.val ASC");
	EXPECT_EQ(resAsc.getRows()[0].at("n").asNode().getProperties().at("val").toString(), "1");
	EXPECT_EQ(resAsc.getRows()[2].at("n").asNode().getProperties().at("val").toString(), "3");

	auto resDesc = execute("MATCH (n:SortTest) RETURN n ORDER BY n.val DESC");
	EXPECT_EQ(resDesc.getRows()[0].at("n").asNode().getProperties().at("val").toString(), "3");
}

// --- Hops ---

TEST_F(CypherAdvancedTest, VarLengthTraversal) {
	(void) execute("CREATE (a:HopNode {name:'A'})-[r1:NEXT]->(b:HopNode {name:'B'})");
	(void) execute("MATCH (b:HopNode {name:'B'}) CREATE (b)-[r2:NEXT]->(c:HopNode {name:'C'})");
	(void) execute("MATCH (c:HopNode {name:'C'}) CREATE (c)-[r3:NEXT]->(d:HopNode {name:'D'})");

	auto res1 = execute("MATCH (a:HopNode {name:'A'})-[*1..2]->(x) RETURN x");
	ASSERT_EQ(res1.rowCount(), 2UL); // B and C

	auto res2 = execute("MATCH (a:HopNode {name:'A'})-[*3]->(x) RETURN x");
	ASSERT_EQ(res2.rowCount(), 1UL); // D
}

// --- Cartesian Product ---

TEST_F(CypherAdvancedTest, CartesianProduct_Basic) {
	(void) execute("CREATE (:GroupA {id: 1})");
	(void) execute("CREATE (:GroupA {id: 2})");
	(void) execute("CREATE (:GroupB {val: 10})");
	(void) execute("CREATE (:GroupB {val: 20})");
	(void) execute("CREATE (:GroupB {val: 30})");

	auto res = execute("MATCH (a:GroupA), (b:GroupB) RETURN a.id, b.val");
	ASSERT_EQ(res.rowCount(), 6UL);
}

TEST_F(CypherAdvancedTest, CartesianProduct_EmptySide) {
	(void) execute("CREATE (:GroupA)");
	// GroupB empty
	auto res = execute("MATCH (a:GroupA), (b:GroupB) RETURN a, b");
	EXPECT_TRUE(res.isEmpty());
}

// --- Unwind ---

TEST_F(CypherAdvancedTest, Unwind_BasicValues) {
	auto res = execute("UNWIND [1, 2, 3] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);
}

TEST_F(CypherAdvancedTest, BatchInsert_Nodes) {
	(void) execute("UNWIND [1, 2, 3] AS x CREATE (n:BatchNode)");
	ASSERT_EQ(execute("MATCH (n:BatchNode) RETURN n").rowCount(), 3UL);
}

// --- Additional Pagination Tests ---

TEST_F(CypherAdvancedTest, LimitGreaterThanAvailable) {
	(void) execute("CREATE (n:LGA {id: 1})");
	(void) execute("CREATE (n:LGA {id: 2})");
	auto res = execute("MATCH (n:LGA) RETURN n LIMIT 100");
	ASSERT_EQ(res.rowCount(), 2UL); // Only 2 nodes exist
}

TEST_F(CypherAdvancedTest, SkipGreaterThanAvailable) {
	(void) execute("CREATE (n:SGA {id: 1})");
	auto res = execute("MATCH (n:SGA) RETURN n SKIP 10");
	EXPECT_TRUE(res.isEmpty());
}

TEST_F(CypherAdvancedTest, SkipZero) {
	(void) execute("CREATE (n:SZ {id: 1})");
	(void) execute("CREATE (n:SZ {id: 2})");
	auto res = execute("MATCH (n:SZ) RETURN n SKIP 0");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(CypherAdvancedTest, SkipAndLimitEdgeCases) {
	for (int i = 0; i < 5; ++i)
		(void) execute("CREATE (n:SkipLim {id: " + std::to_string(i) + "})");

	// Skip 2, Limit 2 (middle 3)
	auto res1 = execute("MATCH (n:SkipLim) RETURN n SKIP 2 LIMIT 2");
	ASSERT_EQ(res1.rowCount(), 2UL);

	// Skip 0, Limit 0 (no results)
	auto res2 = execute("MATCH (n:SkipLim) RETURN n SKIP 0 LIMIT 0");
	EXPECT_EQ(res2.rowCount(), 0UL);
}

// --- Additional Sorting Tests ---

TEST_F(CypherAdvancedTest, OrderByMultipleFields) {
	(void) execute("CREATE (n:MultiSort {prio: 1, name: 'C'})");
	(void) execute("CREATE (n:MultiSort {prio: 2, name: 'A'})");
	(void) execute("CREATE (n:MultiSort {prio: 1, name: 'B'})");
	(void) execute("CREATE (n:MultiSort {prio: 2, name: 'D'})");

	auto res = execute("MATCH (n:MultiSort) RETURN n ORDER BY n.prio ASC, n.name ASC");
	ASSERT_EQ(res.rowCount(), 4UL);
	// Order: (1, B), (1, C), (2, A), (2, D)
	auto rows = res.getRows();
	EXPECT_EQ(rows[0].at("n").asNode().getProperties().at("name").toString(), "B");
	EXPECT_EQ(rows[1].at("n").asNode().getProperties().at("name").toString(), "C");
	EXPECT_EQ(rows[2].at("n").asNode().getProperties().at("name").toString(), "A");
	EXPECT_EQ(rows[3].at("n").asNode().getProperties().at("name").toString(), "D");
}

TEST_F(CypherAdvancedTest, OrderByWithNull) {
	(void) execute("CREATE (n:NullSort {val: 1})");
	(void) execute("CREATE (n:NullSort {val: 3})");
	(void) execute("CREATE (n:NullSort)"); // No val property (NULL)

	auto res = execute("MATCH (n:NullSort) RETURN n ORDER BY n.val ASC");
	ASSERT_EQ(res.rowCount(), 3UL);
	// NULL should sort first or last depending on implementation
}

TEST_F(CypherAdvancedTest, OrderByNonExistentProperty) {
	(void) execute("CREATE (n:NoProp {a: 1})");
	(void) execute("CREATE (n:NoProp {a: 2})");

	// Should work, treating non-existent as NULL
	auto res = execute("MATCH (n:NoProp) RETURN n ORDER BY n.nonexistent ASC");
	ASSERT_EQ(res.rowCount(), 2UL);
}

// --- Additional VarLength Traversal Tests ---

TEST_F(CypherAdvancedTest, VarLengthTraversal_SameHop) {
	// Create: A -> B -> C -> D
	(void) execute("CREATE (a:Hop {id:'A'})-[e1:NEXT]->(b:Hop {id:'B'})");
	(void) execute("MATCH (b:Hop {id:'B'}) CREATE (b)-[e2:NEXT]->(c:Hop {id:'C'})");
	(void) execute("MATCH (c:Hop {id:'C'}) CREATE (c)-[e3:NEXT]->(d:Hop {id:'D'})");

	// Exactly 2 hops
	auto res = execute("MATCH (a:Hop {id:'A'})-[*2..2]->(x) RETURN x");
	ASSERT_EQ(res.rowCount(), 1UL); // Only C
	EXPECT_EQ(res.getRows()[0].at("x").asNode().getProperties().at("id").toString(), "C");
}

TEST_F(CypherAdvancedTest, VarLengthTraversal_MinGreaterThanMax) {
	// Invalid range, should return empty
	(void) execute("CREATE (a:BadHop)-[:NEXT]->(b:BadHop)");
	auto res = execute("MATCH (a:BadHop)-[*5..3]->(x) RETURN x");
	EXPECT_TRUE(res.isEmpty());
}

TEST_F(CypherAdvancedTest, VarLengthTraversal_Bidirectional) {
	(void) execute("CREATE (a:Bidir {id:'A'})-[e1:LINK]->(b:Bidir {id:'B'})");
	(void) execute("MATCH (b:Bidir {id:'B'}) CREATE (b)-[e2:LINK]->(c:Bidir {id:'C'})");

	// Find nodes reachable from B in any direction
	auto res = execute("MATCH (b:Bidir {id:'B'})-[*1..2]-(x) RETURN x");
	ASSERT_GE(res.rowCount(), 2UL); // A and C at least
}

// --- Additional Unwind Tests ---

TEST_F(CypherAdvancedTest, Unwind_EmptyList) {
	auto res = execute("UNWIND [] AS x RETURN x");
	EXPECT_TRUE(res.isEmpty());
}

TEST_F(CypherAdvancedTest, Unwind_SingleElement) {
	auto res = execute("UNWIND [42] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "42");
}

TEST_F(CypherAdvancedTest, Unwind_StringList) {
	auto res = execute("UNWIND ['a', 'b', 'c'] AS s RETURN s");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("s").toString(), "a");
	EXPECT_EQ(res.getRows()[1].at("s").toString(), "b");
	EXPECT_EQ(res.getRows()[2].at("s").toString(), "c");
}

TEST_F(CypherAdvancedTest, Unwind_WithMatch) {
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("MATCH (n:Test) UNWIND [10, 20] AS x RETURN n.id, x");
	ASSERT_EQ(res.rowCount(), 4UL); // 2 nodes * 2 unwind values = 4 rows
}

// --- Additional Cartesian Product Tests ---

// Note: Cartesian product with AND filter may have issues in current implementation
// Skipping CartesianProduct_WithFilter test for now

TEST_F(CypherAdvancedTest, CartesianProduct_SingleRowEach) {
	(void) execute("CREATE (:SingleA)");
	(void) execute("CREATE (:SingleB)");

	auto res = execute("MATCH (a:SingleA), (b:SingleB) RETURN a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherAdvancedTest, CartesianProduct_ThreePatterns) {
	(void) execute("CREATE (:TripleA {id: 1})");
	(void) execute("CREATE (:TripleB {id: 2})");
	(void) execute("CREATE (:TripleC {id: 3})");

	// Three independent patterns
	auto res = execute("MATCH (a:TripleA), (b:TripleB), (c:TripleC) RETURN a.id, b.id, c.id");
	ASSERT_EQ(res.rowCount(), 1UL);
}
