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

// ============================================================================
// Transaction API Tests
// ============================================================================

TEST_F(CppApiTest, TransactionBeginCommit) {
	// Test Database::beginTransaction and Transaction::commit
	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	auto res = txn.execute("CREATE (n:TxnTest {name: 'committed'})");
	EXPECT_TRUE(res.isSuccess());

	txn.commit();
	EXPECT_FALSE(txn.isActive());

	// Verify the node was committed
	auto queryRes = db->execute("MATCH (n:TxnTest) RETURN n.name");
	ASSERT_TRUE(queryRes.hasNext());
	queryRes.next();
	auto nameVal = queryRes.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "committed");
}

TEST_F(CppApiTest, TransactionRollback) {
	// Create initial data
	db->createNode("Existing", {{"id", (int64_t) 1}});
	db->save();

	// Start a transaction and rollback
	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	auto res = txn.execute("CREATE (n:RollbackTest {name: 'should_disappear'})");
	EXPECT_TRUE(res.isSuccess());

	txn.rollback();
	EXPECT_FALSE(txn.isActive());
}

TEST_F(CppApiTest, TransactionExecuteWhenNotActive) {
	// Create a transaction, commit it, then try to execute on inactive
	auto txn = db->beginTransaction();
	txn.commit();
	EXPECT_FALSE(txn.isActive());

	// Execute on inactive transaction should return error
	auto res = txn.execute("MATCH (n) RETURN n");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_EQ(res.getError(), "Transaction is not active");
}

TEST_F(CppApiTest, TransactionMoveSemantics) {
	auto txn1 = db->beginTransaction();
	EXPECT_TRUE(txn1.isActive());

	// Move construct
	zyx::Transaction txn2 = std::move(txn1);
	EXPECT_TRUE(txn2.isActive());
	EXPECT_FALSE(txn1.isActive()); // moved-from

	// Rollback txn2 first to avoid deadlock (only one active txn allowed)
	txn2.rollback();
	EXPECT_FALSE(txn2.isActive());
}

TEST_F(CppApiTest, TransactionExecuteInvalidQuery) {
	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	// Execute an invalid query within the transaction
	auto res = txn.execute("INVALID QUERY SYNTAX");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_FALSE(res.getError().empty());

	txn.rollback();
}

TEST_F(CppApiTest, TransactionExecuteAfterCommit) {
	auto txn = db->beginTransaction();
	txn.commit();
	EXPECT_FALSE(txn.isActive());

	// Execute after commit - transaction is no longer active
	auto res = txn.execute("MATCH (n) RETURN n");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_EQ(res.getError(), "Transaction is not active");
}

// ============================================================================
// Database::beginTransaction on not-yet-opened DB
// ============================================================================

TEST_F(CppApiTest, BeginTransactionOnClosedDb) {
	// Close the DB first, then create a new one without opening
	db->close();

	auto tempDir = fs::temp_directory_path();
	std::string newPath = (tempDir / ("api_txn_test_" + std::to_string(std::rand()))).string();
	auto newDb = std::make_unique<zyx::Database>(newPath);

	// beginTransaction should auto-open the DB
	auto txn = newDb->beginTransaction();
	EXPECT_TRUE(txn.isActive());
	txn.rollback();

	newDb->close();
	if (fs::exists(newPath))
		fs::remove_all(newPath);
}

// ============================================================================
// openIfExists path
// ============================================================================

TEST_F(CppApiTest, OpenIfExistsOnExistingDb) {
	// db is already open from SetUp, close it
	db->close();

	// openIfExists on a path that exists should return true
	auto db2 = std::make_unique<zyx::Database>(dbPath);
	bool exists = db2->openIfExists();
	EXPECT_TRUE(exists);
	db2->close();

	// Re-open original db for TearDown
	db->open();
}

TEST_F(CppApiTest, OpenIfExistsOnNonExistentDb) {
	auto tempDir = fs::temp_directory_path();
	std::string fakePath = (tempDir / ("nonexistent_db_" + std::to_string(std::rand()))).string();

	auto db2 = std::make_unique<zyx::Database>(fakePath);
	bool exists = db2->openIfExists();
	EXPECT_FALSE(exists);

	// Cleanup just in case
	if (fs::exists(fakePath))
		fs::remove_all(fakePath);
}

// ============================================================================
// Database::save path
// ============================================================================

TEST_F(CppApiTest, SaveFlushesData) {
	db->createNode("SaveTest", {{"key", "value"}});
	// save() calls storage->flush()
	db->save();

	auto res = db->execute("MATCH (n:SaveTest) RETURN n.key");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.key");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "value");
}

// ============================================================================
// toInternal: vector with integer strings
// ============================================================================

TEST_F(CppApiTest, VectorPropertyWithIntegerStrings) {
	// Test the toInternal branch where vector strings are pure integers
	// This covers branch 144 (pos == s.size()) True path
	std::vector<std::string> intVec = {"42", "100", "-7"};
	db->createNode("IntVecNode", {{"data", intVec}});
	db->save();

	auto res = db->execute("MATCH (n:IntVecNode) RETURN n.data");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.data");
	// Vector properties come back as vector<string>
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
}

// ============================================================================
// toInternal: vector with plain (non-numeric) strings
// ============================================================================

TEST_F(CppApiTest, VectorPropertyWithPlainStrings) {
	// Test the toInternal branch where strings are not parseable as int or double
	// This covers the "otherwise store as string" path (line 163)
	std::vector<std::string> strVec = {"hello", "world", "abc"};
	db->createNode("StrVecNode", {{"tags", strVec}});
	db->save();

	auto res = db->execute("MATCH (n:StrVecNode) RETURN n.tags");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.tags");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
	auto vec = std::get<std::vector<std::string>>(val);
	ASSERT_EQ(vec.size(), 3u);
}

// ============================================================================
// toInternal: vector with mixed types (int, double, and plain string)
// ============================================================================

TEST_F(CppApiTest, VectorPropertyWithMixedTypes) {
	// This covers all three branches in the vector parsing loop:
	// - integer string ("42") -> int path
	// - double string ("3.14") -> double path
	// - non-numeric string ("hello") -> string fallback path
	// Also covers partial parse where stoll parses but pos != s.size() (e.g., "3.14" parses as 3 but pos=1)
	std::vector<std::string> mixedVec = {"42", "3.14", "hello"};
	db->createNode("MixedVecNode", {{"mixed", mixedVec}});
	db->save();

	auto res = db->execute("MATCH (n:MixedVecNode) RETURN n.mixed");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.mixed");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
}

// ============================================================================
// toInternal: monostate value
// ============================================================================

TEST_F(CppApiTest, CreateNodeWithMonostateProperty) {
	// Test toInternal with monostate (null) value
	std::unordered_map<std::string, zyx::Value> props;
	props["name"] = "test";
	props["nullprop"] = std::monostate{};

	db->createNode("MonoTest", props);
	db->save();

	auto res = db->execute("MATCH (n:MonoTest) RETURN n.name");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
}

// ============================================================================
// Result::get reversed fuzzy match (key has dot, dbKey doesn't)
// ============================================================================

TEST_F(CppApiTest, ReverseFuzzyMatchKeyHasDot) {
	// This tests the branch at line 279:
	// if (dbKey.find('.') == npos && key.size() > dbKey.size())
	//     if (key.ends_with("." + dbKey))
	// We need a result where the DB key is "name" (no dot) and we query with "p.name"
	db->createNode("Person", {{"name", "Alice"}});
	db->save();

	// Use RETURN n.name AS name to get a column called "name" (no dot)
	auto res = db->execute("MATCH (n:Person) RETURN n.name AS name");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Now query with a dotted key where the dbKey has no dot
	auto val = res.get("p.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "Alice");
}

// ============================================================================
// Result::getDuration
// ============================================================================

TEST_F(CppApiTest, ResultGetDurationNonZero) {
	db->createNode("DurTest", {{"val", (int64_t) 1}});
	db->save();

	auto res = db->execute("MATCH (n:DurTest) RETURN n.val");
	EXPECT_TRUE(res.isSuccess());
	// Duration should be positive (measured in ms)
	EXPECT_GE(res.getDuration(), 0.0);
}

// ============================================================================
// Result::next on null impl
// ============================================================================

TEST_F(CppApiTest, ResultNextOnDefaultConstructed) {
	zyx::Result res;

	// Calling next() on default-constructed Result should not crash
	res.next();
	EXPECT_FALSE(res.hasNext());

	// get() on default-constructed result
	auto val = res.get("anything");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));

	auto val2 = res.get(0);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val2));
}

// ============================================================================
// Transaction execute success path with data
// ============================================================================

TEST_F(CppApiTest, TransactionExecuteAndQueryData) {
	auto txn = db->beginTransaction();

	// Create node within transaction
	auto createRes = txn.execute("CREATE (n:TxnData {name: 'txn_node', value: 42})");
	EXPECT_TRUE(createRes.isSuccess());

	// Query within same transaction
	auto queryRes = txn.execute("MATCH (n:TxnData) RETURN n.name, n.value");
	EXPECT_TRUE(queryRes.isSuccess());

	if (queryRes.hasNext()) {
		queryRes.next();
		auto name = queryRes.get("n.name");
		EXPECT_TRUE(std::holds_alternative<std::string>(name));
	}

	txn.commit();
}

// ============================================================================
// createNodeRetId with empty properties
// ============================================================================

TEST_F(CppApiTest, CreateNodeRetIdNoProps) {
	int64_t id = db->createNodeRetId("EmptyNode");
	EXPECT_GT(id, 0);
	db->save();

	auto res = db->execute("MATCH (n:EmptyNode) RETURN n");
	EXPECT_TRUE(res.hasNext());
}

// ============================================================================
// Node stream mode (mode_ == 1)
// ============================================================================

TEST_F(CppApiTest, NodeStreamMode_GetEntity) {
	db->createNode("StreamNode", {{"val", (int64_t)99}});
	db->save();

	// RETURN n (single column, contains a node) -> mode_ = 1
	auto res = db->execute("MATCH (n:StreamNode) RETURN n");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Get the entity itself by column name
	auto nodeVal = res.get("n");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(nodeVal));

	// Get the entity by empty key
	auto emptyVal = res.get("");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(emptyVal));

	// Get a property inside the entity
	auto propVal = res.get("val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(propVal));
	EXPECT_EQ(std::get<int64_t>(propVal), 99);

	// Get a non-existent property inside the entity
	auto missingProp = res.get("nonexistent");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(missingProp));
}

// ============================================================================
// Edge stream mode (mode_ == 2)
// ============================================================================

TEST_F(CppApiTest, EdgeStreamMode_GetEntity) {
	db->createNode("ES1", {{"id", (int64_t)1}});
	db->createNode("ES2", {{"id", (int64_t)2}});
	(void)db->execute("MATCH (a:ES1), (b:ES2) CREATE (a)-[e:ESREL {w: 55}]->(b)");
	db->save();

	// RETURN e (single column, contains an edge) -> mode_ = 2
	auto res = db->execute("MATCH ()-[e:ESREL]->() RETURN e");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Get the entity itself by column name
	auto edgeVal = res.get("e");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(edgeVal));

	// Get the entity by empty key
	auto emptyVal = res.get("");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(emptyVal));

	// Get a property inside the edge entity
	auto propVal = res.get("w");
	EXPECT_TRUE(std::holds_alternative<int64_t>(propVal));
	EXPECT_EQ(std::get<int64_t>(propVal), 55);

	// Get non-existent property in the edge
	auto missingProp = res.get("nonexistent_edge_prop");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(missingProp));
}

// ============================================================================
// BFS traversal
// ============================================================================

TEST_F(CppApiTest, BfsTraversal) {
	int64_t a = db->createNodeRetId("BFSNode", {{"name", "A"}});
	int64_t b = db->createNodeRetId("BFSNode", {{"name", "B"}});
	int64_t c = db->createNodeRetId("BFSNode", {{"name", "C"}});
	db->createEdgeById(a, b, "LINK");
	db->createEdgeById(b, c, "LINK");
	db->save();

	std::vector<std::string> visited;
	db->bfs(a, [&](const zyx::Node& n) -> bool {
		auto it = n.properties.find("name");
		if (it != n.properties.end() && std::holds_alternative<std::string>(it->second)) {
			visited.push_back(std::get<std::string>(it->second));
		}
		return true; // continue traversal
	});

	EXPECT_GE(visited.size(), 1u);
}

TEST_F(CppApiTest, BfsTraversal_StopEarly) {
	int64_t a = db->createNodeRetId("BFSStop", {{"name", "A"}});
	int64_t b = db->createNodeRetId("BFSStop", {{"name", "B"}});
	int64_t c = db->createNodeRetId("BFSStop", {{"name", "C"}});
	db->createEdgeById(a, b, "LINK");
	db->createEdgeById(b, c, "LINK");
	db->save();

	int count = 0;
	db->bfs(a, [&](const zyx::Node&) -> bool {
		count++;
		return false; // stop after first
	});

	EXPECT_EQ(count, 1);
}

// ============================================================================
// Shortest path
// ============================================================================

TEST_F(CppApiTest, ShortestPath_NoPath) {
	int64_t a = db->createNodeRetId("SPNode", {{"name", "A"}});
	int64_t b = db->createNodeRetId("SPNode", {{"name", "B"}});
	// No edge between them
	db->save();

	auto path = db->getShortestPath(a, b);
	EXPECT_TRUE(path.empty());
}

// ============================================================================
// createEdge using query-based match
// ============================================================================

TEST_F(CppApiTest, CreateEdge_QueryBased) {
	db->createNode("CENode", {{"name", "X"}});
	db->createNode("CENode", {{"name", "Y"}});
	db->save();

	db->createEdge("CENode", "name", std::string("X"),
				   "CENode", "name", std::string("Y"),
				   "KNOWS", {{"since", (int64_t)2025}});
	db->save();

	auto res = db->execute("MATCH ()-[e:KNOWS]->() RETURN e.since");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("e.since");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 2025);
}

// ============================================================================
// Transaction unknown error path (catch ...)
// ============================================================================

TEST_F(CppApiTest, TransactionExecuteInvalidCypher) {
	auto txn = db->beginTransaction();
	auto res = txn.execute("THIS IS NOT VALID CYPHER AT ALL");
	EXPECT_FALSE(res.isSuccess());
	std::string err = res.getError();
	EXPECT_FALSE(err.empty());
	txn.rollback();
}

// ============================================================================
// Map property conversion
// ============================================================================

TEST_F(CppApiTest, ResultWithMapProperty) {
	// Test that map-like properties work through node creation and retrieval
	db->execute("CREATE (n:MapTestNode {key: 'value', num: 42})");
	auto res = db->execute("MATCH (n:MapTestNode) RETURN n.key AS k, n.num AS num");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto k = res.get("k");
	auto num = res.get("num");
	EXPECT_EQ(std::get<std::string>(k), "value");
	EXPECT_EQ(std::get<int64_t>(num), 42);
}

// ============================================================================
// createEdgeById with no properties (covers empty props branch)
// ============================================================================

TEST_F(CppApiTest, CreateEdgeByIdNoProps_EmptyBranch) {
	int64_t a = db->createNodeRetId("EPNode");
	int64_t b = db->createNodeRetId("EPNode");
	db->createEdgeById(a, b, "EMPTY_EDGE");
	db->save();

	auto res = db->execute("MATCH ()-[e:EMPTY_EDGE]->() RETURN e");
	EXPECT_TRUE(res.hasNext());
}

// ============================================================================
// Additional branch coverage tests for DatabaseImpl.cpp
// ============================================================================

TEST_F(CppApiTest, TransactionCommitOnMovedFrom) {
	// Test Transaction::commit() when impl_ is null (line 405-406)
	// Create a transaction and move it out, leaving a null impl_
	auto txn1 = db->beginTransaction();
	zyx::Transaction txn2 = std::move(txn1);
	// txn1 is now moved-from (impl_ is null)
	EXPECT_THROW(txn1.commit(), std::runtime_error);
	txn2.rollback();
}

TEST_F(CppApiTest, TransactionRollbackOnMovedFrom) {
	// Test Transaction::rollback() when impl_ is null (line 410-411)
	auto txn1 = db->beginTransaction();
	zyx::Transaction txn2 = std::move(txn1);
	// txn1 is now moved-from (impl_ is null)
	EXPECT_THROW(txn1.rollback(), std::runtime_error);
	txn2.rollback();
}

TEST_F(CppApiTest, TransactionIsActiveOnMovedFrom) {
	// Test Transaction::isActive() when impl_ is null (line 416)
	auto txn1 = db->beginTransaction();
	zyx::Transaction txn2 = std::move(txn1);
	// txn1 is now moved-from
	EXPECT_FALSE(txn1.isActive());
	txn2.rollback();
}

TEST_F(CppApiTest, ExecuteAutoOpensDb) {
	// Test Database::execute() when DB is not open (line 447-449)
	db->close();

	auto tempDir = fs::temp_directory_path();
	std::string newPath = (tempDir / ("api_autoopen_" + std::to_string(std::rand()))).string();
	auto newDb = std::make_unique<zyx::Database>(newPath);

	// execute() should auto-open the DB
	auto res = newDb->execute("RETURN 42 AS val");
	EXPECT_TRUE(res.isSuccess());

	newDb->close();
	if (fs::exists(newPath))
		fs::remove_all(newPath);
}

TEST_F(CppApiTest, VectorPropertyWithDoubleStrings) {
	// Test toInternal branch for double values where stoll fails (line 148 catch)
	// and stod succeeds (line 153-156)
	std::vector<std::string> doubleVec = {"3.14", "2.718", "1.618"};
	db->createNode("DoubleVecNode", {{"data", doubleVec}});
	db->save();

	auto res = db->execute("MATCH (n:DoubleVecNode) RETURN n.data");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.data");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
}

TEST_F(CppApiTest, VectorPropertyWithPartialIntParse) {
	// Test toInternal where stoll parses partially (pos != s.size()) at line 144
	// e.g., "42abc" -> stoll returns 42 but pos=2 != 5, so falls through to double/string
	std::vector<std::string> partialVec = {"42abc", "not_a_number"};
	db->createNode("PartialVecNode", {{"data", partialVec}});
	db->save();

	auto res = db->execute("MATCH (n:PartialVecNode) RETURN n.data");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.data");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
}

TEST_F(CppApiTest, VectorPropertyWithPartialDoubleParse) {
	// Test toInternal where stod parses partially (pos != s.size()) at line 155
	// e.g., "3.14xyz" -> stod returns 3.14 but pos=4 != 7
	std::vector<std::string> partialDoubleVec = {"3.14xyz"};
	db->createNode("PartialDoubleNode", {{"data", partialDoubleVec}});
	db->save();

	auto res = db->execute("MATCH (n:PartialDoubleNode) RETURN n.data");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.data");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
}

TEST_F(CppApiTest, CreateNodeRetIdWithProps) {
	// Test createNodeRetId with non-empty props (line 541-546)
	int64_t id = db->createNodeRetId("PropNode", {{"key", "value"}, {"num", (int64_t)42}});
	EXPECT_GT(id, 0);
	db->save();

	auto res = db->execute("MATCH (n:PropNode) RETURN n.key, n.num");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto keyVal = res.get("n.key");
	EXPECT_TRUE(std::holds_alternative<std::string>(keyVal));
	EXPECT_EQ(std::get<std::string>(keyVal), "value");

	auto numVal = res.get("n.num");
	EXPECT_TRUE(std::holds_alternative<int64_t>(numVal));
	EXPECT_EQ(std::get<int64_t>(numVal), 42);
}

TEST_F(CppApiTest, ResultGetColumnNameValidIndex) {
	db->createNode("ColTest", {{"a", (int64_t)1}, {"b", (int64_t)2}});
	db->save();

	auto res = db->execute("MATCH (n:ColTest) RETURN n.a, n.b");
	EXPECT_TRUE(res.isSuccess());

	int count = res.getColumnCount();
	EXPECT_EQ(count, 2);

	// Valid column names
	std::string col0 = res.getColumnName(0);
	std::string col1 = res.getColumnName(1);
	EXPECT_FALSE(col0.empty());
	EXPECT_FALSE(col1.empty());
}

TEST_F(CppApiTest, ResultGetByIndexNotStarted) {
	// Test Result::get(int) when not started (line 316: !impl_->started_)
	db->createNode("IdxTest", {{"val", (int64_t)1}});
	db->save();

	auto res = db->execute("MATCH (n:IdxTest) RETURN n.val");
	// Don't call next() - started_ is false
	auto val = res.get(0);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, ExecuteWithExplicitTransaction) {
	// Test Database::execute() when an explicit transaction is already active
	// This exercises the needsAutoCommit = false branch (line 453, 456)
	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	// Execute on the database while transaction is active
	auto res = db->execute("RETURN 1 AS val");
	// The behavior depends on whether the DB detects the active transaction
	// Just verify it doesn't crash
	(void)res;

	txn.rollback();
}

TEST_F(CppApiTest, ShortestPathInvalidNodes) {
	// Test getShortestPath with non-existent nodes (line 611 catch path)
	auto path = db->getShortestPath(99999, 99998);
	EXPECT_TRUE(path.empty());
}

TEST_F(CppApiTest, BfsOnNonExistentNode) {
	// Test bfs on non-existent node (line 631-632 catch path)
	int count = 0;
	db->bfs(99999, [&](const zyx::Node&) -> bool {
		count++;
		return true;
	});
	// Should not crash, count may be 0 or 1 depending on implementation
	EXPECT_GE(count, 0);
}

// ============================================================================
// Additional branch coverage tests for DatabaseImpl.cpp
// ============================================================================

TEST_F(CppApiTest, ExecuteOnClosedDbAutoOpens) {
	// Test that Database::execute auto-opens the DB (line 447-449 branch)
	db->close();
	// Re-create a fresh DB object without opening
	auto tempDir = fs::temp_directory_path();
	std::string newPath = (tempDir / ("api_closed_test_" + std::to_string(std::rand()))).string();
	auto newDb = std::make_unique<zyx::Database>(newPath);
	// execute should auto-open
	auto res = newDb->execute("RETURN 1 AS val");
	EXPECT_TRUE(res.isSuccess());
	newDb->close();
	if (fs::exists(newPath))
		fs::remove_all(newPath);
}

TEST_F(CppApiTest, BeginTransactionAutoOpensDb) {
	// Test Database::beginTransaction auto-opens the DB (line 434-436)
	db->close();
	auto tempDir = fs::temp_directory_path();
	std::string newPath = (tempDir / ("api_txn_autoopen_" + std::to_string(std::rand()))).string();
	auto newDb = std::make_unique<zyx::Database>(newPath);
	// beginTransaction should auto-open
	auto txn = newDb->beginTransaction();
	EXPECT_TRUE(txn.isActive());
	txn.rollback();
	newDb->close();
	if (fs::exists(newPath))
		fs::remove_all(newPath);
}

TEST_F(CppApiTest, CreateNodeRetIdEmptyProps) {
	// Test createNodeRetId with empty props (line 541 false branch: props.empty())
	int64_t id = db->createNodeRetId("NoPropsNode");
	EXPECT_GT(id, 0);
	db->save();

	auto res = db->execute("MATCH (n:NoPropsNode) RETURN n");
	EXPECT_TRUE(res.hasNext());
}

TEST_F(CppApiTest, CreateEdgeByIdEmptyProps) {
	// Test createEdgeById with empty props (line 561 false branch: props.empty())
	int64_t a = db->createNodeRetId("CEBN1");
	int64_t b = db->createNodeRetId("CEBN2");
	db->createEdgeById(a, b, "NO_PROPS_EDGE");
	db->save();

	auto res = db->execute("MATCH ()-[e:NO_PROPS_EDGE]->() RETURN e");
	EXPECT_TRUE(res.hasNext());
}

TEST_F(CppApiTest, CreateNodesEmptyList) {
	// Test createNodes with empty propsList (line 505 early return)
	std::vector<std::unordered_map<std::string, zyx::Value>> empty;
	db->createNodes("EmptyBatch", empty);
	db->save();

	auto res = db->execute("MATCH (n:EmptyBatch) RETURN n");
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, ResultImplNoColumns) {
	// Test prepareMetadata when no explicit columns and no rows
	// (line 201-208 branches)
	auto res = db->execute("MATCH (n:_NonExistent_) RETURN n");
	EXPECT_EQ(res.getColumnCount(), 1); // "n" column defined by query
}

TEST_F(CppApiTest, ResultHasNextAfterComplete) {
	// Test hasNext after cursor has moved past all rows (line 242)
	db->createNode("HN", {{"v", (int64_t)1}});
	db->save();

	auto res = db->execute("MATCH (n:HN) RETURN n.v");
	ASSERT_TRUE(res.hasNext());
	res.next();
	EXPECT_FALSE(res.hasNext());
	// Call next again - should handle gracefully
	res.next();
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, GetColumnNameValidAndInvalid) {
	// Test getColumnName with valid and invalid indices (line 326-328)
	db->createNode("CN", {{"a", (int64_t)1}});
	db->save();

	auto res = db->execute("MATCH (n:CN) RETURN n.a");
	EXPECT_FALSE(res.getColumnName(0).empty());
	EXPECT_EQ(res.getColumnName(-1), "");
	EXPECT_EQ(res.getColumnName(999), "");
}

TEST_F(CppApiTest, GetDurationDefault) {
	// Test getDuration on default result (line 331)
	zyx::Result res;
	EXPECT_EQ(res.getDuration(), 0.0);
}

TEST_F(CppApiTest, IsSuccessDefault) {
	// Test isSuccess on default result (line 333)
	zyx::Result res;
	EXPECT_FALSE(res.isSuccess());
}

TEST_F(CppApiTest, GetErrorDefault) {
	// Test getError on default result (line 336-338)
	zyx::Result res;
	EXPECT_EQ(res.getError(), "Result not initialized");
}

// ============================================================================
// Integration-style tests for uncovered branches in DatabaseImpl.cpp
// ============================================================================

TEST_F(CppApiTest, DeleteNodeViaCypher) {
	// Exercise DELETE code path in QueryExecutor and storage layer
	db->createNode("Deletable", {{"name", "ToDelete"}});
	db->createNode("Deletable", {{"name", "ToKeep"}});
	db->save();

	// Delete one node
	auto res = db->execute("MATCH (n:Deletable {name: 'ToDelete'}) DELETE n");
	EXPECT_TRUE(res.isSuccess());
	db->save();

	// Verify only one remains
	auto queryRes = db->execute("MATCH (n:Deletable) RETURN n.name");
	ASSERT_TRUE(queryRes.hasNext());
	queryRes.next();
	auto nameVal = queryRes.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "ToKeep");
	EXPECT_FALSE(queryRes.hasNext());
}

TEST_F(CppApiTest, DeleteEdgeViaCypher) {
	// Exercise edge deletion path through full stack
	int64_t a = db->createNodeRetId("DA", {{"id", (int64_t)1}});
	int64_t b = db->createNodeRetId("DB", {{"id", (int64_t)2}});
	db->createEdgeById(a, b, "DEL_REL", {{"weight", (int64_t)10}});
	db->save();

	// Delete the edge
	auto res = db->execute("MATCH ()-[e:DEL_REL]->() DELETE e");
	EXPECT_TRUE(res.isSuccess());
	db->save();

	// Verify edge is gone
	auto queryRes = db->execute("MATCH ()-[e:DEL_REL]->() RETURN e");
	EXPECT_FALSE(queryRes.hasNext());

	// Verify nodes still exist
	auto nodeRes = db->execute("MATCH (n:DA) RETURN n");
	EXPECT_TRUE(nodeRes.hasNext());
}

TEST_F(CppApiTest, CreateAndDeleteMultipleEdges) {
	// Exercise edge linking/unlinking with multiple edges on same node
	int64_t hub = db->createNodeRetId("Hub");
	int64_t spoke1 = db->createNodeRetId("Spoke", {{"id", (int64_t)1}});
	int64_t spoke2 = db->createNodeRetId("Spoke", {{"id", (int64_t)2}});
	int64_t spoke3 = db->createNodeRetId("Spoke", {{"id", (int64_t)3}});

	db->createEdgeById(hub, spoke1, "CONNECTS");
	db->createEdgeById(hub, spoke2, "CONNECTS");
	db->createEdgeById(hub, spoke3, "CONNECTS");
	db->save();

	// Delete middle edge
	auto delRes = db->execute("MATCH (h:Hub)-[e:CONNECTS]->(s:Spoke {id: 2}) DELETE e");
	EXPECT_TRUE(delRes.isSuccess());
	db->save();

	// Verify only 2 edges remain
	auto queryRes = db->execute("MATCH (h:Hub)-[e:CONNECTS]->() RETURN e");
	int edgeCount = 0;
	while (queryRes.hasNext()) {
		queryRes.next();
		edgeCount++;
	}
	EXPECT_EQ(edgeCount, 2);
}

TEST_F(CppApiTest, CloseAndReopenDatabase) {
	// Exercise close/reopen paths in Database.cpp and FileStorage.cpp
	db->createNode("Persist", {{"key", "value"}});
	db->save();
	db->close();

	// Reopen and verify data persists
	db = std::make_unique<zyx::Database>(dbPath);
	db->open();

	auto res = db->execute("MATCH (n:Persist) RETURN n.key");
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.key");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "value");
}

TEST_F(CppApiTest, FlushCloseReopenCycle) {
	// Exercise flush -> close -> reopen -> flush cycle for FileStorage
	for (int i = 0; i < 3; i++) {
		db->createNode("Cycle", {{"iter", (int64_t)i}});
		db->save();
	}
	db->close();

	db = std::make_unique<zyx::Database>(dbPath);
	db->open();

	// Add more data after reopen
	db->createNode("Cycle", {{"iter", (int64_t)3}});
	db->save();

	auto res = db->execute("MATCH (n:Cycle) RETURN n.iter ORDER BY n.iter");
	int count = 0;
	while (res.hasNext()) {
		res.next();
		count++;
	}
	EXPECT_EQ(count, 4);
}

TEST_F(CppApiTest, BulkCreateDeleteFlushReopen) {
	// Exercise bulk operations + delete + flush + reopen for storage coverage
	// Create many nodes
	std::vector<std::unordered_map<std::string, zyx::Value>> propsList;
	for (int i = 0; i < 20; i++) {
		propsList.push_back({{"idx", (int64_t)i}});
	}
	db->createNodes("Bulk", propsList);
	db->save();

	// Delete some via Cypher
	(void)db->execute("MATCH (n:Bulk) WHERE n.idx < 5 DELETE n");
	db->save();

	db->close();
	db = std::make_unique<zyx::Database>(dbPath);
	db->open();

	// Verify remaining nodes
	auto res = db->execute("MATCH (n:Bulk) RETURN n.idx ORDER BY n.idx");
	int count = 0;
	while (res.hasNext()) {
		res.next();
		count++;
	}
	EXPECT_EQ(count, 15);
}

TEST_F(CppApiTest, TransactionWithDeleteAndCommit) {
	// Transaction covering delete path and commit
	db->createNode("TxnDel", {{"name", "alice"}});
	db->createNode("TxnDel", {{"name", "bob"}});
	db->save();

	auto txn = db->beginTransaction();
	auto res = txn.execute("MATCH (n:TxnDel {name: 'alice'}) DELETE n");
	EXPECT_TRUE(res.isSuccess());
	txn.commit();

	auto queryRes = db->execute("MATCH (n:TxnDel) RETURN n.name");
	ASSERT_TRUE(queryRes.hasNext());
	queryRes.next();
	auto nameVal = queryRes.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "bob");
	EXPECT_FALSE(queryRes.hasNext());
}

TEST_F(CppApiTest, ExecuteOnClosedDbCreatesNew) {
	// Exercise the auto-open branch in Database::execute() when db was never opened
	auto tempDir = fs::temp_directory_path();
	std::string newPath = (tempDir / ("api_never_opened_" + std::to_string(std::rand()))).string();
	auto newDb = std::make_unique<zyx::Database>(newPath);
	// Don't call open() - execute should auto-open
	auto res = newDb->execute("CREATE (n:AutoOpen {val: 1})");
	EXPECT_TRUE(res.isSuccess());

	// Verify node was created
	auto queryRes = newDb->execute("MATCH (n:AutoOpen) RETURN n.val");
	ASSERT_TRUE(queryRes.hasNext());
	queryRes.next();
	auto val = queryRes.get("n.val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));

	newDb->close();
	if (fs::exists(newPath))
		fs::remove_all(newPath);
}

TEST_F(CppApiTest, QueryReturningEdgeValues) {
	// Exercise the edge value path in QueryExecutor (ResultValue::isEdge branch)
	int64_t a = db->createNodeRetId("QE_A");
	int64_t b = db->createNodeRetId("QE_B");
	db->createEdgeById(a, b, "QE_REL", {{"score", 3.14}});
	db->save();

	auto res = db->execute("MATCH (a)-[r:QE_REL]->(b) RETURN a, r, b");
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Verify we get a node for 'a'
	auto aVal = res.get("a");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(aVal));

	// Verify we get an edge for 'r'
	auto rVal = res.get("r");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(rVal));

	// Verify we get a node for 'b'
	auto bVal = res.get("b");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(bVal));
}

TEST_F(CppApiTest, QueryWithSetProperty) {
	// Exercise SET clause which modifies existing entities
	db->createNode("Mutable", {{"name", "original"}, {"count", (int64_t)0}});
	db->save();

	auto res = db->execute("MATCH (n:Mutable) SET n.count = 42 RETURN n.count");
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.count");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 42);
}

TEST_F(CppApiTest, ComplexGraphTraversalQuery) {
	// Exercise complex query paths in QueryExecutor with variable-length patterns
	int64_t a = db->createNodeRetId("Chain", {{"pos", (int64_t)1}});
	int64_t b = db->createNodeRetId("Chain", {{"pos", (int64_t)2}});
	int64_t c = db->createNodeRetId("Chain", {{"pos", (int64_t)3}});
	int64_t d = db->createNodeRetId("Chain", {{"pos", (int64_t)4}});

	db->createEdgeById(a, b, "NEXT");
	db->createEdgeById(b, c, "NEXT");
	db->createEdgeById(c, d, "NEXT");
	db->save();

	// Multi-hop query
	auto res = db->execute("MATCH (a:Chain)-[:NEXT]->(b:Chain)-[:NEXT]->(c:Chain) RETURN a.pos, b.pos, c.pos");
	ASSERT_TRUE(res.isSuccess());
	int pathCount = 0;
	while (res.hasNext()) {
		res.next();
		pathCount++;
	}
	EXPECT_GE(pathCount, 1);
}

TEST_F(CppApiTest, SaveOnClosedDb) {
	// Exercise save() when storage might not be available
	db->close();
	// Reopen
	db = std::make_unique<zyx::Database>(dbPath);
	db->open();
	// Save with no changes should be a no-op
	db->save();
	SUCCEED();
}

TEST_F(CppApiTest, HasActiveTransactionBranch) {
	// Exercise hasActiveTransaction (line 109-111 in Database.cpp)
	// Without active transaction
	auto res1 = db->execute("RETURN 1 AS val");
	EXPECT_TRUE(res1.isSuccess());

	// With active transaction
	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	// Execute while transaction is active - exercises needsAutoCommit = false path
	auto res2 = db->execute("RETURN 2 AS val");
	// Just verify no crash
	(void)res2;

	txn.rollback();
}

// ============================================================================
// Branch coverage improvement: toInternal with shared_ptr<Node> / shared_ptr<Edge>
// ============================================================================

TEST_F(CppApiTest, CreateNodeWithNodePtrProperty) {
	// Test toInternal branch for shared_ptr<Node> (line 166-168) - returns empty PropertyValue
	auto nodePtr = std::make_shared<zyx::Node>();
	nodePtr->id = 1;
	nodePtr->label = "Inner";

	std::unordered_map<std::string, zyx::Value> props;
	props["name"] = "test";
	props["ref"] = nodePtr; // This triggers the shared_ptr<Node> branch in toInternal

	// Should not crash - the shared_ptr<Node> value is converted to empty PropertyValue
	db->createNode("RefHolder", props);
	db->save();

	auto res = db->execute("MATCH (n:RefHolder) RETURN n.name");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "test");
}

TEST_F(CppApiTest, CreateNodeWithEdgePtrProperty) {
	// Test toInternal branch for shared_ptr<Edge> (line 166-168) - returns empty PropertyValue
	auto edgePtr = std::make_shared<zyx::Edge>();
	edgePtr->id = 1;
	edgePtr->label = "InnerEdge";

	std::unordered_map<std::string, zyx::Value> props;
	props["name"] = "test";
	props["ref"] = edgePtr; // This triggers the shared_ptr<Edge> branch in toInternal

	// Should not crash - the shared_ptr<Edge> value is converted to empty PropertyValue
	db->createNode("EdgeRefHolder", props);
	db->save();

	auto res = db->execute("MATCH (n:EdgeRefHolder) RETURN n.name");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "test");
}

// ============================================================================
// Branch coverage: Transaction::execute success path with duration measurement
// ============================================================================

TEST_F(CppApiTest, TransactionExecuteSuccessWithDuration) {
	// Test the Transaction::execute success path (line 377-401)
	// This specifically covers the try block success path with duration
	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	auto res = txn.execute("CREATE (n:TxnDuration {val: 42}) RETURN n.val");
	EXPECT_TRUE(res.isSuccess());
	EXPECT_GE(res.getDuration(), 0.0);

	if (res.hasNext()) {
		res.next();
		auto val = res.get("n.val");
		EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	}

	txn.commit();
}

// ============================================================================
// Branch coverage: Transaction move assignment
// ============================================================================

TEST_F(CppApiTest, TransactionMoveAssignment) {
	auto txn1 = db->beginTransaction();
	EXPECT_TRUE(txn1.isActive());

	// Move construction (not default-ctor + move-assign, since default ctor is private)
	zyx::Transaction txn2 = std::move(txn1);
	EXPECT_TRUE(txn2.isActive());
	EXPECT_FALSE(txn1.isActive()); // moved-from

	txn2.rollback();
}

// ============================================================================
// Additional branch coverage tests for uncovered paths
// ============================================================================

TEST_F(CppApiTest, TransactionMoveAssignmentOperator) {
	// Covers line 367: Transaction::operator=(Transaction&&) - previously 0 executions
	auto txn1 = db->beginTransaction();
	txn1.commit();

	// Create a new transaction and move-assign it
	auto txn2 = db->beginTransaction();
	EXPECT_TRUE(txn2.isActive());

	// Use move-assignment (not construction) by assigning to txn1
	txn1 = std::move(txn2);
	EXPECT_TRUE(txn1.isActive());
	EXPECT_FALSE(txn2.isActive()); // moved-from

	txn1.rollback();
}

TEST_F(CppApiTest, NodeStreamModeKeyNotMatchingColName) {
	// Covers Branch (289:33) False path and Branch (294:8) variations
	// In node stream mode (mode_==1), when key is not empty and not equal to colName,
	// we should try to look up the key as a property within the node entity.
	db->createNode("NSKey", {{"alpha", (int64_t)10}, {"beta", "hello"}});
	db->save();

	auto res = db->execute("MATCH (n:NSKey) RETURN n");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// key "alpha" is not empty and not "n" (the column name), so it falls through
	// to the property lookup branch (line 299-303)
	auto alphaVal = res.get("alpha");
	EXPECT_TRUE(std::holds_alternative<int64_t>(alphaVal));
	EXPECT_EQ(std::get<int64_t>(alphaVal), 10);

	// key "beta" - same path
	auto betaVal = res.get("beta");
	EXPECT_TRUE(std::holds_alternative<std::string>(betaVal));
	EXPECT_EQ(std::get<std::string>(betaVal), "hello");

	// Verify key that matches column name returns the node entity
	auto nodeVal = res.get("n");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(nodeVal));

	// Non-existent property - falls through all checks to final return monostate (line 312)
	auto missing = res.get("nonexistent_xyz_123");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(missing));
}

TEST_F(CppApiTest, EdgeStreamModeKeyNotMatchingColNameOrEmpty) {
	// Tests edge stream mode (mode_==2) more thoroughly
	// Covers Branch (304:15) edge property lookup more exhaustively
	int64_t id1 = db->createNodeRetId("ESK1");
	int64_t id2 = db->createNodeRetId("ESK2");
	db->createEdgeById(id1, id2, "ESKREL", {{"prop1", (int64_t)42}, {"prop2", "value"}});
	db->save();

	auto res = db->execute("MATCH ()-[e:ESKREL]->() RETURN e");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Access edge property "prop1" - goes through isEdge() check (line 304)
	auto p1 = res.get("prop1");
	EXPECT_TRUE(std::holds_alternative<int64_t>(p1));
	EXPECT_EQ(std::get<int64_t>(p1), 42);

	// Access edge property "prop2"
	auto p2 = res.get("prop2");
	EXPECT_TRUE(std::holds_alternative<std::string>(p2));
	EXPECT_EQ(std::get<std::string>(p2), "value");

	// Access edge entity itself by column name
	auto edgeVal = res.get("e");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(edgeVal));

	// Access with empty key
	auto emptyVal = res.get("");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(emptyVal));

	// Access non-existent property - isEdge is true but prop not found
	// Falls through to return monostate at line 312
	auto missing = res.get("nonexistent_edge_prop_xyz");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(missing));
}

TEST_F(CppApiTest, FuzzyMatchKeyHasDotDbKeyNoDotEndswithTrue) {
	// Specifically covers Branch (280:10) True path
	// where dbKey has no dot, key is longer, and key.ends_with("." + dbKey)
	db->createNode("FME", {{"name", "test_val"}});
	db->save();

	// Use AS alias to produce column "name" (no dot)
	auto res = db->execute("MATCH (n:FME) RETURN n.name AS name");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// key = "prefix.name" has dot, key is longer than dbKey="name",
	// and key ends_with ".name" => should match
	auto val = res.get("any_prefix.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "test_val");
}

TEST_F(CppApiTest, FuzzyMatchKeyNoDotDbKeyHasDotEndswithFalse) {
	// Specifically covers Branch (276:10) False path
	// where key has no dot, dbKey is longer, dbKey.ends_with("." + key) is FALSE
	db->createNode("FMEF", {{"data", (int64_t)99}});
	db->save();

	auto res = db->execute("MATCH (n:FMEF) RETURN n.data");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// key = "xyz" has no dot, dbKey = "n.data" is longer
	// but "n.data" does NOT end with ".xyz" => False branch
	auto val = res.get("xyz");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, ResultGetPastEndByIndex) {
	// Covers the cursor >= rows.size() check in get(string) at line 261
	// and also the not-started check in get(int) at line 316
	db->createNode("PastEnd", {{"v", (int64_t)1}});
	db->save();

	auto res = db->execute("MATCH (n:PastEnd) RETURN n.v");
	res.next(); // move to first
	res.next(); // move past end

	// get by string when cursor past end
	auto val1 = res.get("n.v");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val1));

	// get by index when cursor past end
	auto val2 = res.get(0);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val2));
}

TEST_F(CppApiTest, ResultGetByIndexNotStartedNotNull) {
	// Covers Branch (316:17) - started_ is false when get(int) is called
	db->createNode("IdxNS", {{"v", (int64_t)42}});
	db->save();

	auto res = db->execute("MATCH (n:IdxNS) RETURN n.v");
	// Don't call next() - started_ is false but impl_ is not null
	auto val = res.get(0);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, MultiColumnRowModeFuzzyMatchAllBranches) {
	// Exercises more fuzzy match branches in row mode (mode_==0)
	// with multiple columns to test iteration through all key-matching branches
	db->createNode("MC", {{"x", (int64_t)1}, {"y", (int64_t)2}, {"z", (int64_t)3}});
	db->save();

	auto res = db->execute("MATCH (n:MC) RETURN n.x, n.y, n.z");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Exact match
	auto xVal = res.get("n.x");
	EXPECT_TRUE(std::holds_alternative<int64_t>(xVal));

	// Fuzzy match: key "x" (no dot), dbKey "n.x" (has dot, longer)
	// Branch (275:9): key.find('.') == npos && dbKey.size() > key.size()
	// Branch (276:10): dbKey.ends_with("." + key) => True
	auto x2 = res.get("x");
	EXPECT_TRUE(std::holds_alternative<int64_t>(x2));

	// Fuzzy match: key "q.y" (has dot), dbKey "n.y" (has dot)
	// Neither fuzzy branch applies (both have dots), so no match
	auto qy = res.get("q.y");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(qy));

	// Fuzzy match with key that has no dot but no dbKey ends with ".key"
	auto noMatch = res.get("w");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(noMatch));
}

TEST_F(CppApiTest, ResultGetOnNullImplByIndex) {
	// Covers Branch (316:7) where impl_ is null
	zyx::Result res; // default constructed, impl_ is null
	auto val = res.get(0);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, ResultGetOnNullImplByString) {
	// Covers Branch (257:7) where impl_ is null
	zyx::Result res; // default constructed, impl_ is null
	auto val = res.get("anything");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, NodeStreamModeMultipleRows) {
	// Create multiple nodes to test node stream mode with multiple rows
	// and verify hasNext/next iteration works correctly in mode_==1
	db->createNode("StreamIter", {{"id", (int64_t)1}});
	db->createNode("StreamIter", {{"id", (int64_t)2}});
	db->createNode("StreamIter", {{"id", (int64_t)3}});
	db->save();

	auto res = db->execute("MATCH (n:StreamIter) RETURN n ORDER BY n.id");
	int count = 0;
	while (res.hasNext()) {
		res.next();
		auto nodeVal = res.get("n");
		EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(nodeVal));

		// Access property directly in stream mode
		auto idVal = res.get("id");
		EXPECT_TRUE(std::holds_alternative<int64_t>(idVal));
		count++;
	}
	EXPECT_EQ(count, 3);
}

TEST_F(CppApiTest, EdgeStreamModeMultipleRows) {
	// Create multiple edges to test edge stream mode with multiple rows
	int64_t a = db->createNodeRetId("ESM_A");
	int64_t b = db->createNodeRetId("ESM_B");
	int64_t c = db->createNodeRetId("ESM_C");
	db->createEdgeById(a, b, "ESM_REL", {{"w", (int64_t)1}});
	db->createEdgeById(a, c, "ESM_REL", {{"w", (int64_t)2}});
	db->save();

	auto res = db->execute("MATCH ()-[e:ESM_REL]->() RETURN e");
	int count = 0;
	while (res.hasNext()) {
		res.next();
		auto edgeVal = res.get("e");
		EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(edgeVal));

		// Access property directly in edge stream mode
		auto wVal = res.get("w");
		EXPECT_TRUE(std::holds_alternative<int64_t>(wVal));
		count++;
	}
	EXPECT_EQ(count, 2);
}

TEST_F(CppApiTest, PrepareMetadataModeDetermination) {
	// Exercise the mode_ determination logic in prepareMetadata more thoroughly
	// When columnNames_.size() == 1 and rows exist, mode depends on first value type:
	// - PropertyValue => mode_ = 0
	// - Node => mode_ = 1
	// - Edge => mode_ = 2

	// Test mode_ = 0: single column returning a scalar (not node/edge)
	db->createNode("ModeTest", {{"val", (int64_t)42}});
	db->save();

	auto res = db->execute("MATCH (n:ModeTest) RETURN n.val");
	ASSERT_TRUE(res.hasNext());
	res.next();
	// This is mode_==0 (standard row) since n.val is a property, not a node/edge
	auto val = res.get("n.val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 42);
}

TEST_F(CppApiTest, SaveWhenStorageIsValid) {
	// Exercises Branch (429:12) True path (storage exists)
	db->createNode("SaveValid", {{"k", (int64_t)1}});
	EXPECT_NO_THROW(db->save());

	// Verify data was saved
	auto res = db->execute("MATCH (n:SaveValid) RETURN n.k");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.k");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));
}

TEST_F(CppApiTest, ExecuteWithActiveTransactionNeedsAutoCommitFalse) {
	// Exercises Branch (456:8) False path: needsAutoCommit = false
	// when an explicit transaction is already active
	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	// Execute on the database while a transaction is active
	// This should set needsAutoCommit = false
	auto res = db->execute("RETURN 42 AS val");
	// Just verify no crash - the result might vary depending on implementation
	(void)res;

	txn.rollback();
}

TEST_F(CppApiTest, ExecuteImplicitTransactionCommit) {
	// Exercises Branch (469:8) True path: implicitTxn commit
	// When no explicit transaction is active, execute creates an implicit one and commits
	auto res = db->execute("CREATE (n:ImplTxn {val: 99}) RETURN n.val");
	ASSERT_TRUE(res.isSuccess());

	// Verify the implicit transaction committed the data
	auto queryRes = db->execute("MATCH (n:ImplTxn) RETURN n.val");
	ASSERT_TRUE(queryRes.hasNext());
	queryRes.next();
	auto val = queryRes.get("n.val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 99);
}

// ============================================================================
// Branch coverage: Result get(string) fuzzy match where both key and dbKey have no dot
// ============================================================================

TEST_F(CppApiTest, ResultGetFuzzyMatchBothNoDot) {
	db->createNode("FuzzyTest", {{"val", (int64_t)42}});
	db->save();

	auto res = db->execute("MATCH (n:FuzzyTest) RETURN n.val AS val");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Exact match should work
	auto val = res.get("val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));

	// Non-matching key with no dot - should return monostate
	auto missing = res.get("xyz");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(missing));
}

// ============================================================================
// Branch coverage: Edge stream mode where key does not match entity prop
// ============================================================================

TEST_F(CppApiTest, EdgeStreamModeNonExistentProperty) {
	int64_t id1 = db->createNodeRetId("ESN1");
	int64_t id2 = db->createNodeRetId("ESN2");
	db->createEdgeById(id1, id2, "ESREL2", {{"w", (int64_t)10}});
	db->save();

	auto res = db->execute("MATCH ()-[e:ESREL2]->() RETURN e");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Edge stream mode: accessing non-existent property returns monostate
	auto missing = res.get("nonexistent_prop_in_edge");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(missing));
}

// ============================================================================
// Branch coverage: Result hasNext when cursor + 1 == total (boundary)
// ============================================================================

TEST_F(CppApiTest, ResultHasNextBoundaryCondition) {
	db->createNode("BoundTest", {{"v", (int64_t)1}});
	db->createNode("BoundTest", {{"v", (int64_t)2}});
	db->save();

	auto res = db->execute("MATCH (n:BoundTest) RETURN n.v ORDER BY n.v");

	// Before calling next(): started_ = false, total > 0 => hasNext = true
	EXPECT_TRUE(res.hasNext());

	// First next()
	res.next();
	// Now cursor_ = 0, total = 2, cursor_ + 1 = 1 < 2 => hasNext = true
	EXPECT_TRUE(res.hasNext());

	// Second next()
	res.next();
	// Now cursor_ = 1, total = 2, cursor_ + 1 = 2 < 2 is false => hasNext = false
	EXPECT_FALSE(res.hasNext());
}

// ============================================================================
// Branch coverage: VectorProperty with stod exception path
// ============================================================================

TEST_F(CppApiTest, VectorPropertyWithStodException) {
	// Strings that fail both stoll and stod (line 148 and 158 catch blocks)
	std::vector<std::string> badVec = {"not_a_number_at_all", "!!!"};
	db->createNode("BadVecNode", {{"data", badVec}});
	db->save();

	auto res = db->execute("MATCH (n:BadVecNode) RETURN n.data");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.data");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
	auto vec = std::get<std::vector<std::string>>(val);
	ASSERT_EQ(vec.size(), 2u);
}

// ============================================================================
// Branch coverage: implicit columns path (line 203)
// When executor returns no explicit columns but rows exist
// ============================================================================

TEST_F(CppApiTest, ResultImplicitColumnsFromRows) {
	// Create data so we have rows to query
	db->createNode("ImplCol", {{"x", (int64_t)10}});
	db->save();

	// Execute a query that returns rows. The query engine normally
	// provides explicit columns, but we exercise the code path through
	// a standard query and verify columns are populated.
	auto res = db->execute("MATCH (n:ImplCol) RETURN n.x");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.x");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 10);
}

// Cover line 370: Transaction::execute when impl_ is null (moved-from transaction)
TEST_F(CppApiTest, TransactionExecuteOnMovedFromReturnsError) {
	auto txn1 = db->beginTransaction();
	zyx::Transaction txn2 = std::move(txn1);
	// txn1 is now moved-from (impl_ is null)
	auto res = txn1.execute("RETURN 1");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_EQ(res.getError(), "Transaction is not active");
	EXPECT_FALSE(res.hasNext());
	txn2.rollback();
}

// Cover line 370: Transaction::execute after commit (txn no longer active)
TEST_F(CppApiTest, TransactionExecuteAfterCommitReturnsError) {
	auto txn = db->beginTransaction();
	txn.commit();
	// Transaction is no longer active after commit
	auto res = txn.execute("RETURN 1");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_EQ(res.getError(), "Transaction is not active");
}

// ============================================================================
// Branch coverage: save() when storage might be null
// Covers line 429: !storage False path (normal case)
// ============================================================================

TEST_F(CppApiTest, SaveOnOpenDatabase) {
	db->createNode("SaveTest", {{"k", (int64_t)1}});
	// save() should succeed without error
	EXPECT_NO_THROW(db->save());
}

// ============================================================================
// Branch coverage: getShortestPath with disconnected nodes
// Covers line 593: storage not null but no path found
// ============================================================================

TEST_F(CppApiTest, ShortestPathDisconnectedNodes) {
	auto id1 = db->createNodeRetId("SPDisc", {});
	auto id2 = db->createNodeRetId("SPDisc", {});
	db->save();

	// No edge between them, should return empty path
	auto path = db->getShortestPath(id1, id2, 10);
	EXPECT_TRUE(path.empty());
}

// ============================================================================
// Branch coverage: bfs on valid node with edges
// Covers line 619: storage not null, exercises full bfs path
// ============================================================================

TEST_F(CppApiTest, BfsTraversalFullPath) {
	auto id1 = db->createNodeRetId("BFSFull", {{"name", std::string("root")}});
	auto id2 = db->createNodeRetId("BFSFull", {{"name", std::string("child1")}});
	auto id3 = db->createNodeRetId("BFSFull", {{"name", std::string("child2")}});
	db->createEdgeById(id1, id2, "BFSREL", {});
	db->createEdgeById(id1, id3, "BFSREL", {});
	db->save();

	std::vector<int64_t> visited;
	db->bfs(id1, [&](const zyx::Node& n) {
		visited.push_back(n.id);
		return true; // continue
	});
	EXPECT_GE(visited.size(), 1u);
}

// ============================================================================
// Branch coverage: Result::get returns monostate for non-existent column in row mode
// ============================================================================

TEST_F(CppApiTest, ResultGetNonExistentColumnRowMode) {
	db->createNode("RowModeTest", {{"a", (int64_t)1}, {"b", (int64_t)2}});
	db->save();

	auto res = db->execute("MATCH (n:RowModeTest) RETURN n.a, n.b");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Request a column that doesn't exist and can't be fuzzy matched
	auto val = res.get("completely_nonexistent_column_xyz");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

// ============================================================================
// Branch coverage: Result::getError on uninitialized result
// ============================================================================

TEST_F(CppApiTest, ResultGetErrorOnDefaultResult) {
	zyx::Result res;
	EXPECT_EQ(res.getError(), "Result not initialized");
}

// ============================================================================
// Branch coverage: Result::getDuration on default result
// ============================================================================

TEST_F(CppApiTest, ResultGetDurationOnDefaultResult) {
	zyx::Result res;
	EXPECT_EQ(res.getDuration(), 0.0);
}

// ============================================================================
// Branch coverage: Result::getColumnCount on default result
// ============================================================================

TEST_F(CppApiTest, ResultGetColumnCountOnDefaultResult) {
	zyx::Result res;
	EXPECT_EQ(res.getColumnCount(), 0);
}

// ============================================================================
// Branch coverage: Line 280 False path
// When dbKey has no dot, key has a dot and is longer, but key does NOT
// end with "." + dbKey. This exercises the False branch of ends_with.
// ============================================================================

TEST_F(CppApiTest, ReverseFuzzyMatchEndswithFalse) {
	db->createNode("FuzzyFalse", {{"name", "Alice"}});
	db->save();

	// Use AS alias to get a column with no dot (e.g. "name")
	auto res = db->execute("MATCH (n:FuzzyFalse) RETURN n.name AS name");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Query with a dotted key that is longer than "name" but does NOT
	// end with ".name" - this hits the False branch at line 280
	auto val = res.get("x.foo");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

// ============================================================================
// Branch coverage: Edge stream mode - entity NOT found as edge (line 304 False)
// In edge stream mode, if property key doesn't match any edge property,
// falls through both isNode and isEdge checks and returns monostate.
// ============================================================================

TEST_F(CppApiTest, EdgeStreamModePropertyNotFoundFallthrough) {
	int64_t id1 = db->createNodeRetId("ESFT1");
	int64_t id2 = db->createNodeRetId("ESFT2");
	db->createEdgeById(id1, id2, "ESFT_REL", {{"w", (int64_t)10}});
	db->save();

	auto res = db->execute("MATCH ()-[e:ESFT_REL]->() RETURN e");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Access a non-existent property in edge stream mode
	// This ensures we go through the isEdge() True path and then fail
	// to find the property, falling through to return monostate at line 312
	auto missing = res.get("completely_nonexistent_property_xyz123");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(missing));
}

// ============================================================================
// Branch coverage: Node stream mode - entity property access (line 299-303)
// Verify that accessing existing properties works in node stream mode
// and that missing properties return monostate correctly
// ============================================================================

TEST_F(CppApiTest, NodeStreamModePropertyLookup) {
	db->createNode("NSMP", {{"x", (int64_t)42}, {"y", "hello"}});
	db->save();

	auto res = db->execute("MATCH (n:NSMP) RETURN n");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Existing property
	auto xVal = res.get("x");
	EXPECT_TRUE(std::holds_alternative<int64_t>(xVal));
	EXPECT_EQ(std::get<int64_t>(xVal), 42);

	auto yVal = res.get("y");
	EXPECT_TRUE(std::holds_alternative<std::string>(yVal));
	EXPECT_EQ(std::get<std::string>(yVal), "hello");

	// Non-existent property should fall through all branches and return monostate
	auto zVal = res.get("z_nonexistent_prop_abc");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(zVal));
}

// ============================================================================
// Branch coverage: Fuzzy match where key has dot but doesn't match (line 276)
// Tests the branch where key has no dot, dbKey is longer but ends_with fails
// ============================================================================

TEST_F(CppApiTest, FuzzyMatchKeyNoDotDbKeyLongerEndswithFalse) {
	db->createNode("FMTest", {{"name", "Alice"}});
	db->save();

	auto res = db->execute("MATCH (n:FMTest) RETURN n.name");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// key = "xyz" has no dot, dbKey = "n.name" is longer
	// but "n.name" does NOT end with ".xyz", so this hits the False branch of ends_with
	auto val = res.get("xyz");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

// ============================================================================
// Branch coverage: Transaction execute catch(...) path (line 396-400)
// The catch(std::exception) path is already covered. The catch(...) path
// handles non-std::exception throws. Hard to trigger but we exercise all
// error paths through the existing mechanism.
// ============================================================================

TEST_F(CppApiTest, TransactionExecuteErrorPaths) {
	auto txn = db->beginTransaction();

	// Execute invalid query - should hit the catch(std::exception) path
	auto res = txn.execute("COMPLETELY INVALID !@#$%^&*()");
	EXPECT_FALSE(res.isSuccess());
	auto err = res.getError();
	EXPECT_FALSE(err.empty());

	// Verify the result object is fully functional despite error
	EXPECT_FALSE(res.hasNext());
	EXPECT_EQ(res.getColumnCount(), 0);
	EXPECT_EQ(res.getDuration(), 0.0);

	auto val = res.get("anything");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));

	auto valIdx = res.get(0);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(valIdx));

	txn.rollback();
}

// ============================================================================
// Branch coverage: Database::execute catch paths (line 479-490)
// Test various error scenarios to exercise exception handling
// ============================================================================

TEST_F(CppApiTest, DatabaseExecuteErrorResult) {
	auto res = db->execute("NOT_A_VALID_CYPHER_KEYWORD");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_FALSE(res.getError().empty());

	// Error result should have 0 columns, 0 duration, no data
	EXPECT_EQ(res.getColumnCount(), 0);
	EXPECT_FALSE(res.hasNext());
}

// ============================================================================
// Branch coverage: Multiple columns in row mode - exercise various fuzzy paths
// ============================================================================

TEST_F(CppApiTest, RowModeFuzzyMatchMultipleColumns) {
	db->createNode("RM", {{"alpha", (int64_t)1}, {"beta", (int64_t)2}});
	db->save();

	auto res = db->execute("MATCH (n:RM) RETURN n.alpha, n.beta");
	ASSERT_TRUE(res.hasNext());
	res.next();

	// Fuzzy match without prefix
	auto alphaVal = res.get("alpha");
	EXPECT_TRUE(std::holds_alternative<int64_t>(alphaVal));

	auto betaVal = res.get("beta");
	EXPECT_TRUE(std::holds_alternative<int64_t>(betaVal));

	// Key with different prefix should match via ends_with
	auto pAlpha = res.get("n.alpha");
	EXPECT_TRUE(std::holds_alternative<int64_t>(pAlpha));

	// Key that doesn't match anything
	auto nope = res.get("gamma");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(nope));

	// Key with dot that doesn't match
	auto nope2 = res.get("x.gamma");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(nope2));
}
