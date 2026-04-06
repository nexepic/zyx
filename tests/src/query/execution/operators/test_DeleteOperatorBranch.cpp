/**
 * @file test_DeleteOperatorBranch.cpp
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
#include <stdexcept>

#include "graph/core/Database.hpp"
#include "graph/query/execution/operators/DeleteOperator.hpp"
#include "graph/query/execution/Record.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

// Mock child operator
class DeleteMockChild : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t currentIndex = 0;

	explicit DeleteMockChild(std::vector<RecordBatch> data = {}) : batches(std::move(data)) {}

	void open() override { currentIndex = 0; }
	std::optional<RecordBatch> next() override {
		if (currentIndex >= batches.size()) return std::nullopt;
		return batches[currentIndex++];
	}
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"n"}; }
	[[nodiscard]] std::string toString() const override { return "DeleteMockChild"; }
};

class DeleteOperatorBranchTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_delete_branch_" + boost::uuids::to_string(uuid) + ".dat");
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

// Child returns nullopt immediately
TEST_F(DeleteOperatorBranchTest, ChildExhaustedImmediately) {
	auto mock = std::make_unique<DeleteMockChild>();

	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"n"}, false);
	op->open();

	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

// Delete a node with no relationships (non-detach mode)
TEST_F(DeleteOperatorBranchTest, DeleteNode_NoRelationships) {
	int64_t labelId = dm->getOrCreateLabelId("Person");
	Node n1(0, labelId);
	dm->addNode(n1);

	Record r1;
	r1.setNode("n", dm->getNode(1));

	auto mock = std::make_unique<DeleteMockChild>(std::vector<RecordBatch>{{r1}});

	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"n"}, false);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	// Verify node was deleted
	Node deleted = dm->getNode(1);
	EXPECT_FALSE(deleted.isActive());

	op->close();
}

// Delete a node with relationships (non-detach) -> should throw
TEST_F(DeleteOperatorBranchTest, DeleteNode_WithRelationships_NonDetach_Throws) {
	int64_t labelId = dm->getOrCreateLabelId("Person");
	Node n1(0, labelId);
	Node n2(0, labelId);
	dm->addNode(n1);
	dm->addNode(n2);

	int64_t edgeLabelId = dm->getOrCreateLabelId("KNOWS");
	Edge e1(0, 1, 2, edgeLabelId);
	dm->addEdge(e1);

	db->getStorage()->flush();

	Record r1;
	r1.setNode("n", dm->getNode(1));

	auto mock = std::make_unique<DeleteMockChild>(std::vector<RecordBatch>{{r1}});

	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"n"}, false);
	op->open();

	EXPECT_THROW(op->next(), std::runtime_error);

	op->close();
}

// DETACH DELETE a node with relationships - should delete edges first
TEST_F(DeleteOperatorBranchTest, DetachDeleteNode_WithRelationships) {
	int64_t labelId = dm->getOrCreateLabelId("Person");
	Node n1(0, labelId);
	Node n2(0, labelId);
	dm->addNode(n1);
	dm->addNode(n2);

	int64_t edgeLabelId = dm->getOrCreateLabelId("KNOWS");
	Edge e1(0, 1, 2, edgeLabelId);
	dm->addEdge(e1);

	db->getStorage()->flush();

	Record r1;
	r1.setNode("n", dm->getNode(1));

	auto mock = std::make_unique<DeleteMockChild>(std::vector<RecordBatch>{{r1}});

	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"n"}, true);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	// Node should be deleted
	Node deleted = dm->getNode(1);
	EXPECT_FALSE(deleted.isActive());

	op->close();
}

// Delete an edge
TEST_F(DeleteOperatorBranchTest, DeleteEdge) {
	int64_t labelId = dm->getOrCreateLabelId("Person");
	Node n1(0, labelId);
	Node n2(0, labelId);
	dm->addNode(n1);
	dm->addNode(n2);

	int64_t edgeLabelId = dm->getOrCreateLabelId("KNOWS");
	Edge e1(0, 1, 2, edgeLabelId);
	dm->addEdge(e1);

	db->getStorage()->flush();

	Record r1;
	r1.setEdge("e", dm->getEdge(1));

	auto mock = std::make_unique<DeleteMockChild>(std::vector<RecordBatch>{{r1}});

	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"e"}, false);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	op->close();
}

// Variable not found as node or edge -> skip both branches
TEST_F(DeleteOperatorBranchTest, VariableNotBound) {
	Record r1;
	r1.setValue("x", int64_t(42));

	auto mock = std::make_unique<DeleteMockChild>(std::vector<RecordBatch>{{r1}});

	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"missing"}, false);
	op->open();

	// Should pass through without deleting anything
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	op->close();
}

// Multiple variables in delete list
TEST_F(DeleteOperatorBranchTest, MultipleVariables) {
	int64_t labelId = dm->getOrCreateLabelId("Person");
	Node n1(0, labelId);
	Node n2(0, labelId);
	dm->addNode(n1);
	dm->addNode(n2);

	Record r1;
	r1.setNode("a", dm->getNode(1));
	r1.setNode("b", dm->getNode(2));

	auto mock = std::make_unique<DeleteMockChild>(std::vector<RecordBatch>{{r1}});

	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"a", "b"}, false);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	// Both nodes should be deleted
	EXPECT_FALSE(dm->getNode(1).isActive());
	EXPECT_FALSE(dm->getNode(2).isActive());

	op->close();
}

// toString for detach and non-detach
TEST_F(DeleteOperatorBranchTest, ToString_NonDetach) {
	auto mock = std::make_unique<DeleteMockChild>();
	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"n"}, false);
	EXPECT_EQ(op->toString(), "Delete(n)");
}

TEST_F(DeleteOperatorBranchTest, ToString_Detach) {
	auto mock = std::make_unique<DeleteMockChild>();
	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"n"}, true);
	EXPECT_EQ(op->toString(), "DetachDelete(n)");
}

TEST_F(DeleteOperatorBranchTest, ToString_MultipleVariables) {
	auto mock = std::make_unique<DeleteMockChild>();
	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"a", "b", "c"}, false);
	EXPECT_EQ(op->toString(), "Delete(a, b, c)");
}

// getChildren and getOutputVariables
TEST_F(DeleteOperatorBranchTest, GetChildrenAndOutputVars) {
	auto mock = std::make_unique<DeleteMockChild>();
	auto *mockRaw = mock.get();

	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"n"}, false);

	auto children = op->getChildren();
	ASSERT_EQ(children.size(), 1UL);
	EXPECT_EQ(children[0], mockRaw);

	auto vars = op->getOutputVariables();
	EXPECT_EQ(vars.size(), 1UL);
	EXPECT_EQ(vars[0], "n");
}

// open/close with null child
TEST_F(DeleteOperatorBranchTest, OpenClose_NullChild) {
	auto op = std::make_unique<DeleteOperator>(dm, nullptr, std::vector<std::string>{"n"}, false);
	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());
}

// DETACH DELETE node with no relationships (connectedEdges is empty, detach_ is true)
TEST_F(DeleteOperatorBranchTest, DetachDeleteNode_NoRelationships) {
	int64_t labelId = dm->getOrCreateLabelId("Person");
	Node n1(0, labelId);
	dm->addNode(n1);

	Record r1;
	r1.setNode("n", dm->getNode(1));

	auto mock = std::make_unique<DeleteMockChild>(std::vector<RecordBatch>{{r1}});

	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"n"}, true);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	EXPECT_FALSE(dm->getNode(1).isActive());

	op->close();
}

// DETACH DELETE node with multiple relationships (for loop iterates multiple times)
TEST_F(DeleteOperatorBranchTest, DetachDeleteNode_MultipleRelationships) {
	int64_t labelId = dm->getOrCreateLabelId("Person");
	Node n1(0, labelId);
	Node n2(0, labelId);
	Node n3(0, labelId);
	dm->addNode(n1);
	dm->addNode(n2);
	dm->addNode(n3);

	int64_t edgeLabelId = dm->getOrCreateLabelId("KNOWS");
	Edge e1(0, 1, 2, edgeLabelId);
	Edge e2(0, 1, 3, edgeLabelId);
	Edge e3(0, 3, 1, edgeLabelId);
	dm->addEdge(e1);
	dm->addEdge(e2);
	dm->addEdge(e3);

	db->getStorage()->flush();

	Record r1;
	r1.setNode("n", dm->getNode(1));

	auto mock = std::make_unique<DeleteMockChild>(std::vector<RecordBatch>{{r1}});

	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"n"}, true);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	// Node should be deleted after detaching all 3 edges
	EXPECT_FALSE(dm->getNode(1).isActive());

	op->close();
}

// Delete with multiple records in a single batch
TEST_F(DeleteOperatorBranchTest, MultipleRecordsInBatch) {
	int64_t labelId = dm->getOrCreateLabelId("Person");
	Node n1(0, labelId);
	Node n2(0, labelId);
	Node n3(0, labelId);
	dm->addNode(n1);
	dm->addNode(n2);
	dm->addNode(n3);

	Record r1, r2, r3;
	r1.setNode("n", dm->getNode(1));
	r2.setNode("n", dm->getNode(2));
	r3.setNode("n", dm->getNode(3));

	auto mock = std::make_unique<DeleteMockChild>(std::vector<RecordBatch>{{r1, r2, r3}});

	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"n"}, false);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 3UL);

	EXPECT_FALSE(dm->getNode(1).isActive());
	EXPECT_FALSE(dm->getNode(2).isActive());
	EXPECT_FALSE(dm->getNode(3).isActive());

	op->close();
}

// toString with single variable (covers i < variables_.size() - 1 being false)
TEST_F(DeleteOperatorBranchTest, ToString_SingleVariable) {
	auto mock = std::make_unique<DeleteMockChild>();
	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"x"}, false);
	EXPECT_EQ(op->toString(), "Delete(x)");
}

// toString with two variables (covers both true and false branches of the comma condition)
TEST_F(DeleteOperatorBranchTest, ToString_TwoVariables) {
	auto mock = std::make_unique<DeleteMockChild>();
	auto op = std::make_unique<DeleteOperator>(dm, std::move(mock), std::vector<std::string>{"a", "b"}, true);
	EXPECT_EQ(op->toString(), "DetachDelete(a, b)");
}
