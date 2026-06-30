/**
 * @file test_SortOperator_Ordering.cpp
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

#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <vector>

#include "graph/query/execution/operators/SortOperator.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/concurrent/ThreadPool.hpp"

using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

// Mock Operator for testing
class SortMockOperator : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t current_index = 0;

	explicit SortMockOperator(std::vector<RecordBatch> data = {}) : batches(std::move(data)) {}

	void open() override { current_index = 0; }
	std::optional<RecordBatch> next() override {
		if (current_index >= batches.size()) {
			return std::nullopt;
		}
		return batches[current_index++];
	}
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"x"}; }
	[[nodiscard]] std::string toString() const override { return "SortMock"; }
};

class SortOperatorOrderingTest : public ::testing::Test {
protected:
	void SetUp() override {}
	void TearDown() override {}
};

/**
 * Test SortOperator open/close with null child
 * Covers: if (child_) false branch in open() and close()
 */
TEST_F(SortOperatorOrderingTest, OpenCloseWithNullChild) {
	std::vector<SortItem> sortItems;
	auto op = std::make_unique<SortOperator>(nullptr, sortItems);
	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());
}

/**
 * Test SortOperator toString with null expression in sort item
 * Covers: item.expression null branch in toString()
 */
TEST_F(SortOperatorOrderingTest, ToStringWithNullExpression) {
	SortItem item;
	item.expression = nullptr;
	item.ascending = true;

	std::vector<SortItem> sortItems = {item};
	auto op = std::make_unique<SortOperator>(nullptr, sortItems);

	std::string str = op->toString();
	EXPECT_TRUE(str.find("Sort(") != std::string::npos);
	EXPECT_TRUE(str.find("ASC") != std::string::npos);
}

/**
 * Test SortOperator toString with DESC sort item
 * Covers: item.ascending false branch in toString()
 */
TEST_F(SortOperatorOrderingTest, ToStringWithDescending) {
	auto expr = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	SortItem item(expr, false); // descending

	std::vector<SortItem> sortItems = {item};
	auto op = std::make_unique<SortOperator>(nullptr, sortItems);

	std::string str = op->toString();
	EXPECT_TRUE(str.find("DESC") != std::string::npos);
}

/**
 * Test SortOperator toString with multiple sort items (comma separator)
 * Covers: i < sortItems_.size() - 1 true branch (comma insertion)
 */
TEST_F(SortOperatorOrderingTest, ToStringWithMultipleSortItems) {
	auto expr1 = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	auto expr2 = std::make_shared<graph::query::expressions::VariableReferenceExpression>("y");
	SortItem item1(expr1, true);
	SortItem item2(expr2, false);

	std::vector<SortItem> sortItems = {item1, item2};
	auto op = std::make_unique<SortOperator>(nullptr, sortItems);

	std::string str = op->toString();
	EXPECT_TRUE(str.find(", ") != std::string::npos);
	EXPECT_TRUE(str.find("ASC") != std::string::npos);
	EXPECT_TRUE(str.find("DESC") != std::string::npos);
}

TEST_F(SortOperatorOrderingTest, ToStringWithTopNOnly) {
	auto op = std::make_unique<SortOperator>(nullptr, std::vector<SortItem>{}, 3);

	EXPECT_EQ(op->toString(), "Sort(LIMIT 3)");
}

/**
 * Test SortOperator sorting in descending order
 * Covers: item.ascending false branch in performSort() (valA > valB)
 */
TEST_F(SortOperatorOrderingTest, SortDescending) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	r2.setValue("x", PropertyValue(static_cast<int64_t>(3)));
	r3.setValue("x", PropertyValue(static_cast<int64_t>(2)));

	auto mock = new SortMockOperator({{r1, r2, r3}});

	auto expr = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	SortItem item(expr, false); // descending
	std::vector<SortItem> sortItems = {item};

	auto op = std::make_unique<SortOperator>(
		std::unique_ptr<PhysicalOperator>(mock), sortItems);

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 3UL);

	// Verify descending order: 3, 2, 1
	auto val0 = (*batch)[0].getValue("x");
	auto val1 = (*batch)[1].getValue("x");
	auto val2 = (*batch)[2].getValue("x");
	ASSERT_TRUE(val0.has_value());
	ASSERT_TRUE(val1.has_value());
	ASSERT_TRUE(val2.has_value());
	EXPECT_EQ(std::get<int64_t>(val0->getVariant()), 3);
	EXPECT_EQ(std::get<int64_t>(val1->getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(val2->getVariant()), 1);

	op->close();
}

/**
 * Test SortOperator with empty input
 * Covers: exhausted input returns nullopt
 */
TEST_F(SortOperatorOrderingTest, EmptyInput) {
	auto mock = new SortMockOperator({});

	auto expr = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	SortItem item(expr, true);
	std::vector<SortItem> sortItems = {item};

	auto op = std::make_unique<SortOperator>(
		std::unique_ptr<PhysicalOperator>(mock), sortItems);

	op->open();
	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

/**
 * Test SortOperator ascending sort (confirms ASC path)
 * Covers: item.ascending true branch in performSort() (valA < valB)
 */
TEST_F(SortOperatorOrderingTest, SortAscending) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(3)));
	r2.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	r3.setValue("x", PropertyValue(static_cast<int64_t>(2)));

	auto mock = new SortMockOperator({{r1, r2, r3}});

	auto expr = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	SortItem item(expr, true); // ascending
	std::vector<SortItem> sortItems = {item};

	auto op = std::make_unique<SortOperator>(
		std::unique_ptr<PhysicalOperator>(mock), sortItems);

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 3UL);

	// Verify ascending order: 1, 2, 3
	auto val0 = (*batch)[0].getValue("x");
	auto val1 = (*batch)[1].getValue("x");
	auto val2 = (*batch)[2].getValue("x");
	ASSERT_TRUE(val0.has_value());
	ASSERT_TRUE(val1.has_value());
	ASSERT_TRUE(val2.has_value());
	EXPECT_EQ(std::get<int64_t>(val0->getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(val1->getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(val2->getVariant()), 3);

	op->close();
}

/**
 * Test SortOperator with null expression in sort item during sort
 * Covers: item.expression null branch in performSort() (both values stay default)
 */
TEST_F(SortOperatorOrderingTest, SortWithNullExpression) {
	Record r1, r2;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	r2.setValue("x", PropertyValue(static_cast<int64_t>(2)));

	auto mock = new SortMockOperator({{r1, r2}});

	SortItem item;
	item.expression = nullptr;
	item.ascending = true;
	std::vector<SortItem> sortItems = {item};

	auto op = std::make_unique<SortOperator>(
		std::unique_ptr<PhysicalOperator>(mock), sortItems);

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL);

	op->close();
}

/**
 * Test SortOperator with equal values (falls through to next sort key)
 * Covers: valA == valB branch (continue to next sort key) and return false (strictly equal)
 */
TEST_F(SortOperatorOrderingTest, SortWithEqualValues) {
	Record r1, r2;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	r1.setValue("y", PropertyValue(static_cast<int64_t>(2)));
	r2.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	r2.setValue("y", PropertyValue(static_cast<int64_t>(1)));

	auto mock = new SortMockOperator({{r1, r2}});

	auto exprX = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	auto exprY = std::make_shared<graph::query::expressions::VariableReferenceExpression>("y");
	SortItem item1(exprX, true);
	SortItem item2(exprY, true);
	std::vector<SortItem> sortItems = {item1, item2};

	auto op = std::make_unique<SortOperator>(
		std::unique_ptr<PhysicalOperator>(mock), sortItems);

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL);

	// Both have x=1, so sorted by y: y=1 first, y=2 second
	auto val0 = (*batch)[0].getValue("y");
	auto val1 = (*batch)[1].getValue("y");
	ASSERT_TRUE(val0.has_value());
	ASSERT_TRUE(val1.has_value());
	EXPECT_EQ(std::get<int64_t>(val0->getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(val1->getVariant()), 2);

	op->close();
}

/**
 * Test SortOperator with parallel sort path (threadPool_ + large dataset)
 * Covers: the parallel chunk sort + k-way merge branch in performSort()
 */
TEST_F(SortOperatorOrderingTest, ParallelSort_LargeDataset) {
	// Build dataset above PARALLEL_SORT_THRESHOLD (8192)
	constexpr size_t N = 20001;
	RecordBatch batch;
	batch.reserve(N);
	for (size_t i = 0; i < N; ++i) {
		Record r;
		r.setValue("x", PropertyValue(static_cast<int64_t>(N - i)));
		batch.push_back(std::move(r));
	}

	auto mock = new SortMockOperator({std::move(batch)});

	auto expr = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	SortItem item(expr, true); // ascending
	std::vector<SortItem> sortItems = {item};

	auto op = std::make_unique<SortOperator>(
		std::unique_ptr<PhysicalOperator>(mock), sortItems);

	// Wire in a real thread pool with multiple threads
	graph::concurrent::ThreadPool pool(3);
	op->setThreadPool(&pool);

	op->open();

	// Collect all output batches
	std::vector<Record> allRecords;
	while (auto b = op->next()) {
		allRecords.insert(allRecords.end(),
			std::make_move_iterator(b->begin()),
			std::make_move_iterator(b->end()));
	}

	ASSERT_EQ(allRecords.size(), N);

	// Verify ascending order
	for (size_t i = 1; i < allRecords.size(); ++i) {
		auto prev = allRecords[i - 1].getValue("x");
		auto curr = allRecords[i].getValue("x");
		ASSERT_TRUE(prev.has_value() && curr.has_value());
		EXPECT_LE(std::get<int64_t>(prev->getVariant()),
				  std::get<int64_t>(curr->getVariant()));
	}

	op->close();
}

TEST_F(SortOperatorOrderingTest, TopNUsesParallelBatchRetentionForLargeBatch) {
	constexpr size_t N = 5000;
	RecordBatch batch;
	batch.reserve(N);
	for (size_t i = 0; i < N; ++i) {
		Record record;
		record.setValue("x", PropertyValue(static_cast<int64_t>(N - i)));
		batch.push_back(std::move(record));
	}

	auto expr = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	SortItem item(expr, true);
	auto op = std::make_unique<SortOperator>(
			std::make_unique<SortMockOperator>(std::vector<RecordBatch>{{std::move(batch)}}),
			std::vector<SortItem>{item},
			5);

	graph::concurrent::ThreadPool pool(4);
	op->setThreadPool(&pool);

	op->open();
	auto result = op->next();
	ASSERT_TRUE(result.has_value());
	ASSERT_EQ(result->size(), 5U);
	for (size_t i = 0; i < result->size(); ++i) {
		auto value = (*result)[i].getValue("x");
		ASSERT_TRUE(value.has_value());
		EXPECT_EQ(std::get<int64_t>(value->getVariant()), static_cast<int64_t>(i + 1));
	}
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

/**
 * Test SortOperator with single-threaded thread pool (falls back to sequential)
 * Covers: threadPool_->isSingleThreaded() == true branch
 */
TEST_F(SortOperatorOrderingTest, SingleThreadedPoolFallsBackToSequential) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(3)));
	r2.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	r3.setValue("x", PropertyValue(static_cast<int64_t>(2)));

	auto mock = new SortMockOperator({{r1, r2, r3}});

	auto expr = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	SortItem item(expr, true);
	std::vector<SortItem> sortItems = {item};

	auto op = std::make_unique<SortOperator>(
		std::unique_ptr<PhysicalOperator>(mock), sortItems);

	// Single-threaded pool
	graph::concurrent::ThreadPool pool(1);
	op->setThreadPool(&pool);

	op->open();
	auto result = op->next();
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->size(), 3UL);

	auto val0 = (*result)[0].getValue("x");
	EXPECT_EQ(std::get<int64_t>(val0->getVariant()), 1);

	op->close();
}

/**
 * Test SortOperator with multiple input batches
 * Covers: the while(true) loop in next() accumulating multiple batches
 */
TEST_F(SortOperatorOrderingTest, MultipleBatchesAccumulated) {
	RecordBatch batch1, batch2;
	Record r1, r2, r3, r4;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(4)));
	r2.setValue("x", PropertyValue(static_cast<int64_t>(2)));
	r3.setValue("x", PropertyValue(static_cast<int64_t>(3)));
	r4.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	batch1 = {r1, r2};
	batch2 = {r3, r4};

	auto mock = new SortMockOperator({batch1, batch2});

	auto expr = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	SortItem item(expr, true);
	std::vector<SortItem> sortItems = {item};

	auto op = std::make_unique<SortOperator>(
		std::unique_ptr<PhysicalOperator>(mock), sortItems);

	op->open();
	auto result = op->next();
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->size(), 4UL);

	// Verify sorted: 1, 2, 3, 4
	for (size_t i = 0; i < 4; ++i) {
		auto v = (*result)[i].getValue("x");
		EXPECT_EQ(std::get<int64_t>(v->getVariant()), static_cast<int64_t>(i + 1));
	}

	// Second call should return nullopt
	EXPECT_FALSE(op->next().has_value());

	op->close();
}

TEST_F(SortOperatorOrderingTest, TopNDescendingKeepsOnlyBestRows) {
	RecordBatch batch1, batch2;
	Record r1, r2, r3, r4, r5;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	r2.setValue("x", PropertyValue(static_cast<int64_t>(5)));
	r3.setValue("x", PropertyValue(static_cast<int64_t>(3)));
	r4.setValue("x", PropertyValue(static_cast<int64_t>(2)));
	r5.setValue("x", PropertyValue(static_cast<int64_t>(4)));
	batch1 = {r1, r2, r3};
	batch2 = {r4, r5};

	auto mock = new SortMockOperator({batch1, batch2});

	auto expr = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	SortItem item(expr, false);
	std::vector<SortItem> sortItems = {item};

	auto op = std::make_unique<SortOperator>(
		std::unique_ptr<PhysicalOperator>(mock), sortItems, 2);

	op->open();
	auto result = op->next();
	ASSERT_TRUE(result.has_value());
	ASSERT_EQ(result->size(), 2UL);

	auto first = (*result)[0].getValue("x");
	auto second = (*result)[1].getValue("x");
	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(std::get<int64_t>(first->getVariant()), 5);
	EXPECT_EQ(std::get<int64_t>(second->getVariant()), 4);
	EXPECT_FALSE(op->next().has_value());

	op->close();
}

TEST_F(SortOperatorOrderingTest, TopNLimitZeroDoesNotConsumeChild) {
	Record r1;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(1)));

	auto mock = new SortMockOperator({{r1}});
	auto *mockPtr = mock;

	auto expr = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	SortItem item(expr, true);
	std::vector<SortItem> sortItems = {item};

	auto op = std::make_unique<SortOperator>(
		std::unique_ptr<PhysicalOperator>(mock), sortItems, 0);

	op->open();
	EXPECT_FALSE(op->next().has_value());
	EXPECT_EQ(mockPtr->current_index, 0UL);

	op->close();
}

TEST_F(SortOperatorOrderingTest, DirectAccessSortsNodesEdgesAndMapProperties) {
	Record nodeWithProp;
	Node nodeHigh(20, 1);
	nodeHigh.addProperty("rank", PropertyValue(static_cast<int64_t>(2)));
	nodeWithProp.setNode("n", nodeHigh);

	Record nodeMissingProp;
	nodeMissingProp.setNode("n", Node(10, 1));

	auto nodeProp = std::make_shared<graph::query::expressions::VariableReferenceExpression>("n", "rank");
	SortItem byNodeProp(nodeProp, true);
	auto nodeOp = std::make_unique<SortOperator>(
		std::make_unique<SortMockOperator>(std::vector<RecordBatch>{{nodeWithProp, nodeMissingProp}}),
		std::vector<SortItem>{byNodeProp});
	nodeOp->open();
	auto nodeBatch = nodeOp->next();
	ASSERT_TRUE(nodeBatch.has_value());
	ASSERT_EQ(nodeBatch->size(), 2U);
	EXPECT_EQ((*nodeBatch)[0].getNode("n")->getId(), 10);
	EXPECT_EQ((*nodeBatch)[1].getNode("n")->getId(), 20);
	nodeOp->close();

	Record edgeWithProp;
	Edge edgeHigh(30, 1, 2, 9);
	edgeHigh.addProperty("rank", PropertyValue(static_cast<int64_t>(3)));
	edgeWithProp.setEdge("e", edgeHigh);

	Record edgeMissingProp;
	edgeMissingProp.setEdge("e", Edge(25, 1, 2, 9));

	auto edgeId = std::make_shared<graph::query::expressions::VariableReferenceExpression>("e");
	auto edgeProp = std::make_shared<graph::query::expressions::VariableReferenceExpression>("e", "rank");
	SortItem byEdgeId(edgeId, false);
	SortItem byEdgeProp(edgeProp, true);
	auto edgeOp = std::make_unique<SortOperator>(
		std::make_unique<SortMockOperator>(std::vector<RecordBatch>{{edgeMissingProp, edgeWithProp}}),
		std::vector<SortItem>{byEdgeId, byEdgeProp});
	edgeOp->open();
	auto edgeBatch = edgeOp->next();
	ASSERT_TRUE(edgeBatch.has_value());
	ASSERT_EQ(edgeBatch->size(), 2U);
	EXPECT_EQ((*edgeBatch)[0].getEdge("e")->getId(), 30);
	EXPECT_EQ((*edgeBatch)[1].getEdge("e")->getId(), 25);
	edgeOp->close();

	Record mapWithProp;
	mapWithProp.setValue("m", PropertyValue(PropertyValue::MapType{{"rank", PropertyValue(static_cast<int64_t>(1))}}));
	Record mapMissingProp;
	mapMissingProp.setValue("m", PropertyValue(PropertyValue::MapType{}));
	Record scalarValue;
	scalarValue.setValue("m", PropertyValue(static_cast<int64_t>(99)));
	auto mapProp = std::make_shared<graph::query::expressions::VariableReferenceExpression>("m", "rank");
	SortItem byMapProp(mapProp, true);
	auto mapOp = std::make_unique<SortOperator>(
		std::make_unique<SortMockOperator>(std::vector<RecordBatch>{{mapWithProp, mapMissingProp, scalarValue}}),
		std::vector<SortItem>{byMapProp});
	mapOp->open();
	auto mapBatch = mapOp->next();
	ASSERT_TRUE(mapBatch.has_value());
	ASSERT_EQ(mapBatch->size(), 3U);
	size_t defaultKeyRows = 0;
	for (size_t i = 0; i < 2; ++i) {
		auto value = (*mapBatch)[i].getValue("m");
		ASSERT_TRUE(value.has_value());
		if (value->getType() == PropertyType::MAP) {
			EXPECT_FALSE(value->getMap().contains("rank"));
		} else {
			EXPECT_EQ(value->getType(), PropertyType::INTEGER);
		}
		++defaultKeyRows;
	}
	EXPECT_EQ(defaultKeyRows, 2U);
	EXPECT_TRUE((*mapBatch)[2].getValue("m")->getMap().contains("rank"));
	mapOp->close();
}

TEST_F(SortOperatorOrderingTest, MissingDirectVariableSortsAsNull) {
	Record r;
	r.setValue("x", PropertyValue(static_cast<int64_t>(1)));

	auto missingVariable = std::make_shared<graph::query::expressions::VariableReferenceExpression>("missing");
	SortItem byMissing(missingVariable, true);
	auto op = std::make_unique<SortOperator>(
		std::make_unique<SortMockOperator>(std::vector<RecordBatch>{{r}}),
		std::vector<SortItem>{byMissing});

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1U);
	op->close();
}

TEST_F(SortOperatorOrderingTest, ParameterExpressionUsesQueryContextDuringSort) {
	Record low;
	low.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	Record high;
	high.setValue("x", PropertyValue(static_cast<int64_t>(2)));

	auto parameter = std::make_shared<graph::query::expressions::ParameterExpression>("rank");
	SortItem byParameter(parameter, true);
	auto op = std::make_unique<SortOperator>(
		std::make_unique<SortMockOperator>(std::vector<RecordBatch>{{high, low}}),
		std::vector<SortItem>{byParameter});

	graph::query::QueryContext context;
	context.parameters.emplace("rank", PropertyValue(static_cast<int64_t>(1)));
	op->setQueryContext(&context);

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2U);
	op->close();
}
