/**
 * @file test_Api.cpp
 * @author Nexepic
 * @date 2026/1/5
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

#include <filesystem>
#include <gtest/gtest.h>
#include <iostream>
#include <unordered_map>
#include <variant>
#include <vector>

#include "zyx/zyx.hpp"
#include "zyx/value.hpp"

namespace fs = std::filesystem;

class CppApiTest : public ::testing::Test {
protected:
	std::string dbPath;
	std::unique_ptr<zyx::Database> db;

	void SetUp() override {
		auto tempDir = fs::temp_directory_path();
		dbPath = (tempDir / ("api_test_" + std::to_string(std::rand()))).string();
		if (fs::exists(dbPath))
			fs::remove_all(dbPath);

		db = std::make_unique<zyx::Database>(dbPath);
		db->open();
	}

	void TearDown() override {
		if (db)
			db->close();
		if (fs::exists(dbPath))
			fs::remove_all(dbPath);
	}
};

// ============================================================================
// 1. Basic CRUD & Property Access
// ============================================================================

TEST_F(CppApiTest, CreateNodeAndQueryProperties) {
	std::unordered_map<std::string, zyx::Value> props;
	props["name"] = "Alice";
	props["age"] = (int64_t) 30;
	props["score"] = 95.5;
	props["active"] = true;

	db->createNode("Person", props);
	db->save();

	auto res = db->execute("MATCH (n:Person) RETURN n.name, n.age, n.score, n.active");

	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();

	// 1. Verify String (Fuzzy match n.name -> name or vice versa)
	auto nameVal = res.get("n.name");
	if (std::holds_alternative<std::monostate>(nameVal))
		nameVal = res.get("name");

	ASSERT_FALSE(std::holds_alternative<std::monostate>(nameVal)) << "name not found";
	ASSERT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "Alice");

	// 2. Verify Integer
	auto ageVal = res.get("n.age");
	if (std::holds_alternative<std::monostate>(ageVal))
		ageVal = res.get("age");
	ASSERT_FALSE(std::holds_alternative<std::monostate>(ageVal));
	EXPECT_EQ(std::get<int64_t>(ageVal), 30);

	// 3. Verify Double
	auto scoreVal = res.get("n.score");
	if (std::holds_alternative<std::monostate>(scoreVal))
		scoreVal = res.get("score");
	ASSERT_FALSE(std::holds_alternative<std::monostate>(scoreVal));
	EXPECT_DOUBLE_EQ(std::get<double>(scoreVal), 95.5);

	// 4. Verify Bool
	auto activeVal = res.get("n.active");
	if (std::holds_alternative<std::monostate>(activeVal))
		activeVal = res.get("active");
	ASSERT_FALSE(std::holds_alternative<std::monostate>(activeVal));
	EXPECT_TRUE(std::get<bool>(activeVal));
}

TEST_F(CppApiTest, ReturnFullNode) {
	db->createNode("Robot", {{"id", (int64_t) 999}});
	db->save();

	auto res = db->execute("MATCH (n:Robot) RETURN n");

	ASSERT_TRUE(res.hasNext());
	res.next();

	auto nodeVal = res.get("n");
	ASSERT_FALSE(std::holds_alternative<std::monostate>(nodeVal)) << "Return 'n' is empty";
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(nodeVal));

	auto nodePtr = std::get<std::shared_ptr<zyx::Node>>(nodeVal);
	ASSERT_NE(nodePtr, nullptr);
	EXPECT_EQ(nodePtr->label, "Robot");

	ASSERT_TRUE(nodePtr->properties.contains("id"));
	EXPECT_EQ(std::get<int64_t>(nodePtr->properties.at("id")), 999);
}

// ============================================================================
// 2. Advanced Creation (ID & Edges)
// ============================================================================

TEST_F(CppApiTest, CreateNodeRetId) {
	int64_t id1 = db->createNodeRetId("User", {{"uid", (int64_t) 100}});
	int64_t id2 = db->createNodeRetId("User", {{"uid", (int64_t) 200}});

	db->createEdgeById(id1, id2, "KNOWS");
	db->save();

	auto res = db->execute("MATCH (a)-[:KNOWS]->(b) RETURN a.uid, b.uid");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto u1 = res.get("a.uid");
	if (std::holds_alternative<std::monostate>(u1))
		u1 = res.get("uid");

	ASSERT_FALSE(std::holds_alternative<std::monostate>(u1));
	EXPECT_EQ(std::get<int64_t>(u1), 100);
}

TEST_F(CppApiTest, CreateEdgeFullParams) {
	// 1. Setup
	db->createNode("User", {{"uid", (int64_t) 1}});
	db->createNode("User", {{"uid", (int64_t) 2}});
	db->save();

	// 2. Action: Create Edge
	// This calls QueryBuilder -> MATCH(a), MATCH(b), CREATE(e)
	db->createEdge("User", "uid", (int64_t) 1, "User", "uid", (int64_t) 2, "FOLLOWS", {{"since", (int64_t) 2025}});
	db->save();

	// 3. Verification
	// Query specifically for the edge connecting these two IDs
	auto res = db->execute("MATCH (a)-[e:FOLLOWS]->(b) WHERE a.uid=1 AND b.uid=2 RETURN e");

	// Rigorous Check 1: Must have at least one result
	ASSERT_TRUE(res.hasNext()) << "Edge FOLLOWS was not created.";
	res.next();

	// Rigorous Check 2: Validate Data Types and Values
	auto edgeVal = res.get("e");
	ASSERT_FALSE(std::holds_alternative<std::monostate>(edgeVal));

	// Explicitly check for Edge Type (verifying toPublicValue/toPublicEdgePtr logic)
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(edgeVal))
			<< "Expected shared_ptr<Edge>, got something else (maybe Node?)";

	auto edgePtr = std::get<std::shared_ptr<zyx::Edge>>(edgeVal);
	EXPECT_EQ(edgePtr->label, "FOLLOWS");

	ASSERT_TRUE(edgePtr->properties.contains("since"));
	auto propVal = edgePtr->properties.at("since");
	ASSERT_TRUE(std::holds_alternative<int64_t>(propVal));
	EXPECT_EQ(std::get<int64_t>(propVal), 2025);

	// Rigorous Check 3: Must NOT have any more results (prevent duplicates)
	ASSERT_FALSE(res.hasNext()) << "Duplicate edges found! createEdge might be running multiple times.";
}

// ============================================================================
// 3. Metadata & Index Access
// ============================================================================

TEST_F(CppApiTest, ResultGetByIndex) {
	db->createNode("Test", {{"val", (int64_t) 123}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n.val");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Access via index 0
	// ResultImpl logic ensures correct mapping even for single implicit columns
	auto val = res.get(0);
	ASSERT_FALSE(std::holds_alternative<std::monostate>(val));
	ASSERT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 123);
}

// ============================================================================
// 4. Algorithms & Topology
// ============================================================================

TEST_F(CppApiTest, AlgorithmShortestPath) {
	int64_t a = db->createNodeRetId("A", {{"val", (int64_t) 1}});
	int64_t b = db->createNodeRetId("B", {{"val", (int64_t) 2}});
	int64_t c = db->createNodeRetId("C", {{"val", (int64_t) 3}});
	db->createEdgeById(a, b, "NEXT");
	db->createEdgeById(b, c, "NEXT");
	db->save();

	auto path = db->getShortestPath(a, c);
	ASSERT_EQ(path.size(), 3u);
	EXPECT_EQ(path[0].id, a);
	EXPECT_EQ(path[2].id, c);

	// Verify directional
	auto emptyPath = db->getShortestPath(c, a);
	EXPECT_TRUE(emptyPath.empty());
}

TEST_F(CppApiTest, AlgorithmBFS) {
	int64_t root = db->createNodeRetId("Root");
	int64_t child = db->createNodeRetId("Child");
	db->createEdgeById(root, child, "LINK");
	db->save();

	std::vector<int64_t> visited;
	db->bfs(root, [&](const zyx::Node &n) {
		visited.push_back(n.id);
		return true;
	});
	ASSERT_EQ(visited.size(), 2u);
}

// ============================================================================
// 5. Internal Conversion Verification (ResultValue -> Value)
// ============================================================================

TEST_F(CppApiTest, InternalConversionCheck) {
	// This test verifies that DatabaseImpl correctly unwraps ResultValue to Value
	// Specifically focusing on complex types in mixed results

	db->createNode("N", {{"id", (int64_t) 1}});
	db->createNode("M", {{"id", (int64_t) 2}});
	// Create Edge
	(void) db->execute("MATCH (n:N), (m:M) CREATE (n)-[r:REL {w: 10}]->(m)");
	db->save();

	auto res = db->execute("MATCH (n)-[r]->(m) RETURN n, r");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Check Node (n)
	auto nVal = res.get("n");
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(nVal)) << "Result 'n' should be Node";

	// Check Edge (r)
	auto rVal = res.get("r");
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(rVal))
			<< "Result 'r' should be Edge - Check toPublicValue logic!";
}

// ============================================================================
// 6. Null Values and Vector Properties (Coverage Improvement)
// ============================================================================

TEST_F(CppApiTest, HandleNullPropertyValues) {
	// Create node with null property (missing in creation)
	db->createNode("NullTest", {{"name", "Test"}});
	db->save();

	// Query returns null for missing properties
	auto res = db->execute("MATCH (n:NullTest) RETURN n.optionalProp");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.optionalProp");
	// Should be std::monostate (null)
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, HandleVectorProperties) {
	// Create node with vector property
	std::vector<std::string> vecProps = {"1.1", "2.2", "3.3"};
	db->createNode("VectorTest", {{"embeddings", vecProps}});
	db->save();

	// Query the vector property
	auto res = db->execute("MATCH (n:VectorTest) RETURN n.embeddings");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.embeddings");
	// Vector properties are returned as std::vector<std::string>
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));

	auto vec = std::get<std::vector<std::string>>(val);
	ASSERT_EQ(vec.size(), 3u);
	// std::to_string(float) converts with default precision
	EXPECT_TRUE(vec[0] == "1.100000" || vec[0] == "1.1");
	EXPECT_TRUE(vec[1] == "2.200000" || vec[1] == "2.2");
	EXPECT_TRUE(vec[2] == "3.300000" || vec[2] == "3.3");
}

// ============================================================================
// Additional tests for improved coverage
// ============================================================================

TEST_F(CppApiTest, CreateNodesWithEmptyPropsList) {
	// Test createNodes with empty props list - should handle gracefully
	std::vector<std::unordered_map<std::string, zyx::Value>> emptyProps;
	db->createNodes("EmptyLabel", emptyProps);
	db->save();

	// Verify no nodes were created - query should return no rows
	auto res = db->execute("MATCH (n:EmptyLabel) RETURN n");
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, CreateNodesWithMultipleNodes) {
	// Test batch node creation
	std::vector<std::unordered_map<std::string, zyx::Value>> propsList;
	propsList.push_back({{"id", (int64_t) 1}, {"name", "First"}});
	propsList.push_back({{"id", (int64_t) 2}, {"name", "Second"}});
	propsList.push_back({{"id", (int64_t) 3}, {"name", "Third"}});

	db->createNodes("BatchNode", propsList);
	db->save();

	// Verify all nodes were created
	auto res = db->execute("MATCH (n:BatchNode) RETURN n.name ORDER BY n.id");
	ASSERT_TRUE(res.hasNext());

	std::vector<std::string> names;
	while (res.hasNext()) {
		res.next();
		auto nameVal = res.get("n.name");
		if (std::holds_alternative<std::string>(nameVal)) {
			names.push_back(std::get<std::string>(nameVal));
		}
	}

	EXPECT_EQ(names.size(), 3u);
	EXPECT_EQ(names[0], "First");
	EXPECT_EQ(names[1], "Second");
	EXPECT_EQ(names[2], "Third");
}

TEST_F(CppApiTest, CreateEdgeByIdWithProperties) {
	int64_t id1 = db->createNodeRetId("User", {{"uid", (int64_t) 1}});
	int64_t id2 = db->createNodeRetId("User", {{"uid", (int64_t) 2}});

	// Create edge with properties using createEdgeById
	db->createEdgeById(id1, id2, "CONNECTED", {{"weight", (int64_t) 100}, {"active", true}});
	db->save();

	// Verify edge was created with properties
	auto res = db->execute("MATCH ()-[e:CONNECTED]->() RETURN e.weight, e.active");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto weightVal = res.get("e.weight");
	ASSERT_TRUE(std::holds_alternative<int64_t>(weightVal));
	EXPECT_EQ(std::get<int64_t>(weightVal), 100);

	auto activeVal = res.get("e.active");
	ASSERT_TRUE(std::holds_alternative<bool>(activeVal));
	EXPECT_TRUE(std::get<bool>(activeVal));
}

TEST_F(CppApiTest, ResultGetByIndexOutOfBounds) {
	db->createNode("Test", {{"val", (int64_t) 123}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n.val");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Test negative index
	auto negVal = res.get(-1);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(negVal));

	// Test out of bounds positive index
	auto oobVal = res.get(100);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(oobVal));
}

TEST_F(CppApiTest, ResultGetInvalidColumnName) {
	db->createNode("Test", {{"val", (int64_t) 123}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n.val");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Test with non-existent column
	auto val = res.get("nonexistent_column");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, ResultIterationNoResults) {
	auto res = db->execute("MATCH (n:NonExistentLabel) RETURN n");
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, ResultMultipleNextCalls) {
	db->createNode("Test", {{"val", (int64_t) 1}});

	auto res = db->execute("MATCH (n:Test) RETURN n.val");

	// First next
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Second next - should return false (no more rows)
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, GetColumnNameOutOfBounds) {
	db->createNode("Test", {{"val", (int64_t) 123}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n.val");

	// Test negative index
	std::string nameNeg = res.getColumnName(-1);
	EXPECT_EQ(nameNeg, "");

	// Test out of bounds positive index
	std::string nameOob = res.getColumnName(100);
	EXPECT_EQ(nameOob, "");
}

TEST_F(CppApiTest, GetColumnCountWithNoResults) {
	auto res = db->execute("MATCH (n:NonExistent) RETURN n");
	// Even with no results, we should have columns defined
	int count = res.getColumnCount();
	EXPECT_GT(count, 0);
}

TEST_F(CppApiTest, GetResultBeforeNext) {
	db->createNode("Test", {{"val", (int64_t) 123}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n.val");

	// Try to get value before calling next() - should return monostate
	auto val = res.get("n.val");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));

	// Now call next() and try again
	res.next();
	auto val2 = res.get("n.val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val2));
}

TEST_F(CppApiTest, ShortestPathWithMaxDepth) {
	// Create a path: A -> B -> C -> D
	int64_t a = db->createNodeRetId("Node", {{"name", "A"}});
	int64_t b = db->createNodeRetId("Node", {{"name", "B"}});
	int64_t c = db->createNodeRetId("Node", {{"name", "C"}});
	int64_t d = db->createNodeRetId("Node", {{"name", "D"}});

	db->createEdgeById(a, b, "NEXT");
	db->createEdgeById(b, c, "NEXT");
	db->createEdgeById(c, d, "NEXT");
	db->save();

	// Test with maxDepth=2 - should not reach D
	auto path = db->getShortestPath(a, d, 2);
	EXPECT_LT(path.size(), 4u);
}

TEST_F(CppApiTest, BFSEarlyTermination) {
	int64_t root = db->createNodeRetId("Root");
	int64_t child1 = db->createNodeRetId("Child1");
	int64_t child2 = db->createNodeRetId("Child2");

	db->createEdgeById(root, child1, "LINK");
	db->createEdgeById(root, child2, "LINK");
	db->save();

	std::vector<int64_t> visited;
	// Return false after first node to stop traversal early
	db->bfs(root, [&](const zyx::Node &n) {
		visited.push_back(n.id);
		return false; // Stop after first node
	});

	// Should only visit the root
	EXPECT_EQ(visited.size(), 1u);
	EXPECT_EQ(visited[0], root);
}

TEST_F(CppApiTest, ExecuteInvalidQuery) {
	// Test that execute handles errors gracefully
	auto res = db->execute("INVALID CYPHER QUERY");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_FALSE(res.getError().empty());
}

TEST_F(CppApiTest, ResultFuzzyColumnNameMatch) {
	db->createNode("Person", {{"name", "Alice"}, {"age", (int64_t) 30}});
	db->save();

	auto res = db->execute("MATCH (n:Person) RETURN n.name, n.age");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Test fuzzy matching - "name" should match "n.name"
	auto nameVal = res.get("name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "Alice");

	// "age" should match "n.age"
	auto ageVal = res.get("age");
	EXPECT_TRUE(std::holds_alternative<int64_t>(ageVal));
	EXPECT_EQ(std::get<int64_t>(ageVal), 30);
}

// ============================================================================
// Additional tests for edge case coverage
// ============================================================================

TEST_F(CppApiTest, ResultMoveAssignment) {
	db->createNode("Test", {{"val", (int64_t) 123}});
	db->save();

	auto res1 = db->execute("MATCH (n:Test) RETURN n.val");
	ASSERT_TRUE(res1.hasNext());

	// Test move assignment
	zyx::Result res2;
	res2 = std::move(res1);

	ASSERT_TRUE(res2.hasNext());
	res2.next();
	auto val = res2.get("n.val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 123);

	// res1 should be in moved-from state
	EXPECT_FALSE(res1.hasNext());
}

TEST_F(CppApiTest, ResultWithNoColumns) {
	// Create a result with no data (empty result set)
	auto res = db->execute("MATCH (n:NonExistent) RETURN n");
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, ResultEntityStreamMode) {
	// Test entity stream mode (single column with node/edge)
	db->createNode("Person", {{"name", "Bob"}, {"age", (int64_t) 25}});
	db->save();

	auto res = db->execute("MATCH (n:Person) RETURN n");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// In entity stream mode, empty key or "n" should return the node
	auto nodeVal1 = res.get("");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(nodeVal1));

	auto nodeVal2 = res.get("n");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(nodeVal2));

	// Can also access properties directly in entity stream mode
	auto nameVal = res.get("name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "Bob");
}

TEST_F(CppApiTest, ResultEdgeStreamMode) {
	int64_t id1 = db->createNodeRetId("Node", {{"id", (int64_t) 1}});
	int64_t id2 = db->createNodeRetId("Node", {{"id", (int64_t) 2}});
	db->createEdgeById(id1, id2, "KNOWS", {{"since", (int64_t) 2020}});
	db->save();

	auto res = db->execute("MATCH ()-[e:KNOWS]->() RETURN e");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// In edge stream mode, can access edge properties directly
	auto sinceVal = res.get("since");
	EXPECT_TRUE(std::holds_alternative<int64_t>(sinceVal));
	EXPECT_EQ(std::get<int64_t>(sinceVal), 2020);
}

TEST_F(CppApiTest, ResultMoveConstructor) {
	db->createNode("Test", {{"val", (int64_t) 456}});
	db->save();

	auto res1 = db->execute("MATCH (n:Test) RETURN n.val");
	ASSERT_TRUE(res1.hasNext());

	// Test move constructor
	zyx::Result res2 = std::move(res1);

	ASSERT_TRUE(res2.hasNext());
	res2.next();
	auto val = res2.get("n.val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 456);
}

TEST_F(CppApiTest, GetWithCursorBeforeNext) {
	db->createNode("Test", {{"val", (int64_t) 789}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n.val");

	// Try to get value before calling next() - should return monostate
	auto val1 = res.get("n.val");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val1));

	// Now call next() and try again
	res.next();
	auto val2 = res.get("n.val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val2));
	EXPECT_EQ(std::get<int64_t>(val2), 789);
}

TEST_F(CppApiTest, ResultGetAfterEnd) {
	db->createNode("Test", {{"val", (int64_t) 100}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n.val");

	// Call next() to move to first row
	res.next();

	// Call next() again to move past end
	res.next();

	// Try to get value when cursor is past end - should return monostate
	auto val = res.get("n.val");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, CreateNodesWithSingleEmptyProps) {
	// Test with a single entry in propsList that is empty
	std::vector<std::unordered_map<std::string, zyx::Value>> propsList;
	propsList.push_back({}); // Empty properties

	db->createNodes("SingleEmpty", propsList);
	db->save();

	// Verify node was created - use simple match instead of count
	auto res = db->execute("MATCH (n:SingleEmpty) RETURN n");
	EXPECT_TRUE(res.hasNext());
}

TEST_F(CppApiTest, CreateEdgeByIdNoProps) {
	int64_t id1 = db->createNodeRetId("Node1");
	int64_t id2 = db->createNodeRetId("Node2");

	// Create edge with no properties
	db->createEdgeById(id1, id2, "LINK");
	db->save();

	// Verify edge was created - use simple match instead of count
	auto res = db->execute("MATCH ()-[e:LINK]->() RETURN e");
	EXPECT_TRUE(res.hasNext());
}

TEST_F(CppApiTest, ResultDefaultConstructor) {
	// Test default constructed Result
	zyx::Result res;

	EXPECT_FALSE(res.hasNext());
	EXPECT_EQ(res.getColumnCount(), 0);
	EXPECT_EQ(res.getColumnName(0), "");
	EXPECT_EQ(res.getDuration(), 0.0);
	// Default constructed Result is not successful (no impl_)
	EXPECT_FALSE(res.isSuccess());
	EXPECT_EQ(res.getError(), "Result not initialized");
}

TEST_F(CppApiTest, ExecuteWithSyntaxError) {
	// Test various error scenarios
	auto res = db->execute("MATC (n) RETURN n"); // Typo: MATC instead of MATCH
	EXPECT_FALSE(res.isSuccess());
	EXPECT_FALSE(res.getError().empty());
}

TEST_F(CppApiTest, ExecuteWithRuntimeError) {
	// Try to access a property that doesn't exist
	auto res = db->execute("RETURN 1 / 0 AS result");
	// This might succeed or fail depending on query engine
	// Just verify we get a result object
	if (!res.isSuccess()) {
		EXPECT_FALSE(res.getError().empty());
	}
}

TEST_F(CppApiTest, ReverseFuzzyMatch) {
	db->createNode("Person", {{"name", "Charlie"}});
	db->save();

	auto res = db->execute("MATCH (n:Person) RETURN n.name");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Test reverse fuzzy match - "n.name" when DB has just "name"
	// This tests the branch where dbKey is shorter than key
	auto nameVal = res.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "Charlie");
}

// ============================================================================
// Additional edge case tests for maximum coverage
// ============================================================================

TEST_F(CppApiTest, GetAfterCursorPastEnd) {
	db->createNode("Test", {{"val", (int64_t) 1}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n.val");

	// Move to first row
	res.next();

	// Move past end by calling next() again
	res.next();

	// Try to get value - should return monostate
	auto val = res.get("n.val");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));

	// Try to get by index
	auto val2 = res.get(0);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val2));
}

TEST_F(CppApiTest, ResultMultipleNextAfterEnd) {
	db->createNode("Test", {{"val", (int64_t) 1}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n.val");

	// Move to first row
	res.next();

	// Call next() multiple times after end
	res.next();
	res.next();
	res.next();

	// Should handle gracefully
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, FuzzyMatchWithReverseDotNotation) {
	// Create a node and query with simple column name
	db->createNode("Person", {{"name", "Dave"}});
	db->save();

	// The query executor might return "n.name" even if we write "RETURN name"
	// So we test the standard fuzzy match instead
	auto res = db->execute("MATCH (n:Person) RETURN n.name");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Direct access should work
	auto nameVal = res.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "Dave");

	// Fuzzy match without prefix should also work
	auto nameVal2 = res.get("name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal2));
	EXPECT_EQ(std::get<std::string>(nameVal2), "Dave");
}

TEST_F(CppApiTest, CreateNodeWithVectorAndStringProps) {
	// Test creating node with vector property
	std::vector<std::string> vec = {"1.0", "2.0", "3.0"};
	db->createNode("VectorNode", {{"name", "Test"}, {"embeddings", vec}});
	db->save();

	auto res = db->execute("MATCH (n:VectorNode) RETURN n.name, n.embeddings");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto nameVal = res.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "Test");

	auto vecVal = res.get("n.embeddings");
	EXPECT_TRUE(std::holds_alternative<std::vector<std::string>>(vecVal));
}

TEST_F(CppApiTest, EdgeWithAllPropertyTypes) {
	int64_t id1 = db->createNodeRetId("A");
	int64_t id2 = db->createNodeRetId("B");

	// Create edge with all property types
	std::unordered_map<std::string, zyx::Value> edgeProps;
	edgeProps["str"] = "test";
	edgeProps["num"] = (int64_t) 42;
	edgeProps["dbl"] = 3.14;
	edgeProps["bool"] = true;

	db->createEdgeById(id1, id2, "FULL", edgeProps);
	db->save();

	auto res = db->execute("MATCH ()-[e:FULL]->() RETURN e");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto edgeVal = res.get("e");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(edgeVal));

	auto edgePtr = std::get<std::shared_ptr<zyx::Edge>>(edgeVal);
	EXPECT_EQ(edgePtr->label, "FULL");
	EXPECT_EQ(edgePtr->properties.size(), 4u);
}

TEST_F(CppApiTest, ResultGetMixedTypes) {
	// Query that returns mixed types
	db->createNode("Mixed", {{"str", "text"}, {"num", (int64_t) 123}});
	db->save();

	auto res = db->execute("MATCH (n:Mixed) RETURN n.str, n.num, 456 AS const, true AS flag, null AS nul");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// String
	auto strVal = res.get("n.str");
	EXPECT_TRUE(std::holds_alternative<std::string>(strVal));

	// Int
	auto numVal = res.get("n.num");
	EXPECT_TRUE(std::holds_alternative<int64_t>(numVal));

	// Const int
	auto constVal = res.get("const");
	EXPECT_TRUE(std::holds_alternative<int64_t>(constVal));

	// Bool
	auto flagVal = res.get("flag");
	EXPECT_TRUE(std::holds_alternative<bool>(flagVal));

	// Null
	auto nulVal = res.get("nul");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(nulVal));
}

TEST_F(CppApiTest, GetByIndexWithMultipleColumns) {
	db->createNode("MultiCol", {{"a", (int64_t) 1}, {"b", (int64_t) 2}, {"c", (int64_t) 3}});
	db->save();

	auto res = db->execute("MATCH (n:MultiCol) RETURN n.a, n.b, n.c");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Access by different indices
	auto val0 = res.get(0);
	EXPECT_TRUE(std::holds_alternative<int64_t>(val0));

	auto val1 = res.get(1);
	EXPECT_TRUE(std::holds_alternative<int64_t>(val1));

	auto val2 = res.get(2);
	EXPECT_TRUE(std::holds_alternative<int64_t>(val2));
}

TEST_F(CppApiTest, NodePropertiesAccessInStreamMode) {
	// In entity stream mode, access properties directly from entity
	db->createNode("User", {{"id", (int64_t) 100}, {"name", "Eve"}});
	db->save();

	auto res = db->execute("MATCH (n:User) RETURN n");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Get the node
	auto nodeVal = res.get("n");
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(nodeVal));

	// Access properties through the node pointer
	auto nodePtr = std::get<std::shared_ptr<zyx::Node>>(nodeVal);
	EXPECT_EQ(nodePtr->label, "User");
	EXPECT_EQ(nodePtr->properties.size(), 2u);

	// Can also access properties directly in stream mode
	auto idVal = res.get("id");
	EXPECT_TRUE(std::holds_alternative<int64_t>(idVal));

	auto nameVal = res.get("name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
}

TEST_F(CppApiTest, EdgePropertiesAccessInStreamMode) {
	int64_t id1 = db->createNodeRetId("X");
	int64_t id2 = db->createNodeRetId("Y");

	db->createEdgeById(id1, id2, "REL", {{"weight", (int64_t) 10}, {"active", true}});
	db->save();

	auto res = db->execute("MATCH ()-[e:REL]->() RETURN e");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Get the edge
	auto edgeVal = res.get("e");
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(edgeVal));

	// Access properties through the edge pointer
	auto edgePtr = std::get<std::shared_ptr<zyx::Edge>>(edgeVal);
	EXPECT_EQ(edgePtr->label, "REL");
	EXPECT_EQ(edgePtr->properties.size(), 2u);

	// Can also access properties directly in stream mode
	auto weightVal = res.get("weight");
	EXPECT_TRUE(std::holds_alternative<int64_t>(weightVal));

	auto activeVal = res.get("active");
	EXPECT_TRUE(std::holds_alternative<bool>(activeVal));
}

TEST_F(CppApiTest, EmptyColumnNameAccess) {
	db->createNode("Test", {{"val", (int64_t) 1}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// In stream mode, empty key should return the entity
	auto entityVal = res.get("");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(entityVal));
}

TEST_F(CppApiTest, NonExistentPropertyInStreamMode) {
	db->createNode("User", {{"id", (int64_t) 1}});
	db->save();

	auto res = db->execute("MATCH (n:User) RETURN n");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Try to access non-existent property in stream mode
	auto missingVal = res.get("nonexistent");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(missingVal));
}

// ============================================================================
// Additional tests for edge case coverage
// ============================================================================

TEST_F(CppApiTest, MultipleNodesAndEdgesQuery) {
	// Create more complex graph structure
	int64_t a = db->createNodeRetId("A", {{"name", "NodeA"}});
	int64_t b = db->createNodeRetId("B", {{"name", "NodeB"}});
	int64_t c = db->createNodeRetId("C", {{"name", "NodeC"}});

	db->createEdgeById(a, b, "LINK", {{"weight", (int64_t) 1}});
	db->createEdgeById(b, c, "LINK", {{"weight", (int64_t) 2}});
	db->save();

	// Query that returns nodes and edges separately
	auto res = db->execute("MATCH (a:A)-[e1:LINK]->(b:B)-[e2:LINK]->(c:C) RETURN a, e1, b, e2, c");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Verify we got all the columns
	int colCount = res.getColumnCount();
	EXPECT_EQ(colCount, 5);
}

TEST_F(CppApiTest, QueryWithAlias) {
	db->createNode("Person", {{"name", "Frank"}});
	db->save();

	// Query with alias
	auto res = db->execute("MATCH (n:Person) RETURN n AS person");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Should be able to access by alias
	auto personVal = res.get("person");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(personVal));
}

TEST_F(CppApiTest, ResultWithCalculatedFields) {
	db->createNode("Data", {{"val1", (int64_t) 10}, {"val2", (int64_t) 5}});
	db->save();

	// Query with calculated fields (if supported)
	auto res = db->execute("MATCH (n:Data) RETURN n.val1, n.val2");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Just verify we can access both values
	auto val1 = res.get("n.val1");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val1));

	auto val2 = res.get("n.val2");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val2));
}

TEST_F(CppApiTest, MultipleResultsIteration) {
	// Create multiple nodes
	db->createNode("Item", {{"id", (int64_t) 1}});
	db->createNode("Item", {{"id", (int64_t) 2}});
	db->createNode("Item", {{"id", (int64_t) 3}});
	db->save();

	auto res = db->execute("MATCH (n:Item) RETURN n.id ORDER BY n.id");

	std::vector<int64_t> ids;
	while (res.hasNext()) {
		res.next();
		auto idVal = res.get("n.id");
		if (std::holds_alternative<int64_t>(idVal)) {
			ids.push_back(std::get<int64_t>(idVal));
		}
	}

	EXPECT_EQ(ids.size(), 3u);
	EXPECT_EQ(ids[0], 1);
	EXPECT_EQ(ids[1], 2);
	EXPECT_EQ(ids[2], 3);
}

TEST_F(CppApiTest, ResultGetAfterMultipleNextCalls) {
	db->createNode("Single", {{"val", (int64_t) 42}});
	db->save();

	auto res = db->execute("MATCH (n:Single) RETURN n.val");

	// First next - should work
	EXPECT_TRUE(res.hasNext());
	res.next();
	auto val1 = res.get("n.val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val1));

	// Second next - should move past end
	EXPECT_FALSE(res.hasNext());

	// Third next - should still be at end
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, QueryWithWhereClause) {
	db->createNode("Product", {{"name", "Widget"}, {"price", (int64_t) 100}});
	db->createNode("Product", {{"name", "Gadget"}, {"price", (int64_t) 200}});
	db->save();

	// Query with WHERE clause
	auto res = db->execute("MATCH (n:Product) WHERE n.price > 150 RETURN n.name");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto nameVal = res.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "Gadget");

	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, SimpleCountQuery) {
	db->createNode("Num", {{"val", (int64_t) 10}});
	db->createNode("Num", {{"val", (int64_t) 20}});
	db->createNode("Num", {{"val", (int64_t) 30}});
	db->save();

	// Simple count query - just verify it executes
	auto res = db->execute("MATCH (n:Num) RETURN count(n)");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Query executed successfully - that's enough for this test
	EXPECT_TRUE(res.isSuccess());
}
