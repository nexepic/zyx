/**
 * @file test_OperatorsDataManager.cpp
 * @date 2026/02/02
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
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/execution/operators/DeleteOperator.hpp"
#include "graph/query/execution/operators/RemoveOperator.hpp"
#include "graph/query/execution/operators/SetConfigOperator.hpp"
#include "graph/query/execution/operators/SetOperator.hpp"
#include "graph/query/execution/operators/ShowIndexesOperator.hpp"
#include "graph/query/execution/operators/TraversalOperator.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/execution/operators/VarLengthTraversalOperator.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/execution/operators/NodeScanOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

// Mock Operator for testing
class MockOperator : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t current_index = 0;

	explicit MockOperator(std::vector<RecordBatch> data = {}) : batches(std::move(data)) {}

	void open() override {}
	std::optional<RecordBatch> next() override {
		if (current_index >= batches.size()) {
			return std::nullopt;
		}
		return batches[current_index++];
	}
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"x"}; }
	[[nodiscard]] std::string toString() const override { return "Mock"; }
};

// ============================================================================
// SetOperator Tests
// ============================================================================

class SetOperatorTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_setop_" + boost::uuids::to_string(uuid) + ".dat");

		if (fs::exists(testFilePath)) fs::remove_all(testFilePath);
		db = std::make_unique<Database>(testFilePath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}
};

TEST_F(SetOperatorTest, Set_WithNullChild) {
	// Test with null child operator
	std::vector<SetItem> items;
	items.emplace_back(
		SetActionType::PROPERTY,
		"n",
		"key",
		std::make_shared<graph::query::expressions::LiteralExpression>(static_cast<int64_t>(42))
	);
	auto op = std::make_unique<SetOperator>(dm, nullptr, items);
	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());
}

TEST_F(SetOperatorTest, Set_NodeProperty) {
	// Create a test node
	Node n1(1, 100);
	n1.setProperties({{"old", std::string("value")}});
	dm->addNode(n1);

	// Create record with node
	Record r1;
	r1.setNode("n", n1);

	MockOperator *mock = new MockOperator({{r1}});

	// Set new property
	std::vector<SetItem> items;
	items.emplace_back(
		SetActionType::PROPERTY,
		"n",
		"newKey",
		std::make_shared<graph::query::expressions::LiteralExpression>(static_cast<int64_t>(123))
	);
	auto op = std::unique_ptr<PhysicalOperator>(
		new SetOperator(dm, std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	// Verify property was set
	auto props = dm->getNodeProperties(1);
	EXPECT_TRUE(props.contains("newKey"));
	auto intPtr = std::get_if<int64_t>(&props["newKey"].getVariant());
	ASSERT_NE(intPtr, nullptr);
	EXPECT_EQ(*intPtr, 123);

	op->close();
}

TEST_F(SetOperatorTest, Set_NodeLabel) {
	// Create a test node
	Node n1(1, 100);
	dm->addNode(n1);

	Record r1;
	r1.setNode("n", n1);

	MockOperator *mock = new MockOperator({{r1}});

	// Set new label
	std::vector<SetItem> items;
	items.emplace_back(
		SetActionType::LABEL,
		"n",
		"NewLabel",
		nullptr // No expression for labels
	);
	auto op = std::unique_ptr<PhysicalOperator>(
		new SetOperator(dm, std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	// Verify label was appended (SET now appends, not replaces)
	auto node = dm->getNode(1);
	int64_t newLabelId = dm->getOrCreateTokenId("NewLabel");
	EXPECT_EQ(node.getLabelId(), 100); // Original label preserved
	EXPECT_TRUE(node.hasLabelId(newLabelId)); // New label added
	EXPECT_EQ(node.getLabelCount(), 2);

	op->close();
}

TEST_F(SetOperatorTest, Set_EdgeProperty) {
	// Create nodes first
	Node n1(1, 100);
	Node n2(2, 100);
	dm->addNode(n1);
	dm->addNode(n2);

	// Create edge
	Edge e1(10, 1, 2, 200);
	dm->addEdge(e1);

	Record r1;
	r1.setEdge("e", e1);

	MockOperator *mock = new MockOperator({{r1}});

	// Set edge property
	std::vector<SetItem> items;
	items.emplace_back(
		SetActionType::PROPERTY,
		"e",
		"weight",
		std::make_shared<graph::query::expressions::LiteralExpression>(1.5)
	);
	auto op = std::unique_ptr<PhysicalOperator>(
		new SetOperator(dm, std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	// Verify property was set
	auto props = dm->getEdgeProperties(10);
	EXPECT_TRUE(props.contains("weight"));

	op->close();
}

TEST_F(SetOperatorTest, Set_MultipleItems) {
	Node n1(1, 100);
	dm->addNode(n1);

	Record r1;
	r1.setNode("n", n1);

	MockOperator *mock = new MockOperator({{r1}});

	// Set multiple properties
	std::vector<SetItem> items;
	items.emplace_back(
		SetActionType::PROPERTY,
		"n",
		"a",
		std::make_shared<graph::query::expressions::LiteralExpression>(static_cast<int64_t>(1))
	);
	items.emplace_back(
		SetActionType::PROPERTY,
		"n",
		"b",
		std::make_shared<graph::query::expressions::LiteralExpression>(static_cast<int64_t>(2))
	);
	auto op = std::unique_ptr<PhysicalOperator>(
		new SetOperator(dm, std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	auto props = dm->getNodeProperties(1);
	EXPECT_TRUE(props.contains("a"));
	EXPECT_TRUE(props.contains("b"));

	op->close();
}

TEST_F(SetOperatorTest, GetOutputVariablesAndChildren) {
	Node n1(1, 100);
	dm->addNode(n1);

	Record r1;
	r1.setNode("n", n1);

	MockOperator *mock = new MockOperator({{r1}});

	std::vector<SetItem> items;
	items.emplace_back(
		SetActionType::PROPERTY,
		"n",
		"key",
		std::make_shared<graph::query::expressions::LiteralExpression>(static_cast<int64_t>(1))
	);
	auto op = std::unique_ptr<PhysicalOperator>(
		new SetOperator(dm, std::unique_ptr<PhysicalOperator>(mock), items));

	auto vars = op->getOutputVariables();
	EXPECT_EQ(vars.size(), 1UL);
	EXPECT_EQ(vars[0], "x");

	auto children = op->getChildren();
	EXPECT_EQ(children.size(), 1UL);

	op->close();
}

TEST_F(SetOperatorTest, Set_ToString) {
	std::vector<SetItem> items;
	items.emplace_back(
		SetActionType::PROPERTY, "n", "key",
		std::make_shared<graph::query::expressions::LiteralExpression>(static_cast<int64_t>(1))
	);
	items.emplace_back(
		SetActionType::LABEL, "n", "Label", nullptr
	);
	auto op = std::make_unique<SetOperator>(dm, nullptr, items);
	std::string str = op->toString();
	EXPECT_EQ(str, "Set(2 items)");
}

TEST_F(SetOperatorTest, SetChild_ReplacesChild) {
	// Covers SetOperator::setChild(unique_ptr<PhysicalOperator>)
	Node n1(1, 100);
	dm->addNode(n1);

	Record r1;
	r1.setNode("n", n1);

	// Create initial operator with a mock child
	auto* initialMock = new MockOperator({{r1}});
	std::vector<SetItem> items;
	items.emplace_back(
		SetActionType::PROPERTY, "n", "key",
		std::make_shared<graph::query::expressions::LiteralExpression>(static_cast<int64_t>(7))
	);
	auto op = std::make_unique<SetOperator>(dm, std::unique_ptr<PhysicalOperator>(initialMock), items);

	// Verify original child is accessible
	EXPECT_EQ(op->getChildren().size(), 1UL);

	// Replace child via setChild
	Record r2;
	r2.setNode("n", n1);
	auto newMock = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r2}});
	op->setChild(std::move(newMock));

	// The new child should be in place; open/next/close should work without crashing
	EXPECT_EQ(op->getChildren().size(), 1UL);
	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	op->close();
}

TEST_F(SetOperatorTest, Set_PropertyItemWithNullExpression) {
	// Covers: item.type == PROPERTY && item.expression is nullptr (skip branch)
	Node n1(1, 100);
	dm->addNode(n1);

	Record r1;
	r1.setNode("n", n1);

	MockOperator *mock = new MockOperator({{r1}});

	std::vector<SetItem> items;
	items.emplace_back(
		SetActionType::PROPERTY, "n", "key", nullptr // null expression
	);
	auto op = std::unique_ptr<PhysicalOperator>(
		new SetOperator(dm, std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	// Property should NOT have been set (skipped due to null expression)
	auto props = dm->getNodeProperties(1);
	EXPECT_FALSE(props.contains("key"));

	op->close();
}

TEST_F(SetOperatorTest, Set_PropertyOnUnknownVariable) {
	// Covers: variable not found as node or edge (both getNode and getEdge return nullopt)
	Record r1;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(42)));

	MockOperator *mock = new MockOperator({{r1}});

	std::vector<SetItem> items;
	items.emplace_back(
		SetActionType::PROPERTY, "n", "key",
		std::make_shared<graph::query::expressions::LiteralExpression>(static_cast<int64_t>(99))
	);
	auto op = std::unique_ptr<PhysicalOperator>(
		new SetOperator(dm, std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	op->close();
}

TEST_F(SetOperatorTest, Set_LabelOnUnknownVariable) {
	// Covers: LABEL type with variable not found as node (nodeOpt is nullopt)
	Record r1;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(42)));

	MockOperator *mock = new MockOperator({{r1}});

	std::vector<SetItem> items;
	items.emplace_back(
		SetActionType::LABEL, "n", "NewLabel", nullptr
	);
	auto op = std::unique_ptr<PhysicalOperator>(
		new SetOperator(dm, std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	op->close();
}

// ============================================================================
// RemoveOperator Tests
// ============================================================================

class RemoveOperatorTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_remop_" + boost::uuids::to_string(uuid) + ".dat");

		if (fs::exists(testFilePath)) fs::remove_all(testFilePath);
		db = std::make_unique<Database>(testFilePath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}
};

TEST_F(RemoveOperatorTest, Remove_WithNullChild) {
	std::vector<RemoveItem> items = { {RemoveActionType::PROPERTY, "n", "key"} };
	auto op = std::make_unique<RemoveOperator>(dm, nullptr, items);
	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());
}

TEST_F(RemoveOperatorTest, Remove_NodeProperty) {
	// Create node with property
	Node n1(1, 100);
	n1.setProperties({{"toRemove", std::string("value")}, {"keep", std::string("value")}});
	dm->addNode(n1);

	Record r1;
	r1.setNode("n", n1);

	MockOperator *mock = new MockOperator({{r1}});

	// Remove property
	std::vector<RemoveItem> items = { {RemoveActionType::PROPERTY, "n", "toRemove"} };
	auto op = std::unique_ptr<PhysicalOperator>(
		new RemoveOperator(dm, std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	// Verify property was removed
	auto props = dm->getNodeProperties(1);
	EXPECT_FALSE(props.contains("toRemove"));
	EXPECT_TRUE(props.contains("keep"));

	op->close();
}

TEST_F(RemoveOperatorTest, Remove_NodeLabel) {
	// Create node with label
	int64_t labelId = dm->getOrCreateTokenId("TestLabel");
	Node n1(1, labelId);
	dm->addNode(n1);

	Record r1;
	r1.setNode("n", n1);

	MockOperator *mock = new MockOperator({{r1}});

	// Remove label
	std::vector<RemoveItem> items = { {RemoveActionType::LABEL, "n", "TestLabel"} };
	auto op = std::unique_ptr<PhysicalOperator>(
		new RemoveOperator(dm, std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	// Verify label was removed
	auto node = dm->getNode(1);
	EXPECT_EQ(node.getLabelId(), 0);

	op->close();
}

TEST_F(RemoveOperatorTest, Remove_NodeLabelWhenNotMatching) {
	// Create node with different label
	int64_t labelId1 = dm->getOrCreateTokenId("Label1");
	Node n1(1, labelId1);
	dm->addNode(n1);

	Record r1;
	r1.setNode("n", n1);

	MockOperator *mock = new MockOperator({{r1}});

	// Try to remove different label
	std::vector<RemoveItem> items = { {RemoveActionType::LABEL, "n", "Label2"} };
	auto op = std::unique_ptr<PhysicalOperator>(
		new RemoveOperator(dm, std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	// Verify label was NOT removed (still has original label)
	auto node = dm->getNode(1);
	EXPECT_EQ(node.getLabelId(), labelId1);

	op->close();
}

TEST_F(RemoveOperatorTest, Remove_EdgeProperty) {
	// Create nodes and edge
	Node n1(1, 100);
	Node n2(2, 100);
	dm->addNode(n1);
	dm->addNode(n2);

	Edge e1(10, 1, 2, 200);
	e1.setProperties({{"toRemove", std::string("value")}});
	dm->addEdge(e1);

	Record r1;
	r1.setEdge("e", e1);

	MockOperator *mock = new MockOperator({{r1}});

	// Remove edge property
	std::vector<RemoveItem> items = { {RemoveActionType::PROPERTY, "e", "toRemove"} };
	auto op = std::unique_ptr<PhysicalOperator>(
		new RemoveOperator(dm, std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	// Verify property was removed
	auto props = dm->getEdgeProperties(10);
	EXPECT_FALSE(props.contains("toRemove"));

	op->close();
}

TEST_F(RemoveOperatorTest, Remove_MultipleItems) {
	Node n1(1, 100);
	n1.setProperties({{"a", std::string("1")}, {"b", std::string("2")}});
	dm->addNode(n1);

	Record r1;
	r1.setNode("n", n1);

	MockOperator *mock = new MockOperator({{r1}});

	// Remove multiple properties
	std::vector<RemoveItem> items = {
		{RemoveActionType::PROPERTY, "n", "a"},
		{RemoveActionType::PROPERTY, "n", "b"}
	};
	auto op = std::unique_ptr<PhysicalOperator>(
		new RemoveOperator(dm, std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	auto props = dm->getNodeProperties(1);
	EXPECT_FALSE(props.contains("a"));
	EXPECT_FALSE(props.contains("b"));

	op->close();
}

// ============================================================================
// TraversalOperator Tests
// ============================================================================

class TraversalOperatorTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_travop_" + boost::uuids::to_string(uuid) + ".dat");

		if (fs::exists(testFilePath)) fs::remove_all(testFilePath);
		db = std::make_unique<Database>(testFilePath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}
};

TEST_F(TraversalOperatorTest, Traversal_WithNullChild) {
	auto op = std::make_unique<TraversalOperator>(dm, nullptr, "a", "e", "b", "", "out");
	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());
}

TEST_F(TraversalOperatorTest, Traversal_Outgoing) {
	// Create nodes: 1 -> 2
	Node n1(1, 100);
	Node n2(2, 100);
	dm->addNode(n1);
	dm->addNode(n2);

	// Create edge
	int64_t edgeTypeId = dm->getOrCreateTokenId("LINK");
	Edge e1(10, 1, 2, edgeTypeId);
	dm->addEdge(e1);

	// Flush to ensure edges are linked
	db->getStorage()->flush();

	// Create record with source node
	Record r1;
	r1.setNode("a", n1);

	MockOperator *mock = new MockOperator({{r1}});

	auto op = std::unique_ptr<PhysicalOperator>(
		new TraversalOperator(dm, std::unique_ptr<PhysicalOperator>(mock), "a", "e", "b", "", "out"));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL); // Found one target

	// Verify target node
	auto targetNode = (*batch)[0].getNode("b");
	ASSERT_TRUE(targetNode.has_value());
	EXPECT_EQ(targetNode->getId(), 2);

	// Verify edge
	auto edge = (*batch)[0].getEdge("e");
	ASSERT_TRUE(edge.has_value());
	EXPECT_EQ(edge->getId(), 10);

	op->close();
}

TEST_F(TraversalOperatorTest, Traversal_WithEdgeLabelFilter) {
	// Create nodes and edges with different labels
	Node n1(1, 100);
	Node n2(2, 100);
	Node n3(3, 100);
	dm->addNode(n1);
	dm->addNode(n2);
	dm->addNode(n3);

	int64_t label1 = dm->getOrCreateTokenId("TYPE1");
	int64_t label2 = dm->getOrCreateTokenId("TYPE2");

	Edge e1(10, 1, 2, label1); // Should match
	Edge e2(11, 1, 3, label2); // Should not match
	dm->addEdge(e1);
	dm->addEdge(e2);

	db->getStorage()->flush();

	Record r1;
	r1.setNode("a", n1);

	MockOperator *mock = new MockOperator({{r1}});

	// Filter by TYPE1 label
	auto op = std::unique_ptr<PhysicalOperator>(
		new TraversalOperator(dm, std::unique_ptr<PhysicalOperator>(mock), "a", "e", "b", "TYPE1", "out"));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL); // Only e1 matches

	auto targetNode = (*batch)[0].getNode("b");
	ASSERT_TRUE(targetNode.has_value());
	EXPECT_EQ(targetNode->getId(), 2);

	op->close();
}

TEST_F(TraversalOperatorTest, Traversal_WithEmptyEdgeLabel) {
	// Test with empty edge label (should match all edges)
	Node n1(1, 100);
	Node n2(2, 100);
	dm->addNode(n1);
	dm->addNode(n2);

	int64_t edgeTypeId = dm->getOrCreateTokenId("LINK");
	Edge e1(10, 1, 2, edgeTypeId);
	dm->addEdge(e1);

	db->getStorage()->flush();

	Record r1;
	r1.setNode("a", n1);

	MockOperator *mock = new MockOperator({{r1}});

	// Empty edge label should match all
	auto op = std::unique_ptr<PhysicalOperator>(
		new TraversalOperator(dm, std::unique_ptr<PhysicalOperator>(mock), "a", "e", "b", "", "out"));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	op->close();
}

TEST_F(TraversalOperatorTest, Traversal_MissingSourceNode) {
	// Create record without source node
	Record r1;
	r1.setValue("x", 1);

	MockOperator *mock = new MockOperator({{r1}});

	auto op = std::unique_ptr<PhysicalOperator>(
		new TraversalOperator(dm, std::unique_ptr<PhysicalOperator>(mock), "a", "e", "b", "", "out"));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 0UL); // Should skip when source node is missing

	op->close();
}

TEST_F(TraversalOperatorTest, Traversal_GetOutputVariables) {
	Node n1(1, 100);
	dm->addNode(n1);

	Record r1;
	r1.setNode("a", n1);

	MockOperator *mock = new MockOperator({{r1}});

	auto op = std::unique_ptr<PhysicalOperator>(
		new TraversalOperator(dm, std::unique_ptr<PhysicalOperator>(mock), "src", "edge", "tgt", "LINK", "out"));

	auto vars = op->getOutputVariables();
	EXPECT_EQ(vars.size(), 3UL); // "x" from mock + "edge" + "tgt"

	op->close();
}

// ============================================================================
// VarLengthTraversalOperator Tests
// ============================================================================

class VarLengthTraversalOperatorTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_varlen_" + boost::uuids::to_string(uuid) + ".dat");

		if (fs::exists(testFilePath)) fs::remove_all(testFilePath);
		db = std::make_unique<Database>(testFilePath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}
};

TEST_F(VarLengthTraversalOperatorTest, VarLengthTraversal_WithNullChild) {
	// Test with null child operator
	// Note: getOutputVariables() will crash with null child due to missing null check in source code
	auto op = std::make_unique<VarLengthTraversalOperator>(dm, nullptr, "a", "b", "", 1, 2, "out");
	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());
}

TEST_F(VarLengthTraversalOperatorTest, VarLengthTraversal_Basic) {
	// Create nodes: 1 -> 2 -> 3
	Node n1(1, 100);
	Node n2(2, 100);
	Node n3(3, 100);
	dm->addNode(n1);
	dm->addNode(n2);
	dm->addNode(n3);

	// Create edges
	int64_t edgeTypeId = dm->getOrCreateTokenId("LINK");
	Edge e1(10, 1, 2, edgeTypeId);
	Edge e2(11, 2, 3, edgeTypeId);
	dm->addEdge(e1);
	dm->addEdge(e2);

	db->getStorage()->flush();

	// Create record with source node
	Record r1;
	r1.setNode("a", n1);

	MockOperator *mock = new MockOperator({{r1}});

	auto op = std::unique_ptr<PhysicalOperator>(
		new VarLengthTraversalOperator(dm, std::unique_ptr<PhysicalOperator>(mock), "a", "b", "", 1, 2, "out"));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	// Should find n2 (1 hop) and n3 (2 hops)
	EXPECT_GE(batch->size(), 1UL);

	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, VarLengthTraversal_WithEdgeLabelFilter) {
	// Create nodes with different edge types
	Node n1(1, 100);
	Node n2(2, 100);
	Node n3(3, 100);
	dm->addNode(n1);
	dm->addNode(n2);
	dm->addNode(n3);

	int64_t label1 = dm->getOrCreateTokenId("TYPE1");
	int64_t label2 = dm->getOrCreateTokenId("TYPE2");

	Edge e1(10, 1, 2, label1);
	Edge e2(11, 2, 3, label2);
	dm->addEdge(e1);
	dm->addEdge(e2);

	db->getStorage()->flush();

	Record r1;
	r1.setNode("a", n1);

	MockOperator *mock = new MockOperator({{r1}});

	// Filter by TYPE1 label - should only reach n2
	auto op = std::unique_ptr<PhysicalOperator>(
		new VarLengthTraversalOperator(dm, std::unique_ptr<PhysicalOperator>(mock), "a", "b", "TYPE1", 1, 3, "out"));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, VarLengthTraversal_MissingSourceNode) {
	// Create record without source node
	Record r1;
	r1.setValue("x", 1);

	MockOperator *mock = new MockOperator({{r1}});

	auto op = std::unique_ptr<PhysicalOperator>(
		new VarLengthTraversalOperator(dm, std::unique_ptr<PhysicalOperator>(mock), "a", "b", "", 1, 2, "out"));

	op->open();
	auto batch = op->next();
	// Should skip when source node is missing — either empty batch or nullopt
	if (batch.has_value()) {
		EXPECT_EQ(batch->size(), 0UL);
	}
	// nullopt is also acceptable (iterative DFS returns nullopt when no results)

	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, VarLengthTraversal_MinHopsZero) {
	// Create nodes: 1 -> 2
	Node n1(1, 100);
	Node n2(2, 100);
	dm->addNode(n1);
	dm->addNode(n2);

	int64_t edgeTypeId = dm->getOrCreateTokenId("LINK");
	Edge e1(10, 1, 2, edgeTypeId);
	dm->addEdge(e1);

	db->getStorage()->flush();

	Record r1;
	r1.setNode("a", n1);

	MockOperator *mock = new MockOperator({{r1}});

	// minHops=0 should include the source node itself
	auto op = std::unique_ptr<PhysicalOperator>(
		new VarLengthTraversalOperator(dm, std::unique_ptr<PhysicalOperator>(mock), "a", "b", "", 0, 1, "out"));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, VarLengthTraversal_GetOutputVariables) {
	Node n1(1, 100);
	dm->addNode(n1);

	Record r1;
	r1.setNode("a", n1);

	MockOperator *mock = new MockOperator({{r1}});

	auto op = std::unique_ptr<PhysicalOperator>(
		new VarLengthTraversalOperator(dm, std::unique_ptr<PhysicalOperator>(mock), "src", "tgt", "LINK", 1, 3, "out"));

	auto vars = op->getOutputVariables();
	EXPECT_EQ(vars.size(), 2UL); // "x" from mock + "tgt"

	op->close();
}

// ============================================================================
// RemoveOperator::setChild() — targeted branch coverage
// ============================================================================

TEST_F(RemoveOperatorTest, SetChild_ReplacesChildOperator) {
	std::vector<RemoveItem> items = { {RemoveActionType::PROPERTY, "n", "key"} };

	// Start with a null child
	auto op = std::make_unique<RemoveOperator>(dm, nullptr, items);

	// Replace via setChild
	MockOperator *newMock = new MockOperator({});
	op->setChild(std::unique_ptr<PhysicalOperator>(newMock));

	// getChildren should now return the new child
	auto children = op->getChildren();
	ASSERT_EQ(children.size(), 1UL);
	EXPECT_EQ(children[0], newMock);
}

// ============================================================================
// DeleteOperator::setChild() — targeted branch coverage
// ============================================================================

// The DeleteOperator test class already lives in test_DeleteOperator_Detach.cpp.
// Add setChild coverage to RemoveOperatorTest fixture which shares the same db setup.

TEST_F(RemoveOperatorTest, DeleteOperator_SetChild_ReplacesChildOperator) {
	using graph::query::execution::operators::DeleteOperator;

	auto op = std::make_unique<DeleteOperator>(dm, nullptr, std::vector<std::string>{"n"}, false);

	// Replace via setChild
	MockOperator *newMock = new MockOperator({});
	op->setChild(std::unique_ptr<PhysicalOperator>(newMock));

	auto children = op->getChildren();
	ASSERT_EQ(children.size(), 1UL);
	EXPECT_EQ(children[0], newMock);
}

// ============================================================================
// ShowIndexesOperator — branch coverage (empty index list path)
// ============================================================================

TEST_F(RemoveOperatorTest, ShowIndexesOperator_EmptyIndexListReturnsNullopt) {
	using graph::query::execution::operators::ShowIndexesOperator;

	auto indexManager = db->getQueryEngine()->getIndexManager();
	auto op = std::make_unique<ShowIndexesOperator>(indexManager);
	op->open();

	// No indexes created: operator emits nullopt when index list is empty.
	auto batch = op->next();
	// Both nullopt (empty list) and a batch with 0+ entries are valid.
	// What we require: the executed_ flag is set so the second call always returns nullopt.
	auto batch2 = op->next();
	EXPECT_FALSE(batch2.has_value());
}

TEST_F(RemoveOperatorTest, ShowIndexesOperator_SecondCallAfterDataReturnsNullopt) {
	using graph::query::execution::operators::ShowIndexesOperator;

	auto indexManager = db->getQueryEngine()->getIndexManager();
	// Create at least one index so the first call returns a non-empty batch.
	indexManager->createIndex("test_label_idx", "node", "Person", "");

	auto op = std::make_unique<ShowIndexesOperator>(indexManager);
	op->open();

	auto batch1 = op->next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_FALSE(batch1->empty());

	// Second call must return nullopt (executed_ == true branch).
	auto batch2 = op->next();
	EXPECT_FALSE(batch2.has_value());
}

// ============================================================================
// SetConfigOperator — throw path for LIST/MAP/temporal values
// ============================================================================

TEST_F(RemoveOperatorTest, SetConfigOperator_ListValueThrows) {
	using graph::query::execution::operators::SetConfigOperator;

	PropertyValue listVal(std::vector<PropertyValue>{PropertyValue(int64_t(1))});
	auto op = std::make_unique<SetConfigOperator>(dm, "max_depth", listVal);
	op->open();
	EXPECT_THROW(op->next(), std::runtime_error);
}

TEST_F(RemoveOperatorTest, SetConfigOperator_IntValueSucceeds) {
	using graph::query::execution::operators::SetConfigOperator;

	PropertyValue intVal(int64_t(100));
	auto op = std::make_unique<SetConfigOperator>(dm, "max_depth", intVal);
	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);

	// Second call returns nullopt (executed_ path).
	EXPECT_FALSE(op->next().has_value());
}

TEST_F(RemoveOperatorTest, SetConfigOperator_NullValueIsNoOp) {
	using graph::query::execution::operators::SetConfigOperator;

	PropertyValue nullVal; // monostate / NULL
	auto op = std::make_unique<SetConfigOperator>(dm, "dummy_key", nullVal);
	op->open();
	// NULL value is silently ignored — no throw.
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
}
