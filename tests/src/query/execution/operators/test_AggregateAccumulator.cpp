/**
 * @file test_AggregateAccumulator.cpp
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
#include <string>

#include "graph/query/execution/operators/AggregateAccumulator.hpp"

using namespace graph;
using namespace graph::query::execution::operators;

// ============================================================================
// CountAccumulator branch coverage
// ============================================================================

class CountAccumulatorTest : public ::testing::Test {};

// Count with all NULL values - the isNull branch is true every time
TEST_F(CountAccumulatorTest, AllNullValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COUNT);
	acc->update(PropertyValue());
	acc->update(PropertyValue());
	acc->update(PropertyValue());
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 0);
}

// Count with mix of null and non-null - both branches of isNull
TEST_F(CountAccumulatorTest, MixedNullAndNonNull) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COUNT);
	acc->update(PropertyValue(int64_t(1)));    // non-null -> count++
	acc->update(PropertyValue());              // null -> skip
	acc->update(PropertyValue(std::string("x"))); // non-null -> count++
	acc->update(PropertyValue());              // null -> skip
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 2);
}

// Count with no updates at all
TEST_F(CountAccumulatorTest, NoUpdates) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COUNT);
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 0);
}

// Clone of empty accumulator
TEST_F(CountAccumulatorTest, CloneEmpty) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COUNT);
	auto clone = acc->clone();
	EXPECT_EQ(std::get<int64_t>(clone->getResult().getVariant()), 0);
}

// ============================================================================
// SumAccumulator branch coverage
// ============================================================================

class SumAccumulatorTest : public ::testing::Test {};

// Sum with only double values (no int) - hasDouble_ true, hasInt_ false
TEST_F(SumAccumulatorTest, OnlyDoubleValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_SUM);
	acc->update(PropertyValue(1.5));
	acc->update(PropertyValue(2.5));
	auto result = acc->getResult();
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 4.0);
}

// Sum with only integer values (no double) - hasInt_ true, hasDouble_ false
TEST_F(SumAccumulatorTest, OnlyIntegerValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_SUM);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(int64_t(20)));
	auto result = acc->getResult();
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 30);
}

// Sum with mixed int and double - both hasInt_ and hasDouble_ true
TEST_F(SumAccumulatorTest, MixedIntAndDouble) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_SUM);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(2.5));
	auto result = acc->getResult();
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 12.5);
}

// Sum with NULL values interspersed - tests the null skip branch
TEST_F(SumAccumulatorTest, WithNullValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_SUM);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue());  // null - should skip
	acc->update(PropertyValue(int64_t(20)));
	auto result = acc->getResult();
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 30);
}

// Sum with non-numeric values (string, bool) - neither int nor double branch
TEST_F(SumAccumulatorTest, NonNumericValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_SUM);
	acc->update(PropertyValue(std::string("hello")));
	acc->update(PropertyValue(true));
	// Neither hasInt_ nor hasDouble_ is true
	auto result = acc->getResult();
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// Sum clone with mixed types
TEST_F(SumAccumulatorTest, CloneMixed) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_SUM);
	acc->update(PropertyValue(int64_t(5)));
	acc->update(PropertyValue(3.5));
	auto clone = acc->clone();
	EXPECT_DOUBLE_EQ(std::get<double>(clone->getResult().getVariant()), 8.5);
}

// Sum reset restores all fields
TEST_F(SumAccumulatorTest, ResetClearsAllFlags) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_SUM);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(2.5));
	acc->reset();
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// AvgAccumulator branch coverage
// ============================================================================

class AvgAccumulatorTest : public ::testing::Test {};

// Avg with only integer values - the INTEGER branch
TEST_F(AvgAccumulatorTest, OnlyIntegers) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_AVG);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(int64_t(30)));
	auto result = acc->getResult();
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 20.0);
}

// Avg with only double values - the DOUBLE branch
TEST_F(AvgAccumulatorTest, OnlyDoubles) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_AVG);
	acc->update(PropertyValue(1.0));
	acc->update(PropertyValue(3.0));
	auto result = acc->getResult();
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 2.0);
}

// Avg with mixed int and double
TEST_F(AvgAccumulatorTest, MixedIntAndDouble) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_AVG);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(20.0));
	auto result = acc->getResult();
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 15.0);
}

// Avg with NULL values - null skip branch
TEST_F(AvgAccumulatorTest, WithNullValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_AVG);
	acc->update(PropertyValue());       // null -> skip
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue());       // null -> skip
	auto result = acc->getResult();
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 10.0);
}

// Avg with non-numeric (string) - neither int nor double branch
TEST_F(AvgAccumulatorTest, NonNumericValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_AVG);
	acc->update(PropertyValue(std::string("not a number")));
	// count_ remains 0
	auto result = acc->getResult();
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// Avg with zero count (no values)
TEST_F(AvgAccumulatorTest, EmptyReturnsNull) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_AVG);
	auto result = acc->getResult();
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// MinAccumulator branch coverage
// ============================================================================

class MinAccumulatorTest : public ::testing::Test {};

// Min with first value sets hasValue_ - the !hasValue_ branch
TEST_F(MinAccumulatorTest, SingleValue) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MIN);
	acc->update(PropertyValue(int64_t(42)));
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 42);
}

// Min where second value is less - the value < minValue_ branch (true)
TEST_F(MinAccumulatorTest, SecondValueIsSmaller) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MIN);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(int64_t(5)));  // 5 < 10 -> update min
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 5);
}

// Min where second value is NOT less - the value < minValue_ branch (false)
TEST_F(MinAccumulatorTest, SecondValueIsLarger) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MIN);
	acc->update(PropertyValue(int64_t(5)));
	acc->update(PropertyValue(int64_t(10)));  // 10 > 5 -> no update
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 5);
}

// Min where second value equals the min - the value < minValue_ branch (false)
TEST_F(MinAccumulatorTest, SecondValueIsEqual) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MIN);
	acc->update(PropertyValue(int64_t(7)));
	acc->update(PropertyValue(int64_t(7)));  // 7 == 7 -> no update
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 7);
}

// Min with NULL values interspersed
TEST_F(MinAccumulatorTest, NullValuesSkipped) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MIN);
	acc->update(PropertyValue());              // null -> skip
	acc->update(PropertyValue(int64_t(20)));   // first real value
	acc->update(PropertyValue());              // null -> skip
	acc->update(PropertyValue(int64_t(10)));   // smaller -> update
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 10);
}

// Min with no values (hasValue_ false)
TEST_F(MinAccumulatorTest, EmptyReturnsNull) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MIN);
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
}

// Min with double values, decreasing then increasing
TEST_F(MinAccumulatorTest, DoubleValues_DecreasingThenIncreasing) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MIN);
	acc->update(PropertyValue(5.0));
	acc->update(PropertyValue(3.0));   // smaller
	acc->update(PropertyValue(7.0));   // larger -> no update
	EXPECT_DOUBLE_EQ(std::get<double>(acc->getResult().getVariant()), 3.0);
}

// ============================================================================
// MaxAccumulator branch coverage
// ============================================================================

class MaxAccumulatorTest : public ::testing::Test {};

// Max with first value sets hasValue_ - the !hasValue_ branch
TEST_F(MaxAccumulatorTest, SingleValue) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MAX);
	acc->update(PropertyValue(int64_t(42)));
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 42);
}

// Max where second value is greater - the value > maxValue_ branch (true)
TEST_F(MaxAccumulatorTest, SecondValueIsLarger) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MAX);
	acc->update(PropertyValue(int64_t(5)));
	acc->update(PropertyValue(int64_t(10)));  // 10 > 5 -> update max
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 10);
}

// Max where second value is NOT greater - the value > maxValue_ branch (false)
TEST_F(MaxAccumulatorTest, SecondValueIsSmaller) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MAX);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(int64_t(5)));  // 5 < 10 -> no update
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 10);
}

// Max where second value equals the max - the value > maxValue_ branch (false)
TEST_F(MaxAccumulatorTest, SecondValueIsEqual) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MAX);
	acc->update(PropertyValue(int64_t(7)));
	acc->update(PropertyValue(int64_t(7)));  // 7 == 7 -> no update
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 7);
}

// Max with NULL values interspersed
TEST_F(MaxAccumulatorTest, NullValuesSkipped) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MAX);
	acc->update(PropertyValue());              // null -> skip
	acc->update(PropertyValue(int64_t(10)));   // first real value
	acc->update(PropertyValue());              // null -> skip
	acc->update(PropertyValue(int64_t(20)));   // larger -> update
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 20);
}

// Max with no values (hasValue_ false)
TEST_F(MaxAccumulatorTest, EmptyReturnsNull) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MAX);
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
}

// Max with double values, increasing then decreasing
TEST_F(MaxAccumulatorTest, DoubleValues_IncreasingThenDecreasing) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MAX);
	acc->update(PropertyValue(3.0));
	acc->update(PropertyValue(7.0));   // larger
	acc->update(PropertyValue(5.0));   // smaller -> no update
	EXPECT_DOUBLE_EQ(std::get<double>(acc->getResult().getVariant()), 7.0);
}

// Max with three values all increasing
TEST_F(MaxAccumulatorTest, MultipleIncreasingValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MAX);
	acc->update(PropertyValue(int64_t(1)));
	acc->update(PropertyValue(int64_t(2)));
	acc->update(PropertyValue(int64_t(3)));
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 3);
}

// ============================================================================
// CollectAccumulator branch coverage
// ============================================================================

class CollectAccumulatorTest : public ::testing::Test {};

// Collect with no values - empty list
TEST_F(CollectAccumulatorTest, EmptyCollect) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COLLECT);
	auto result = acc->getResult();
	auto list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_TRUE(list.empty());
}

// Collect with single value - the i > 0 branch is false
TEST_F(CollectAccumulatorTest, SingleValue) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COLLECT);
	acc->update(PropertyValue(int64_t(42)));
	auto result = acc->getResult();
	auto list = std::get<std::vector<PropertyValue>>(result.getVariant());
	ASSERT_EQ(list.size(), 1u);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 42);
}

// Collect with multiple values - the i > 0 branch is true
TEST_F(CollectAccumulatorTest, MultipleValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COLLECT);
	acc->update(PropertyValue(int64_t(1)));
	acc->update(PropertyValue(int64_t(2)));
	acc->update(PropertyValue(int64_t(3)));
	auto result = acc->getResult();
	auto list = std::get<std::vector<PropertyValue>>(result.getVariant());
	ASSERT_EQ(list.size(), 3u);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 3);
}

// Collect with NULL values (collect includes nulls)
TEST_F(CollectAccumulatorTest, WithNullValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COLLECT);
	acc->update(PropertyValue(int64_t(1)));
	acc->update(PropertyValue());
	acc->update(PropertyValue(int64_t(3)));
	auto result = acc->getResult();
	auto list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 3u);
}

// Collect with mixed types
TEST_F(CollectAccumulatorTest, MixedTypes) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COLLECT);
	acc->update(PropertyValue(int64_t(1)));
	acc->update(PropertyValue(std::string("hello")));
	acc->update(PropertyValue(3.14));
	auto result = acc->getResult();
	auto list = std::get<std::vector<PropertyValue>>(result.getVariant());
	ASSERT_EQ(list.size(), 3u);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 1);
	EXPECT_EQ(std::get<std::string>(list[1].getVariant()), "hello");
	EXPECT_DOUBLE_EQ(std::get<double>(list[2].getVariant()), 3.14);
}

// Collect clone
TEST_F(CollectAccumulatorTest, ClonePreservesValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COLLECT);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(int64_t(20)));
	auto clone = acc->clone();
	auto list = std::get<std::vector<PropertyValue>>(clone->getResult().getVariant());
	ASSERT_EQ(list.size(), 2u);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 10);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 20);
}

// Collect reset
TEST_F(CollectAccumulatorTest, ResetClearsValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COLLECT);
	acc->update(PropertyValue(int64_t(1)));
	acc->reset();
	auto list = std::get<std::vector<PropertyValue>>(acc->getResult().getVariant());
	EXPECT_TRUE(list.empty());
}

// ============================================================================
// createAccumulator factory branch coverage
// ============================================================================

class CreateAccumulatorTest : public ::testing::Test {};

// Test each individual enum value through the switch
TEST_F(CreateAccumulatorTest, AllKnownTypes) {
	EXPECT_NE(createAccumulator(AggregateFunctionType::AGG_COUNT), nullptr);
	EXPECT_NE(createAccumulator(AggregateFunctionType::AGG_SUM), nullptr);
	EXPECT_NE(createAccumulator(AggregateFunctionType::AGG_AVG), nullptr);
	EXPECT_NE(createAccumulator(AggregateFunctionType::AGG_MIN), nullptr);
	EXPECT_NE(createAccumulator(AggregateFunctionType::AGG_MAX), nullptr);
	EXPECT_NE(createAccumulator(AggregateFunctionType::AGG_COLLECT), nullptr);
}

// Test the default branch with an invalid enum value
TEST_F(CreateAccumulatorTest, InvalidTypeReturnsNull) {
	// Force an invalid enum value to cover the default branch
	auto result = createAccumulator(static_cast<AggregateFunctionType>(999));
	EXPECT_EQ(result, nullptr);
}

// ============================================================================
// CountDistinctAccumulator: reset(), clone(), NULL update()
// ============================================================================

class CountDistinctAccumulatorTest : public ::testing::Test {};

TEST_F(CountDistinctAccumulatorTest, ResetClearsSeenSet) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COUNT, true);
	acc->update(PropertyValue(int64_t(1)));
	acc->update(PropertyValue(int64_t(2)));
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 2);
	acc->reset();
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 0);
}

TEST_F(CountDistinctAccumulatorTest, ClonePreservesState) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COUNT, true);
	acc->update(PropertyValue(std::string("a")));
	acc->update(PropertyValue(std::string("b")));
	auto cloned = acc->clone();
	EXPECT_EQ(std::get<int64_t>(cloned->getResult().getVariant()), 2);
	// Modifying original does not affect clone
	acc->reset();
	EXPECT_EQ(std::get<int64_t>(cloned->getResult().getVariant()), 2);
}

TEST_F(CountDistinctAccumulatorTest, NullUpdateSkipped) {
	// NULL value is skipped in CountDistinct — covers the isNull branch
	auto acc = createAccumulator(AggregateFunctionType::AGG_COUNT, true);
	acc->update(PropertyValue()); // null -> skip
	acc->update(PropertyValue(int64_t(5)));
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 1);
}

// ============================================================================
// CollectDistinctAccumulator: reset(), clone()
// ============================================================================

class CollectDistinctAccumulatorTest : public ::testing::Test {};

TEST_F(CollectDistinctAccumulatorTest, ResetClearsAll) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COLLECT, true);
	acc->update(PropertyValue(int64_t(1)));
	acc->update(PropertyValue(int64_t(2)));
	acc->reset();
	auto list = std::get<std::vector<PropertyValue>>(acc->getResult().getVariant());
	EXPECT_TRUE(list.empty());
}

TEST_F(CollectDistinctAccumulatorTest, ClonePreservesState) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COLLECT, true);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(int64_t(20)));
	acc->update(PropertyValue(int64_t(10))); // duplicate, ignored
	auto cloned = acc->clone();
	auto list = std::get<std::vector<PropertyValue>>(cloned->getResult().getVariant());
	EXPECT_EQ(list.size(), 2u);
}

// ============================================================================
// StDevAccumulator: reset(), clone(), NULL update(), DOUBLE update()
// ============================================================================

class StDevAccumulatorTest : public ::testing::Test {};

TEST_F(StDevAccumulatorTest, ResetClearsState) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_STDEV);
	acc->update(PropertyValue(int64_t(2)));
	acc->update(PropertyValue(int64_t(4)));
	acc->reset();
	// After reset, n_ < 2 so result is NULL
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(StDevAccumulatorTest, ClonePreservesState) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_STDEV);
	acc->update(PropertyValue(int64_t(2)));
	acc->update(PropertyValue(int64_t(4)));
	auto cloned = acc->clone();
	// Both should give the same result
	EXPECT_EQ(acc->getResult().getType(), PropertyType::DOUBLE);
	EXPECT_EQ(cloned->getResult().getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(
		std::get<double>(acc->getResult().getVariant()),
		std::get<double>(cloned->getResult().getVariant())
	);
}

TEST_F(StDevAccumulatorTest, NullUpdateSkipped) {
	// NULL value is skipped — covers the isNull branch in StDevAccumulator::update
	auto acc = createAccumulator(AggregateFunctionType::AGG_STDEV);
	acc->update(PropertyValue()); // null -> skip
	// n_ stays 0, result is NULL (n_ < 2)
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(StDevAccumulatorTest, DoubleUpdateCoveredBranch) {
	// DOUBLE value hits the DOUBLE branch in StDevAccumulator::update
	auto acc = createAccumulator(AggregateFunctionType::AGG_STDEV);
	acc->update(PropertyValue(2.0));
	acc->update(PropertyValue(4.0));
	auto result = acc->getResult();
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	// stdev of [2.0, 4.0] = sqrt(2) ≈ 1.414
	EXPECT_NEAR(std::get<double>(result.getVariant()), std::sqrt(2.0), 1e-9);
}

TEST_F(StDevAccumulatorTest, StDevPopulation_CloneAndReset) {
	// Tests StDevAccumulator(population=true) — AGG_STDEVP
	auto acc = createAccumulator(AggregateFunctionType::AGG_STDEVP);
	acc->update(PropertyValue(int64_t(2)));
	acc->update(PropertyValue(int64_t(4)));
	auto cloned = acc->clone();
	EXPECT_EQ(cloned->getResult().getType(), PropertyType::DOUBLE);

	acc->reset();
	// n_ = 0, n_ < 1 so result is NULL
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// PercentileDiscAccumulator: reset(), clone()
// ============================================================================

class PercentileDiscAccumulatorTest : public ::testing::Test {};

TEST_F(PercentileDiscAccumulatorTest, ResetClearsValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_PERCENTILE_DISC, false, 0.5);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(int64_t(20)));
	acc->reset();
	// After reset, values_ is empty -> getResult returns NULL
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(PercentileDiscAccumulatorTest, ClonePreservesValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_PERCENTILE_DISC, false, 0.5);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(int64_t(20)));
	auto cloned = acc->clone();
	// Median of [10, 20] at p=0.5 -> idx=0 -> value=10
	EXPECT_EQ(std::get<int64_t>(cloned->getResult().getVariant()), 10);
}

// ============================================================================
// PercentileContAccumulator: reset(), clone(), NULL update(), DOUBLE update()
// ============================================================================

class PercentileContAccumulatorTest : public ::testing::Test {};

TEST_F(PercentileContAccumulatorTest, ResetClearsValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_PERCENTILE_CONT, false, 0.5);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(int64_t(20)));
	acc->reset();
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(PercentileContAccumulatorTest, ClonePreservesValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_PERCENTILE_CONT, false, 0.5);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(int64_t(20)));
	auto cloned = acc->clone();
	// Continuous percentile at p=0.5 of [10, 20]: rank=0.5, lo=0, hi=1
	// result = 10 + 0.5*(20-10) = 15.0
	EXPECT_DOUBLE_EQ(std::get<double>(cloned->getResult().getVariant()), 15.0);
}

TEST_F(PercentileContAccumulatorTest, NullUpdateSkipped) {
	// NULL is skipped — covers the isNull branch in PercentileContAccumulator::update
	auto acc = createAccumulator(AggregateFunctionType::AGG_PERCENTILE_CONT, false, 0.5);
	acc->update(PropertyValue()); // null -> skip
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(PercentileContAccumulatorTest, DoubleUpdateCoveredBranch) {
	// DOUBLE values hit the DOUBLE branch in PercentileContAccumulator::update
	// Also tests interpolation with hi DOUBLE values
	auto acc = createAccumulator(AggregateFunctionType::AGG_PERCENTILE_CONT, false, 0.5);
	acc->update(PropertyValue(1.0));
	acc->update(PropertyValue(3.0));
	// rank = 0.5, lo=0(1.0), hi=1(3.0), frac=0.5 -> result = 1.0 + 0.5*2.0 = 2.0
	EXPECT_DOUBLE_EQ(std::get<double>(acc->getResult().getVariant()), 2.0);
}

TEST_F(PercentileContAccumulatorTest, SingleElementReturnsSelf) {
	// lo == hi (single element): covers the lo == hi branch in getResult
	auto acc = createAccumulator(AggregateFunctionType::AGG_PERCENTILE_CONT, false, 0.0);
	acc->update(PropertyValue(int64_t(42)));
	// rank = 0, lo = 0, hi = 0 -> lo == hi -> return sorted[0]
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 42);
}

TEST_F(PercentileContAccumulatorTest, StringTypeIgnored) {
	// String values are neither INTEGER nor DOUBLE — they should be ignored
	auto acc = createAccumulator(AggregateFunctionType::AGG_PERCENTILE_CONT, false, 0.5);
	acc->update(PropertyValue(std::string("hello")));
	// No values added -> result is NULL
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
}
