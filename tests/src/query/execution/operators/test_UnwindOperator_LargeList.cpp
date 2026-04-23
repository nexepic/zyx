/**
 * @file test_UnwindOperator_LargeList.cpp
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

#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "graph/query/execution/operators/UnwindOperator.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"

using namespace graph::query::execution;
using namespace graph::query::execution::operators;
using namespace graph::query::expressions;
using namespace graph;

// ============================================================================
// Mock child operator
// ============================================================================

class UnwindBranchMockChild : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t currentIndex = 0;
	bool opened = false;
	bool closed = false;

	explicit UnwindBranchMockChild(std::vector<RecordBatch> data = {})
		: batches(std::move(data)) {}

	void open() override { opened = true; currentIndex = 0; }

	std::optional<RecordBatch> next() override {
		if (currentIndex >= batches.size())
			return std::nullopt;
		return batches[currentIndex++];
	}

	void close() override { closed = true; }

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		return {"n"};
	}

	[[nodiscard]] std::string toString() const override { return "UnwindBranchMockChild"; }
};

// ============================================================================
// Tests targeting uncovered branches
// ============================================================================

class UnwindOperatorLargeListTest : public ::testing::Test {};

// Test: Source mode with large list exceeding DEFAULT_BATCH_SIZE
// This exercises the while loop batch boundary in Case A
TEST_F(UnwindOperatorLargeListTest, SourceMode_LargeListExceedsBatchSize) {
	// Create a list larger than DEFAULT_BATCH_SIZE (1000)
	std::vector<PropertyValue> bigList;
	bigList.reserve(1500);
	for (int64_t i = 0; i < 1500; ++i) {
		bigList.emplace_back(i);
	}

	auto op = std::make_unique<UnwindOperator>(nullptr, "x", bigList);
	op->open();

	// First batch should have exactly DEFAULT_BATCH_SIZE (1000) records
	auto batch1 = op->next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1000UL);

	// Verify first element
	auto v0 = (*batch1)[0].getValue("x");
	ASSERT_TRUE(v0.has_value());
	EXPECT_EQ(std::get<int64_t>(v0->getVariant()), 0);

	// Verify last element in first batch
	auto v999 = (*batch1)[999].getValue("x");
	ASSERT_TRUE(v999.has_value());
	EXPECT_EQ(std::get<int64_t>(v999->getVariant()), 999);

	// Second batch should have remaining 500 records
	auto batch2 = op->next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 500UL);

	// Verify first element of second batch
	auto v1000 = (*batch2)[0].getValue("x");
	ASSERT_TRUE(v1000.has_value());
	EXPECT_EQ(std::get<int64_t>(v1000->getVariant()), 1000);

	// Should be exhausted now
	auto batch3 = op->next();
	EXPECT_FALSE(batch3.has_value());

	op->close();
}

// Note: Empty batch from child is not tested here because UnwindOperator
// does not handle empty batches from child gracefully (it would access
// index 0 of an empty vector). This is a known source code limitation.

// Test: Pipeline mode with large list that exceeds batch size per record
// This hits the batch size boundary inside the inner while loop (line 130)
TEST_F(UnwindOperatorLargeListTest, PipelineMode_LargeListPerRecord) {
	Record r1;
	r1.setValue("n", int64_t(1));

	auto mock = std::make_unique<UnwindBranchMockChild>(
		std::vector<RecordBatch>{{r1}});

	// Create a list larger than DEFAULT_BATCH_SIZE
	std::vector<PropertyValue> bigList;
	bigList.reserve(1500);
	for (int64_t i = 0; i < 1500; ++i) {
		bigList.emplace_back(i);
	}

	auto op = std::make_unique<UnwindOperator>(std::move(mock), "x", bigList);
	op->open();

	// First batch should be DEFAULT_BATCH_SIZE records
	auto batch1 = op->next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1000UL);

	// Second batch should have remaining 500
	auto batch2 = op->next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 500UL);

	// Should be exhausted (upstream returns nullopt, outputBatch is empty)
	auto batch3 = op->next();
	EXPECT_FALSE(batch3.has_value());

	op->close();
}

// Test: Pipeline mode with multiple records in same batch and literal list
// Tests the branch where currentList_ is non-empty on subsequent records (line 123)
TEST_F(UnwindOperatorLargeListTest, PipelineMode_LiteralListMultipleRecords) {
	Record r1, r2, r3;
	r1.setValue("n", int64_t(1));
	r2.setValue("n", int64_t(2));
	r3.setValue("n", int64_t(3));

	auto mock = std::make_unique<UnwindBranchMockChild>(
		std::vector<RecordBatch>{{r1, r2, r3}});

	std::vector<PropertyValue> list = {PropertyValue(std::string("a")), PropertyValue(std::string("b"))};
	auto op = std::make_unique<UnwindOperator>(std::move(mock), "x", list);
	op->open();

	// 3 records x 2 list items = 6 total
	size_t total = 0;
	while (auto batch = op->next()) {
		total += batch->size();
	}
	EXPECT_EQ(total, 6UL);

	op->close();
}

// Test: Pipeline mode where upstream exhausts with non-empty output batch
// This exercises line 98-99: outputBatch is not empty when upstream returns nullopt
TEST_F(UnwindOperatorLargeListTest, PipelineMode_UpstreamExhaustedWithPartialOutput) {
	Record r1;
	r1.setValue("n", int64_t(42));

	auto mock = std::make_unique<UnwindBranchMockChild>(
		std::vector<RecordBatch>{{r1}});

	std::vector<PropertyValue> list = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2))};
	auto op = std::make_unique<UnwindOperator>(std::move(mock), "x", list);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL);

	auto x0 = (*batch)[0].getValue("x");
	ASSERT_TRUE(x0.has_value());
	EXPECT_EQ(std::get<int64_t>(x0->getVariant()), 1);

	auto x1 = (*batch)[1].getValue("x");
	ASSERT_TRUE(x1.has_value());
	EXPECT_EQ(std::get<int64_t>(x1->getVariant()), 2);

	// Now truly exhausted
	EXPECT_FALSE(op->next().has_value());

	op->close();
}

// Test: Pipeline mode with expression-based list, multiple batches from child
// Exercises needsEval_ reset on new child batch (line 106)
TEST_F(UnwindOperatorLargeListTest, PipelineMode_ExpressionMultipleBatches) {
	Record r1, r2;
	r1.setValue("n", int64_t(1));
	r2.setValue("n", int64_t(2));

	auto mock = std::make_unique<UnwindBranchMockChild>(
		std::vector<RecordBatch>{{r1}, {r2}});

	std::vector<PropertyValue> innerList = {PropertyValue(int64_t(10)), PropertyValue(int64_t(20))};
	PropertyValue listVal(innerList);
	auto listExpr = std::make_shared<ListLiteralExpression>(listVal);

	auto op = std::make_unique<UnwindOperator>(std::move(mock), "x", listExpr);
	op->open();

	size_t total = 0;
	while (auto batch = op->next()) {
		total += batch->size();
	}
	// 2 records x 2 list items = 4
	EXPECT_EQ(total, 4UL);

	op->close();
}

// Test: Source mode with expression - second call after already evaluated
TEST_F(UnwindOperatorLargeListTest, SourceMode_ExpressionSecondCallReturnsNullopt) {
	std::vector<PropertyValue> innerList = {PropertyValue(int64_t(1))};
	PropertyValue listVal(innerList);
	auto listExpr = std::make_shared<ListLiteralExpression>(listVal);

	auto op = std::make_unique<UnwindOperator>(nullptr, "x", listExpr);
	op->open();

	auto batch1 = op->next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1UL);

	auto batch2 = op->next();
	EXPECT_FALSE(batch2.has_value());

	op->close();
}

// Test: Source mode literal list - reopen and run again
TEST_F(UnwindOperatorLargeListTest, SourceMode_LiteralReopen) {
	std::vector<PropertyValue> list = {PropertyValue(int64_t(1))};
	auto op = std::make_unique<UnwindOperator>(nullptr, "x", list);

	// First run
	op->open();
	auto batch1 = op->next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_FALSE(op->next().has_value());

	// Reopen
	op->open();
	auto batch2 = op->next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 1UL);

	op->close();
}

// Test: Pipeline mode transitioning between child batches
TEST_F(UnwindOperatorLargeListTest, PipelineMode_TransitionBetweenBatches) {
	Record r1, r2;
	r1.setValue("n", int64_t(1));
	r2.setValue("n", int64_t(2));

	auto mock = std::make_unique<UnwindBranchMockChild>(
		std::vector<RecordBatch>{{r1}, {r2}});

	std::vector<PropertyValue> list = {PropertyValue(std::string("x"))};
	auto op = std::make_unique<UnwindOperator>(std::move(mock), "val", list);
	op->open();

	size_t total = 0;
	while (auto batch = op->next()) {
		total += batch->size();
		for (const auto &rec : *batch) {
			auto v = rec.getValue("val");
			ASSERT_TRUE(v.has_value());
			EXPECT_EQ(std::get<std::string>(v->getVariant()), "x");
		}
	}
	EXPECT_EQ(total, 2UL);

	op->close();
}
