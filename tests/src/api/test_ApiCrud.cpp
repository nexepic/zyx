/**
 * @file test_ApiCrud.cpp
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

#include <unordered_map>
#include <variant>
#include <vector>

#include "ApiTestFixture.hpp"

// ============================================================================
// Basic CRUD & Property Access
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
// Advanced Creation (ID & Edges)
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
	db->createEdge("User", "uid", (int64_t) 1, "User", "uid", (int64_t) 2, "FOLLOWS", {{"since", (int64_t) 2025}});
	db->save();

	// 3. Verification
	auto res = db->execute("MATCH (a)-[e:FOLLOWS]->(b) WHERE a.uid=1 AND b.uid=2 RETURN e");

	ASSERT_TRUE(res.hasNext()) << "Edge FOLLOWS was not created.";
	res.next();

	auto edgeVal = res.get("e");
	ASSERT_FALSE(std::holds_alternative<std::monostate>(edgeVal));

	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(edgeVal))
			<< "Expected shared_ptr<Edge>, got something else (maybe Node?)";

	auto edgePtr = std::get<std::shared_ptr<zyx::Edge>>(edgeVal);
	EXPECT_EQ(edgePtr->type, "FOLLOWS");

	ASSERT_TRUE(edgePtr->properties.contains("since"));
	auto propVal = edgePtr->properties.at("since");
	ASSERT_TRUE(std::holds_alternative<int64_t>(propVal));
	EXPECT_EQ(std::get<int64_t>(propVal), 2025);

	ASSERT_FALSE(res.hasNext()) << "Duplicate edges found! createEdge might be running multiple times.";
}

TEST_F(CppApiTest, CreateNodesWithEmptyPropsList) {
	std::vector<std::unordered_map<std::string, zyx::Value>> emptyProps;
	db->createNodes("EmptyLabel", emptyProps);
	db->save();

	auto res = db->execute("MATCH (n:EmptyLabel) RETURN n");
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, CreateNodesWithMultipleNodes) {
	std::vector<std::unordered_map<std::string, zyx::Value>> propsList;
	propsList.push_back({{"id", (int64_t) 1}, {"name", "First"}});
	propsList.push_back({{"id", (int64_t) 2}, {"name", "Second"}});
	propsList.push_back({{"id", (int64_t) 3}, {"name", "Third"}});

	db->createNodes("BatchNode", propsList);
	db->save();

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

	db->createEdgeById(id1, id2, "CONNECTED", {{"weight", (int64_t) 100}, {"active", true}});
	db->save();

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

TEST_F(CppApiTest, CreateNodesWithSingleEmptyProps) {
	std::vector<std::unordered_map<std::string, zyx::Value>> propsList;
	propsList.push_back({});

	db->createNodes("SingleEmpty", propsList);
	db->save();

	auto res = db->execute("MATCH (n:SingleEmpty) RETURN n");
	EXPECT_TRUE(res.hasNext());
}

TEST_F(CppApiTest, CreateEdgeByIdNoProps) {
	int64_t id1 = db->createNodeRetId("Node1");
	int64_t id2 = db->createNodeRetId("Node2");

	db->createEdgeById(id1, id2, "LINK");
	db->save();

	auto res = db->execute("MATCH ()-[e:LINK]->() RETURN e");
	EXPECT_TRUE(res.hasNext());
}

TEST_F(CppApiTest, EdgeWithAllPropertyTypes) {
	int64_t id1 = db->createNodeRetId("A");
	int64_t id2 = db->createNodeRetId("B");

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
	EXPECT_EQ(edgePtr->type, "FULL");
	EXPECT_EQ(edgePtr->properties.size(), 4u);
}

TEST_F(CppApiTest, CreateNodeRetIdNoProps) {
	int64_t id = db->createNodeRetId("EmptyNode");
	EXPECT_GT(id, 0);
	db->save();

	auto res = db->execute("MATCH (n:EmptyNode) RETURN n");
	EXPECT_TRUE(res.hasNext());
}

TEST_F(CppApiTest, CreateNodeRetIdWithProps) {
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
// Delete Operations
// ============================================================================

TEST_F(CppApiTest, DeleteNodeViaCypher) {
	db->createNode("Deletable", {{"name", "ToDelete"}});
	db->createNode("Deletable", {{"name", "ToKeep"}});
	db->save();

	auto res = db->execute("MATCH (n:Deletable {name: 'ToDelete'}) DELETE n");
	EXPECT_TRUE(res.isSuccess());
	db->save();

	auto queryRes = db->execute("MATCH (n:Deletable) RETURN n.name");
	ASSERT_TRUE(queryRes.hasNext());
	queryRes.next();
	auto nameVal = queryRes.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "ToKeep");
	EXPECT_FALSE(queryRes.hasNext());
}

TEST_F(CppApiTest, DeleteEdgeViaCypher) {
	int64_t a = db->createNodeRetId("DA", {{"id", (int64_t)1}});
	int64_t b = db->createNodeRetId("DB", {{"id", (int64_t)2}});
	db->createEdgeById(a, b, "DEL_REL", {{"weight", (int64_t)10}});
	db->save();

	auto res = db->execute("MATCH ()-[e:DEL_REL]->() DELETE e");
	EXPECT_TRUE(res.isSuccess());
	db->save();

	auto queryRes = db->execute("MATCH ()-[e:DEL_REL]->() RETURN e");
	EXPECT_FALSE(queryRes.hasNext());

	auto nodeRes = db->execute("MATCH (n:DA) RETURN n");
	EXPECT_TRUE(nodeRes.hasNext());
}

TEST_F(CppApiTest, CreateAndDeleteMultipleEdges) {
	int64_t hub = db->createNodeRetId("Hub");
	int64_t spoke1 = db->createNodeRetId("Spoke", {{"id", (int64_t)1}});
	int64_t spoke2 = db->createNodeRetId("Spoke", {{"id", (int64_t)2}});
	int64_t spoke3 = db->createNodeRetId("Spoke", {{"id", (int64_t)3}});

	db->createEdgeById(hub, spoke1, "CONNECTS");
	db->createEdgeById(hub, spoke2, "CONNECTS");
	db->createEdgeById(hub, spoke3, "CONNECTS");
	db->save();

	auto delRes = db->execute("MATCH (h:Hub)-[e:CONNECTS]->(s:Spoke {id: 2}) DELETE e");
	EXPECT_TRUE(delRes.isSuccess());
	db->save();

	auto queryRes = db->execute("MATCH (h:Hub)-[e:CONNECTS]->() RETURN e");
	int edgeCount = 0;
	while (queryRes.hasNext()) {
		queryRes.next();
		edgeCount++;
	}
	EXPECT_EQ(edgeCount, 2);
}

// ============================================================================
// Query Execution
// ============================================================================

TEST_F(CppApiTest, MultipleNodesAndEdgesQuery) {
	int64_t a = db->createNodeRetId("A", {{"name", "NodeA"}});
	int64_t b = db->createNodeRetId("B", {{"name", "NodeB"}});
	int64_t c = db->createNodeRetId("C", {{"name", "NodeC"}});

	db->createEdgeById(a, b, "LINK", {{"weight", (int64_t) 1}});
	db->createEdgeById(b, c, "LINK", {{"weight", (int64_t) 2}});
	db->save();

	auto res = db->execute("MATCH (a:A)-[e1:LINK]->(b:B)-[e2:LINK]->(c:C) RETURN a, e1, b, e2, c");
	ASSERT_TRUE(res.hasNext());
	res.next();

	int colCount = res.getColumnCount();
	EXPECT_EQ(colCount, 5);
}

TEST_F(CppApiTest, QueryWithAlias) {
	db->createNode("Person", {{"name", "Frank"}});
	db->save();

	auto res = db->execute("MATCH (n:Person) RETURN n AS person");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto personVal = res.get("person");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(personVal));
}

TEST_F(CppApiTest, QueryWithWhereClause) {
	db->createNode("Product", {{"name", "Widget"}, {"price", (int64_t) 100}});
	db->createNode("Product", {{"name", "Gadget"}, {"price", (int64_t) 200}});
	db->save();

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

	auto res = db->execute("MATCH (n:Num) RETURN count(n)");
	ASSERT_TRUE(res.hasNext());
	res.next();

	EXPECT_TRUE(res.isSuccess());
}

TEST_F(CppApiTest, QueryWithSetProperty) {
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
	int64_t a = db->createNodeRetId("Chain", {{"pos", (int64_t)1}});
	int64_t b = db->createNodeRetId("Chain", {{"pos", (int64_t)2}});
	int64_t c = db->createNodeRetId("Chain", {{"pos", (int64_t)3}});
	int64_t d = db->createNodeRetId("Chain", {{"pos", (int64_t)4}});

	db->createEdgeById(a, b, "NEXT");
	db->createEdgeById(b, c, "NEXT");
	db->createEdgeById(c, d, "NEXT");
	db->save();

	auto res = db->execute("MATCH (a:Chain)-[:NEXT]->(b:Chain)-[:NEXT]->(c:Chain) RETURN a.pos, b.pos, c.pos");
	ASSERT_TRUE(res.isSuccess());
	int pathCount = 0;
	while (res.hasNext()) {
		res.next();
		pathCount++;
	}
	EXPECT_GE(pathCount, 1);
}

TEST_F(CppApiTest, QueryReturningEdgeValues) {
	int64_t a = db->createNodeRetId("QE_A");
	int64_t b = db->createNodeRetId("QE_B");
	db->createEdgeById(a, b, "QE_REL", {{"score", 3.14}});
	db->save();

	auto res = db->execute("MATCH (a)-[r:QE_REL]->(b) RETURN a, r, b");
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto aVal = res.get("a");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(aVal));

	auto rVal = res.get("r");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(rVal));

	auto bVal = res.get("b");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(bVal));
}

// ============================================================================
// Error Handling
// ============================================================================

TEST_F(CppApiTest, ExecuteInvalidQuery) {
	auto res = db->execute("INVALID CYPHER QUERY");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_FALSE(res.getError().empty());
}

TEST_F(CppApiTest, ExecuteWithRuntimeError) {
	auto res = db->execute("RETURN 1 / 0 AS result");
	if (!res.isSuccess()) {
		EXPECT_FALSE(res.getError().empty());
	}
}

TEST_F(CppApiTest, DatabaseExecuteErrorResult) {
	auto res = db->execute("NOT_A_VALID_CYPHER_KEYWORD");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_FALSE(res.getError().empty());

	EXPECT_EQ(res.getColumnCount(), 0);
	EXPECT_FALSE(res.hasNext());
}
