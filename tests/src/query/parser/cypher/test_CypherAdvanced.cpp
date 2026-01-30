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
