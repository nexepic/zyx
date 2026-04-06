/**
 * @file test_CreateEdgeOperatorBranch.cpp
 * @date 2026/03/21
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
#include "graph/query/execution/operators/CreateEdgeOperator.hpp"
#include "graph/query/execution/Record.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

// Mock child operator for edge tests
class EdgeMockChild : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t currentIndex = 0;
	std::vector<std::string> vars;

	explicit EdgeMockChild(std::vector<RecordBatch> data = {}, std::vector<std::string> outputVars = {"x"})
		: batches(std::move(data)), vars(std::move(outputVars)) {}

	void open() override { currentIndex = 0; }
	std::optional<RecordBatch> next() override {
		if (currentIndex >= batches.size()) return std::nullopt;
		return batches[currentIndex++];
	}
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return vars; }
	[[nodiscard]] std::string toString() const override { return "EdgeMockChild"; }
};

class CreateEdgeOperatorBranchTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_createedge_branch_" + boost::uuids::to_string(uuid) + ".dat");
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

// No child -> returns nullopt (the !child_ branch)
TEST_F(CreateEdgeOperatorBranchTest, NoChild_ReturnsNullopt) {
	std::unordered_map<std::string, PropertyValue> props;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");

	op->open();
	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

// Child returns nullopt immediately
TEST_F(CreateEdgeOperatorBranchTest, ChildExhausted_ReturnsNullopt) {
	auto mock = std::make_unique<EdgeMockChild>();
	std::unordered_map<std::string, PropertyValue> props;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::move(mock));

	op->open();
	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

// Both source and target present -> edge created
TEST_F(CreateEdgeOperatorBranchTest, BothNodesPresent_EdgeCreated) {
	Node n1(0, dm->getOrCreateLabelId("Person"));
	Node n2(0, dm->getOrCreateLabelId("Person"));
	dm->addNode(n1);
	dm->addNode(n2);

	Record r1;
	r1.setNode("a", dm->getNode(1));
	r1.setNode("b", dm->getNode(2));

	auto mock = std::make_unique<EdgeMockChild>(std::vector<RecordBatch>{{r1}});
	std::unordered_map<std::string, PropertyValue> props;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::move(mock));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto edge = (*batch)[0].getEdge("e");
	ASSERT_TRUE(edge.has_value());
	EXPECT_EQ(edge->getSourceNodeId(), 1);
	EXPECT_EQ(edge->getTargetNodeId(), 2);

	op->close();
}

// Source missing -> no edge created (srcNode && tgtNode is false)
TEST_F(CreateEdgeOperatorBranchTest, SourceMissing_NoEdge) {
	Node n2(0, dm->getOrCreateLabelId("Person"));
	dm->addNode(n2);

	Record r1;
	r1.setNode("b", dm->getNode(1));
	// "a" is not set

	auto mock = std::make_unique<EdgeMockChild>(std::vector<RecordBatch>{{r1}});
	std::unordered_map<std::string, PropertyValue> props;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::move(mock));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 0UL);

	op->close();
}

// Target missing -> no edge created
TEST_F(CreateEdgeOperatorBranchTest, TargetMissing_NoEdge) {
	Node n1(0, dm->getOrCreateLabelId("Person"));
	dm->addNode(n1);

	Record r1;
	r1.setNode("a", dm->getNode(1));
	// "b" is not set

	auto mock = std::make_unique<EdgeMockChild>(std::vector<RecordBatch>{{r1}});
	std::unordered_map<std::string, PropertyValue> props;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::move(mock));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 0UL);

	op->close();
}

// Edge with properties (props non-empty branch)
TEST_F(CreateEdgeOperatorBranchTest, WithProperties) {
	Node n1(0, dm->getOrCreateLabelId("Person"));
	Node n2(0, dm->getOrCreateLabelId("Person"));
	dm->addNode(n1);
	dm->addNode(n2);

	Record r1;
	r1.setNode("a", dm->getNode(1));
	r1.setNode("b", dm->getNode(2));

	auto mock = std::make_unique<EdgeMockChild>(std::vector<RecordBatch>{{r1}});
	std::unordered_map<std::string, PropertyValue> props = {
		{"weight", PropertyValue(2.5)},
		{"type", PropertyValue(std::string("strong"))}
	};
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::move(mock));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto edge = (*batch)[0].getEdge("e");
	ASSERT_TRUE(edge.has_value());
	auto edgeProps = edge->getProperties();
	EXPECT_TRUE(edgeProps.contains("weight"));
	EXPECT_TRUE(edgeProps.contains("type"));

	op->close();
}

// Edge without properties (props empty branch - !props_.empty() is false)
TEST_F(CreateEdgeOperatorBranchTest, WithoutProperties) {
	Node n1(0, dm->getOrCreateLabelId("Person"));
	Node n2(0, dm->getOrCreateLabelId("Person"));
	dm->addNode(n1);
	dm->addNode(n2);

	Record r1;
	r1.setNode("a", dm->getNode(1));
	r1.setNode("b", dm->getNode(2));

	auto mock = std::make_unique<EdgeMockChild>(std::vector<RecordBatch>{{r1}});
	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", emptyProps, "a", "b");
	op->setChild(std::move(mock));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	op->close();
}

// getOutputVariables: variable already exists in child vars (dedup branch)
TEST_F(CreateEdgeOperatorBranchTest, GetOutputVariables_Dedup) {
	auto mock = std::make_unique<EdgeMockChild>(
		std::vector<RecordBatch>{}, std::vector<std::string>{"e", "a"});
	std::unordered_map<std::string, PropertyValue> props;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::move(mock));

	auto vars = op->getOutputVariables();
	// "e" already in child vars, should not be duplicated
	int count = 0;
	for (const auto &v : vars) {
		if (v == "e") count++;
	}
	EXPECT_EQ(count, 1);
}

// getOutputVariables: variable not in child vars (no dedup)
TEST_F(CreateEdgeOperatorBranchTest, GetOutputVariables_NoDedup) {
	auto mock = std::make_unique<EdgeMockChild>(
		std::vector<RecordBatch>{}, std::vector<std::string>{"x"});
	std::unordered_map<std::string, PropertyValue> props;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::move(mock));

	auto vars = op->getOutputVariables();
	EXPECT_EQ(vars.size(), 2UL);  // "x" + "e"
	EXPECT_TRUE(std::find(vars.begin(), vars.end(), "e") != vars.end());
}

// getOutputVariables without child
TEST_F(CreateEdgeOperatorBranchTest, GetOutputVariables_NoChild) {
	std::unordered_map<std::string, PropertyValue> props;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");

	auto vars = op->getOutputVariables();
	EXPECT_EQ(vars.size(), 1UL);
	EXPECT_EQ(vars[0], "e");
}

// toString
TEST_F(CreateEdgeOperatorBranchTest, ToString) {
	std::unordered_map<std::string, PropertyValue> props;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	std::string str = op->toString();
	EXPECT_TRUE(str.find("CreateEdge") != std::string::npos);
	EXPECT_TRUE(str.find("LINK") != std::string::npos);
	EXPECT_TRUE(str.find("src=a") != std::string::npos);
	EXPECT_TRUE(str.find("tgt=b") != std::string::npos);
}

// getChildren with and without child
TEST_F(CreateEdgeOperatorBranchTest, GetChildren) {
	std::unordered_map<std::string, PropertyValue> props;

	// Without child
	auto op1 = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	EXPECT_TRUE(op1->getChildren().empty());

	// With child
	auto mock = std::make_unique<EdgeMockChild>();
	auto *mockRaw = mock.get();
	auto op2 = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	op2->setChild(std::move(mock));
	auto children = op2->getChildren();
	ASSERT_EQ(children.size(), 1UL);
	EXPECT_EQ(children[0], mockRaw);
}

// open/close with null child
TEST_F(CreateEdgeOperatorBranchTest, OpenClose_NullChild) {
	std::unordered_map<std::string, PropertyValue> props;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());
}

// Multiple records: some with both nodes, some with missing nodes
TEST_F(CreateEdgeOperatorBranchTest, MixedRecords) {
	Node n1(0, dm->getOrCreateLabelId("Person"));
	Node n2(0, dm->getOrCreateLabelId("Person"));
	dm->addNode(n1);
	dm->addNode(n2);

	Record r1;  // Both nodes present
	r1.setNode("a", dm->getNode(1));
	r1.setNode("b", dm->getNode(2));

	Record r2;  // Only source
	r2.setNode("a", dm->getNode(1));

	Record r3;  // Both present
	r3.setNode("a", dm->getNode(2));
	r3.setNode("b", dm->getNode(1));

	auto mock = std::make_unique<EdgeMockChild>(std::vector<RecordBatch>{{r1, r2, r3}});
	std::unordered_map<std::string, PropertyValue> props;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::move(mock));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	// r1 and r3 have both nodes, r2 is missing target -> 2 edges
	EXPECT_EQ(batch->size(), 2UL);

	op->close();
}

// Both source and target missing -> empty output
TEST_F(CreateEdgeOperatorBranchTest, BothEndpointsMissing_NoEdge) {
	Record r1;
	r1.setValue("x", int64_t(42));
	// Neither "a" nor "b" set

	auto mock = std::make_unique<EdgeMockChild>(std::vector<RecordBatch>{{r1}});
	std::unordered_map<std::string, PropertyValue> props;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::move(mock));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 0UL);

	op->close();
}

// Multiple batches from child
TEST_F(CreateEdgeOperatorBranchTest, MultipleBatches) {
	Node n1(0, dm->getOrCreateLabelId("Person"));
	Node n2(0, dm->getOrCreateLabelId("Person"));
	dm->addNode(n1);
	dm->addNode(n2);

	Record r1;
	r1.setNode("a", dm->getNode(1));
	r1.setNode("b", dm->getNode(2));

	Record r2;
	r2.setNode("a", dm->getNode(2));
	r2.setNode("b", dm->getNode(1));

	auto mock = std::make_unique<EdgeMockChild>(
		std::vector<RecordBatch>{{r1}, {r2}});
	std::unordered_map<std::string, PropertyValue> props;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::move(mock));

	op->open();

	auto batch1 = op->next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1UL);

	auto batch2 = op->next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 1UL);

	// Third call should return nullopt
	auto batch3 = op->next();
	EXPECT_FALSE(batch3.has_value());

	op->close();
}

// Child returns empty batch followed by real batch
TEST_F(CreateEdgeOperatorBranchTest, ChildReturnsEmptyBatchThenReal) {
	Node n1(0, dm->getOrCreateLabelId("Person"));
	Node n2(0, dm->getOrCreateLabelId("Person"));
	dm->addNode(n1);
	dm->addNode(n2);

	RecordBatch emptyBatch;
	Record r1;
	r1.setNode("a", dm->getNode(1));
	r1.setNode("b", dm->getNode(2));
	RecordBatch realBatch = {r1};

	auto mock = std::make_unique<EdgeMockChild>(
		std::vector<RecordBatch>{emptyBatch, realBatch});
	std::unordered_map<std::string, PropertyValue> props;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::move(mock));

	op->open();

	// First batch is empty - should return empty output
	auto batch1 = op->next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 0UL);

	// Second batch has real data
	auto batch2 = op->next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 1UL);

	op->close();
}

// open/close propagates to child
TEST_F(CreateEdgeOperatorBranchTest, OpenClosePropagates) {
	auto mock = std::make_unique<EdgeMockChild>();
	std::unordered_map<std::string, PropertyValue> props;
	auto op = std::make_unique<CreateEdgeOperator>(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::move(mock));

	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());
}
