/**
 * @file test_CreateEdgeOperator.cpp
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
#include "graph/query/execution/operators/CreateEdgeOperator.hpp"
#include "graph/query/execution/Record.hpp"

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
// CreateEdgeOperator Tests
// ============================================================================

class CreateEdgeOperatorTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_createedge_" + boost::uuids::to_string(uuid) + ".dat");

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

TEST_F(CreateEdgeOperatorTest, CreateEdge_WithNullChild) {
	std::unordered_map<std::string, PropertyValue> props;
	auto *op = new CreateEdgeOperator(dm, "e", "LINK", props, "a", "b");

	std::unique_ptr<PhysicalOperator> opPtr(op);
	EXPECT_NO_THROW(opPtr->open());
	EXPECT_NO_THROW(opPtr->close());
	EXPECT_EQ(opPtr->getChildren().size(), 0UL);
}

TEST_F(CreateEdgeOperatorTest, CreateEdge_Basic) {
	// Create source and target nodes
	Node n1(1, 100);
	Node n2(2, 100);
	dm->addNode(n1);
	dm->addNode(n2);

	// Create record with both nodes
	Record r1;
	r1.setNode("a", n1);
	r1.setNode("b", n2);

	MockOperator *mock = new MockOperator({{r1}});

	std::unordered_map<std::string, PropertyValue> props;
	auto *op = new CreateEdgeOperator(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::unique_ptr<PhysicalOperator>(mock));

	std::unique_ptr<PhysicalOperator> opPtr(op);
	opPtr->open();
	auto batch = opPtr->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	// Verify edge was created
	auto edge = (*batch)[0].getEdge("e");
	ASSERT_TRUE(edge.has_value());
	EXPECT_EQ(edge->getSourceNodeId(), 1);
	EXPECT_EQ(edge->getTargetNodeId(), 2);

	opPtr->close();
}

TEST_F(CreateEdgeOperatorTest, CreateEdge_WithProperties) {
	// Create source and target nodes
	Node n1(1, 100);
	Node n2(2, 100);
	dm->addNode(n1);
	dm->addNode(n2);

	Record r1;
	r1.setNode("a", n1);
	r1.setNode("b", n2);

	MockOperator *mock = new MockOperator({{r1}});

	// Create edge with properties
	std::unordered_map<std::string, PropertyValue> props = {{"weight", 1.5}};
	auto *op = new CreateEdgeOperator(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::unique_ptr<PhysicalOperator>(mock));

	std::unique_ptr<PhysicalOperator> opPtr(op);
	opPtr->open();
	auto batch = opPtr->next();
	ASSERT_TRUE(batch.has_value());

	// Verify properties were set
	auto edge = (*batch)[0].getEdge("e");
	ASSERT_TRUE(edge.has_value());
	auto edgeProps = edge->getProperties();
	EXPECT_TRUE(edgeProps.contains("weight"));

	opPtr->close();
}

TEST_F(CreateEdgeOperatorTest, CreateEdge_WithEmptyProperties) {
	// Create source and target nodes
	Node n1(1, 100);
	Node n2(2, 100);
	dm->addNode(n1);
	dm->addNode(n2);

	Record r1;
	r1.setNode("a", n1);
	r1.setNode("b", n2);

	MockOperator *mock = new MockOperator({{r1}});

	// Create edge with empty properties
	std::unordered_map<std::string, PropertyValue> props;
	auto *op = new CreateEdgeOperator(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::unique_ptr<PhysicalOperator>(mock));

	std::unique_ptr<PhysicalOperator> opPtr(op);
	opPtr->open();
	auto batch = opPtr->next();
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("e");
	ASSERT_TRUE(edge.has_value());

	opPtr->close();
}

TEST_F(CreateEdgeOperatorTest, CreateEdge_MissingSourceNode) {
	// Create only target node
	Node n2(2, 100);
	dm->addNode(n2);

	Record r1;
	r1.setNode("b", n2);
	// Source node "a" is missing

	MockOperator *mock = new MockOperator({{r1}});

	std::unordered_map<std::string, PropertyValue> props;
	auto *op = new CreateEdgeOperator(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::unique_ptr<PhysicalOperator>(mock));

	std::unique_ptr<PhysicalOperator> opPtr(op);
	opPtr->open();
	auto batch = opPtr->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 0UL); // No edge created when source is missing

	opPtr->close();
}

TEST_F(CreateEdgeOperatorTest, CreateEdge_MissingTargetNode) {
	// Create only source node
	Node n1(1, 100);
	dm->addNode(n1);

	Record r1;
	r1.setNode("a", n1);
	// Target node "b" is missing

	MockOperator *mock = new MockOperator({{r1}});

	std::unordered_map<std::string, PropertyValue> props;
	auto *op = new CreateEdgeOperator(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::unique_ptr<PhysicalOperator>(mock));

	std::unique_ptr<PhysicalOperator> opPtr(op);
	opPtr->open();
	auto batch = opPtr->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 0UL); // No edge created when target is missing

	opPtr->close();
}

TEST_F(CreateEdgeOperatorTest, CreateEdge_GetOutputVariables) {
	// Create source and target nodes
	Node n1(1, 100);
	Node n2(2, 100);
	dm->addNode(n1);
	dm->addNode(n2);

	Record r1;
	r1.setNode("a", n1);
	r1.setNode("b", n2);

	MockOperator *mock = new MockOperator({{r1}});

	std::unordered_map<std::string, PropertyValue> props;
	auto *op = new CreateEdgeOperator(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::unique_ptr<PhysicalOperator>(mock));

	std::unique_ptr<PhysicalOperator> opPtr(op);
	auto vars = opPtr->getOutputVariables();
	// "x" from mock + "a" + "b" + "e" (with deduplication)
	EXPECT_GE(vars.size(), 2UL); // At least "x" + "e"
	EXPECT_TRUE(std::find(vars.begin(), vars.end(), "e") != vars.end());

	opPtr->close();
}

TEST_F(CreateEdgeOperatorTest, CreateEdge_MultipleRecords) {
	// Create nodes
	Node n1(1, 100);
	Node n2(2, 100);
	Node n3(3, 100);
	dm->addNode(n1);
	dm->addNode(n2);
	dm->addNode(n3);

	// Create multiple records
	Record r1, r2, r3;
	r1.setNode("a", n1);
	r1.setNode("b", n2);

	r2.setNode("a", n2);
	r2.setNode("b", n3);

	r3.setNode("a", n3);
	r3.setNode("b", n1);

	MockOperator *mock = new MockOperator({{r1, r2, r3}});

	std::unordered_map<std::string, PropertyValue> props;
	auto *op = new CreateEdgeOperator(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::unique_ptr<PhysicalOperator>(mock));

	std::unique_ptr<PhysicalOperator> opPtr(op);
	opPtr->open();
	auto batch = opPtr->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 3UL); // All 3 edges created

	opPtr->close();
}

TEST_F(CreateEdgeOperatorTest, CreateEdge_ChildReturnsNullopt) {
	// Create source and target nodes
	Node n1(1, 100);
	Node n2(2, 100);
	dm->addNode(n1);
	dm->addNode(n2);

	// Mock operator that returns nullopt immediately
	MockOperator *mock = new MockOperator({}); // Empty batches

	std::unordered_map<std::string, PropertyValue> props;
	auto *op = new CreateEdgeOperator(dm, "e", "LINK", props, "a", "b");
	op->setChild(std::unique_ptr<PhysicalOperator>(mock));

	std::unique_ptr<PhysicalOperator> opPtr(op);
	opPtr->open();
	auto batch = opPtr->next();
	EXPECT_FALSE(batch.has_value()); // Should return nullopt when child is exhausted

	opPtr->close();
}
