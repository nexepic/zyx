/**
 * @file test_CartesianProductOperator.cpp
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

#include "graph/query/execution/operators/CartesianProductOperator.hpp"
#include "graph/query/execution/Record.hpp"

using namespace graph::query::execution;
using namespace graph::query::execution::operators;
using namespace graph;

// ============================================================================
// Mock child operator
// ============================================================================

class CPMockOperator : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t currentIndex = 0;
	bool opened = false;
	bool closed = false;
	std::vector<std::string> outputVars;

	explicit CPMockOperator(std::vector<RecordBatch> data = {}, std::vector<std::string> vars = {"x"})
		: batches(std::move(data)), outputVars(std::move(vars)) {}

	void open() override { opened = true; currentIndex = 0; }

	std::optional<RecordBatch> next() override {
		if (currentIndex >= batches.size())
			return std::nullopt;
		return batches[currentIndex++];
	}

	void close() override { closed = true; }

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		return outputVars;
	}

	[[nodiscard]] std::string toString() const override { return "CPMock"; }
};

// ============================================================================
// CartesianProductOperator Tests
// ============================================================================

class CartesianProductOperatorTest : public ::testing::Test {};

// Basic cartesian product: 2 left x 2 right = 4 results
TEST_F(CartesianProductOperatorTest, BasicProduct) {
	Record l1, l2;
	l1.setValue("a", int64_t(1));
	l2.setValue("a", int64_t(2));

	Record r1, r2;
	r1.setValue("b", int64_t(10));
	r2.setValue("b", int64_t(20));

	auto left = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{l1, l2}}, std::vector<std::string>{"a"});
	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{r1, r2}}, std::vector<std::string>{"b"});

	auto op = std::make_unique<CartesianProductOperator>(std::move(left), std::move(right));
	op->open();

	size_t totalRecords = 0;
	while (auto batch = op->next()) {
		totalRecords += batch->size();
	}
	EXPECT_EQ(totalRecords, 4UL);

	op->close();
}

// Right side empty -> returns nullopt immediately (rightBuffer_.empty() branch)
TEST_F(CartesianProductOperatorTest, RightSideEmpty) {
	Record l1;
	l1.setValue("a", int64_t(1));

	auto left = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{l1}}, std::vector<std::string>{"a"});
	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{}, std::vector<std::string>{"b"});

	auto op = std::make_unique<CartesianProductOperator>(std::move(left), std::move(right));
	op->open();

	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

// Left side empty -> returns nullopt (left->next() returns nullopt)
TEST_F(CartesianProductOperatorTest, LeftSideEmpty) {
	Record r1;
	r1.setValue("b", int64_t(10));

	auto left = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{}, std::vector<std::string>{"a"});
	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{r1}}, std::vector<std::string>{"b"});

	auto op = std::make_unique<CartesianProductOperator>(std::move(left), std::move(right));
	op->open();

	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

// Both sides empty
TEST_F(CartesianProductOperatorTest, BothSidesEmpty) {
	auto left = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{}, std::vector<std::string>{"a"});
	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{}, std::vector<std::string>{"b"});

	auto op = std::make_unique<CartesianProductOperator>(std::move(left), std::move(right));
	op->open();

	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

// Single left record x single right record
TEST_F(CartesianProductOperatorTest, SingleLeftSingleRight) {
	Record l1;
	l1.setValue("a", int64_t(1));
	Record r1;
	r1.setValue("b", int64_t(10));

	auto left = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{l1}}, std::vector<std::string>{"a"});
	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{r1}}, std::vector<std::string>{"b"});

	auto op = std::make_unique<CartesianProductOperator>(std::move(left), std::move(right));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);

	// Verify merged record has both a and b
	auto aVal = (*batch)[0].getValue("a");
	ASSERT_TRUE(aVal.has_value());
	EXPECT_EQ(std::get<int64_t>(aVal->getVariant()), 1);

	auto bVal = (*batch)[0].getValue("b");
	ASSERT_TRUE(bVal.has_value());
	EXPECT_EQ(std::get<int64_t>(bVal->getVariant()), 10);

	// Second call should return nullopt
	auto batch2 = op->next();
	EXPECT_FALSE(batch2.has_value());

	op->close();
}

// Multiple left batches x right side
TEST_F(CartesianProductOperatorTest, MultipleLeftBatches) {
	Record l1, l2;
	l1.setValue("a", int64_t(1));
	l2.setValue("a", int64_t(2));

	Record r1;
	r1.setValue("b", int64_t(10));

	auto left = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{l1}, {l2}}, std::vector<std::string>{"a"});
	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{r1}}, std::vector<std::string>{"b"});

	auto op = std::make_unique<CartesianProductOperator>(std::move(left), std::move(right));
	op->open();

	size_t totalRecords = 0;
	while (auto batch = op->next()) {
		totalRecords += batch->size();
	}
	// 2 left records x 1 right record = 2
	EXPECT_EQ(totalRecords, 2UL);

	op->close();
}

// Right side has multiple batches (materialized together)
TEST_F(CartesianProductOperatorTest, MultipleRightBatches) {
	Record l1;
	l1.setValue("a", int64_t(1));

	Record r1, r2;
	r1.setValue("b", int64_t(10));
	r2.setValue("b", int64_t(20));

	auto left = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{l1}}, std::vector<std::string>{"a"});
	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{r1}, {r2}}, std::vector<std::string>{"b"});

	auto op = std::make_unique<CartesianProductOperator>(std::move(left), std::move(right));
	op->open();

	size_t totalRecords = 0;
	while (auto batch = op->next()) {
		totalRecords += batch->size();
	}
	// 1 left x 2 right (materialized from 2 batches) = 2
	EXPECT_EQ(totalRecords, 2UL);

	op->close();
}

// open()/close() propagates to children
TEST_F(CartesianProductOperatorTest, OpenClosePropagates) {
	auto left = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{}, std::vector<std::string>{"a"});
	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{}, std::vector<std::string>{"b"});
	auto *leftRaw = left.get();
	auto *rightRaw = right.get();

	auto op = std::make_unique<CartesianProductOperator>(std::move(left), std::move(right));

	EXPECT_FALSE(leftRaw->opened);
	EXPECT_FALSE(rightRaw->opened);

	op->open();
	EXPECT_TRUE(leftRaw->opened);
	EXPECT_TRUE(rightRaw->opened);

	op->close();
	EXPECT_TRUE(leftRaw->closed);
	EXPECT_TRUE(rightRaw->closed);
}

// getOutputVariables merges left and right
TEST_F(CartesianProductOperatorTest, GetOutputVariables) {
	auto left = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{}, std::vector<std::string>{"a", "b"});
	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{}, std::vector<std::string>{"c"});

	auto op = std::make_unique<CartesianProductOperator>(std::move(left), std::move(right));

	auto vars = op->getOutputVariables();
	ASSERT_EQ(vars.size(), 3UL);
	EXPECT_EQ(vars[0], "a");
	EXPECT_EQ(vars[1], "b");
	EXPECT_EQ(vars[2], "c");
}

// toString returns "CartesianProduct"
TEST_F(CartesianProductOperatorTest, ToString) {
	auto left = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{}, std::vector<std::string>{"a"});
	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{}, std::vector<std::string>{"b"});

	auto op = std::make_unique<CartesianProductOperator>(std::move(left), std::move(right));
	EXPECT_EQ(op->toString(), "CartesianProduct");
}

// getChildren returns left and right
TEST_F(CartesianProductOperatorTest, GetChildren) {
	auto left = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{}, std::vector<std::string>{"a"});
	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{}, std::vector<std::string>{"b"});
	auto *leftRaw = left.get();
	auto *rightRaw = right.get();

	auto op = std::make_unique<CartesianProductOperator>(std::move(left), std::move(right));
	auto children = op->getChildren();
	ASSERT_EQ(children.size(), 2UL);
	EXPECT_EQ(children[0], leftRaw);
	EXPECT_EQ(children[1], rightRaw);
}

// Left exhausts mid-batch with partial output
TEST_F(CartesianProductOperatorTest, LeftExhaustedWithPartialOutput) {
	Record l1;
	l1.setValue("a", int64_t(1));

	Record r1, r2, r3;
	r1.setValue("b", int64_t(10));
	r2.setValue("b", int64_t(20));
	r3.setValue("b", int64_t(30));

	auto left = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{l1}}, std::vector<std::string>{"a"});
	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{r1, r2, r3}}, std::vector<std::string>{"b"});

	auto op = std::make_unique<CartesianProductOperator>(std::move(left), std::move(right));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 3UL);

	auto batch2 = op->next();
	EXPECT_FALSE(batch2.has_value());

	op->close();
}

// Null left operator - the !left_ branch in next()
TEST_F(CartesianProductOperatorTest, NullLeftOperator) {
	Record r1;
	r1.setValue("b", int64_t(10));

	// Right side has data but left is null
	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{r1}}, std::vector<std::string>{"b"});

	auto op = std::make_unique<CartesianProductOperator>(nullptr, std::move(right));
	op->open();

	// rightBuffer_ is non-empty, but left_ is null -> the !left_ branch
	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

// Null right operator - open with null right
TEST_F(CartesianProductOperatorTest, NullRightOperator) {
	Record l1;
	l1.setValue("a", int64_t(1));

	auto left = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{l1}}, std::vector<std::string>{"a"});

	auto op = std::make_unique<CartesianProductOperator>(std::move(left), nullptr);
	op->open();

	// rightBuffer_ is empty -> returns nullopt
	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

// Cover branch: left exhausted after partial output has accumulated (line 69)
// When currentLeftBatch_ exhausts and outputBatch is non-empty, return partial
TEST_F(CartesianProductOperatorTest, LeftExhaustedWithNonEmptyPartialBatch) {
	// Create left with 2 batches of 1 record each
	Record l1, l2;
	l1.setValue("a", int64_t(1));
	l2.setValue("a", int64_t(2));

	// Right side has 1 record
	Record r1;
	r1.setValue("b", int64_t(10));

	auto left = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{l1}, {l2}}, std::vector<std::string>{"a"});
	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{r1}}, std::vector<std::string>{"b"});

	auto op = std::make_unique<CartesianProductOperator>(std::move(left), std::move(right));
	op->open();

	// First next() should return batch with at least 1 record
	auto batch1 = op->next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_GE(batch1->size(), 1UL);

	// Collect all remaining records
	size_t totalRecords = batch1->size();
	while (auto batch = op->next()) {
		totalRecords += batch->size();
	}
	// Total should be 2 (2 left x 1 right)
	EXPECT_EQ(totalRecords, 2UL);

	op->close();
}

// Cover branch: rightIdx_ >= rightBuffer_.size() (line 90)
// This triggers advancing leftIdx_ and resetting rightIdx_
TEST_F(CartesianProductOperatorTest, RightIdxResetOnRightBufferExhausted) {
	// Multiple left records in one batch, multiple right records
	Record l1, l2, l3;
	l1.setValue("a", int64_t(1));
	l2.setValue("a", int64_t(2));
	l3.setValue("a", int64_t(3));

	Record r1, r2;
	r1.setValue("b", int64_t(10));
	r2.setValue("b", int64_t(20));

	auto left = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{l1, l2, l3}}, std::vector<std::string>{"a"});
	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{r1, r2}}, std::vector<std::string>{"b"});

	auto op = std::make_unique<CartesianProductOperator>(std::move(left), std::move(right));
	op->open();

	size_t totalRecords = 0;
	while (auto batch = op->next()) {
		totalRecords += batch->size();
	}
	// 3 left x 2 right = 6
	EXPECT_EQ(totalRecords, 6UL);

	op->close();
}

// Cover the !left_ branch returning nullopt when right has data (line 63-64)
TEST_F(CartesianProductOperatorTest, NullLeftWithNonEmptyRight) {
	Record r1, r2;
	r1.setValue("b", int64_t(10));
	r2.setValue("b", int64_t(20));

	auto right = std::make_unique<CPMockOperator>(
		std::vector<RecordBatch>{{r1, r2}}, std::vector<std::string>{"b"});

	auto op = std::make_unique<CartesianProductOperator>(nullptr, std::move(right));
	op->open();

	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}
