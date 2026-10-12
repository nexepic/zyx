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
#include <limits>
#include <memory>
#include <string>

#include "graph/query/execution/LogicalAggregateAccumulator.hpp"
#include "query/execution/BudgetMemoryResource.hpp"

using namespace graph;
using namespace graph::query::execution;

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

TEST_F(CountAccumulatorTest, CountsRowsWithoutMaterializingInputValue) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COUNT);
	acc->updateRow();
	acc->updateRow();
	acc->update(PropertyValue());
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

TEST_F(CountAccumulatorTest, ResetClearsRowsAndNonNullValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COUNT);
	acc->updateRow();
	acc->update(PropertyValue(int64_t(7)));
	acc->reset();
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 0);
}

namespace {

	class RowForwardingAccumulator final : public AggregateAccumulator {
	public:
		void update(const PropertyValue &value) override { value_ = value; }
		[[nodiscard]] PropertyValue getResult() const override { return value_; }
		void reset() override { value_ = PropertyValue(); }
		[[nodiscard]] std::unique_ptr<AggregateAccumulator> clone() const override {
			auto result = std::make_unique<RowForwardingAccumulator>();
			result->value_ = value_;
			return result;
		}

	private:
		PropertyValue value_;
	};

} // namespace

TEST(LogicalAggregateAccumulatorBaseTest, DefaultRowUpdateDelegatesToTheValueUpdateContract) {
	RowForwardingAccumulator concrete;
	AggregateAccumulator &accumulator = concrete;
	accumulator.updateRow();
	EXPECT_EQ(accumulator.getResult(), PropertyValue(int64_t(1)));
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

// Cypher numeric aggregates reject a non-NULL value outside the numeric domain.
TEST_F(SumAccumulatorTest, RejectsNonNumericValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_SUM);
	EXPECT_THROW(acc->update(PropertyValue(std::string("hello"))), std::invalid_argument);
	EXPECT_THROW(acc->update(PropertyValue(true)), std::invalid_argument);
}

TEST_F(SumAccumulatorTest, PositiveOverflowKeepsThePriorValueAndAllowsFurtherUpdates) {
	SumAccumulator acc;
	acc.update(PropertyValue(std::numeric_limits<int64_t>::max()));
	EXPECT_THROW(acc.update(PropertyValue(int64_t{1})), std::overflow_error);
	EXPECT_EQ(acc.getResult(), PropertyValue(std::numeric_limits<int64_t>::max()));
	acc.update(PropertyValue(int64_t{-1}));
	EXPECT_EQ(acc.getResult(), PropertyValue(std::numeric_limits<int64_t>::max() - 1));
}

TEST_F(SumAccumulatorTest, NegativeOverflowKeepsThePriorValueAndResetRestoresAnEmptyState) {
	SumAccumulator acc;
	acc.update(PropertyValue(std::numeric_limits<int64_t>::min()));
	EXPECT_THROW(acc.update(PropertyValue(int64_t{-1})), std::overflow_error);
	EXPECT_EQ(acc.getResult(), PropertyValue(std::numeric_limits<int64_t>::min()));
	acc.reset();
	EXPECT_EQ(acc.getResult().getType(), PropertyType::NULL_TYPE);
	acc.update(PropertyValue(int64_t{7}));
	EXPECT_EQ(acc.getResult(), PropertyValue(7));
}

TEST_F(SumAccumulatorTest, OppositeSignIntegerExtremesRemainExact) {
	for (bool minimumFirst : {false, true}) {
		SumAccumulator acc;
		acc.update(PropertyValue(minimumFirst ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max()));
		acc.update(PropertyValue(minimumFirst ? std::numeric_limits<int64_t>::max() : std::numeric_limits<int64_t>::min()));
		EXPECT_EQ(acc.getResult(), PropertyValue(int64_t{-1}));
	}
}

TEST_F(SumAccumulatorTest, FloatingInputDoesNotSilentlyMaskIntegerOverflow) {
	SumAccumulator acc;
	acc.update(PropertyValue(0.5));
	acc.update(PropertyValue(std::numeric_limits<int64_t>::max()));
	const PropertyValue before = acc.getResult();
	EXPECT_THROW(acc.update(PropertyValue(int64_t{1})), std::overflow_error);
	EXPECT_EQ(acc.getResult(), before);
	EXPECT_EQ(acc.clone()->getResult(), before);
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

TEST_F(AvgAccumulatorTest, RejectsNonNumericValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_AVG);
	EXPECT_THROW(acc->update(PropertyValue(std::string("not a number"))), std::invalid_argument);
}

// Avg with zero count (no values)
TEST_F(AvgAccumulatorTest, EmptyReturnsNull) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_AVG);
	auto result = acc->getResult();
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(AvgAccumulatorTest, CloneAndResetKeepIndependentRunningState) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_AVG);
	acc->update(PropertyValue(int64_t(4)));
	acc->update(PropertyValue(8.0));
	auto clone = acc->clone();
	acc->reset();
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
	EXPECT_DOUBLE_EQ(std::get<double>(clone->getResult().getVariant()), 6.0);
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

TEST_F(MinAccumulatorTest, CloneAndResetPreserveIndependentMinimum) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MIN);
	acc->update(PropertyValue(int64_t(9)));
	acc->update(PropertyValue(int64_t(3)));
	auto clone = acc->clone();
	acc->reset();
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(clone->getResult(), PropertyValue(int64_t(3)));
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

TEST_F(MaxAccumulatorTest, CloneAndResetPreserveIndependentMaximum) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MAX);
	acc->update(PropertyValue(int64_t(3)));
	acc->update(PropertyValue(int64_t(9)));
	auto clone = acc->clone();
	acc->reset();
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(clone->getResult(), PropertyValue(int64_t(9)));
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

// Cypher collect() ignores NULL inputs.
TEST_F(CollectAccumulatorTest, IgnoresNullValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COLLECT);
	acc->update(PropertyValue(int64_t(1)));
	acc->update(PropertyValue());
	acc->update(PropertyValue(int64_t(3)));
	auto result = acc->getResult();
	auto list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list, (std::vector<PropertyValue>{PropertyValue(1), PropertyValue(3)}));
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

TEST_F(CreateAccumulatorTest, RejectsInvalidPercentilesBeforeAccumulationCanIndexOutOfBounds) {
	for (const AggregateFunctionType type:
		 {AggregateFunctionType::AGG_PERCENTILE_DISC, AggregateFunctionType::AGG_PERCENTILE_CONT}) {
		EXPECT_THROW(static_cast<void>(createAccumulator(type, false, -0.01)), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(createAccumulator(type, false, 1.01)), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(createAccumulator(type, false, std::numeric_limits<double>::infinity())),
				 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(createAccumulator(type, false, std::numeric_limits<double>::quiet_NaN())),
				 std::invalid_argument);
	}
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

TEST_F(CountDistinctAccumulatorTest, DistinguishesValuesWithoutStringifying) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COUNT, true);
	acc->update(PropertyValue(int64_t(1)));
	acc->update(PropertyValue(std::string("1")));
	acc->update(PropertyValue(int64_t(1)));
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 2);
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

TEST_F(CollectDistinctAccumulatorTest, KeepsTypedDistinctValuesWithSameText) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COLLECT, true);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(std::string("10")));

	auto list = std::get<std::vector<PropertyValue>>(acc->getResult().getVariant());
	ASSERT_EQ(list.size(), 2u);
	EXPECT_EQ(list[0].getType(), PropertyType::INTEGER);
	EXPECT_EQ(list[1].getType(), PropertyType::STRING);
}

TEST_F(CollectDistinctAccumulatorTest, IgnoresNullValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COLLECT, true);
	acc->update(PropertyValue());
	acc->update(PropertyValue(int64_t(1)));
	acc->update(PropertyValue());
	acc->update(PropertyValue(int64_t(1)));

	EXPECT_EQ(acc->getResult(), PropertyValue(std::vector<PropertyValue>{PropertyValue(1)}));
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

TEST_F(PercentileContAccumulatorTest, RejectsStringType) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_PERCENTILE_CONT, false, 0.5);
	EXPECT_THROW(acc->update(PropertyValue(std::string("hello"))), std::invalid_argument);
}

TEST(LogicalAggregateAccumulatorTest, CalculatesSampleAndPopulationStandardDeviationForTheCanonicalDataset) {
	auto sample = createAccumulator(AggregateFunctionType::AGG_STDEV);
	auto population = createAccumulator(AggregateFunctionType::AGG_STDEVP);
	for (const int64_t value: {2, 4, 4, 4, 5, 5, 7, 9}) {
		sample->update(PropertyValue(value));
		population->update(PropertyValue(value));
	}
	EXPECT_NEAR(std::get<double>(sample->getResult().getVariant()), 2.138089935, 1e-9);
	EXPECT_NEAR(std::get<double>(population->getResult().getVariant()), 2.0, 1e-9);
}

TEST(LogicalAggregateAccumulatorTest, CalculatesDiscretePercentileAtBothBoundsAndAtTheMedian) {
	for (const auto [percentile, expected]: std::initializer_list<std::pair<double, int64_t>>{
				 {0.0, 1}, {0.5, 3}, {1.0, 5}}) {
		auto accumulator = createAccumulator(AggregateFunctionType::AGG_PERCENTILE_DISC, false, percentile);
		for (const int64_t value: {1, 2, 3, 4, 5}) {
			accumulator->update(PropertyValue(value));
		}
		EXPECT_EQ(accumulator->getResult(), PropertyValue(expected));
	}
}

TEST(LogicalAggregateAccumulatorTest, InterpolatesContinuousPercentilesAtFractionalRanks) {
	auto accumulator = createAccumulator(AggregateFunctionType::AGG_PERCENTILE_CONT, false, 0.25);
	for (const int64_t value: {1, 2, 3, 4}) {
		accumulator->update(PropertyValue(value));
	}
	const PropertyValue result = accumulator->getResult();
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 1.75);
}

TEST(LogicalAggregateAccumulatorTest, PreservesSingleValuePopulationAndExactContinuousPercentileSemantics) {
	auto population = createAccumulator(AggregateFunctionType::AGG_STDEVP);
	population->update(PropertyValue(int64_t(5)));
	EXPECT_EQ(population->getResult(), PropertyValue(0.0));

	auto continuous = createAccumulator(AggregateFunctionType::AGG_PERCENTILE_CONT, false, 0.5);
	for (const int64_t value: {1, 2, 3}) {
		continuous->update(PropertyValue(value));
	}
	EXPECT_EQ(continuous->getResult(), PropertyValue(int64_t(2)));
}

TEST(LogicalAggregateAccumulatorTest, TakesOwnedPayloadResultsAndSupportsReuse) {
	const PropertyValue large(std::string(8192U, 'z'));
	const PropertyValue small(std::string(4096U, 'a'));
	for (const auto function: {AggregateFunctionType::AGG_MIN, AggregateFunctionType::AGG_MAX,
			AggregateFunctionType::AGG_COLLECT}) {
		for (const bool distinct: {false, true}) {
			auto accumulator = createAccumulator(function, distinct);
			accumulator->update(large);
			accumulator->update(small);
			accumulator->update(large);
			accumulator->update(PropertyValue());
			PropertyValue expected;
			if (function == AggregateFunctionType::AGG_MIN) expected = small;
			else if (function == AggregateFunctionType::AGG_MAX) expected = large;
			else expected = PropertyValue(distinct ? std::vector<PropertyValue>{large, small} :
					std::vector<PropertyValue>{large, small, large});
			EXPECT_EQ(accumulator->getResult(), expected);
			PropertyValue transferred = accumulator->takeResult();
			EXPECT_EQ(transferred, expected);
			EXPECT_EQ(accumulator->retainedBytes(), 0U);
			const auto empty = accumulator->takeResult();
			if (function == AggregateFunctionType::AGG_COLLECT) EXPECT_TRUE(empty.getList().empty());
			else EXPECT_EQ(empty, PropertyValue());
			accumulator->update(small);
			const auto fresh = accumulator->takeResult();
			EXPECT_EQ(fresh, function == AggregateFunctionType::AGG_COLLECT ?
					PropertyValue(std::vector<PropertyValue>{small}) : small);
			accumulator.reset();
			EXPECT_EQ(transferred, expected);
		}
	}
}

TEST(LogicalAggregateAccumulatorBaseTest, DestructiveFinalizationResetsScalarState) {
	auto accumulator = createAccumulator(AggregateFunctionType::AGG_COUNT);
	accumulator->updateRow();
	accumulator->updateRow();
	EXPECT_EQ(accumulator->takeResult(), PropertyValue(2));
	EXPECT_EQ(accumulator->getResult(), PropertyValue(0));
	accumulator->updateRow();
	EXPECT_EQ(accumulator->takeResult(), PropertyValue(1));
}

TEST(LogicalAggregateAccumulatorTest, DestructivePercentilesMatchSnapshotsReleaseCapacityAndSupportReuse) {
	for (const bool continuous : {false, true}) {
		for (const double percentile : {0.0, 0.25, 0.5, 1.0}) {
			auto accumulator = createAccumulator(continuous ? AggregateFunctionType::AGG_PERCENTILE_CONT :
				AggregateFunctionType::AGG_PERCENTILE_DISC, false, percentile);
			EXPECT_EQ(accumulator->takeResult(), PropertyValue());
			for (const auto &value : {PropertyValue(9), PropertyValue(1), PropertyValue(), PropertyValue(5.0), PropertyValue(5)})
				accumulator->update(value);
			const auto snapshot = accumulator->getResult();
			const auto retained = accumulator->retainedBytes();
			ASSERT_GT(retained, 0U);
			EXPECT_EQ(accumulator->getResult(), snapshot);
			EXPECT_EQ(accumulator->retainedBytes(), retained);
			auto clone = accumulator->clone();
			const auto taken = accumulator->takeResult();
			EXPECT_EQ(taken, snapshot); EXPECT_EQ(taken.getType(), snapshot.getType());
			EXPECT_EQ(accumulator->retainedBytes(), 0U);
			EXPECT_EQ(accumulator->takeResult(), PropertyValue());
			EXPECT_EQ(clone->takeResult(), snapshot); EXPECT_EQ(clone->retainedBytes(), 0U);
			accumulator->update(PropertyValue(7));
			EXPECT_EQ(accumulator->takeResult(), PropertyValue(7));
			EXPECT_EQ(accumulator->retainedBytes(), 0U);
		}
	}
}

TEST(LogicalAggregateAccumulatorTest, PercentileArrayGrowthUsesSharedAdmissionAndPreservesStateOnRejection) {
	for (const auto function : {AggregateFunctionType::AGG_PERCENTILE_DISC, AggregateFunctionType::AGG_PERCENTILE_CONT}) {
		MaterializedByteBudget budget(4096U);
		detail::BudgetMemoryResource memory(budget);
		MaterializedByteReservation downstream(&budget); downstream.set(3072U);
		auto accumulator = createAccumulator(function, false, 0.25, &memory);
		size_t accepted = 0U;
		for (; accepted < 256U; ++accepted) {
			try { accumulator->update(PropertyValue(static_cast<int64_t>(accepted))); }
			catch (const std::runtime_error &) { break; }
		}
		ASSERT_GT(accepted, 1U); ASSERT_LT(accepted, 256U);
		const auto snapshot = accumulator->getResult();
		const auto charged = budget.used();
		EXPECT_GT(charged, downstream.bytes());
		EXPECT_GT(accumulator->retainedBytes(), 0U);
		EXPECT_EQ(accumulator->retainedBytesForAdmission(), 0U);
		EXPECT_THROW(accumulator->update(PropertyValue(10000)), std::runtime_error);
		EXPECT_EQ(accumulator->getResult(), snapshot); EXPECT_EQ(budget.used(), charged);
		auto clone = accumulator->clone();
		EXPECT_EQ(clone->retainedBytesForAdmission(), clone->retainedBytes());
		downstream.reset();
		EXPECT_NO_THROW(accumulator->update(PropertyValue(10000)));
		const auto updated = accumulator->getResult();
		EXPECT_EQ(clone->takeResult(), snapshot);
		EXPECT_EQ(accumulator->takeResult(), updated); EXPECT_EQ(budget.used(), 0U);
		accumulator->update(PropertyValue(7));
		EXPECT_EQ(accumulator->takeResult(), PropertyValue(7)); EXPECT_EQ(budget.used(), 0U);
	}
}

TEST(LogicalAggregateAccumulatorTest, PayloadAdmissionRejectsBeforeMutatingCollectMinAndMax) {
	PropertyValue::MapType map; map.max_load_factor(4.0F);
	for (int i = 0; i < 32; ++i) map.emplace(std::to_string(i), PropertyValue(std::string(256U, 'x')));
	const PropertyValue nested(std::move(map));
	for (const auto function : {AggregateFunctionType::AGG_COLLECT, AggregateFunctionType::AGG_MIN, AggregateFunctionType::AGG_MAX}) {
		auto accumulator = createAccumulator(function);
		const auto before = accumulator->getResult();
		size_t admitted = 0U;
		EXPECT_THROW(accumulator->updateAdmitted(nested, [&](size_t bytes) {
			admitted = bytes; if (bytes > 512U) throw std::runtime_error("test allowance");
		}), std::runtime_error);
		EXPECT_GT(admitted, 512U); EXPECT_EQ(accumulator->getResult(), before);
		accumulator->updateAdmitted(nested, [](size_t) {});
		const auto result = accumulator->takeResult();
		EXPECT_EQ(function == AggregateFunctionType::AGG_COLLECT ? result.getList().front() : result, nested);
		EXPECT_EQ(accumulator->retainedBytes(), 0U);
	}
}

TEST(LogicalAggregateAccumulatorTest, AdmittedExtremaReplacementPreservesExistingValueOnRejection) {
	for (const auto function : {AggregateFunctionType::AGG_MIN, AggregateFunctionType::AGG_MAX}) {
		auto accumulator = createAccumulator(function);
		const PropertyValue seed(std::string(128U, 'm'));
		accumulator->updateAdmitted(seed, [](size_t) {});
		const PropertyValue winner(std::string(1024U, function == AggregateFunctionType::AGG_MIN ? 'a' : 'z'));
		EXPECT_THROW(accumulator->updateAdmitted(winner, [](size_t bytes) {
			if (bytes > 512U) throw std::runtime_error("test allowance");
		}), std::runtime_error);
		EXPECT_EQ(accumulator->getResult(), seed);
		const PropertyValue loser(std::string(1024U, function == AggregateFunctionType::AGG_MIN ? 'z' : 'a'));
		EXPECT_NO_THROW(accumulator->updateAdmitted(loser, [](size_t) { throw std::runtime_error("unused admission"); }));
		EXPECT_EQ(accumulator->getResult(), seed);
		accumulator->updateAdmitted(winner, [](size_t) {});
		EXPECT_EQ(accumulator->takeResult(), winner);
	}
}

TEST(LogicalAggregateAccumulatorTest, CollectCancellationDuringNestedCopyPreservesExistingState) {
	auto accumulator = createAccumulator(AggregateFunctionType::AGG_COLLECT);
	accumulator->updateAdmitted(PropertyValue(7), [](size_t) {});
	const auto before = accumulator->getResult();
	std::vector<PropertyValue> values;
	for (int i = 0; i < 64; ++i) values.emplace_back(std::string(256U, 'x'));
	const PropertyValue nested(std::move(values));
	size_t positiveCalls = 0U;
	EXPECT_THROW(accumulator->updateAdmitted(nested, [&](size_t bytes) {
		if (bytes != 0U && ++positiveCalls == 3U) throw std::runtime_error("test cancellation");
	}), std::runtime_error);
	EXPECT_EQ(positiveCalls, 3U);
	EXPECT_EQ(accumulator->getResult(), before);
	accumulator->updateAdmitted(nested, [](size_t) {});
	const auto result = accumulator->takeResult();
	ASSERT_EQ(result.getList().size(), 2U);
	EXPECT_EQ(result.getList()[0], PropertyValue(7));
	EXPECT_EQ(result.getList()[1], nested);
	EXPECT_EQ(accumulator->retainedBytes(), 0U);
}

TEST_F(CollectDistinctAccumulatorTest, AdmissionRejectionAndCancellationPermitRetry) {
	CollectDistinctAccumulator accumulator;
	accumulator.update(PropertyValue(7));
	const PropertyValue nested(std::vector<PropertyValue>(64U, PropertyValue(std::string(256U, 'x'))));
	const auto before = accumulator.getResult();
	EXPECT_THROW(accumulator.updateAdmitted(nested, [](size_t bytes) {
		if (bytes > 512U) throw std::runtime_error("allowance");
	}), std::runtime_error);
	EXPECT_EQ(accumulator.getResult(), before);
	size_t calls = 0U;
	EXPECT_THROW(accumulator.updateAdmitted(nested, [&](size_t bytes) {
		if (bytes && ++calls == 3U) throw std::runtime_error("cancel");
	}), std::runtime_error);
	EXPECT_EQ(accumulator.getResult(), before);
	accumulator.updateAdmitted(nested, [](size_t) {});
	EXPECT_NO_THROW(accumulator.updateAdmitted(nested, [](size_t) { throw std::runtime_error("duplicate copied"); }));
	EXPECT_NO_THROW(accumulator.updateAdmitted(PropertyValue(), [](size_t) { throw std::runtime_error("null copied"); }));
	const auto result = accumulator.takeResult();
	ASSERT_EQ(result.getList().size(), 2U);
	EXPECT_EQ(result.getList()[0], PropertyValue(7));
	EXPECT_EQ(result.getList()[1], nested);
	EXPECT_EQ(accumulator.retainedBytes(), 0U);
	accumulator.updateAdmitted(nested, [](size_t) {});
	ASSERT_EQ(accumulator.getResult().getList().size(), 1U);
}

TEST_F(CollectDistinctAccumulatorTest, StringAdmissionChargesBothRetainedCopiesAndSkipsDuplicates) {
	CollectDistinctAccumulator accumulator;
	const PropertyValue value(std::string(4096U, 's'));
	size_t peak = 0U;
	EXPECT_THROW(accumulator.updateAdmitted(value, [&](size_t bytes) {
		peak = bytes;
		if (bytes > 4096U) throw std::runtime_error("allowance");
	}), std::runtime_error);
	EXPECT_GE(peak, 8192U);
	EXPECT_TRUE(accumulator.getResult().getList().empty());
	accumulator.updateAdmitted(value, [](size_t) {});
	EXPECT_NO_THROW(accumulator.updateAdmitted(value, [](size_t) { throw std::runtime_error("duplicate copied"); }));
	accumulator.updateAdmitted(PropertyValue(1), [](size_t) {});
	accumulator.updateAdmitted(PropertyValue(1.0), [](size_t) {});
	accumulator.updateAdmitted(PropertyValue("1"), [](size_t) {});
	const auto result = accumulator.takeResult();
	ASSERT_EQ(result.getList().size(), 4U);
	EXPECT_EQ(result.getList()[0], value);
	EXPECT_EQ(result.getList()[1], PropertyValue(1));
	EXPECT_EQ(result.getList()[2], PropertyValue(1.0));
	EXPECT_EQ(result.getList()[3], PropertyValue("1"));
}

TEST_F(CreateAccumulatorTest, ObjectAdmissionRunsBeforeCreationAndPropagatesRejection) {
	for (const auto function : {AggregateFunctionType::AGG_COUNT, AggregateFunctionType::AGG_SUM,
		AggregateFunctionType::AGG_AVG, AggregateFunctionType::AGG_MIN, AggregateFunctionType::AGG_MAX,
		AggregateFunctionType::AGG_COLLECT, AggregateFunctionType::AGG_STDEV, AggregateFunctionType::AGG_STDEVP,
		AggregateFunctionType::AGG_PERCENTILE_DISC, AggregateFunctionType::AGG_PERCENTILE_CONT}) {
		for (const bool distinct : {false, true}) {
			size_t calls = 0U;
			EXPECT_THROW(static_cast<void>(createAccumulator(function, distinct, 0.5, nullptr, [&](size_t bytes) {
				++calls; EXPECT_GE(bytes, sizeof(AggregateAccumulator));
				throw std::runtime_error("object budget");
			})), std::runtime_error);
			EXPECT_EQ(calls, 1U);
			auto accumulator = createAccumulator(function, distinct, 0.5, nullptr, [](size_t) {});
			ASSERT_NE(accumulator, nullptr);
			EXPECT_NO_THROW(accumulator->update(PropertyValue(7)));
		}
	}
}

TEST_F(CollectDistinctAccumulatorTest, RetainedAccountingIncludesBothSparseMapBucketArrays) {
	PropertyValue::MapType sparse;
	sparse.reserve(1024U);
	sparse.emplace("one", PropertyValue(7));
	const size_t bucketBytes = sparse.bucket_count() * sizeof(void *);
	const PropertyValue value(std::move(sparse));
	CollectDistinctAccumulator accumulator;
	accumulator.updateAdmitted(value, [](size_t) {});
	EXPECT_GE(accumulator.retainedBytes(), 2U * bucketBytes);
	const size_t retained = accumulator.retainedBytes();
	accumulator.updateAdmitted(value, [](size_t) { throw std::runtime_error("duplicate allocation"); });
	EXPECT_EQ(accumulator.retainedBytes(), retained);
	auto clone = accumulator.clone();
	EXPECT_EQ(clone->getResult(), accumulator.getResult());
	EXPECT_GE(clone->retainedBytes(), graph::query::execution::detail::retainedPropertyValueBytes(clone->getResult().getList()[0]));
	const auto result = accumulator.takeResult();
	ASSERT_EQ(result.getList().size(), 1U);
	EXPECT_EQ(result.getList()[0], value);
	EXPECT_EQ(accumulator.retainedBytes(), 0U);
}
