/**
 * @file test_ExpressionEvaluatorDataManager.cpp
 * @brief Tests for ExpressionEvaluator methods requiring DataManager
 *        (ExistsExpression, PatternComprehensionExpression, ShortestPathExpression,
 *         MapProjectionExpression ALL_PROPERTIES)
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/expressions/ShortestPathExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::expressions;
using namespace graph::query::execution;

class ExpressionEvaluatorDMTest : public ::testing::Test {
protected:
	fs::path testFilePath;
	std::shared_ptr<storage::FileStorage> fileStorage;
	std::shared_ptr<storage::DataManager> dm;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_eval_dm_" + boost::uuids::to_string(uuid) + ".db");
		fileStorage = std::make_shared<storage::FileStorage>(
			testFilePath.string(), 4096, storage::OpenMode::OPEN_CREATE_NEW_FILE);
		fileStorage->open();
		dm = fileStorage->getDataManager();
	}

	void TearDown() override {
		if (fileStorage) {
			fileStorage->close();
			fileStorage.reset();
		}
		dm.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}

	// Create a node with label and properties, return its ID
	int64_t createNode(const std::string& label,
					   const std::unordered_map<std::string, PropertyValue>& props = {}) {
		auto labelId = dm->getOrCreateTokenId(label);
		Node node(0, labelId);
		dm->addNode(node);
		if (!props.empty()) {
			dm->addNodeProperties(node.getId(), props);
		}
		return node.getId();
	}

	// Create an edge with type and properties, return its ID
	int64_t createEdge(int64_t source, int64_t target, const std::string& type,
					   const std::unordered_map<std::string, PropertyValue>& props = {}) {
		auto typeId = dm->getOrCreateTokenId(type);
		Edge edge(0, source, target, typeId);
		dm->addEdge(edge);
		if (!props.empty()) {
			dm->addEdgeProperties(edge.getId(), props);
		}
		return edge.getId();
	}
};

// ============================================================================
// ExistsExpression with DataManager
// ============================================================================

TEST_F(ExpressionEvaluatorDMTest, Exists_OutgoingRelationship_True) {
	auto nId = createNode("Person", {{"name", PropertyValue(std::string("Alice"))}});
	auto mId = createNode("Person", {{"name", PropertyValue(std::string("Bob"))}});
	createEdge(nId, mId, "KNOWS");

	Record record;
	Node n(nId, dm->getOrCreateTokenId("Person"));
	record.setNode("n", n);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ExistsExpression expr("(n)-[:KNOWS]->()", "n", "KNOWS", "", PatternDirection::PAT_OUTGOING);
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(ExpressionEvaluatorDMTest, Exists_OutgoingRelationship_False) {
	auto nId = createNode("Person", {{"name", PropertyValue(std::string("Alice"))}});

	Record record;
	Node n(nId, dm->getOrCreateTokenId("Person"));
	record.setNode("n", n);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ExistsExpression expr("(n)-[:KNOWS]->()", "n", "KNOWS", "", PatternDirection::PAT_OUTGOING);
	auto result = evaluator.evaluate(&expr);
	EXPECT_FALSE(std::get<bool>(result.getVariant()));
}

TEST_F(ExpressionEvaluatorDMTest, Exists_IncomingRelationship) {
	auto nId = createNode("Person");
	auto mId = createNode("Person");
	createEdge(mId, nId, "FOLLOWS");

	Record record;
	Node n(nId, dm->getOrCreateTokenId("Person"));
	record.setNode("n", n);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ExistsExpression expr("(n)<-[:FOLLOWS]-()", "n", "FOLLOWS", "", PatternDirection::PAT_INCOMING);
	auto result = evaluator.evaluate(&expr);
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(ExpressionEvaluatorDMTest, Exists_BothDirections) {
	auto nId = createNode("Person");
	auto mId = createNode("Person");
	createEdge(nId, mId, "KNOWS");

	Record record;
	Node n(nId, dm->getOrCreateTokenId("Person"));
	record.setNode("n", n);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ExistsExpression expr("(n)-[:KNOWS]-()", "n", "KNOWS", "", PatternDirection::PAT_BOTH);
	auto result = evaluator.evaluate(&expr);
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(ExpressionEvaluatorDMTest, Exists_NoRelType_AnyRelation) {
	auto nId = createNode("Person");
	auto mId = createNode("Person");
	createEdge(nId, mId, "ANYTHING");

	Record record;
	Node n(nId, dm->getOrCreateTokenId("Person"));
	record.setNode("n", n);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ExistsExpression expr("(n)-[]->()", "n", "", "", PatternDirection::PAT_OUTGOING);
	auto result = evaluator.evaluate(&expr);
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(ExpressionEvaluatorDMTest, Exists_TargetLabelFilter) {
	auto nId = createNode("Person");
	auto mId = createNode("Company");
	createEdge(nId, mId, "WORKS_AT");

	Record record;
	Node n(nId, dm->getOrCreateTokenId("Person"));
	record.setNode("n", n);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	// Target label matches
	ExistsExpression exprMatch("(n)-[:WORKS_AT]->(c:Company)", "n", "WORKS_AT", "Company", PatternDirection::PAT_OUTGOING);
	auto result1 = evaluator.evaluate(&exprMatch);
	EXPECT_TRUE(std::get<bool>(result1.getVariant()));

	// Target label doesn't match
	ExistsExpression exprNoMatch("(n)-[:WORKS_AT]->(c:School)", "n", "WORKS_AT", "School", PatternDirection::PAT_OUTGOING);
	auto result2 = evaluator.evaluate(&exprNoMatch);
	EXPECT_FALSE(std::get<bool>(result2.getVariant()));
}

TEST_F(ExpressionEvaluatorDMTest, Exists_SourceNodeNotInRecord_ReturnsFalse) {
	Record record;
	// Don't put any node in the record

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ExistsExpression expr("(n)-[:KNOWS]->()", "n", "KNOWS", "", PatternDirection::PAT_OUTGOING);
	auto result = evaluator.evaluate(&expr);
	EXPECT_FALSE(std::get<bool>(result.getVariant()));
}

// ============================================================================
// PatternComprehensionExpression with DataManager
// ============================================================================

TEST_F(ExpressionEvaluatorDMTest, PatternComprehension_CollectsTargetIds) {
	auto nId = createNode("Person", {{"name", PropertyValue(std::string("Alice"))}});
	auto m1Id = createNode("Person", {{"name", PropertyValue(std::string("Bob"))}});
	auto m2Id = createNode("Person", {{"name", PropertyValue(std::string("Charlie"))}});
	createEdge(nId, m1Id, "KNOWS");
	createEdge(nId, m2Id, "KNOWS");

	Record record;
	Node n(nId, dm->getOrCreateTokenId("Person"));
	record.setNode("n", n);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	// No map expression → collect target IDs
	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->(m)", "n", "m", "KNOWS", "", PatternDirection::PAT_OUTGOING, nullptr);
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	EXPECT_EQ(result.getList().size(), 2u);
}

TEST_F(ExpressionEvaluatorDMTest, PatternComprehension_WithMapExpression) {
	auto nId = createNode("Person", {{"name", PropertyValue(std::string("Alice"))}});
	auto m1Id = createNode("Person", {{"name", PropertyValue(std::string("Bob"))}});
	createEdge(nId, m1Id, "KNOWS");

	Record record;
	Node n(nId, dm->getOrCreateTokenId("Person"));
	record.setNode("n", n);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	// Map expression: use a literal to verify the map path works
	auto mapExpr = std::make_unique<LiteralExpression>(std::string("found"));
	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->(m)", "n", "m", "KNOWS", "", PatternDirection::PAT_OUTGOING, std::move(mapExpr));
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	ASSERT_EQ(result.getList().size(), 1u);
	EXPECT_EQ(std::get<std::string>(result.getList()[0].getVariant()), "found");
}

TEST_F(ExpressionEvaluatorDMTest, PatternComprehension_IncomingDirection) {
	auto nId = createNode("Person");
	auto mId = createNode("Person");
	createEdge(mId, nId, "FOLLOWS");

	Record record;
	Node n(nId, dm->getOrCreateTokenId("Person"));
	record.setNode("n", n);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	PatternComprehensionExpression expr(
		"(n)<-[:FOLLOWS]-(m)", "n", "m", "FOLLOWS", "", PatternDirection::PAT_INCOMING, nullptr);
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getList().size(), 1u);
}

TEST_F(ExpressionEvaluatorDMTest, PatternComprehension_BothDirection) {
	auto nId = createNode("Person");
	auto mId = createNode("Person");
	createEdge(nId, mId, "KNOWS");

	Record record;
	Node n(nId, dm->getOrCreateTokenId("Person"));
	record.setNode("n", n);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]-(m)", "n", "m", "KNOWS", "", PatternDirection::PAT_BOTH, nullptr);
	auto result = evaluator.evaluate(&expr);
	EXPECT_FALSE(result.getList().empty());
}

TEST_F(ExpressionEvaluatorDMTest, PatternComprehension_RelTypeFilter) {
	auto nId = createNode("Person");
	auto m1Id = createNode("Person");
	auto m2Id = createNode("Person");
	createEdge(nId, m1Id, "KNOWS");
	createEdge(nId, m2Id, "HATES");

	Record record;
	Node n(nId, dm->getOrCreateTokenId("Person"));
	record.setNode("n", n);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->(m)", "n", "m", "KNOWS", "", PatternDirection::PAT_OUTGOING, nullptr);
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getList().size(), 1u);
}

TEST_F(ExpressionEvaluatorDMTest, PatternComprehension_TargetLabelFilter) {
	auto nId = createNode("Person");
	auto m1Id = createNode("Person");
	auto m2Id = createNode("Company");
	createEdge(nId, m1Id, "KNOWS");
	createEdge(nId, m2Id, "KNOWS");

	Record record;
	Node n(nId, dm->getOrCreateTokenId("Person"));
	record.setNode("n", n);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->(m:Person)", "n", "m", "KNOWS", "Person", PatternDirection::PAT_OUTGOING, nullptr);
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getList().size(), 1u);
}

TEST_F(ExpressionEvaluatorDMTest, PatternComprehension_SourceNotInRecord_EmptyList) {
	Record record;

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->(m)", "n", "m", "KNOWS", "", PatternDirection::PAT_OUTGOING, nullptr);
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	EXPECT_TRUE(result.getList().empty());
}

TEST_F(ExpressionEvaluatorDMTest, PatternComprehension_NoTargetVar) {
	auto nId = createNode("Person");
	auto mId = createNode("Person");
	createEdge(nId, mId, "KNOWS");

	Record record;
	Node n(nId, dm->getOrCreateTokenId("Person"));
	record.setNode("n", n);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	// Empty target var - should still work
	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->()", "n", "", "KNOWS", "", PatternDirection::PAT_OUTGOING, nullptr);
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
}

// ============================================================================
// ShortestPathExpression with DataManager
// ============================================================================

TEST_F(ExpressionEvaluatorDMTest, ShortestPath_DirectConnection) {
	auto aId = createNode("Person", {{"name", PropertyValue(std::string("Alice"))}});
	auto bId = createNode("Person", {{"name", PropertyValue(std::string("Bob"))}});
	createEdge(aId, bId, "KNOWS");

	Record record;
	Node a(aId, dm->getOrCreateTokenId("Person"));
	Node b(bId, dm->getOrCreateTokenId("Person"));
	record.setNode("a", a);
	record.setNode("b", b);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ShortestPathExpression expr("a", "b", "", PatternDirection::PAT_OUTGOING, 1, -1, false);
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	EXPECT_FALSE(result.getList().empty());
}

TEST_F(ExpressionEvaluatorDMTest, ShortestPath_NoPath) {
	auto aId = createNode("Person");
	auto bId = createNode("Person");
	// No edge between them

	Record record;
	Node a(aId, dm->getOrCreateTokenId("Person"));
	Node b(bId, dm->getOrCreateTokenId("Person"));
	record.setNode("a", a);
	record.setNode("b", b);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ShortestPathExpression expr("a", "b", "", PatternDirection::PAT_OUTGOING, 1, -1, false);
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorDMTest, ShortestPath_TwoHops) {
	auto aId = createNode("Person");
	auto bId = createNode("Person");
	auto cId = createNode("Person");
	createEdge(aId, bId, "KNOWS");
	createEdge(bId, cId, "KNOWS");

	Record record;
	Node a(aId, dm->getOrCreateTokenId("Person"));
	Node c(cId, dm->getOrCreateTokenId("Person"));
	record.setNode("a", a);
	record.setNode("b", c);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ShortestPathExpression expr("a", "b", "", PatternDirection::PAT_OUTGOING, 1, -1, false);
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
}

TEST_F(ExpressionEvaluatorDMTest, ShortestPath_AllShortestPaths) {
	auto aId = createNode("Person");
	auto bId = createNode("Person");
	createEdge(aId, bId, "KNOWS");

	Record record;
	Node a(aId, dm->getOrCreateTokenId("Person"));
	Node b(bId, dm->getOrCreateTokenId("Person"));
	record.setNode("a", a);
	record.setNode("b", b);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ShortestPathExpression expr("a", "b", "", PatternDirection::PAT_OUTGOING, 1, -1, true);
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
}

TEST_F(ExpressionEvaluatorDMTest, ShortestPath_WithRelTypeFilter) {
	auto aId = createNode("Person");
	auto bId = createNode("Person");
	createEdge(aId, bId, "KNOWS");
	createEdge(aId, bId, "LIKES");

	Record record;
	Node a(aId, dm->getOrCreateTokenId("Person"));
	Node b(bId, dm->getOrCreateTokenId("Person"));
	record.setNode("a", a);
	record.setNode("b", b);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ShortestPathExpression expr("a", "b", "KNOWS", PatternDirection::PAT_OUTGOING, 1, -1, false);
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
}

TEST_F(ExpressionEvaluatorDMTest, ShortestPath_BothDirection) {
	auto aId = createNode("Person");
	auto bId = createNode("Person");
	createEdge(bId, aId, "KNOWS"); // Edge goes from b to a

	Record record;
	Node a(aId, dm->getOrCreateTokenId("Person"));
	Node b(bId, dm->getOrCreateTokenId("Person"));
	record.setNode("a", a);
	record.setNode("b", b);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ShortestPathExpression expr("a", "b", "", PatternDirection::PAT_BOTH, 1, -1, false);
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
}

TEST_F(ExpressionEvaluatorDMTest, ShortestPath_IncomingDirection) {
	auto aId = createNode("Person");
	auto bId = createNode("Person");
	createEdge(bId, aId, "KNOWS");

	Record record;
	Node a(aId, dm->getOrCreateTokenId("Person"));
	Node b(bId, dm->getOrCreateTokenId("Person"));
	record.setNode("a", a);
	record.setNode("b", b);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ShortestPathExpression expr("a", "b", "", PatternDirection::PAT_INCOMING, 1, -1, false);
	auto result = evaluator.evaluate(&expr);
	// The BFS uses incoming edges, so should find path from a to b via incoming edge
	// (incoming from a's perspective = edges where a is the target)
	EXPECT_EQ(result.getType(), PropertyType::LIST);
}

TEST_F(ExpressionEvaluatorDMTest, ShortestPath_MissingStartNode) {
	auto bId = createNode("Person");

	Record record;
	// Don't set node "a" in record
	Node b(bId, dm->getOrCreateTokenId("Person"));
	record.setNode("b", b);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ShortestPathExpression expr("a", "b", "", PatternDirection::PAT_OUTGOING, 1, -1, false);
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorDMTest, ShortestPath_MissingEndNode) {
	auto aId = createNode("Person");

	Record record;
	Node a(aId, dm->getOrCreateTokenId("Person"));
	record.setNode("a", a);
	// Don't set node "b" in record

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ShortestPathExpression expr("a", "b", "", PatternDirection::PAT_OUTGOING, 1, -1, false);
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// MapProjectionExpression ALL_PROPERTIES with DataManager
// ============================================================================

TEST_F(ExpressionEvaluatorDMTest, MapProjection_AllProperties) {
	auto nId = createNode("Person", {
		{"name", PropertyValue(std::string("Alice"))},
		{"age", PropertyValue(int64_t(30))}
	});

	Record record;
	Node n(nId, dm->getOrCreateTokenId("Person"));
	std::unordered_map<std::string, PropertyValue> props;
	props["name"] = PropertyValue(std::string("Alice"));
	props["age"] = PropertyValue(int64_t(30));
	n.setProperties(props);
	record.setNode("n", n);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_ALL_PROPERTIES, "");
	MapProjectionExpression expr("n", std::move(items));

	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getType(), PropertyType::MAP);
	auto map = result.getMap();
	EXPECT_TRUE(map.count("name"));
	EXPECT_TRUE(map.count("age"));
}

TEST_F(ExpressionEvaluatorDMTest, MapProjection_AllProperties_NoDataManager) {
	Record record;
	Node n(1, 100);
	record.setNode("n", n);

	EvaluationContext ctx(record); // No DataManager
	ExpressionEvaluator evaluator(ctx);

	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_ALL_PROPERTIES, "");
	MapProjectionExpression expr("n", std::move(items));

	// Should not crash, just return empty map
	auto result = evaluator.evaluate(&expr);
	EXPECT_EQ(result.getType(), PropertyType::MAP);
}

// ============================================================================
// Exists with incoming edge and target label
// ============================================================================

TEST_F(ExpressionEvaluatorDMTest, Exists_IncomingWithTargetLabel) {
	auto nId = createNode("Person");
	auto mId = createNode("Company");
	createEdge(mId, nId, "EMPLOYS");

	Record record;
	Node n(nId, dm->getOrCreateTokenId("Person"));
	record.setNode("n", n);

	EvaluationContext ctx(record, dm.get());
	ExpressionEvaluator evaluator(ctx);

	ExistsExpression expr("(n)<-[:EMPLOYS]-(c:Company)", "n", "EMPLOYS", "Company", PatternDirection::PAT_INCOMING);
	auto result = evaluator.evaluate(&expr);
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}
