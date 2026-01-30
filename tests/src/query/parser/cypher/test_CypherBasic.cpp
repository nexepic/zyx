/**
 * @file test_CypherBasic.cpp
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

class CypherBasicTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_cypher_basic_" + boost::uuids::to_string(uuid) + ".dat");
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

	[[nodiscard]] std::string resolveLabel(int64_t labelId) const {
		return db->getStorage()->getDataManager()->resolveLabel(labelId);
	}
};

// --- Basic CRUD ---

TEST_F(CypherBasicTest, CreateAndMatchNode) {
	auto res1 = execute("CREATE (n:User {name: 'Alice', age: 30}) RETURN n");
	ASSERT_EQ(res1.rowCount(), 1UL);
	const auto &node1 = res1.getRows()[0].at("n").asNode();
	EXPECT_EQ(resolveLabel(node1.getLabelId()), "User");
	EXPECT_EQ(node1.getProperties().at("name").toString(), "Alice");

	auto res2 = execute("MATCH (n:User) RETURN n");
	ASSERT_EQ(res2.rowCount(), 1UL);
}

TEST_F(CypherBasicTest, CreateAndMatchChain) {
	(void) execute("CREATE (a:Person {name: 'A'})-[r:KNOWS {since: 2020}]->(b:Person {name: 'B'})");
	auto res = execute("MATCH (n:Person)-[r]->(m) RETURN n, r, m");
	ASSERT_EQ(res.rowCount(), 1UL);
	const auto &row = res.getRows()[0];
	EXPECT_TRUE(row.at("n").isNode());
	EXPECT_TRUE(row.at("m").isNode());
	EXPECT_TRUE(row.at("r").isEdge());
	EXPECT_EQ(resolveLabel(row.at("r").asEdge().getLabelId()), "KNOWS");
}

// --- Updates (SET/REMOVE/DELETE) ---

TEST_F(CypherBasicTest, UpdateProperties) {
	(void) execute("CREATE (n:UpdateTest {val: 1})");
	(void) execute("MATCH (n:UpdateTest) SET n.val = 2");
	auto res = execute("MATCH (n:UpdateTest) RETURN n");
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("val").toString(), "2");
}

TEST_F(CypherBasicTest, AddNewProperty) {
	(void) execute("CREATE (n:UpdateTest {val: 1})");
	(void) execute("MATCH (n:UpdateTest) SET n.tag = 'new'");
	auto res = execute("MATCH (n:UpdateTest) RETURN n");
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("tag").toString(), "new");
}

TEST_F(CypherBasicTest, DeleteNodes) {
	(void) execute("CREATE (n:DeleteMe)");
	(void) execute("MATCH (n:DeleteMe) DELETE n");
	auto res = execute("MATCH (n:DeleteMe) RETURN n");
	EXPECT_EQ(res.rowCount(), 0UL);
}

TEST_F(CypherBasicTest, DeleteNodeWithEdgesConstraint) {
	(void) execute("CREATE (a:ConstraintTest)-[:REL]->(b:ConstraintTest)");
	EXPECT_THROW({ (void) execute("MATCH (n:ConstraintTest) DELETE n"); }, std::runtime_error);
	EXPECT_EQ(execute("MATCH (n:ConstraintTest) RETURN n").rowCount(), 2UL);
}

TEST_F(CypherBasicTest, DetachDeleteNodes) {
	(void) execute("CREATE (a:DetachTest)-[r:REL]->(b:DetachTest)");
	(void) execute("MATCH (n:DetachTest) DETACH DELETE n");
	EXPECT_EQ(execute("MATCH (n:DetachTest) RETURN n").rowCount(), 0UL);
}

TEST_F(CypherBasicTest, DeleteEdgeOnly) {
	(void) execute("CREATE (a:EdgeDel)-[r:TO_BE_DELETED]->(b:EdgeDel)");
	(void) execute("MATCH (a:EdgeDel)-[r:TO_BE_DELETED]->(b) DELETE r");
	EXPECT_EQ(execute("MATCH (n:EdgeDel) RETURN n").rowCount(), 2UL);
	EXPECT_EQ(execute("MATCH ()-[r:TO_BE_DELETED]->() RETURN r").rowCount(), 0UL);
}

TEST_F(CypherBasicTest, RemoveProperty) {
	(void) execute("CREATE (n:RemProp {keep: 1, remove_me: 2})");
	(void) execute("MATCH (n:RemProp) REMOVE n.remove_me");
	auto props = execute("MATCH (n:RemProp) RETURN n").getRows()[0].at("n").asNode().getProperties();
	EXPECT_TRUE(props.contains("keep"));
	EXPECT_FALSE(props.contains("remove_me"));
}

TEST_F(CypherBasicTest, SetLabel) {
	(void) execute("CREATE (n:OldLabel {id: 1})");
	(void) execute("MATCH (n:OldLabel) SET n:NewLabel");
	EXPECT_EQ(execute("MATCH (n:OldLabel) RETURN n").rowCount(), 0UL);
	EXPECT_EQ(execute("MATCH (n:NewLabel) RETURN n").rowCount(), 1UL);
}

TEST_F(CypherBasicTest, RemoveLabel) {
	(void) execute("CREATE (n:TagToRemove {id: 99})");
	(void) execute("MATCH (n:TagToRemove) REMOVE n:TagToRemove");
	EXPECT_EQ(execute("MATCH (n:TagToRemove) RETURN n").rowCount(), 0UL);
	auto res = execute("MATCH (n) WHERE n.id = 99 RETURN n");
	EXPECT_EQ(resolveLabel(res.getRows()[0].at("n").asNode().getLabelId()), "");
}

// --- Filtering ---

TEST_F(CypherBasicTest, FilterByWhereClause) {
	(void) execute("CREATE (n:Item {price: 100})");
	(void) execute("CREATE (n:Item {price: 200})");
	(void) execute("CREATE (n:Item {price: 50})");

	EXPECT_EQ(execute("MATCH (n:Item) WHERE n.price = 200 RETURN n").rowCount(), 1UL);
	EXPECT_EQ(execute("MATCH (n:Item) WHERE n.price <> 100 RETURN n").rowCount(), 2UL);
	EXPECT_EQ(execute("MATCH (n:Item) WHERE n.price > 90 RETURN n").rowCount(), 2UL);
}

TEST_F(CypherBasicTest, FilterTraversalTarget) {
	(void) execute("CREATE (a:Node)-[:LINK]->(b:Target {id: 1})");
	(void) execute("CREATE (a:Node)-[:LINK]->(c:Other {id: 2})");
	auto res = execute("MATCH (n:Node)-[r]->(t:Target) RETURN t");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(resolveLabel(res.getRows()[0].at("t").asNode().getLabelId()), "Target");
}

// --- MERGE ---

TEST_F(CypherBasicTest, Merge_CreatesNewNode) {
	(void) execute("MERGE (n:MergeNew {key: 'u1'}) ON CREATE SET n.status = 'created'");
	auto res = execute("MATCH (n:MergeNew) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("status").toString(), "created");
}

TEST_F(CypherBasicTest, Merge_MatchesExisting) {
	(void) execute("CREATE (n:MergeExist {key: 'u2', count: 0})");
	(void) execute("MERGE (n:MergeExist {key: 'u2'}) ON MATCH SET n.count = 1");
	auto res = execute("MATCH (n:MergeExist) RETURN n");
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("count").toString(), "1");
}
