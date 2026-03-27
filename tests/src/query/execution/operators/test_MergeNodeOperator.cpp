/**
 * @file test_MergeNodeOperator.cpp
 * @date 2026
 *
 * @copyright Copyright (c) 2026
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
#include <memory>

#include "graph/core/Database.hpp"
#include "graph/query/execution/operators/MergeNodeOperator.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/core/Node.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

// Mock Operator for testing
class MergeNodeMockOperator : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t current_index = 0;

	explicit MergeNodeMockOperator(std::vector<RecordBatch> data = {}) : batches(std::move(data)) {}

	void open() override { current_index = 0; }
	std::optional<RecordBatch> next() override {
		if (current_index >= batches.size()) return std::nullopt;
		return batches[current_index++];
	}
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"n"}; }
	[[nodiscard]] std::string toString() const override { return "MergeNodeMock"; }
};

class MergeNodeOperatorTest : public ::testing::Test {
protected:
	fs::path dbPath;
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() / ("test_merge_node_" + to_string(uuid) + ".db");
		db = std::make_unique<Database>(dbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		im = db->getQueryEngine()->getIndexManager();
	}

	void TearDown() override {
		if (db) db->close();
		if (fs::exists(dbPath)) fs::remove(dbPath);
	}
};

TEST_F(MergeNodeOperatorTest, CreateNewNode_NoExisting) {
	// MERGE (n:Person {name: "Alice"}) - no existing node
	std::unordered_map<std::string, PropertyValue> matchProps;
	matchProps["name"] = PropertyValue(std::string("Alice"));

	MergeNodeOperator op(dm, im, "n", {"Person"}, matchProps, {}, {});

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	// Verify the node was set in the record
	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);
	EXPECT_NE(node->getId(), 0);

	// Second call should return nullopt (executed_)
	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value());

	op.close();
}

TEST_F(MergeNodeOperatorTest, MatchExistingNode) {
	// Pre-create a node
	int64_t labelId = dm->getOrCreateLabelId("Person");
	Node existingNode(0, labelId);
	dm->addNode(existingNode);
	std::unordered_map<std::string, PropertyValue> props;
	props["name"] = PropertyValue(std::string("Bob"));
	dm->addNodeProperties(existingNode.getId(), props);

	// MERGE (n:Person {name: "Bob"}) - should match existing
	std::unordered_map<std::string, PropertyValue> matchProps;
	matchProps["name"] = PropertyValue(std::string("Bob"));

	MergeNodeOperator op(dm, im, "n", {"Person"}, matchProps, {}, {});

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);
	EXPECT_EQ(node->getId(), existingNode.getId());

	op.close();
}

TEST_F(MergeNodeOperatorTest, DeletedNode_StillVisible) {
	// deleteNode marks for deletion but MergeNodeOperator still sees it via scan
	int64_t labelId = dm->getOrCreateLabelId("Skip");
	Node n1(0, labelId);
	dm->addNode(n1);
	dm->deleteNode(n1);

	// Merge finds the node (deletion doesn't prevent scan matching)
	MergeNodeOperator op(dm, im, "n", {"Skip"}, {}, {}, {});

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);

	op.close();
}

TEST_F(MergeNodeOperatorTest, LabelMismatch_CreatesNew) {
	// Create a node with label "Dog"
	int64_t dogLabelId = dm->getOrCreateLabelId("Dog");
	Node dogNode(0, dogLabelId);
	dm->addNode(dogNode);

	// Merge with label "Cat" - should not match
	MergeNodeOperator op(dm, im, "n", {"Cat"}, {}, {}, {});

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);
	EXPECT_NE(node->getId(), dogNode.getId());

	op.close();
}

TEST_F(MergeNodeOperatorTest, PropertyMismatch_CreatesNew) {
	// Create a node with name="X"
	int64_t labelId = dm->getOrCreateLabelId("PMism");
	Node n1(0, labelId);
	dm->addNode(n1);
	std::unordered_map<std::string, PropertyValue> props;
	props["name"] = PropertyValue(std::string("X"));
	dm->addNodeProperties(n1.getId(), props);

	// Merge with name="Y" - should not match
	std::unordered_map<std::string, PropertyValue> matchProps;
	matchProps["name"] = PropertyValue(std::string("Y"));

	MergeNodeOperator op(dm, im, "n", {"PMism"}, matchProps, {}, {});

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);
	EXPECT_NE(node->getId(), n1.getId());

	op.close();
}

TEST_F(MergeNodeOperatorTest, WithChild_Pipeline) {
	// MATCH (m) MERGE (n:New)
	auto child = std::make_unique<MergeNodeMockOperator>();
	Record r;
	r.setValue("m", PropertyValue(int64_t(1)));
	child->batches.push_back({r});

	MergeNodeOperator op(dm, im, "n", {"New"}, {}, {}, {});
	op.setChild(std::move(child));

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);

	// Second call - child exhausted
	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value());

	op.close();
}

TEST_F(MergeNodeOperatorTest, NodeAlreadyInRecord_Skip) {
	// If the variable is already set in the record, processMerge returns early
	auto child = std::make_unique<MergeNodeMockOperator>();
	Record r;
	Node existingNode(99, 0);
	r.setNode("n", existingNode);
	child->batches.push_back({r});

	MergeNodeOperator op(dm, im, "n", {"Label"}, {}, {}, {});
	op.setChild(std::move(child));

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	// The node should be the same one we put in
	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);
	EXPECT_EQ(node->getId(), 99);

	op.close();
}

TEST_F(MergeNodeOperatorTest, OnCreate_AppliesUpdates) {
	// MERGE (n:OCNode) ON CREATE SET n.status = 'new'
	auto statusExpr = std::make_shared<graph::query::expressions::LiteralExpression>(std::string("new"));
	std::vector<SetItem> onCreateItems;
	onCreateItems.emplace_back(SetActionType::PROPERTY, "n", "status", statusExpr);

	MergeNodeOperator op(dm, im, "n", {"OCNode"}, {}, onCreateItems, {});

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());

	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);

	// Check that the property was set
	auto props = node->getProperties();
	auto it = props.find("status");
	ASSERT_NE(it, props.end());
	EXPECT_EQ(std::get<std::string>(it->second.getVariant()), "new");

	op.close();
}

TEST_F(MergeNodeOperatorTest, OnMatch_AppliesUpdates) {
	// Pre-create a node
	int64_t labelId = dm->getOrCreateLabelId("OMNode");
	Node n1(0, labelId);
	dm->addNode(n1);
	std::unordered_map<std::string, PropertyValue> props;
	props["name"] = PropertyValue(std::string("existing"));
	dm->addNodeProperties(n1.getId(), props);

	// MERGE (n:OMNode {name: "existing"}) ON MATCH SET n.updated = true
	auto updatedExpr = std::make_shared<graph::query::expressions::LiteralExpression>(true);
	std::vector<SetItem> onMatchItems;
	onMatchItems.emplace_back(SetActionType::PROPERTY, "n", "updated", updatedExpr);

	std::unordered_map<std::string, PropertyValue> matchProps;
	matchProps["name"] = PropertyValue(std::string("existing"));

	MergeNodeOperator op(dm, im, "n", {"OMNode"}, matchProps, {}, onMatchItems);

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());

	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);
	EXPECT_EQ(node->getId(), n1.getId());

	// Check that "updated" was set
	auto updatedProps = node->getProperties();
	auto it = updatedProps.find("updated");
	ASSERT_NE(it, updatedProps.end());
	EXPECT_EQ(std::get<bool>(it->second.getVariant()), true);

	op.close();
}

TEST_F(MergeNodeOperatorTest, ApplyUpdates_DifferentVariable_Ignored) {
	// ON MATCH SET m.val = 1 (different variable 'm' vs 'n')
	auto expr = std::make_shared<graph::query::expressions::LiteralExpression>(int64_t(1));
	std::vector<SetItem> onMatchItems;
	onMatchItems.emplace_back(SetActionType::PROPERTY, "m", "val", expr);

	int64_t labelId = dm->getOrCreateLabelId("DiffVar");
	Node n1(0, labelId);
	dm->addNode(n1);

	MergeNodeOperator op(dm, im, "n", {"DiffVar"}, {}, {}, onMatchItems);

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());

	// "m" update should have been skipped since variable is "n"
	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);

	op.close();
}

TEST_F(MergeNodeOperatorTest, ApplyUpdates_NonPropertyType_Ignored) {
	// ON MATCH SET n:NewLabel (LABEL type, not PROPERTY) - should be skipped
	std::vector<SetItem> onMatchItems;
	onMatchItems.emplace_back(SetActionType::LABEL, "n", "NewLabel", nullptr);

	int64_t labelId = dm->getOrCreateLabelId("LblTest");
	Node n1(0, labelId);
	dm->addNode(n1);

	MergeNodeOperator op(dm, im, "n", {"LblTest"}, {}, {}, onMatchItems);

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	// Should not crash, just skip the label SET item
	op.close();
}

TEST_F(MergeNodeOperatorTest, ApplyUpdates_NullExpression_Ignored) {
	// ON MATCH SET n.val = <null expression>
	std::vector<SetItem> onMatchItems;
	onMatchItems.emplace_back(SetActionType::PROPERTY, "n", "val", nullptr);

	int64_t labelId = dm->getOrCreateLabelId("NullExpr");
	Node n1(0, labelId);
	dm->addNode(n1);

	MergeNodeOperator op(dm, im, "n", {"NullExpr"}, {}, {}, onMatchItems);

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	// Should not crash when expression is null
	op.close();
}

TEST_F(MergeNodeOperatorTest, EmptyLabel_FullScan) {
	// MERGE (n {name: "X"}) - no label, does full scan
	int64_t labelId = dm->getOrCreateLabelId("AnyLabel");
	Node n1(0, labelId);
	dm->addNode(n1);
	std::unordered_map<std::string, PropertyValue> props;
	props["name"] = PropertyValue(std::string("X"));
	dm->addNodeProperties(n1.getId(), props);

	std::unordered_map<std::string, PropertyValue> matchProps;
	matchProps["name"] = PropertyValue(std::string("X"));

	MergeNodeOperator op(dm, im, "n", std::vector<std::string>{}, matchProps, {}, {});

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());

	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);
	// Should find the existing node since label check is skipped when targetLabelId_ == 0
	EXPECT_EQ(node->getId(), n1.getId());

	op.close();
}

TEST_F(MergeNodeOperatorTest, ToString) {
	MergeNodeOperator op(dm, im, "n", {"Person"}, {}, {}, {});
	EXPECT_EQ(op.toString(), "MergeNode(n:Person)");
}

TEST_F(MergeNodeOperatorTest, EmptyMatchProps_CreateWithNoProperties) {
	// MERGE (n:EmptyProps) - no match properties
	MergeNodeOperator op(dm, im, "n", {"EmptyProps"}, {}, {}, {});

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());

	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);

	op.close();
}

TEST_F(MergeNodeOperatorTest, PropertyKeyExists_ValueMismatch) {
	// Covers the it->second != v branch (line 119) where key exists but value differs
	int64_t labelId = dm->getOrCreateLabelId("ValMis");
	Node n1(0, labelId);
	dm->addNode(n1);
	std::unordered_map<std::string, PropertyValue> props;
	props["name"] = PropertyValue(std::string("Alice"));
	props["age"] = PropertyValue(int64_t(25));
	dm->addNodeProperties(n1.getId(), props);

	// Match with same key "name" but different value "Bob"
	// The key will be found (it != props.end()) but value won't match (it->second != v)
	std::unordered_map<std::string, PropertyValue> matchProps;
	matchProps["name"] = PropertyValue(std::string("Bob"));

	MergeNodeOperator op(dm, im, "n", {"ValMis"}, matchProps, {}, {});

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());

	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);
	// Should create a new node since value mismatched
	EXPECT_NE(node->getId(), n1.getId());

	op.close();
}

TEST_F(MergeNodeOperatorTest, OnCreate_MultipleProperties) {
	// Tests ON CREATE SET with multiple property updates
	auto statusExpr = std::make_shared<graph::query::expressions::LiteralExpression>(std::string("created"));
	auto scoreExpr = std::make_shared<graph::query::expressions::LiteralExpression>(int64_t(100));
	std::vector<SetItem> onCreateItems;
	onCreateItems.emplace_back(SetActionType::PROPERTY, "n", "status", statusExpr);
	onCreateItems.emplace_back(SetActionType::PROPERTY, "n", "score", scoreExpr);

	MergeNodeOperator op(dm, im, "n", {"MultiCreate"}, {}, onCreateItems, {});

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());

	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);

	auto nodeProps = node->getProperties();
	EXPECT_EQ(std::get<std::string>(nodeProps["status"].getVariant()), "created");
	EXPECT_EQ(std::get<int64_t>(nodeProps["score"].getVariant()), 100);

	op.close();
}

TEST_F(MergeNodeOperatorTest, OnMatch_MultipleProperties) {
	// Tests ON MATCH SET with multiple property updates
	int64_t labelId = dm->getOrCreateLabelId("MultiMatch");
	Node n1(0, labelId);
	dm->addNode(n1);

	auto statusExpr = std::make_shared<graph::query::expressions::LiteralExpression>(std::string("matched"));
	auto scoreExpr = std::make_shared<graph::query::expressions::LiteralExpression>(int64_t(200));
	std::vector<SetItem> onMatchItems;
	onMatchItems.emplace_back(SetActionType::PROPERTY, "n", "status", statusExpr);
	onMatchItems.emplace_back(SetActionType::PROPERTY, "n", "score", scoreExpr);

	MergeNodeOperator op(dm, im, "n", {"MultiMatch"}, {}, {}, onMatchItems);

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());

	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);
	EXPECT_EQ(node->getId(), n1.getId());

	auto nodeProps = node->getProperties();
	EXPECT_EQ(std::get<std::string>(nodeProps["status"].getVariant()), "matched");
	EXPECT_EQ(std::get<int64_t>(nodeProps["score"].getVariant()), 200);

	op.close();
}

// ============================================================================
// Additional Branch Coverage Tests
// ============================================================================

TEST_F(MergeNodeOperatorTest, WithChild_MultipleBatches) {
	// Tests child pipeline with multiple records in a batch
	// Covers the for loop over batchOpt at line 49
	auto child = std::make_unique<MergeNodeMockOperator>();
	RecordBatch batch;
	Record r1;
	r1.setValue("m", PropertyValue(int64_t(1)));
	Record r2;
	r2.setValue("m", PropertyValue(int64_t(2)));
	batch.push_back(r1);
	batch.push_back(r2);
	child->batches.push_back(batch);

	MergeNodeOperator op(dm, im, "n", {"BatchTest"}, {}, {}, {});
	op.setChild(std::move(child));

	op.open();
	auto result = op.next();
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->size(), 2UL);

	// Both records should have node "n" set
	for (const auto& rec : *result) {
		auto node = rec.getNode("n");
		ASSERT_NE(node, std::nullopt);
	}

	// Second call - child exhausted
	auto result2 = op.next();
	EXPECT_FALSE(result2.has_value());

	op.close();
}

TEST_F(MergeNodeOperatorTest, CreateNode_EmptyMatchProps) {
	// Tests CREATE path when matchProps_ is empty (line 144 false branch)
	// No properties should be added to the new node
	MergeNodeOperator op(dm, im, "n", {"EmptyPropCreate"}, {}, {}, {});

	op.open();
	auto result = op.next();
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->size(), 1UL);

	auto node = (*result)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);
	EXPECT_NE(node->getId(), 0);

	// Node should have no properties (empty matchProps)
	auto nodeProps = node->getProperties();
	EXPECT_TRUE(nodeProps.empty());

	op.close();
}

TEST_F(MergeNodeOperatorTest, MultipleMatchProps_PropertyMismatchOnSecondKey) {
	// Tests property matching where first key matches but second doesn't
	// Covers the inner loop break at line 120-122
	int64_t labelId = dm->getOrCreateLabelId("MultiKey");
	Node n1(0, labelId);
	dm->addNode(n1);
	std::unordered_map<std::string, PropertyValue> props;
	props["name"] = PropertyValue(std::string("Alice"));
	props["age"] = PropertyValue(int64_t(30));
	dm->addNodeProperties(n1.getId(), props);

	// Match with name="Alice" but age=25 - first key matches, second doesn't
	std::unordered_map<std::string, PropertyValue> matchProps;
	matchProps["name"] = PropertyValue(std::string("Alice"));
	matchProps["age"] = PropertyValue(int64_t(25));

	MergeNodeOperator op(dm, im, "n", {"MultiKey"}, matchProps, {}, {});

	op.open();
	auto result = op.next();
	ASSERT_TRUE(result.has_value());

	auto node = (*result)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);
	// Should create a new node since age doesn't match
	EXPECT_NE(node->getId(), n1.getId());

	op.close();
}

TEST_F(MergeNodeOperatorTest, OnCreate_EmptyItems) {
	// Tests ON CREATE with empty items list
	// Covers the items.empty() early return in applyUpdates (line 156)
	std::unordered_map<std::string, PropertyValue> matchProps;
	matchProps["name"] = PropertyValue(std::string("EmptyCreate"));

	std::vector<SetItem> emptyItems;

	MergeNodeOperator op(dm, im, "n", {"EmptyOC"}, matchProps, emptyItems, {});

	op.open();
	auto result = op.next();
	ASSERT_TRUE(result.has_value());

	auto node = (*result)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);

	op.close();
}

TEST_F(MergeNodeOperatorTest, OnMatch_EmptyItems) {
	// Tests ON MATCH with empty items list
	// Covers the items.empty() early return in applyUpdates (line 156)
	int64_t labelId = dm->getOrCreateLabelId("EmptyOM");
	Node n1(0, labelId);
	dm->addNode(n1);

	std::vector<SetItem> emptyItems;

	MergeNodeOperator op(dm, im, "n", {"EmptyOM"}, {}, {}, emptyItems);

	op.open();
	auto result = op.next();
	ASSERT_TRUE(result.has_value());

	auto node = (*result)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);
	EXPECT_EQ(node->getId(), n1.getId());

	op.close();
}

TEST_F(MergeNodeOperatorTest, WithChild_EmptyBatch) {
	// Tests child pipeline that returns an empty batch
	// Covers the case where child returns a batch but it's empty
	auto child = std::make_unique<MergeNodeMockOperator>();
	RecordBatch emptyBatch;
	child->batches.push_back(emptyBatch);

	MergeNodeOperator op(dm, im, "n", {"EmptyBatch"}, {}, {}, {});
	op.setChild(std::move(child));

	op.open();
	auto result = op.next();
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->size(), 0UL);

	op.close();
}

TEST_F(MergeNodeOperatorTest, WithLabelIndex_MatchExistingNode) {
	// Covers line 93: the label index path (hasLabelIndex returns true)
	im->createIndex("", "node", "LblIdx", "");

	int64_t labelId = dm->getOrCreateLabelId("LblIdx");
	Node n1(0, labelId);
	dm->addNode(n1);

	MergeNodeOperator op(dm, im, "n", {"LblIdx"}, {}, {}, {});

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());

	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);
	EXPECT_EQ(node->getId(), n1.getId());

	op.close();
}

TEST_F(MergeNodeOperatorTest, WithPropertyIndex_MatchExistingNode) {
	// Covers line 85-89: the property index path (hasPropertyIndex returns true)
	im->createIndex("", "node", "PropIdx", "name");

	int64_t labelId = dm->getOrCreateLabelId("PropIdx");
	Node n1(0, labelId);
	dm->addNode(n1);
	std::unordered_map<std::string, PropertyValue> props;
	props["name"] = PropertyValue(std::string("IndexedName"));
	dm->addNodeProperties(n1.getId(), props);

	std::unordered_map<std::string, PropertyValue> matchProps;
	matchProps["name"] = PropertyValue(std::string("IndexedName"));

	MergeNodeOperator op(dm, im, "n", {"PropIdx"}, matchProps, {}, {});

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());

	auto node = (*batch)[0].getNode("n");
	ASSERT_NE(node, std::nullopt);
	EXPECT_EQ(node->getId(), n1.getId());

	op.close();
}

TEST_F(MergeNodeOperatorTest, GetOutputVariablesWithChild) {
	// Cover MergeNodeOperator::getOutputVariables() when child_ is set (line 59 True branch)
	auto child = std::make_unique<MergeNodeMockOperator>();
	MergeNodeOperator op(dm, im, "m", {"TestLabel"}, {}, {}, {});
	op.setChild(std::move(child));

	auto vars = op.getOutputVariables();
	// Should include child's output variables ("n") plus our variable ("m")
	ASSERT_EQ(vars.size(), 2UL);
	EXPECT_EQ(vars[0], "n");
	EXPECT_EQ(vars[1], "m");
}

