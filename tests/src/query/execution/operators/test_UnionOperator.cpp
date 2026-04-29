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

namespace {

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

} // namespace
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

// --- Additional coverage tests ---

// Mock with different output variables for testing mismatch
class MismatchMockOperator : public PhysicalOperator {
public:
	std::vector<std::string> vars;
	explicit MismatchMockOperator(std::vector<std::string> v) : vars(std::move(v)) {}
	void open() override {}
	std::optional<RecordBatch> next() override { return std::nullopt; }
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return vars; }
	[[nodiscard]] std::string toString() const override { return "MismatchMock"; }
};

TEST_F(UnionOperatorTest, Union_MismatchedColumns_Throws) {
	auto left = std::make_unique<MismatchMockOperator>(std::vector<std::string>{"x", "y"});
	auto right = std::make_unique<MismatchMockOperator>(std::vector<std::string>{"a", "b"});

	EXPECT_THROW(
		UnionOperator(std::move(left), std::move(right), false),
		std::runtime_error
	);
}

TEST_F(UnionOperatorTest, Union_NextBeforeOpen_Throws) {
	Record r = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});

	UnionOperator op(std::move(left), std::move(right), true);
	// Calling next() before open() should throw
	EXPECT_THROW(op.next(), std::runtime_error);
}

TEST_F(UnionOperatorTest, UnionAll_EmptyLeft) {
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{});
	Record r = makeRecord(1, 10);
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});

	UnionOperator op(std::move(left), std::move(right), true); // UNION ALL

	op.open();

	// Left is empty, should transition to right
	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1UL);

	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value());

	op.close();
}

TEST_F(UnionOperatorTest, UnionAll_EmptyRight) {
	Record r = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{});

	UnionOperator op(std::move(left), std::move(right), true); // UNION ALL

	op.open();

	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1UL);

	// Right is empty
	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value());

	op.close();
}

TEST_F(UnionOperatorTest, Union_FinishedState_ReturnsNullopt) {
	Record r = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{});

	UnionOperator op(std::move(left), std::move(right), true);

	op.open();

	// Exhaust all batches
	while (op.next()) {}

	// Calling next() again in FINISHED state
	auto batch = op.next();
	EXPECT_FALSE(batch.has_value());

	op.close();
}

TEST_F(UnionOperatorTest, Union_ReopenAfterFinished) {
	Record r1 = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1}});

	UnionOperator op(std::move(left), std::move(right), false);

	op.open();
	while (op.next()) {}
	op.close();

	// Re-open should work (state was FINISHED, gets reset)
	op.open();
	// After reopening, left mock has already been exhausted (index past end)
	// So it goes straight to right
	op.close();
}

TEST_F(UnionOperatorTest, Union_OpenInInvalidState_Throws) {
	Record r = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});

	UnionOperator op(std::move(left), std::move(right), false);

	op.open();
	// Calling open() again while in CONSUMING_LEFT should throw
	EXPECT_THROW(op.open(), std::runtime_error);
	op.close();
}

TEST_F(UnionOperatorTest, Union_LargeBatchTriggersChunking) {
	// Create >100 unique records in right to trigger batch size >= 100 check (line 135)
	std::vector<Record> leftRecords;
	for (int64_t i = 0; i < 5; ++i) {
		leftRecords.push_back(makeRecord(i, i));
	}
	std::vector<Record> rightRecords;
	for (int64_t i = 100; i < 250; ++i) {
		rightRecords.push_back(makeRecord(i, i));
	}
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{leftRecords});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{rightRecords});

	UnionOperator op(std::move(left), std::move(right), false);
	op.open();

	size_t totalCount = 0;
	size_t batchCount = 0;
	while (auto batch = op.next()) {
		totalCount += batch->size();
		batchCount++;
	}
	// 5 left + 150 right, all unique -> 155 total
	EXPECT_EQ(totalCount, 155UL);
	// Should have at least 2 batches from right (100 + 50) + 1 from left
	EXPECT_GE(batchCount, 2UL);
	op.close();
}

TEST_F(UnionOperatorTest, Union_RecordMissingVariable) {
	// Test serializeRecord with record missing one of the output variables
	Record r1;
	r1.setValue("x", graph::PropertyValue(int64_t(1)));
	// Missing "y" -> should serialize as "NULL"

	Record r2;
	r2.setValue("x", graph::PropertyValue(int64_t(1)));
	// Also missing "y"

	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r2}});

	UnionOperator op(std::move(left), std::move(right), false);
	op.open();

	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1UL);

	// Duplicate (both missing y) should be deduplicated
	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value());

	op.close();
}

TEST_F(UnionOperatorTest, Union_NullValueInRecord) {
	Record r1;
	r1.setValue("x", graph::PropertyValue());
	r1.setValue("y", graph::PropertyValue(int64_t(10)));

	Record r2;
	r2.setValue("x", graph::PropertyValue());
	r2.setValue("y", graph::PropertyValue(int64_t(10)));

	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r2}});

	UnionOperator op(std::move(left), std::move(right), false); // UNION with dedup

	op.open();

	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1UL);

	// Duplicate null record should be deduplicated
	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value());

	op.close();
}

// ============================================================================
// Additional Branch Coverage Tests
// ============================================================================

TEST_F(UnionOperatorTest, UnionAll_BothEmpty) {
	// Covers UNION ALL with both sides empty - right path returns nullopt immediately
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{});

	UnionOperator op(std::move(left), std::move(right), true); // UNION ALL

	op.open();

	auto batch = op.next();
	EXPECT_FALSE(batch.has_value()); // Left empty, transition to right, right also empty

	op.close();
}

TEST_F(UnionOperatorTest, Union_CloseWithoutOpen) {
	// Covers close() when operators exist but were never opened
	Record r = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});

	UnionOperator op(std::move(left), std::move(right), false);

	// Close without open - should not crash
	EXPECT_NO_THROW(op.close());
}

TEST_F(UnionOperatorTest, Union_NextAfterClose) {
	// Tests calling next() after close - state is FINISHED, returns nullopt
	Record r = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});

	UnionOperator op(std::move(left), std::move(right), true);

	op.open();
	op.close(); // State becomes FINISHED

	auto batch = op.next();
	EXPECT_FALSE(batch.has_value()); // FINISHED state returns nullopt
}

TEST_F(UnionOperatorTest, UnionAll_MultipleBatchesLeftAndRight) {
	// Covers UNION ALL with multiple batches from both sides - ensures
	// left exhaustion properly transitions to consuming right
	Record r1 = makeRecord(1, 10);
	Record r2 = makeRecord(2, 20);
	Record r3 = makeRecord(3, 30);
	Record r4 = makeRecord(4, 40);

	auto left = std::make_unique<MockOperator>();
	left->batches.push_back({r1});
	left->batches.push_back({r2});

	auto right = std::make_unique<MockOperator>();
	right->batches.push_back({r3});
	right->batches.push_back({r4});

	UnionOperator op(std::move(left), std::move(right), true);

	op.open();

	size_t totalRecords = 0;
	size_t batchCount = 0;
	while (auto batch = op.next()) {
		totalRecords += batch->size();
		batchCount++;
	}

	EXPECT_EQ(totalRecords, 4UL);
	EXPECT_EQ(batchCount, 4UL); // 2 from left + 2 from right
	op.close();
}

TEST_F(UnionOperatorTest, Union_SerializeRecord_MultipleVariables) {
	// Tests serialization with multiple output variables to ensure
	// the "|" separator and first flag work correctly
	Record r1;
	r1.setValue("x", graph::PropertyValue(int64_t(1)));
	r1.setValue("y", graph::PropertyValue(std::string("hello")));

	Record r2;
	r2.setValue("x", graph::PropertyValue(int64_t(1)));
	r2.setValue("y", graph::PropertyValue(std::string("hello")));

	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r2}});

	UnionOperator op(std::move(left), std::move(right), false);
	op.open();

	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1UL);

	// Duplicate should be removed
	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value());

	op.close();
}

TEST_F(UnionOperatorTest, Union_PartialDuplicatesInRight) {
	// Right has mix of duplicates and unique - covers the inner loop
	// where some records pass the seenRecords_ check and some don't
	Record r1 = makeRecord(1, 10);
	Record r2 = makeRecord(2, 20);

	Record r3 = makeRecord(1, 10); // duplicate of r1
	Record r4 = makeRecord(3, 30); // unique
	Record r5 = makeRecord(2, 20); // duplicate of r2

	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1, r2}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r3, r4, r5}});

	UnionOperator op(std::move(left), std::move(right), false);
	op.open();

	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 2UL); // Left records

	auto batch2 = op.next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 1UL); // Only (3,30) is unique

	auto batch3 = op.next();
	EXPECT_FALSE(batch3.has_value());

	op.close();
}

// ============================================================================
// Additional Branch Coverage Tests - Round 3
// ============================================================================

TEST_F(UnionOperatorTest, Union_MultipleNext_AfterFinished) {
	// Covers: calling next() multiple times in FINISHED state (line 150)
	Record r = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{});

	UnionOperator op(std::move(left), std::move(right), true);
	op.open();

	// Consume all
	while (op.next()) {}

	// Multiple calls in FINISHED state should all return nullopt
	EXPECT_FALSE(op.next().has_value());
	EXPECT_FALSE(op.next().has_value());
	EXPECT_FALSE(op.next().has_value());

	op.close();
}

TEST_F(UnionOperatorTest, UnionAll_SingleBatchRightAfterEmptyLeft) {
	// Covers: UNION ALL transition from empty left to non-empty right
	// then right returns batch, then next call returns nullopt from right
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{});
	Record r1 = makeRecord(1, 10);
	Record r2 = makeRecord(2, 20);
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1, r2}});

	UnionOperator op(std::move(left), std::move(right), true);
	op.open();

	// Left is empty -> transitions to CONSUMING_RIGHT
	// Right has one batch
	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 2UL);

	// Right is now exhausted -> FINISHED
	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value());

	op.close();
}

TEST_F(UnionOperatorTest, Union_EmptyRightBuffered) {
	// Covers: UNION (dedup) where right is empty, outputBatch is empty
	// after buffering completes, goes to CONSUMING_RIGHT with empty buffer
	Record r1 = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r1}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{});

	UnionOperator op(std::move(left), std::move(right), false);
	op.open();

	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1UL);

	// Right buffer is empty -> FINISHED
	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value());

	op.close();
}

TEST_F(UnionOperatorTest, Union_SerializeRecord_SingleVariable) {
	// Tests serialization with only first variable (first flag never changes)
	// Using single output variable mock
	class SingleVarMockOperator : public PhysicalOperator {
	public:
		std::vector<RecordBatch> batches;
		size_t current_index = 0;
		explicit SingleVarMockOperator(std::vector<RecordBatch> data = {}) : batches(std::move(data)) {}
		void open() override {}
		std::optional<RecordBatch> next() override {
			if (current_index >= batches.size()) return std::nullopt;
			return batches[current_index++];
		}
		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"x"}; }
		[[nodiscard]] std::string toString() const override { return "SingleVarMock"; }
		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override { return {}; }
	};

	Record r1;
	r1.setValue("x", graph::PropertyValue(int64_t(42)));
	Record r2;
	r2.setValue("x", graph::PropertyValue(int64_t(42)));

	auto left = std::make_unique<SingleVarMockOperator>(std::vector<RecordBatch>{{r1}});
	auto right = std::make_unique<SingleVarMockOperator>(std::vector<RecordBatch>{{r2}});

	UnionOperator op(std::move(left), std::move(right), false);
	op.open();

	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1UL);

	// Duplicate should be removed
	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value());

	op.close();
}

TEST_F(UnionOperatorTest, UnionAll_ExhaustLeftThenRight_MultiStep) {
	// Multiple batches from left, then transitions to right with multiple batches
	// Covers the fall-through from CONSUMING_LEFT to CONSUMING_RIGHT in UNION ALL
	Record r1 = makeRecord(1, 10);
	Record r2 = makeRecord(2, 20);

	auto left = std::make_unique<MockOperator>();
	left->batches.push_back({r1}); // 1 batch, 1 record

	Record r3 = makeRecord(3, 30);
	auto right = std::make_unique<MockOperator>();
	right->batches.push_back({r2}); // 1 batch
	right->batches.push_back({r3}); // another batch

	UnionOperator op(std::move(left), std::move(right), true);
	op.open();

	// Batch from left
	auto b1 = op.next();
	ASSERT_TRUE(b1.has_value());
	EXPECT_EQ(b1->size(), 1UL);

	// Transition to right, first batch
	auto b2 = op.next();
	ASSERT_TRUE(b2.has_value());
	EXPECT_EQ(b2->size(), 1UL);

	// Right second batch
	auto b3 = op.next();
	ASSERT_TRUE(b3.has_value());
	EXPECT_EQ(b3->size(), 1UL);

	// Done
	auto b4 = op.next();
	EXPECT_FALSE(b4.has_value());

	op.close();
}

// ============================================================================
// Move-from tests to cover null-pointer defensive branches
// ============================================================================

TEST_F(UnionOperatorTest, MovedFrom_GetOutputVariables_ReturnsEmpty) {
	// After moving, left_ is null, so getOutputVariables() returns empty vector
	// Covers: Branch (168:9) False path
	Record r = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});

	UnionOperator op(std::move(left), std::move(right), false);

	// Move the operator - source's left_/right_ become null
	UnionOperator movedOp(std::move(op));

	// Now op.left_ is null
	auto vars = op.getOutputVariables();
	EXPECT_TRUE(vars.empty());
}

TEST_F(UnionOperatorTest, MovedFrom_GetChildren_ReturnsEmpty) {
	// After moving, left_ and right_ are null, so getChildren() returns empty
	// Covers: Branch (177:6) False path, Branch (178:6) False path
	Record r = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});

	UnionOperator op(std::move(left), std::move(right), true);

	// Move the operator
	UnionOperator movedOp(std::move(op));

	// Now op has null left_ and right_
	auto children = op.getChildren();
	EXPECT_TRUE(children.empty());
}

TEST_F(UnionOperatorTest, MovedFrom_Close_DoesNotCrash) {
	// After moving, close() should handle null left_/right_ gracefully
	// Covers: Branch (158:6) False path, Branch (161:6) False path
	Record r = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});

	UnionOperator op(std::move(left), std::move(right), false);

	// Move the operator
	UnionOperator movedOp(std::move(op));

	// close() on moved-from object should not crash (null checks protect it)
	EXPECT_NO_THROW(op.close());
}

TEST_F(UnionOperatorTest, MovedFrom_ToString_StillWorks) {
	// toString() doesn't depend on left_/right_, should still work after move
	Record r = makeRecord(1, 10);
	auto left = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});
	auto right = std::make_unique<MockOperator>(std::vector<RecordBatch>{{r}});

	UnionOperator op(std::move(left), std::move(right), true);

	UnionOperator movedOp(std::move(op));

	// toString should still return valid string on moved-from
	EXPECT_EQ(op.toString(), "UnionOperator(ALL)");
}

