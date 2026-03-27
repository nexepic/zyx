/**
 * @file test_CreateNodeOperatorBranch.cpp
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
#include "graph/query/execution/operators/CreateNodeOperator.hpp"
#include "graph/query/execution/Record.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

// Mock child operator
class CreateNodeMockChild : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t currentIndex = 0;

	explicit CreateNodeMockChild(std::vector<RecordBatch> data = {}) : batches(std::move(data)) {}

	void open() override { currentIndex = 0; }
	std::optional<RecordBatch> next() override {
		if (currentIndex >= batches.size()) return std::nullopt;
		return batches[currentIndex++];
	}
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"n"}; }
	[[nodiscard]] std::string toString() const override { return "CreateNodeMockChild"; }
};

class CreateNodeOperatorBranchTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_createnode_branch_" + boost::uuids::to_string(uuid) + ".dat");
		if (fs::exists(testFilePath)) fs::remove_all(testFilePath);
		db = std::make_unique<Database>(testFilePath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
	}

	void TearDown() override {
		if (db) db->close();
		if (fs::exists(testFilePath)) fs::remove(testFilePath);
	}
};

// Case B: Source mode (no child) - single create with no properties
TEST_F(CreateNodeOperatorBranchTest, SourceMode_BasicCreate) {
	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto node = (*batch)[0].getNode("n");
	ASSERT_TRUE(node.has_value());
	EXPECT_TRUE(node->getId() > 0);

	// Second call should return nullopt (executed_ = true)
	EXPECT_FALSE(op->next().has_value());

	op->close();
}

// Case B: Source mode with properties
TEST_F(CreateNodeOperatorBranchTest, SourceMode_CreateWithProperties) {
	std::unordered_map<std::string, PropertyValue> props = {
		{"name", PropertyValue(std::string("Alice"))},
		{"age", PropertyValue(int64_t(30))}
	};
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, props);

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto node = (*batch)[0].getNode("n");
	ASSERT_TRUE(node.has_value());

	// Verify properties were persisted
	auto storedProps = dm->getNodeProperties(node->getId());
	EXPECT_TRUE(storedProps.contains("name"));
	EXPECT_TRUE(storedProps.contains("age"));

	op->close();
}

// Case B: Source mode with empty properties (tests !props_.empty() branch = false)
TEST_F(CreateNodeOperatorBranchTest, SourceMode_EmptyProperties) {
	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	EXPECT_FALSE(op->next().has_value());
	op->close();
}

// Case A: Pipeline mode - child returns records with unbound variable
TEST_F(CreateNodeOperatorBranchTest, PipelineMode_UnboundVariable) {
	Record r1;
	r1.setValue("x", int64_t(1));
	Record r2;
	r2.setValue("x", int64_t(2));

	auto mock = std::make_unique<CreateNodeMockChild>(std::vector<RecordBatch>{{r1, r2}});

	std::unordered_map<std::string, PropertyValue> props = {
		{"key", PropertyValue(std::string("val"))}
	};
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, props);
	op->setChild(std::move(mock));

	op->open();

	size_t totalRecords = 0;
	while (auto batch = op->next()) {
		totalRecords += batch->size();
		for (const auto &rec : *batch) {
			auto node = rec.getNode("n");
			ASSERT_TRUE(node.has_value());
			EXPECT_TRUE(node->getId() > 0);
		}
	}
	EXPECT_EQ(totalRecords, 2UL);

	op->close();
}

// Case A: Pipeline mode - child returns empty batch (continue branch)
TEST_F(CreateNodeOperatorBranchTest, PipelineMode_ChildReturnsEmptyBatch) {
	Record r1;
	r1.setValue("x", int64_t(1));

	RecordBatch emptyBatch;
	RecordBatch realBatch = {r1};

	auto mock = std::make_unique<CreateNodeMockChild>(
		std::vector<RecordBatch>{emptyBatch, realBatch});

	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);
	op->setChild(std::move(mock));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_GE(batch->size(), 1UL);

	op->close();
}

// Case A: Pipeline mode - child returns nullopt immediately (upstream exhausted with empty buffer)
TEST_F(CreateNodeOperatorBranchTest, PipelineMode_ChildExhaustedImmediately) {
	auto mock = std::make_unique<CreateNodeMockChild>(std::vector<RecordBatch>{});

	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);
	op->setChild(std::move(mock));

	op->open();
	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

// Case A: Pipeline mode - record has existing node bound to variable (existingNode branch)
TEST_F(CreateNodeOperatorBranchTest, PipelineMode_ExistingNodeBound) {
	Node existingNode(1, 100);
	dm->addNode(existingNode);

	Record r1;
	r1.setNode("n", existingNode);  // variable "n" is already bound

	auto mock = std::make_unique<CreateNodeMockChild>(std::vector<RecordBatch>{{r1}});

	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);
	op->setChild(std::move(mock));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	// The existing node should be passed through without creating new one
	auto node = (*batch)[0].getNode("n");
	ASSERT_TRUE(node.has_value());
	EXPECT_EQ(node->getId(), 1);

	op->close();
}

// Case A: Pipeline mode - existingNode with non-empty buffer (flush + pass-through)
TEST_F(CreateNodeOperatorBranchTest, PipelineMode_ExistingNodeWithPendingBuffer) {
	Node existingNode(1, 100);
	dm->addNode(existingNode);

	// First record has unbound variable -> goes to buffer
	Record r1;
	r1.setValue("x", int64_t(1));

	// Second record has existing node -> flush buffer + pass through
	Record r2;
	r2.setNode("n", existingNode);

	auto mock = std::make_unique<CreateNodeMockChild>(std::vector<RecordBatch>{{r1, r2}});

	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);
	op->setChild(std::move(mock));

	op->open();

	// Should flush the buffered node + include the existing node record
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL);

	op->close();
}

// Pipeline mode - childExhausted_ branch (calling next() after child exhausted)
TEST_F(CreateNodeOperatorBranchTest, PipelineMode_ChildExhaustedSecondCall) {
	Record r1;
	r1.setValue("x", int64_t(1));

	auto mock = std::make_unique<CreateNodeMockChild>(std::vector<RecordBatch>{{r1}});

	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);
	op->setChild(std::move(mock));

	op->open();

	// First call processes r1 and flushes
	auto batch1 = op->next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1UL);

	// Second call: childExhausted_ is true -> flushBuffer returns nullopt (empty buffer)
	auto batch2 = op->next();
	EXPECT_FALSE(batch2.has_value());

	op->close();
}

// Metadata: getOutputVariables with child
TEST_F(CreateNodeOperatorBranchTest, GetOutputVariables_WithChild) {
	auto mock = std::make_unique<CreateNodeMockChild>();
	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);
	op->setChild(std::move(mock));

	auto vars = op->getOutputVariables();
	// Mock returns {"n"}, and CreateNode variable is "n" -> should deduplicate
	EXPECT_EQ(vars.size(), 1UL);
	EXPECT_EQ(vars[0], "n");
}

// Metadata: getOutputVariables without child
TEST_F(CreateNodeOperatorBranchTest, GetOutputVariables_NoChild) {
	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);

	auto vars = op->getOutputVariables();
	EXPECT_EQ(vars.size(), 1UL);
	EXPECT_EQ(vars[0], "n");
}

// Metadata: getOutputVariables with child that has different variable
TEST_F(CreateNodeOperatorBranchTest, GetOutputVariables_DifferentVariable) {
	auto mock = std::make_unique<CreateNodeMockChild>();
	// Mock returns {"n"}, but we create with variable "m"
	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateNodeOperator>(dm, "m", std::vector<std::string>{"Person"}, emptyProps);
	op->setChild(std::move(mock));

	auto vars = op->getOutputVariables();
	// Should have both "n" (from child) and "m"
	EXPECT_EQ(vars.size(), 2UL);
}

// Metadata: toString
TEST_F(CreateNodeOperatorBranchTest, ToString) {
	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);
	EXPECT_EQ(op->toString(), "CreateNode(var=n, labels=Person)");
}

// Metadata: getChildren with and without child
TEST_F(CreateNodeOperatorBranchTest, GetChildren) {
	std::unordered_map<std::string, PropertyValue> emptyProps;

	// Without child
	auto op1 = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);
	EXPECT_TRUE(op1->getChildren().empty());

	// With child
	auto mock = std::make_unique<CreateNodeMockChild>();
	auto *mockRaw = mock.get();
	auto op2 = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);
	op2->setChild(std::move(mock));
	auto children = op2->getChildren();
	ASSERT_EQ(children.size(), 1UL);
	EXPECT_EQ(children[0], mockRaw);
}

// open()/close() propagates to child
TEST_F(CreateNodeOperatorBranchTest, OpenCloseWithChild) {
	auto mock = std::make_unique<CreateNodeMockChild>();
	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);
	op->setChild(std::move(mock));

	// Should not crash
	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());
}

// open()/close() without child
TEST_F(CreateNodeOperatorBranchTest, OpenCloseWithoutChild) {
	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);

	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());
}

// Cover branch: nodeBuffer_.size() >= BATCH_SIZE (line 97)
// When pipeline accumulates >= 1000 records, it should flush mid-stream
TEST_F(CreateNodeOperatorBranchTest, PipelineMode_BatchSizeThresholdFlush) {
	// Create a mock child that returns > 1000 records in a single batch
	std::vector<Record> bigBatch;
	for (int i = 0; i < 1001; ++i) {
		Record r;
		r.setValue("x", int64_t(i));
		bigBatch.push_back(std::move(r));
	}

	auto mock = std::make_unique<CreateNodeMockChild>(std::vector<RecordBatch>{std::move(bigBatch)});

	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);
	op->setChild(std::move(mock));

	op->open();

	// First next() should return a batch due to BATCH_SIZE threshold being hit
	auto batch1 = op->next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_GE(batch1->size(), 1000UL);

	// There should be remaining records
	size_t total = batch1->size();
	while (auto batch = op->next()) {
		total += batch->size();
	}
	EXPECT_EQ(total, 1001UL);

	op->close();
}

// Cover branch: Source mode with non-empty properties (performSingleCreate with props)
TEST_F(CreateNodeOperatorBranchTest, SourceMode_PerformSingleCreate_WithProps) {
	std::unordered_map<std::string, PropertyValue> props = {
		{"name", PropertyValue(std::string("Bob"))},
	};
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Worker"}, props);

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto node = (*batch)[0].getNode("n");
	ASSERT_TRUE(node.has_value());

	// Verify properties stored
	auto storedProps = dm->getNodeProperties(node->getId());
	EXPECT_TRUE(storedProps.contains("name"));

	// Second call returns nullopt (executed_ = true)
	EXPECT_FALSE(op->next().has_value());

	op->close();
}

// Pipeline mode: multiple batches from child where some are empty
TEST_F(CreateNodeOperatorBranchTest, PipelineMode_MultipleEmptyBatches) {
	RecordBatch emptyBatch1;
	RecordBatch emptyBatch2;
	Record r1;
	r1.setValue("x", int64_t(1));
	RecordBatch realBatch = {r1};

	auto mock = std::make_unique<CreateNodeMockChild>(
		std::vector<RecordBatch>{emptyBatch1, emptyBatch2, realBatch});

	std::unordered_map<std::string, PropertyValue> emptyProps;
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Person"}, emptyProps);
	op->setChild(std::move(mock));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_GE(batch->size(), 1UL);

	op->close();
}

// Pipeline mode with properties set on created nodes
TEST_F(CreateNodeOperatorBranchTest, PipelineMode_WithProperties) {
	Record r1;
	r1.setValue("x", int64_t(1));

	auto mock = std::make_unique<CreateNodeMockChild>(std::vector<RecordBatch>{{r1}});

	std::unordered_map<std::string, PropertyValue> props = {
		{"status", PropertyValue(std::string("active"))}
	};
	auto op = std::make_unique<CreateNodeOperator>(dm, "n", std::vector<std::string>{"Task"}, props);
	op->setChild(std::move(mock));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	// Verify the created node has a valid ID
	auto node = (*batch)[0].getNode("n");
	ASSERT_TRUE(node.has_value());
	EXPECT_TRUE(node->getId() > 0);

	op->close();
}
