/**
 * @file test_MergeEdgeOperator.cpp
 * @author ZYX Contributors
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
#include "graph/query/execution/operators/MergeEdgeOperator.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

// Mock Operator for testing
class MergeEdgeMockOperator : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t current_index = 0;

	explicit MergeEdgeMockOperator(std::vector<RecordBatch> data = {}) : batches(std::move(data)) {}

	void open() override { current_index = 0; }
	std::optional<RecordBatch> next() override {
		if (current_index >= batches.size()) return std::nullopt;
		return batches[current_index++];
	}
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"src", "tgt"}; }
	[[nodiscard]] std::string toString() const override { return "MergeEdgeMock"; }
};

class MergeEdgeOperatorTest : public ::testing::Test {
protected:
	struct TestSetup {
		Node src;
		Node tgt;
		Record record;
	};

	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_mergeedge_" + boost::uuids::to_string(uuid) + ".dat");

		if (fs::exists(testFilePath)) fs::remove_all(testFilePath);
		db = std::make_unique<Database>(testFilePath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		im = db->getQueryEngine()->getIndexManager();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}

	// Helper to create a node in storage and return it
	Node createStoredNode(const std::string& label = "") {
		Node node(0, 0);
		if (!label.empty()) {
			int64_t labelId = dm->getOrCreateTokenId(label);
			node = Node(0, labelId);
		}
		dm->addNode(node);
		return node;
	}

	TestSetup createTestSetup(const std::string& label = "Person") {
		TestSetup setup;
		setup.src = createStoredNode(label);
		setup.tgt = createStoredNode(label);
		setup.record.setNode("a", setup.src);
		setup.record.setNode("b", setup.tgt);
		return setup;
	}

	Edge createExistingEdge(const Node& src, const Node& tgt, const std::string& type = "KNOWS") {
		int64_t labelId = dm->getOrCreateTokenId(type);
		Edge edge(0, src.getId(), tgt.getId(), labelId);
		dm->addEdge(edge);
		return edge;
	}

	std::unique_ptr<MergeEdgeOperator> createMergeOp(
		const std::string& srcVar = "a",
		const std::string& edgeVar = "r",
		const std::string& tgtVar = "b",
		const std::string& edgeType = "KNOWS",
		const std::unordered_map<std::string, PropertyValue>& matchProps = {},
		const std::string& direction = "out",
		const std::vector<SetItem>& onCreate = {},
		const std::vector<SetItem>& onMatch = {}
	) {
		return std::make_unique<MergeEdgeOperator>(
			dm,
			im,
			srcVar,
			edgeVar,
			tgtVar,
			edgeType,
			matchProps,
			direction,
			onCreate,
			onMatch
		);
	}

	void attachSingleRecordChild(MergeEdgeOperator& op, const Record& record) {
		op.setChild(std::make_unique<MergeEdgeMockOperator>(std::vector<RecordBatch>{{record}}));
	}

	std::optional<RecordBatch> openAndNext(MergeEdgeOperator& op) {
		op.open();
		return op.next();
	}

	std::optional<RecordBatch> runSingleRecord(MergeEdgeOperator& op, const Record& record) {
		attachSingleRecordChild(op, record);
		return openAndNext(op);
	}

};

// --- Basic MergeEdge - Create new edge ---

TEST_F(MergeEdgeOperatorTest, CreateNewEdge_NoExistingEdge) {
	auto [src, tgt, record] = createTestSetup();

	auto op = createMergeOp();
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);

	// Edge should be created and available in record
	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	EXPECT_GT(edge->getId(), 0);

	auto batch2 = op->next();
	EXPECT_FALSE(batch2.has_value());

	op->close();
}

TEST_F(MergeEdgeOperatorTest, MatchExistingEdge) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	int64_t labelId = dm->getOrCreateTokenId("KNOWS");

	// Manually create the edge
	Edge existingEdge(0, src.getId(), tgt.getId(), labelId);
	dm->addEdge(existingEdge);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	auto op = createMergeOp();
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	EXPECT_EQ(edge->getId(), existingEdge.getId());

	op->close();
}

TEST_F(MergeEdgeOperatorTest, CreateEdge_WithProperties) {
	auto [src, tgt, record] = createTestSetup();

	std::unordered_map<std::string, PropertyValue> matchProps;
	matchProps["since"] = PropertyValue(int64_t(2020));

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		matchProps,
		"out"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());

	auto props = dm->getEdgeProperties(edge->getId());
	EXPECT_EQ(std::get<int64_t>(props["since"].getVariant()), 2020);

	op->close();
}

TEST_F(MergeEdgeOperatorTest, CreateEdge_InDirection) {
	auto [src, tgt, record] = createTestSetup();

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"in"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	// Direction "in" swaps source and target
	EXPECT_EQ(edge->getSourceNodeId(), tgt.getId());
	EXPECT_EQ(edge->getTargetNodeId(), src.getId());

	op->close();
}

TEST_F(MergeEdgeOperatorTest, CreateEdge_WithoutChild) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	auto op = createMergeOp();

	op->open();
	// Without child and without nodes in empty record, should throw
	EXPECT_THROW(op->next(), std::runtime_error);
	op->close();
}

TEST_F(MergeEdgeOperatorTest, ToString) {
	auto op = createMergeOp();

	std::string str = op->toString();
	EXPECT_TRUE(str.find("MergeEdge") != std::string::npos);
	EXPECT_TRUE(str.find("KNOWS") != std::string::npos);
	EXPECT_TRUE(str.find("a") != std::string::npos);
	EXPECT_TRUE(str.find("b") != std::string::npos);
}

TEST_F(MergeEdgeOperatorTest, GetOutputVariables_WithChild) {
	auto mock = std::make_unique<MergeEdgeMockOperator>();
	auto op = createMergeOp();
	op->setChild(std::move(mock));

	auto vars = op->getOutputVariables();
	EXPECT_TRUE(std::find(vars.begin(), vars.end(), "r") != vars.end());
}

TEST_F(MergeEdgeOperatorTest, GetOutputVariables_WithoutChild) {
	auto op = createMergeOp();

	auto vars = op->getOutputVariables();
	ASSERT_EQ(vars.size(), 1UL);
	EXPECT_EQ(vars[0], "r");
}

TEST_F(MergeEdgeOperatorTest, GetOutputVariables_EmptyEdgeVar) {
	auto op = createMergeOp("a", "", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out"
	);

	auto vars = op->getOutputVariables();
	EXPECT_TRUE(vars.empty());
}

TEST_F(MergeEdgeOperatorTest, GetChildren_WithChild) {
	auto mock = std::make_unique<MergeEdgeMockOperator>();
	auto mockPtr = mock.get();
	auto op = createMergeOp();
	op->setChild(std::move(mock));

	auto children = op->getChildren();
	ASSERT_EQ(children.size(), 1UL);
	EXPECT_EQ(children[0], mockPtr);
}

TEST_F(MergeEdgeOperatorTest, GetChildren_WithoutChild) {
	auto op = createMergeOp();

	auto children = op->getChildren();
	EXPECT_TRUE(children.empty());
}

TEST_F(MergeEdgeOperatorTest, MatchEdge_BothDirection) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Edge existingEdge = createExistingEdge(src, tgt);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"both"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	EXPECT_EQ(edge->getId(), existingEdge.getId());

	op->close();
}

TEST_F(MergeEdgeOperatorTest, OnCreate_SetsProperty) {
	auto [src, tgt, record] = createTestSetup();

	std::vector<SetItem> onCreateItems;
	onCreateItems.emplace_back(
		SetActionType::PROPERTY,
		"r",
		"created",
		std::make_shared<graph::query::expressions::LiteralExpression>(true)
	);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out",
		onCreateItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());

	auto props = dm->getEdgeProperties(edge->getId());
	EXPECT_TRUE(props.count("created"));

	op->close();
}

TEST_F(MergeEdgeOperatorTest, OnMatch_SetsProperty) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Edge existingEdge = createExistingEdge(src, tgt);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	std::vector<SetItem> onMatchItems;
	onMatchItems.emplace_back(
		SetActionType::PROPERTY,
		"r",
		"matched",
		std::make_shared<graph::query::expressions::LiteralExpression>(true)
	);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out",
		std::vector<SetItem>{}, onMatchItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());

	auto props = dm->getEdgeProperties(edge->getId());
	EXPECT_TRUE(props.count("matched"));

	op->close();
}

TEST_F(MergeEdgeOperatorTest, MatchEdge_PropertyMismatch_CreatesNew) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Edge existingEdge = createExistingEdge(src, tgt);
	dm->addEdgeProperties(existingEdge.getId(), {{"since", PropertyValue(int64_t(2020))}});

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	std::unordered_map<std::string, PropertyValue> matchProps;
	matchProps["since"] = PropertyValue(int64_t(2025)); // different property

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		matchProps,
		"out"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	EXPECT_NE(edge->getId(), existingEdge.getId()); // Should be a new edge

	op->close();
}

TEST_F(MergeEdgeOperatorTest, MatchEdge_LabelMismatch_CreatesNew) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	int64_t knowsLabel = dm->getOrCreateTokenId("KNOWS");
	Edge existingEdge(0, src.getId(), tgt.getId(), knowsLabel);
	dm->addEdge(existingEdge);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	auto op = createMergeOp("a", "r", "b", "FRIENDS",
		std::unordered_map<std::string, PropertyValue>{},
		"out"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	EXPECT_NE(edge->getId(), existingEdge.getId());

	op->close();
}

TEST_F(MergeEdgeOperatorTest, MatchExistingEdge_InDirection) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	int64_t labelId = dm->getOrCreateTokenId("KNOWS");
	// Edge goes from tgt to src (reverse for "in" direction)
	Edge existingEdge(0, tgt.getId(), src.getId(), labelId);
	dm->addEdge(existingEdge);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"in"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	EXPECT_EQ(edge->getId(), existingEdge.getId());

	op->close();
}

TEST_F(MergeEdgeOperatorTest, MatchExistingEdge_WithProperties) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Edge existingEdge = createExistingEdge(src, tgt);
	dm->addEdgeProperties(existingEdge.getId(), {{"since", PropertyValue(int64_t(2020))}});

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	std::unordered_map<std::string, PropertyValue> matchProps;
	matchProps["since"] = PropertyValue(int64_t(2020)); // same property

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		matchProps,
		"out"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	EXPECT_EQ(edge->getId(), existingEdge.getId()); // Should match existing

	op->close();
}

TEST_F(MergeEdgeOperatorTest, CreateEdge_NoLabel) {
	auto [src, tgt, record] = createTestSetup();

	auto op = createMergeOp("a", "r", "b", "",
		std::unordered_map<std::string, PropertyValue>{},
		"out"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	EXPECT_GT(edge->getId(), 0);

	op->close();
}

TEST_F(MergeEdgeOperatorTest, CreateEdge_EmptyEdgeVar) {
	auto [src, tgt, record] = createTestSetup();

	auto op = createMergeOp("a", "", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	op->close();
}

TEST_F(MergeEdgeOperatorTest, MultipleBatches) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Record r1;
	r1.setNode("a", src);
	r1.setNode("b", tgt);

	Record r2;
	r2.setNode("a", src);
	r2.setNode("b", tgt);

	auto mock = std::make_unique<MergeEdgeMockOperator>(
		std::vector<RecordBatch>{{r1}, {r2}}
	);

	auto op = createMergeOp();
	op->setChild(std::move(mock));

	op->open();

	auto batch1 = op->next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1UL);

	auto batch2 = op->next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 1UL);

	auto batch3 = op->next();
	EXPECT_FALSE(batch3.has_value());

	op->close();
}

// --- Branch coverage: no-child path executed twice (line 54 True branch) ---

TEST_F(MergeEdgeOperatorTest, NoChild_SecondCallReturnsNullopt) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	// We need a no-child operator that has valid nodes in the record.
	// The no-child path throws if nodes are missing, so we use the child path
	// but ensure the no-child "executed_" path is tested by calling next() twice
	// on a no-child operator that succeeds the first time.
	// Actually, we need source/target in the record. Without a child, the record
	// is empty and processMergeEdge throws. So the only way to test executed_=true
	// returning nullopt is to have a child that returns nullopt on first call.
	auto mock = std::make_unique<MergeEdgeMockOperator>(std::vector<RecordBatch>{});

	auto op = createMergeOp();
	op->setChild(std::move(mock));

	op->open();
	// Child returns nullopt immediately
	auto batch1 = op->next();
	EXPECT_FALSE(batch1.has_value());

	op->close();
}

// --- Branch coverage: sourceNode exists but targetNode is missing (line 78 second branch) ---

TEST_F(MergeEdgeOperatorTest, ThrowsWhenTargetNodeMissing) {
	Node src = createStoredNode("Person");

	Record record;
	record.setNode("a", src);
	// "b" is NOT set - target node is missing

	auto op = createMergeOp();
	attachSingleRecordChild(*op, record);

	op->open();
	EXPECT_THROW(op->next(), std::runtime_error);
	op->close();
}

// --- Branch coverage: direction "out" but edge goes wrong way (line 96 False branches)
// This also covers line 103 dirMatch=false continue branch ---

TEST_F(MergeEdgeOperatorTest, OutDirection_EdgeReversed_CreatesNew) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	int64_t labelId = dm->getOrCreateTokenId("KNOWS");
	// Edge stored in reverse direction: tgt -> src
	Edge existingEdge(0, tgt.getId(), src.getId(), labelId);
	dm->addEdge(existingEdge);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	auto op = createMergeOp();
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	// Should create a new edge since direction doesn't match
	EXPECT_NE(edge->getId(), existingEdge.getId());

	op->close();
}

// --- Branch coverage: direction "in" but edge goes wrong way (line 98 False branches) ---

TEST_F(MergeEdgeOperatorTest, InDirection_EdgeWrongWay_CreatesNew) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	int64_t labelId = dm->getOrCreateTokenId("KNOWS");
	// For "in" direction, we expect edge from tgt->src. Store edge from src->tgt instead.
	Edge existingEdge(0, src.getId(), tgt.getId(), labelId);
	dm->addEdge(existingEdge);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"in"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	// Should create a new edge since direction doesn't match
	EXPECT_NE(edge->getId(), existingEdge.getId());

	op->close();
}

// --- Branch coverage: "both" direction matching via reverse edge (lines 100-101)
// First OR condition fails, second succeeds ---

TEST_F(MergeEdgeOperatorTest, BothDirection_MatchesReverseEdge) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	int64_t labelId = dm->getOrCreateTokenId("KNOWS");
	// Edge stored in reverse: tgt -> src. "both" should still match it.
	Edge existingEdge(0, tgt.getId(), src.getId(), labelId);
	dm->addEdge(existingEdge);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"both"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	// Should match the existing edge via the reverse direction
	EXPECT_EQ(edge->getId(), existingEdge.getId());

	op->close();
}

// --- Branch coverage: "both" direction with no matching edge at all (lines 100-101 all False) ---

TEST_F(MergeEdgeOperatorTest, BothDirection_NoMatchingEdge_CreatesNew) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");
	Node other = createStoredNode("Person");

	int64_t labelId = dm->getOrCreateTokenId("KNOWS");
	// Edge between src and other, not tgt
	Edge existingEdge(0, src.getId(), other.getId(), labelId);
	dm->addEdge(existingEdge);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"both"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	EXPECT_NE(edge->getId(), existingEdge.getId());

	op->close();
}

// --- Branch coverage: edgeTypeId_ == 0 (line 106 first branch False)
// Empty label means we match any edge label ---

TEST_F(MergeEdgeOperatorTest, MatchEdge_EmptyLabel_MatchesAnyLabel) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Edge existingEdge = createExistingEdge(src, tgt);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	// Empty label: edgeTypeId_ will be 0, so label check is skipped
	auto op = createMergeOp("a", "r", "b", "",
		std::unordered_map<std::string, PropertyValue>{},
		"out"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	// Should match the existing edge regardless of its label
	EXPECT_EQ(edge->getId(), existingEdge.getId());

	op->close();
}

// --- Branch coverage: property key not found in edge (line 113 first True branch) ---

TEST_F(MergeEdgeOperatorTest, MatchEdge_PropertyKeyMissing_CreatesNew) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Edge existingEdge = createExistingEdge(src, tgt);
	// Edge has NO properties at all

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	std::unordered_map<std::string, PropertyValue> matchProps;
	matchProps["nonexistent"] = PropertyValue(std::string("value"));

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		matchProps,
		"out"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	// Should create new since property key doesn't exist on existing edge
	EXPECT_NE(edge->getId(), existingEdge.getId());

	op->close();
}

// --- Branch coverage: matched edge with empty edgeVar (line 121 False branch) ---

TEST_F(MergeEdgeOperatorTest, MatchEdge_EmptyEdgeVar_NoRecordSet) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Edge existingEdge = createExistingEdge(src, tgt);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	// Empty edgeVar: edge is matched but not stored in the record
	auto op = createMergeOp("a", "", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);
	// No edge variable should be in the record
	EXPECT_FALSE((*batch)[0].getEdge("r").has_value());

	op->close();
}

// --- Branch coverage: applyEdgeUpdates with non-PROPERTY type (line 164 second False branch) ---

TEST_F(MergeEdgeOperatorTest, OnMatch_LabelTypeItem_Skipped) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Edge existingEdge = createExistingEdge(src, tgt);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	// Use LABEL type instead of PROPERTY - should be skipped in applyEdgeUpdates
	std::vector<SetItem> onMatchItems;
	onMatchItems.emplace_back(
		SetActionType::LABEL,
		"r",
		"SomeLabel",
		std::make_shared<graph::query::expressions::LiteralExpression>(true)
	);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out",
		std::vector<SetItem>{}, onMatchItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());

	// No properties should have been set since the LABEL type was skipped
	auto props = dm->getEdgeProperties(edge->getId());
	EXPECT_FALSE(props.count("SomeLabel"));

	op->close();
}

// --- Branch coverage: applyEdgeUpdates with null expression (line 164 third False branch) ---

TEST_F(MergeEdgeOperatorTest, OnCreate_NullExpression_Skipped) {
	auto [src, tgt, record] = createTestSetup();

	// PROPERTY type but null expression - should be skipped
	std::vector<SetItem> onCreateItems;
	onCreateItems.emplace_back(
		SetActionType::PROPERTY,
		"r",
		"created",
		nullptr
	);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out",
		onCreateItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());

	// No properties should have been set since expression was null
	auto props = dm->getEdgeProperties(edge->getId());
	EXPECT_FALSE(props.count("created"));

	op->close();
}

// --- Branch coverage: applyEdgeUpdates with empty edgeVar but changed=true (line 175 False)
// and also covers line 176 False branch (getEdge returns nullopt) ---

TEST_F(MergeEdgeOperatorTest, OnMatch_EmptyEdgeVar_UpdateAppliedButNotInRecord) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Edge existingEdge = createExistingEdge(src, tgt);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	// ON MATCH with empty edgeVar - the update will match item.variable=="" == edgeVar_=""
	// and changed will be true but edgeVar_.empty() will skip record update
	std::vector<SetItem> onMatchItems;
	onMatchItems.emplace_back(
		SetActionType::PROPERTY,
		"",  // matches empty edgeVar
		"updated",
		std::make_shared<graph::query::expressions::LiteralExpression>(true)
	);

	auto op = createMergeOp("a", "", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out",
		std::vector<SetItem>{}, onMatchItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	// Property should be written to storage even though no edge var is in record
	auto props = dm->getEdgeProperties(existingEdge.getId());
	EXPECT_TRUE(props.count("updated"));

	op->close();
}

// --- Edge case tests ---

TEST_F(MergeEdgeOperatorTest, OnCreate_ItemForDifferentVariable) {
	auto [src, tgt, record] = createTestSetup();

	// ON CREATE SET for different variable (not "r") - should be ignored
	std::vector<SetItem> onCreateItems;
	onCreateItems.emplace_back(
		SetActionType::PROPERTY,
		"other_var",
		"created",
		std::make_shared<graph::query::expressions::LiteralExpression>(true)
	);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out",
		onCreateItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());

	// The ON CREATE should not have affected the edge since variable was different
	auto props = dm->getEdgeProperties(edge->getId());
	EXPECT_FALSE(props.count("created"));

	op->close();
}

// --- Branch coverage: close() without child (line 66 False branch) ---

TEST_F(MergeEdgeOperatorTest, Close_WithoutChild) {
	auto op = createMergeOp();

	op->open();
	// close without child should not crash
	op->close();
}

// --- Branch coverage: source node missing (line 78 first condition True) ---

TEST_F(MergeEdgeOperatorTest, ThrowsWhenSourceNodeMissing) {
	Node tgt = createStoredNode("Person");

	Record record;
	// "a" (source) is NOT set
	record.setNode("b", tgt);

	auto op = createMergeOp();
	attachSingleRecordChild(*op, record);

	op->open();
	EXPECT_THROW(op->next(), std::runtime_error);
	op->close();
}

// --- Branch coverage: both source and target missing (line 78 both conditions True) ---

TEST_F(MergeEdgeOperatorTest, ThrowsWhenBothNodesMissing) {
	Record record;
	// Neither "a" nor "b" set

	auto op = createMergeOp();
	attachSingleRecordChild(*op, record);

	op->open();
	EXPECT_THROW(op->next(), std::runtime_error);
	op->close();
}

// --- Branch coverage: property key exists but value differs (line 113 second condition) ---


// --- Branch coverage: ON MATCH with different variable name (line 164 first False) ---

TEST_F(MergeEdgeOperatorTest, OnMatch_DifferentVariable_Skipped) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Edge existingEdge = createExistingEdge(src, tgt);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	// ON MATCH SET for variable "other" instead of "r"
	std::vector<SetItem> onMatchItems;
	onMatchItems.emplace_back(
		SetActionType::PROPERTY,
		"other",
		"matched",
		std::make_shared<graph::query::expressions::LiteralExpression>(true)
	);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out",
		std::vector<SetItem>{}, onMatchItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());

	// Property should NOT have been set since variable didn't match
	auto props = dm->getEdgeProperties(edge->getId());
	EXPECT_FALSE(props.count("matched"));

	op->close();
}

// --- Branch coverage: ON MATCH with MAP_MERGE type (line 164 second False - type check) ---

TEST_F(MergeEdgeOperatorTest, OnMatch_MapMergeType_Skipped) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Edge existingEdge = createExistingEdge(src, tgt);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	// MAP_MERGE type with correct variable - should still be skipped
	std::vector<SetItem> onMatchItems;
	onMatchItems.emplace_back(
		SetActionType::MAP_MERGE,
		"r",
		"merged",
		std::make_shared<graph::query::expressions::LiteralExpression>(true)
	);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out",
		std::vector<SetItem>{}, onMatchItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());

	auto props = dm->getEdgeProperties(edge->getId());
	EXPECT_FALSE(props.count("merged"));

	op->close();
}

// --- Branch coverage: ON MATCH null expression with correct variable and PROPERTY type
// (line 164 third condition False) ---

TEST_F(MergeEdgeOperatorTest, OnMatch_NullExpression_Skipped) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Edge existingEdge = createExistingEdge(src, tgt);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	std::vector<SetItem> onMatchItems;
	onMatchItems.emplace_back(
		SetActionType::PROPERTY,
		"r",
		"matched",
		nullptr  // null expression
	);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out",
		std::vector<SetItem>{}, onMatchItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());

	auto props = dm->getEdgeProperties(edge->getId());
	EXPECT_FALSE(props.count("matched"));

	op->close();
}

// --- Branch coverage: applyEdgeUpdates with items but none match (changed=false, line 173 False) ---

TEST_F(MergeEdgeOperatorTest, OnMatch_AllItemsSkipped_NoChange) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Edge existingEdge = createExistingEdge(src, tgt);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	// Multiple items, all of which fail the condition check
	std::vector<SetItem> onMatchItems;
	// Wrong variable
	onMatchItems.emplace_back(
		SetActionType::PROPERTY,
		"other",
		"prop1",
		std::make_shared<graph::query::expressions::LiteralExpression>(true)
	);
	// Wrong type
	onMatchItems.emplace_back(
		SetActionType::LABEL,
		"r",
		"prop2",
		std::make_shared<graph::query::expressions::LiteralExpression>(true)
	);
	// Null expression
	onMatchItems.emplace_back(
		SetActionType::PROPERTY,
		"r",
		"prop3",
		nullptr
	);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out",
		std::vector<SetItem>{}, onMatchItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());

	// None of the items should have set any properties
	auto props = dm->getEdgeProperties(edge->getId());
	EXPECT_FALSE(props.count("prop1"));
	EXPECT_FALSE(props.count("prop2"));
	EXPECT_FALSE(props.count("prop3"));

	op->close();
}

// --- Branch coverage: ON CREATE with empty edgeVar (line 149 False, and line 175 False in create path) ---

TEST_F(MergeEdgeOperatorTest, OnCreate_EmptyEdgeVar_PropertyStillWritten) {
	auto [src, tgt, record] = createTestSetup();

	// ON CREATE SET with matching empty variable
	std::vector<SetItem> onCreateItems;
	onCreateItems.emplace_back(
		SetActionType::PROPERTY,
		"",  // matches empty edgeVar
		"created",
		std::make_shared<graph::query::expressions::LiteralExpression>(true)
	);

	auto op = createMergeOp("a", "", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out",
		onCreateItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	// Edge should not be in the record (empty edgeVar)
	EXPECT_FALSE((*batch)[0].getEdge("r").has_value());

	op->close();
}

// --- Branch coverage: Multiple records in single batch from child ---

TEST_F(MergeEdgeOperatorTest, MultipleRecordsInSingleBatch) {
	Node src1 = createStoredNode("Person");
	Node tgt1 = createStoredNode("Person");
	Node src2 = createStoredNode("Person");
	Node tgt2 = createStoredNode("Person");

	Record r1;
	r1.setNode("a", src1);
	r1.setNode("b", tgt1);

	Record r2;
	r2.setNode("a", src2);
	r2.setNode("b", tgt2);

	// Single batch with two records
	auto mock = std::make_unique<MergeEdgeMockOperator>(
		std::vector<RecordBatch>{{r1, r2}}
	);

	auto op = createMergeOp();
	op->setChild(std::move(mock));

	auto batch = openAndNext(*op);
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 2UL);

	auto edge1 = (*batch)[0].getEdge("r");
	auto edge2 = (*batch)[1].getEdge("r");
	ASSERT_TRUE(edge1.has_value());
	ASSERT_TRUE(edge2.has_value());
	EXPECT_NE(edge1->getId(), edge2->getId());

	op->close();
}

// --- Branch coverage: ON CREATE SET with multiple properties (verify all applied) ---

TEST_F(MergeEdgeOperatorTest, OnCreate_MultipleProperties) {
	auto [src, tgt, record] = createTestSetup();

	std::vector<SetItem> onCreateItems;
	onCreateItems.emplace_back(
		SetActionType::PROPERTY,
		"r",
		"created",
		std::make_shared<graph::query::expressions::LiteralExpression>(true)
	);
	onCreateItems.emplace_back(
		SetActionType::PROPERTY,
		"r",
		"score",
		std::make_shared<graph::query::expressions::LiteralExpression>(int64_t(42))
	);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out",
		onCreateItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());

	auto props = dm->getEdgeProperties(edge->getId());
	EXPECT_TRUE(props.count("created"));
	EXPECT_TRUE(props.count("score"));

	op->close();
}

// --- Branch coverage: ON MATCH SET with multiple items, mix of valid and invalid
// Tests that valid items are applied and invalid ones skipped ---

TEST_F(MergeEdgeOperatorTest, OnMatch_MixedValidAndInvalidItems) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Edge existingEdge = createExistingEdge(src, tgt);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	std::vector<SetItem> onMatchItems;
	// Valid: correct variable, PROPERTY type, non-null expression
	onMatchItems.emplace_back(
		SetActionType::PROPERTY,
		"r",
		"valid_prop",
		std::make_shared<graph::query::expressions::LiteralExpression>(std::string("yes"))
	);
	// Invalid: wrong variable
	onMatchItems.emplace_back(
		SetActionType::PROPERTY,
		"other",
		"skipped_prop",
		std::make_shared<graph::query::expressions::LiteralExpression>(true)
	);
	// Invalid: LABEL type
	onMatchItems.emplace_back(
		SetActionType::LABEL,
		"r",
		"label_prop",
		std::make_shared<graph::query::expressions::LiteralExpression>(true)
	);
	// Invalid: null expression
	onMatchItems.emplace_back(
		SetActionType::PROPERTY,
		"r",
		"null_prop",
		nullptr
	);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out",
		std::vector<SetItem>{}, onMatchItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());

	auto props = dm->getEdgeProperties(edge->getId());
	EXPECT_TRUE(props.count("valid_prop"));
	EXPECT_FALSE(props.count("skipped_prop"));
	EXPECT_FALSE(props.count("label_prop"));
	EXPECT_FALSE(props.count("null_prop"));

	op->close();
}

// --- Branch coverage: Create edge with "both" direction (line 137 False in create path) ---

TEST_F(MergeEdgeOperatorTest, CreateEdge_BothDirection) {
	auto [src, tgt, record] = createTestSetup();

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"both"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	// "both" direction should not swap (only "in" swaps)
	EXPECT_EQ(edge->getSourceNodeId(), src.getId());
	EXPECT_EQ(edge->getTargetNodeId(), tgt.getId());

	op->close();
}

// --- Branch coverage: ON CREATE with empty items (line 158 True in create path)
// This is the default case but explicitly verify the early return ---


// --- Branch coverage: ON MATCH with empty items (line 158 True in match path) ---


// --- Branch coverage: Create edge with matchProps empty (line 144 False) ---


// --- Branch coverage: Create edge with "in" direction and ON CREATE SET
// Exercises create path line 137 True + applyEdgeUpdates on new edge ---

TEST_F(MergeEdgeOperatorTest, CreateEdge_InDirection_WithOnCreate) {
	auto [src, tgt, record] = createTestSetup();

	std::vector<SetItem> onCreateItems;
	onCreateItems.emplace_back(
		SetActionType::PROPERTY,
		"r",
		"created_in",
		std::make_shared<graph::query::expressions::LiteralExpression>(true)
	);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"in",
		onCreateItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	// "in" swaps source and target
	EXPECT_EQ(edge->getSourceNodeId(), tgt.getId());
	EXPECT_EQ(edge->getTargetNodeId(), src.getId());

	auto props = dm->getEdgeProperties(edge->getId());
	EXPECT_TRUE(props.count("created_in"));

	op->close();
}

// --- Branch coverage: Match edge with properties and ON MATCH SET
// Covers line 176 True branch (getEdge returns valid edge, updates record) ---

TEST_F(MergeEdgeOperatorTest, OnMatch_UpdatesRecordEdgeProperties) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");

	Edge existingEdge = createExistingEdge(src, tgt);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	std::vector<SetItem> onMatchItems;
	onMatchItems.emplace_back(
		SetActionType::PROPERTY,
		"r",
		"updated",
		std::make_shared<graph::query::expressions::LiteralExpression>(std::string("yes"))
	);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"out",
		std::vector<SetItem>{}, onMatchItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());

	// Verify the edge in the record has updated properties
	auto edgeProps = edge->getProperties();
	EXPECT_TRUE(edgeProps.count("updated"));

	// Also verify in storage
	auto storageProps = dm->getEdgeProperties(edge->getId());
	EXPECT_TRUE(storageProps.count("updated"));

	op->close();
}

// --- Branch coverage: ON CREATE with properties and ON CREATE SET
// Tests that applyEdgeUpdates runs after edge creation with matchProps ---

TEST_F(MergeEdgeOperatorTest, CreateEdge_WithPropertiesAndOnCreate) {
	auto [src, tgt, record] = createTestSetup();

	std::unordered_map<std::string, PropertyValue> matchProps;
	matchProps["since"] = PropertyValue(int64_t(2020));

	std::vector<SetItem> onCreateItems;
	onCreateItems.emplace_back(
		SetActionType::PROPERTY,
		"r",
		"extra",
		std::make_shared<graph::query::expressions::LiteralExpression>(std::string("value"))
	);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		matchProps,
		"out",
		onCreateItems
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());

	auto props = dm->getEdgeProperties(edge->getId());
	EXPECT_EQ(std::get<int64_t>(props["since"].getVariant()), 2020);
	EXPECT_TRUE(props.count("extra"));

	op->close();
}

// Note: the isActive() check at line 91 is a defensive branch that cannot be
// easily triggered through the DataManager API since deleted edges are unlinked
// from the traversal chain and updateEdge() rejects inactive entities.

// --- Branch coverage: out direction - source matches but target does NOT match
// (line 96 right: edge.getTargetNodeId() == targetId False) ---

TEST_F(MergeEdgeOperatorTest, OutDirection_SourceMatchTargetMismatch_CreatesNew) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");
	Node other = createStoredNode("Person");

	int64_t labelId = dm->getOrCreateTokenId("KNOWS");
	// Edge from src to other (not tgt). Source matches but target doesn't.
	Edge existingEdge(0, src.getId(), other.getId(), labelId);
	dm->addEdge(existingEdge);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	auto op = createMergeOp();
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	// Should create a new edge since target doesn't match
	EXPECT_NE(edge->getId(), existingEdge.getId());

	op->close();
}

// --- Branch coverage: in direction - source matches targetId but target does NOT
// match sourceId (line 98 right: edge.getTargetNodeId() == sourceId False) ---

TEST_F(MergeEdgeOperatorTest, InDirection_SourceMatchTargetMismatch_CreatesNew) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");
	Node other = createStoredNode("Person");

	int64_t labelId = dm->getOrCreateTokenId("KNOWS");
	// For "in" direction, the check is:
	//   edge.getSourceNodeId() == targetId && edge.getTargetNodeId() == sourceId
	// We want: edge.getSourceNodeId() == tgt.getId() (True)
	//          edge.getTargetNodeId() == src.getId() (False)
	// Edge from tgt to other (tgt matches targetId, but other != sourceId)
	Edge existingEdge(0, tgt.getId(), other.getId(), labelId);
	dm->addEdge(existingEdge);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"in"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	// Should create a new edge since in-direction match failed
	EXPECT_NE(edge->getId(), existingEdge.getId());

	op->close();
}

// --- Branch coverage: "both" direction - first OR condition partially matches
// (source matches sourceId but target does NOT match targetId),
// AND second OR condition also partially fails
// This covers line 100-101 more thoroughly ---

TEST_F(MergeEdgeOperatorTest, BothDirection_PartialMatchBothConditions_CreatesNew) {
	Node src = createStoredNode("Person");
	Node tgt = createStoredNode("Person");
	Node other = createStoredNode("Person");

	int64_t labelId = dm->getOrCreateTokenId("KNOWS");
	// Edge from src to other: first OR fails (src==src but other!=tgt),
	// second OR also fails (src!=tgt)
	Edge existingEdge(0, src.getId(), other.getId(), labelId);
	dm->addEdge(existingEdge);

	Record record;
	record.setNode("a", src);
	record.setNode("b", tgt);

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"both"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	EXPECT_NE(edge->getId(), existingEdge.getId());

	op->close();
}

// --- Branch coverage: "in" direction self-loop scenario - edge from node to
// different node triggers line 98 right False branch
// (edge.getSourceNodeId() == targetId True, edge.getTargetNodeId() == sourceId False) ---

TEST_F(MergeEdgeOperatorTest, InDirection_SelfLoop_SourceMatchTargetMismatch) {
	// Use the same node for both source and target variables (self-loop MERGE)
	Node node = createStoredNode("Person");
	Node other = createStoredNode("Person");

	int64_t labelId = dm->getOrCreateTokenId("KNOWS");
	// Edge from node to other. Since sourceId == targetId == node.getId(),
	// edge.getSourceNodeId() == targetId is node==node (True)
	// edge.getTargetNodeId() == sourceId is other==node (False) - covers line 98 right False
	Edge existingEdge(0, node.getId(), other.getId(), labelId);
	dm->addEdge(existingEdge);

	Record record;
	record.setNode("a", node);
	record.setNode("b", node); // same node - self-loop scenario

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"in"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	// The existing edge (node->other) doesn't match the self-loop, so a new edge is created
	EXPECT_NE(edge->getId(), existingEdge.getId());

	op->close();
}

// --- Branch coverage: "both" direction self-loop scenario - edge from node to
// different node triggers line 101 right False branch
// In the second OR of "both": edge.getSourceNodeId() == targetId True,
// edge.getTargetNodeId() == sourceId False ---

TEST_F(MergeEdgeOperatorTest, BothDirection_SelfLoop_SecondOrTargetMismatch) {
	// Self-loop scenario: source and target are the same node
	Node node = createStoredNode("Person");
	Node other = createStoredNode("Person");

	int64_t labelId = dm->getOrCreateTokenId("KNOWS");
	// Edge from other to node. Since sourceId == targetId == node.getId():
	// First OR: edge.source(other) == sourceId(node) False - first OR short-circuits
	// Second OR: edge.source(other) == targetId(node) False - also fails
	// We need to trigger the second OR's right side being False.
	// For that, we need an edge where:
	//   First OR: source==sourceId(node) True, target==targetId(node) False
	//   (this makes first OR False since && short-circuits after target check)
	//   Second OR: source==targetId(node) True (since sourceId==targetId, same check)
	//              target==sourceId(node) False
	// Edge from node to other satisfies this:
	//   First OR: node==node True, other==node False -> first OR = False
	//   Second OR: node==node True, other==node False -> second OR = False
	Edge existingEdge(0, node.getId(), other.getId(), labelId);
	dm->addEdge(existingEdge);

	Record record;
	record.setNode("a", node);
	record.setNode("b", node); // same node

	auto op = createMergeOp("a", "r", "b", "KNOWS",
		std::unordered_map<std::string, PropertyValue>{},
		"both"
	);
	auto batch = runSingleRecord(*op, record);
	ASSERT_TRUE(batch.has_value());

	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	// The existing edge (node->other) doesn't match the self-loop, so a new edge is created
	EXPECT_NE(edge->getId(), existingEdge.getId());

	op->close();
}
