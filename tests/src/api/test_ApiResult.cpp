/**
 * @file test_ApiResult.cpp
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
// Metadata & Index Access
// ============================================================================

TEST_F(CppApiTest, ResultGetByIndex) {
	db->createNode("Test", {{"val", (int64_t) 123}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n.val");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get(0);
	ASSERT_FALSE(std::holds_alternative<std::monostate>(val));
	ASSERT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 123);
}

TEST_F(CppApiTest, ResultGetByIndexOutOfBounds) {
	db->createNode("Test", {{"val", (int64_t) 123}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n.val");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto negVal = res.get(-1);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(negVal));

	auto oobVal = res.get(100);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(oobVal));
}

TEST_F(CppApiTest, ResultGetInvalidColumnName) {
	db->createNode("Test", {{"val", (int64_t) 123}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n.val");
	ASSERT_TRUE(res.hasNext());
	res.next();

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

	ASSERT_TRUE(res.hasNext());
	res.next();

	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, GetColumnNameOutOfBounds) {
	db->createNode("Test", {{"val", (int64_t) 123}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n.val");

	std::string nameNeg = res.getColumnName(-1);
	EXPECT_EQ(nameNeg, "");

	std::string nameOob = res.getColumnName(100);
	EXPECT_EQ(nameOob, "");
}

TEST_F(CppApiTest, GetColumnCountWithNoResults) {
	auto res = db->execute("MATCH (n:NonExistent) RETURN n");
	int count = res.getColumnCount();
	EXPECT_GT(count, 0);
}

TEST_F(CppApiTest, ResultFuzzyColumnNameMatch) {
	db->createNode("Person", {{"name", "Alice"}, {"age", (int64_t) 30}});
	db->save();

	auto res = db->execute("MATCH (n:Person) RETURN n.name, n.age");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto nameVal = res.get("name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "Alice");

	auto ageVal = res.get("age");
	EXPECT_TRUE(std::holds_alternative<int64_t>(ageVal));
	EXPECT_EQ(std::get<int64_t>(ageVal), 30);
}

// ============================================================================
// Result Move Semantics
// ============================================================================

TEST_F(CppApiTest, ResultMoveAssignment) {
	db->createNode("Test", {{"val", (int64_t) 123}});
	db->save();

	auto res1 = db->execute("MATCH (n:Test) RETURN n.val");
	ASSERT_TRUE(res1.hasNext());

	zyx::Result res2;
	res2 = std::move(res1);

	ASSERT_TRUE(res2.hasNext());
	res2.next();
	auto val = res2.get("n.val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 123);

	EXPECT_FALSE(res1.hasNext());
}

TEST_F(CppApiTest, ResultMoveConstructor) {
	db->createNode("Test", {{"val", (int64_t) 456}});
	db->save();

	auto res1 = db->execute("MATCH (n:Test) RETURN n.val");
	ASSERT_TRUE(res1.hasNext());

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

	auto val1 = res.get("n.val");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val1));

	res.next();
	auto val2 = res.get("n.val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val2));
	EXPECT_EQ(std::get<int64_t>(val2), 789);
}

TEST_F(CppApiTest, ResultDefaultConstructor) {
	zyx::Result res;

	EXPECT_FALSE(res.hasNext());
	EXPECT_EQ(res.getColumnCount(), 0);
	EXPECT_EQ(res.getColumnName(0), "");
	EXPECT_EQ(res.getDuration(), 0.0);
	EXPECT_FALSE(res.isSuccess());
	EXPECT_EQ(res.getError(), "Result not initialized");
}

TEST_F(CppApiTest, GetAfterCursorPastEnd) {
	db->createNode("Test", {{"val", (int64_t) 1}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n.val");

	res.next();
	res.next();

	auto val = res.get("n.val");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));

	auto val2 = res.get(0);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val2));
}

TEST_F(CppApiTest, ResultMultipleNextAfterEnd) {
	db->createNode("Test", {{"val", (int64_t) 1}});
	db->save();

	auto res = db->execute("MATCH (n:Test) RETURN n.val");

	res.next();
	res.next();
	res.next();
	res.next();

	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, ResultGetMixedTypes) {
	db->createNode("Mixed", {{"str", "text"}, {"num", (int64_t) 123}});
	db->save();

	auto res = db->execute("MATCH (n:Mixed) RETURN n.str, n.num, 456 AS const, true AS flag, null AS nul");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto strVal = res.get("n.str");
	EXPECT_TRUE(std::holds_alternative<std::string>(strVal));

	auto numVal = res.get("n.num");
	EXPECT_TRUE(std::holds_alternative<int64_t>(numVal));

	auto constVal = res.get("const");
	EXPECT_TRUE(std::holds_alternative<int64_t>(constVal));

	auto flagVal = res.get("flag");
	EXPECT_TRUE(std::holds_alternative<bool>(flagVal));

	auto nulVal = res.get("nul");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(nulVal));
}

TEST_F(CppApiTest, GetByIndexWithMultipleColumns) {
	db->createNode("MultiCol", {{"a", (int64_t) 1}, {"b", (int64_t) 2}, {"c", (int64_t) 3}});
	db->save();

	auto res = db->execute("MATCH (n:MultiCol) RETURN n.a, n.b, n.c");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val0 = res.get(0);
	EXPECT_TRUE(std::holds_alternative<int64_t>(val0));

	auto val1 = res.get(1);
	EXPECT_TRUE(std::holds_alternative<int64_t>(val1));

	auto val2 = res.get(2);
	EXPECT_TRUE(std::holds_alternative<int64_t>(val2));
}

TEST_F(CppApiTest, ResultWithCalculatedFields) {
	db->createNode("Data", {{"val1", (int64_t) 10}, {"val2", (int64_t) 5}});
	db->save();

	auto res = db->execute("MATCH (n:Data) RETURN n.val1, n.val2");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val1 = res.get("n.val1");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val1));

	auto val2 = res.get("n.val2");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val2));
}

TEST_F(CppApiTest, MultipleResultsIteration) {
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

TEST_F(CppApiTest, ResultGetDurationNonZero) {
	db->createNode("DurTest", {{"val", (int64_t) 1}});
	db->save();

	auto res = db->execute("MATCH (n:DurTest) RETURN n.val");
	EXPECT_TRUE(res.isSuccess());
	EXPECT_GE(res.getDuration(), 0.0);
}

TEST_F(CppApiTest, ResultNextOnDefaultConstructed) {
	zyx::Result res;

	res.next();
	EXPECT_FALSE(res.hasNext());

	auto val = res.get("anything");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));

	auto val2 = res.get(0);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val2));
}

// ============================================================================
// Node Stream Mode (mode_ == 1)
// ============================================================================

TEST_F(CppApiTest, NodeStreamMode_GetEntity) {
	db->createNode("StreamNode", {{"val", (int64_t)99}});
	db->save();

	auto res = db->execute("MATCH (n:StreamNode) RETURN n");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto nodeVal = res.get("n");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(nodeVal));

	auto emptyVal = res.get("");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(emptyVal));

	auto propVal = res.get("val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(propVal));
	EXPECT_EQ(std::get<int64_t>(propVal), 99);

	auto missingProp = res.get("nonexistent");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(missingProp));
}

// ============================================================================
// Edge Stream Mode (mode_ == 2)
// ============================================================================

TEST_F(CppApiTest, EdgeStreamMode_GetEntity) {
	db->createNode("ES1", {{"id", (int64_t)1}});
	db->createNode("ES2", {{"id", (int64_t)2}});
	(void)db->execute("MATCH (a:ES1), (b:ES2) CREATE (a)-[e:ESREL {w: 55}]->(b)");
	db->save();

	auto res = db->execute("MATCH ()-[e:ESREL]->() RETURN e");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto edgeVal = res.get("e");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(edgeVal));

	auto emptyVal = res.get("");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(emptyVal));

	auto propVal = res.get("w");
	EXPECT_TRUE(std::holds_alternative<int64_t>(propVal));
	EXPECT_EQ(std::get<int64_t>(propVal), 55);

	auto missingProp = res.get("nonexistent_edge_prop");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(missingProp));
}

// ============================================================================
// Map Property
// ============================================================================

TEST_F(CppApiTest, ResultWithMapProperty) {
	(void) db->execute("CREATE (n:MapTestNode {key: 'value', num: 42})");
	auto res = db->execute("MATCH (n:MapTestNode) RETURN n.key AS k, n.num AS num");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto k = res.get("k");
	auto num = res.get("num");
	EXPECT_EQ(std::get<std::string>(k), "value");
	EXPECT_EQ(std::get<int64_t>(num), 42);
}

TEST_F(CppApiTest, ResultGetColumnNameValidIndex) {
	db->createNode("ColTest", {{"a", (int64_t)1}, {"b", (int64_t)2}});
	db->save();

	auto res = db->execute("MATCH (n:ColTest) RETURN n.a, n.b");
	EXPECT_TRUE(res.isSuccess());

	int count = res.getColumnCount();
	EXPECT_EQ(count, 2);

	std::string col0 = res.getColumnName(0);
	std::string col1 = res.getColumnName(1);
	EXPECT_FALSE(col0.empty());
	EXPECT_FALSE(col1.empty());
}

TEST_F(CppApiTest, ResultGetByIndexNotStarted) {
	db->createNode("IdxTest", {{"val", (int64_t)1}});
	db->save();

	auto res = db->execute("MATCH (n:IdxTest) RETURN n.val");
	auto val = res.get(0);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, ResultImplNoColumns) {
	auto res = db->execute("MATCH (n:_NonExistent_) RETURN n");
	EXPECT_EQ(res.getColumnCount(), 1);
}

TEST_F(CppApiTest, ResultHasNextAfterComplete) {
	db->createNode("HN", {{"v", (int64_t)1}});
	db->save();

	auto res = db->execute("MATCH (n:HN) RETURN n.v");
	ASSERT_TRUE(res.hasNext());
	res.next();
	EXPECT_FALSE(res.hasNext());
	res.next();
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, GetColumnNameValidAndInvalid) {
	db->createNode("CN", {{"a", (int64_t)1}});
	db->save();

	auto res = db->execute("MATCH (n:CN) RETURN n.a");
	EXPECT_FALSE(res.getColumnName(0).empty());
	EXPECT_EQ(res.getColumnName(-1), "");
	EXPECT_EQ(res.getColumnName(999), "");
}

TEST_F(CppApiTest, NodeStreamModeKeyNotMatchingColName) {
	db->createNode("NSKey", {{"alpha", (int64_t)10}, {"beta", "hello"}});
	db->save();

	auto res = db->execute("MATCH (n:NSKey) RETURN n");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto alphaVal = res.get("alpha");
	EXPECT_TRUE(std::holds_alternative<int64_t>(alphaVal));
	EXPECT_EQ(std::get<int64_t>(alphaVal), 10);

	auto betaVal = res.get("beta");
	EXPECT_TRUE(std::holds_alternative<std::string>(betaVal));
	EXPECT_EQ(std::get<std::string>(betaVal), "hello");

	auto nodeVal = res.get("n");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(nodeVal));

	auto missing = res.get("nonexistent_xyz_123");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(missing));
}

TEST_F(CppApiTest, EdgeStreamModeKeyNotMatchingColNameOrEmpty) {
	int64_t id1 = db->createNodeRetId("ESK1");
	int64_t id2 = db->createNodeRetId("ESK2");
	db->createEdgeById(id1, id2, "ESKREL", {{"prop1", (int64_t)42}, {"prop2", "value"}});
	db->save();

	auto res = db->execute("MATCH ()-[e:ESKREL]->() RETURN e");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto p1 = res.get("prop1");
	EXPECT_TRUE(std::holds_alternative<int64_t>(p1));
	EXPECT_EQ(std::get<int64_t>(p1), 42);

	auto p2 = res.get("prop2");
	EXPECT_TRUE(std::holds_alternative<std::string>(p2));
	EXPECT_EQ(std::get<std::string>(p2), "value");

	auto edgeVal = res.get("e");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(edgeVal));

	auto emptyVal = res.get("");
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(emptyVal));

	auto missing = res.get("nonexistent_edge_prop_xyz");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(missing));
}

TEST_F(CppApiTest, FuzzyMatchKeyHasDotDbKeyNoDotEndswithTrue) {
	db->createNode("FME", {{"name", "test_val"}});
	db->save();

	auto res = db->execute("MATCH (n:FME) RETURN n.name AS name");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("any_prefix.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "test_val");
}

TEST_F(CppApiTest, FuzzyMatchKeyNoDotDbKeyHasDotEndswithFalse) {
	db->createNode("FMEF", {{"data", (int64_t)99}});
	db->save();

	auto res = db->execute("MATCH (n:FMEF) RETURN n.data");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("xyz");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, NodeStreamModeMultipleRows) {
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

		auto idVal = res.get("id");
		EXPECT_TRUE(std::holds_alternative<int64_t>(idVal));
		count++;
	}
	EXPECT_EQ(count, 3);
}

TEST_F(CppApiTest, EdgeStreamModeMultipleRows) {
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

		auto wVal = res.get("w");
		EXPECT_TRUE(std::holds_alternative<int64_t>(wVal));
		count++;
	}
	EXPECT_EQ(count, 2);
}

TEST_F(CppApiTest, PrepareMetadataModeDetermination) {
	db->createNode("ModeTest", {{"val", (int64_t)42}});
	db->save();

	auto res = db->execute("MATCH (n:ModeTest) RETURN n.val");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 42);
}

TEST_F(CppApiTest, ResultGetFuzzyMatchBothNoDot) {
	db->createNode("FuzzyTest", {{"val", (int64_t)42}});
	db->save();

	auto res = db->execute("MATCH (n:FuzzyTest) RETURN n.val AS val");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));

	auto missing = res.get("xyz");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(missing));
}

TEST_F(CppApiTest, ResultHasNextBoundaryCondition) {
	db->createNode("BoundTest", {{"v", (int64_t)1}});
	db->createNode("BoundTest", {{"v", (int64_t)2}});
	db->save();

	auto res = db->execute("MATCH (n:BoundTest) RETURN n.v ORDER BY n.v");

	EXPECT_TRUE(res.hasNext());

	res.next();
	EXPECT_TRUE(res.hasNext());

	res.next();
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, ResultImplicitColumnsFromRows) {
	db->createNode("ImplCol", {{"x", (int64_t)10}});
	db->save();

	auto res = db->execute("MATCH (n:ImplCol) RETURN n.x");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.x");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 10);
}

TEST_F(CppApiTest, ResultGetNonExistentColumnRowMode) {
	db->createNode("RowModeTest", {{"a", (int64_t)1}, {"b", (int64_t)2}});
	db->save();

	auto res = db->execute("MATCH (n:RowModeTest) RETURN n.a, n.b");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("completely_nonexistent_column_xyz");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, ReverseFuzzyMatchKeyHasDot) {
	db->createNode("Person", {{"name", "Alice"}});
	db->save();

	auto res = db->execute("MATCH (n:Person) RETURN n.name AS name");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("p.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "Alice");
}

TEST_F(CppApiTest, ReverseFuzzyMatchEndswithFalse) {
	db->createNode("FuzzyFalse", {{"name", "Alice"}});
	db->save();

	auto res = db->execute("MATCH (n:FuzzyFalse) RETURN n.name AS name");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("x.foo");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, EdgeStreamModePropertyNotFoundFallthrough) {
	int64_t id1 = db->createNodeRetId("ESFT1");
	int64_t id2 = db->createNodeRetId("ESFT2");
	db->createEdgeById(id1, id2, "ESFT_REL", {{"w", (int64_t)10}});
	db->save();

	auto res = db->execute("MATCH ()-[e:ESFT_REL]->() RETURN e");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto missing = res.get("completely_nonexistent_property_xyz123");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(missing));
}

TEST_F(CppApiTest, RowModeFuzzyMatchMultipleColumns) {
	db->createNode("RM", {{"alpha", (int64_t)1}, {"beta", (int64_t)2}});
	db->save();

	auto res = db->execute("MATCH (n:RM) RETURN n.alpha, n.beta");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto alphaVal = res.get("alpha");
	EXPECT_TRUE(std::holds_alternative<int64_t>(alphaVal));

	auto betaVal = res.get("beta");
	EXPECT_TRUE(std::holds_alternative<int64_t>(betaVal));

	auto pAlpha = res.get("n.alpha");
	EXPECT_TRUE(std::holds_alternative<int64_t>(pAlpha));

	auto nope = res.get("gamma");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(nope));

	auto nope2 = res.get("x.gamma");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(nope2));
}
