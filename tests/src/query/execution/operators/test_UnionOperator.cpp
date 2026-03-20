/**
 * @file test_UnionOperator.cpp
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

#include "graph/query/execution/operators/UnionOperator.hpp"
#include "graph/query/execution/Record.hpp"

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
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"x", "y"}; }
	[[nodiscard]] std::string toString() const override { return "Mock"; }
	[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override { return {}; }
};

// Helper to create a record with two values
Record makeRecord(int64_t x, int64_t y) {
	Record r;
	r.setValue("x", x);
	r.setValue("y", y);
	return r;
}

// ============================================================================
// UnionOperator Tests
// ============================================================================

class UnionOperatorTest : public ::testing::Test {
protected:
	void SetUp() override {}
	void TearDown() override {}
};

// --- UNION ALL Tests ---

TEST_F(UnionOperatorTest, UnionAll_Basic) {
	// Left query: records with x=1,2
	Record r1 = makeRecord(1, 10);
	Record r2 = makeRecord(2, 20);

	// Right query: records with x=3,4
	Record r3 = makeRecord(3, 30);
	Record r4 = makeRecord(4, 40);

	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1, r2}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r3, r4}});

	UnionOperator op(std::move(left), std::move(right), true); // UNION ALL

	op.open();

	// First batch: left query results
	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 2UL);

	// Second batch: right query results
	auto batch2 = op.next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 2UL);

	// No more batches
	auto batch3 = op.next();
	EXPECT_FALSE(batch3.has_value());

	op.close();
}

TEST_F(UnionOperatorTest, UnionAll_PreservesDuplicates) {
	// Left query: (1,10), (2,20)
	Record r1 = makeRecord(1, 10);
	Record r2 = makeRecord(2, 20);

	// Right query: (2,20), (3,30) - note r2 is a duplicate
	Record r3 = makeRecord(2, 20);
	Record r4 = makeRecord(3, 30);

	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1, r2}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r3, r4}});

	UnionOperator op(std::move(left), std::move(right), true); // UNION ALL

	op.open();

	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 2UL); // Left: (1,10), (2,20)

	auto batch2 = op.next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 2UL); // Right: (2,20), (3,30) - duplicate preserved

	op.close();
}

// --- UNION (deduplication) Tests ---

TEST_F(UnionOperatorTest, Union_RemovesDuplicates) {
	// Left query: (1,10), (2,20)
	Record r1 = makeRecord(1, 10);
	Record r2 = makeRecord(2, 20);

	// Right query: (2,20), (3,30) - note (2,20) is a duplicate
	Record r3 = makeRecord(2, 20);
	Record r4 = makeRecord(3, 30);

	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1, r2}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r3, r4}});

	UnionOperator op(std::move(left), std::move(right), false); // UNION (not ALL)

	op.open();

	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 2UL); // Left: (1,10), (2,20)

	auto batch2 = op.next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 1UL); // Right: only (3,30) - (2,20) was deduplicated

	// Verify the content of batch2
	ASSERT_EQ(batch2->size(), 1UL);
	auto val = (*batch2)[0].getValue("x");
	ASSERT_TRUE(val.has_value());
	auto intPtr = std::get_if<int64_t>(&val->getVariant());
	ASSERT_NE(intPtr, nullptr);
	EXPECT_EQ(*intPtr, 3); // Only (3,30) remains

	op.close();
}

TEST_F(UnionOperatorTest, Union_EmptyLeft) {
	// Left query: empty
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{});

	// Right query: (1,10), (2,20)
	Record r1 = makeRecord(1, 10);
	Record r2 = makeRecord(2, 20);
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1, r2}});

	UnionOperator op(std::move(left), std::move(right), false);

	op.open();

	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL); // All records from right

	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value());

	op.close();
}

TEST_F(UnionOperatorTest, Union_EmptyRight) {
	// Left query: (1,10), (2,20)
	Record r1 = makeRecord(1, 10);
	Record r2 = makeRecord(2, 20);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1, r2}});

	// Right query: empty
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{});

	UnionOperator op(std::move(left), std::move(right), false);

	op.open();

	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL); // All records from left

	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value());

	op.close();
}

TEST_F(UnionOperatorTest, Union_BothEmpty) {
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{});

	UnionOperator op(std::move(left), std::move(right), false);

	op.open();

	auto batch = op.next();
	EXPECT_FALSE(batch.has_value());

	op.close();
}

TEST_F(UnionOperatorTest, Union_AllDuplicates) {
	// Left query: (1,10), (2,20)
	Record r1 = makeRecord(1, 10);
	Record r2 = makeRecord(2, 20);

	// Right query: all duplicates of left
	Record r3 = makeRecord(1, 10);
	Record r4 = makeRecord(2, 20);

	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1, r2}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r3, r4}});

	UnionOperator op(std::move(left), std::move(right), false);

	op.open();

	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 2UL); // Left: (1,10), (2,20)

	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value()); // Right: all duplicates, nothing returned

	op.close();
}

// --- Multiple Batches Tests ---

TEST_F(UnionOperatorTest, UnionAll_MultipleBatches) {
	// Left query: 3 batches
	Record r1 = makeRecord(1, 10);
	Record r2 = makeRecord(2, 20);
	Record r3 = makeRecord(3, 30);

	auto left = std::make_unique<MockOperator>();
	left->batches.push_back({r1});
	left->batches.push_back({r2});
	left->batches.push_back({r3});

	// Right query: 2 batches
	Record r4 = makeRecord(4, 40);
	Record r5 = makeRecord(5, 50);

	auto right = std::make_unique<MockOperator>();
	right->batches.push_back({r4});
	right->batches.push_back({r5});

	UnionOperator op(std::move(left), std::move(right), true);

	op.open();

	// Get all batches
	std::vector<Record> allRecords;
	while (auto batch = op.next()) {
		for (auto& rec : *batch) {
			allRecords.push_back(rec);
		}
	}

	EXPECT_EQ(allRecords.size(), 5UL); // 3 from left + 2 from right

	op.close();
}

// --- Edge Cases ---

TEST_F(UnionOperatorTest, Union_NullOperators) {
	// Left operator cannot be null
	EXPECT_THROW(
		UnionOperator(nullptr, std::make_unique<MockOperator>(), false),
		std::runtime_error
	);

	// Right operator cannot be null
	EXPECT_THROW(
		UnionOperator(std::make_unique<MockOperator>(), nullptr, false),
		std::runtime_error
	);
}

TEST_F(UnionOperatorTest, Union_GetOutputVariables) {
	Record r = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});

	UnionOperator op(std::move(left), std::move(right), false);

	auto vars = op.getOutputVariables();
	EXPECT_EQ(vars.size(), 2UL);
	EXPECT_EQ(vars[0], "x");
	EXPECT_EQ(vars[1], "y");
}

TEST_F(UnionOperatorTest, Union_ToString) {
	Record r = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});

	UnionOperator opAll(std::move(left), std::move(right), true);
	EXPECT_EQ(opAll.toString(), "UnionOperator(ALL)");

	Record r2 = makeRecord(2, 20);
	auto left2 = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r2}});
	auto right2 = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r2}});

	UnionOperator opUnique(std::move(left2), std::move(right2), false);
	EXPECT_EQ(opUnique.toString(), "UnionOperator(UNIQUE)");
}

TEST_F(UnionOperatorTest, Union_GetChildren) {
	Record r = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});

	MockOperator* leftPtr = left.get();
	MockOperator* rightPtr = right.get();

	UnionOperator op(std::move(left), std::move(right), false);

	auto children = op.getChildren();
	EXPECT_EQ(children.size(), 2UL);
	EXPECT_EQ(children[0], leftPtr);
	EXPECT_EQ(children[1], rightPtr);
}

TEST_F(UnionOperatorTest, Union_OpenMultipleTimes) {
	Record r = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});

	UnionOperator op(std::move(left), std::move(right), false);

	op.open();
	op.close();

	// Opening again should be allowed
	EXPECT_NO_THROW(op.open());
	op.close();
}

TEST_F(UnionOperatorTest, Union_DifferentTypes) {
	// Left: (1, "hello")
	Record r1;
	r1.setValue("x", 1);
	r1.setValue("y", std::string("hello"));

	// Right: (2, "world")
	Record r2;
	r2.setValue("x", 2);
	r2.setValue("y", std::string("world"));

	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r2}});

	UnionOperator op(std::move(left), std::move(right), false);

	op.open();

	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1UL);

	auto batch2 = op.next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 1UL);

	// Verify second record has different types
	auto val = (*batch2)[0].getValue("y");
	ASSERT_TRUE(val.has_value());
	auto strPtr = std::get_if<std::string>(&val->getVariant());
	ASSERT_NE(strPtr, nullptr);
	EXPECT_EQ(*strPtr, "world");

	op.close();
}

TEST_F(UnionOperatorTest, Union_LargeDataset) {
	// Test with many records to verify batching works correctly
	std::vector<Record> leftRecords;
	std::vector<Record> rightRecords;

	for (int64_t i = 0; i < 150; ++i) {
		leftRecords.push_back(makeRecord(i, i * 10));
	}

	for (int64_t i = 100; i < 250; ++i) {
		rightRecords.push_back(makeRecord(i, i * 10));
	}

	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{leftRecords});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{rightRecords});

	UnionOperator op(std::move(left), std::move(right), false);

	op.open();

	// Count total unique records (100-249 should be unique)
	size_t totalCount = 0;
	while (auto batch = op.next()) {
		totalCount += batch->size();
	}

	// 150 from left (0-149) + 100 from right (150-249, excluding duplicates 100-149)
	EXPECT_EQ(totalCount, 250UL);

	op.close();
}

TEST_F(UnionOperatorTest, Union_SingleRecordEach) {
	Record r1 = makeRecord(1, 10);
	Record r2 = makeRecord(2, 20);

	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r2}});

	UnionOperator op(std::move(left), std::move(right), false);

	op.open();

	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1UL);

	auto batch2 = op.next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 1UL);

	auto batch3 = op.next();
	EXPECT_FALSE(batch3.has_value());

	op.close();
}
