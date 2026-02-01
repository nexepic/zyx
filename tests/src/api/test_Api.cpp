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

#include "metrix/metrix.hpp"
#include "metrix/value.hpp"

namespace fs = std::filesystem;

class CppApiTest : public ::testing::Test {
protected:
	std::string dbPath;
	std::unique_ptr<metrix::Database> db;

	void SetUp() override {
		auto tempDir = fs::temp_directory_path();
		dbPath = (tempDir / ("api_test_" + std::to_string(std::rand()))).string();
		if (fs::exists(dbPath))
			fs::remove_all(dbPath);

		db = std::make_unique<metrix::Database>(dbPath);
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
	std::unordered_map<std::string, metrix::Value> props;
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
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<metrix::Node>>(nodeVal));

	auto nodePtr = std::get<std::shared_ptr<metrix::Node>>(nodeVal);
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
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<metrix::Edge>>(edgeVal))
			<< "Expected shared_ptr<Edge>, got something else (maybe Node?)";

	auto edgePtr = std::get<std::shared_ptr<metrix::Edge>>(edgeVal);
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
	db->bfs(root, [&](const metrix::Node &n) {
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
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<metrix::Node>>(nVal)) << "Result 'n' should be Node";

	// Check Edge (r)
	auto rVal = res.get("r");
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<metrix::Edge>>(rVal))
			<< "Result 'r' should be Edge - Check toPublicValue logic!";
}
