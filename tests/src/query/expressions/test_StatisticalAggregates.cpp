/**
 * @file test_StatisticalAggregates.cpp
 * @date 2026/04/17
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

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/operators/AggregateAccumulator.hpp"

using namespace graph;
using namespace graph::query::execution::operators;

// ============================================================================
// StDevAccumulator (sample) tests
// ============================================================================

TEST(StDevAccumulator_Expressions_Test, SampleStDev) {
	StDevAccumulator acc(false);  // sample
	for (int v : {2, 4, 4, 4, 5, 5, 7, 9}) {
		acc.update(PropertyValue(static_cast<int64_t>(v)));
	}
	auto result = acc.getResult();
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 2.138, 0.01);
}

TEST(StDevAccumulator_Expressions_Test, SampleSingleValueReturnsNull) {
	StDevAccumulator acc(false);  // sample
	acc.update(PropertyValue(static_cast<int64_t>(42)));
	auto result = acc.getResult();
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST(StDevAccumulator_Expressions_Test, SampleEmptyReturnsNull) {
	StDevAccumulator acc(false);  // sample
	auto result = acc.getResult();
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// StDevAccumulator (population) tests
// ============================================================================

TEST(StDevAccumulator_Expressions_Test, PopulationStDev) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_STDEVP);
	for (int v : {2, 4, 4, 4, 5, 5, 7, 9}) {
		acc->update(PropertyValue(static_cast<int64_t>(v)));
	}
	auto result = acc->getResult();
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	// Population stdev of [2,4,4,4,5,5,7,9]: mean=5, variance=4*30/64 -> sqrt(32/8) = 2.0? No.
	// mean = 40/8 = 5.0
	// sum of squared deviations = 9+1+1+1+0+0+4+16 = 32
	// population variance = 32/8 = 4.0, stdevP = 2.0
	// Wait, but the user says ~1.87. Let me recompute:
	// Actually: (2-5)^2=9, (4-5)^2=1, (4-5)^2=1, (4-5)^2=1, (5-5)^2=0, (5-5)^2=0, (7-5)^2=4, (9-5)^2=16
	// Sum = 32, pop var = 32/8 = 4.0, stdevP = 2.0
	// Sample var = 32/7 ≈ 4.571, sample stdev ≈ 2.138
	// The user's hint of ~1.87 seems incorrect; the actual population stdev is 2.0.
	EXPECT_NEAR(std::get<double>(result.getVariant()), 2.0, 0.01);
}

TEST(StDevAccumulator_Expressions_Test, PopulationEmptyReturnsNull) {
	StDevAccumulator acc(true);  // population
	auto result = acc.getResult();
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// PercentileDiscAccumulator tests
// ============================================================================

TEST(PercentileDiscAccumulator_Expressions_Test, MedianOfFiveValues) {
	PercentileDiscAccumulator acc(0.5);
	for (int v : {1, 2, 3, 4, 5}) {
		acc.update(PropertyValue(static_cast<int64_t>(v)));
	}
	auto result = acc.getResult();
	ASSERT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 3);
}

TEST(PercentileDiscAccumulator_Expressions_Test, ZeroPercentile) {
	PercentileDiscAccumulator acc(0.0);
	for (int v : {1, 2, 3, 4, 5}) {
		acc.update(PropertyValue(static_cast<int64_t>(v)));
	}
	auto result = acc.getResult();
	ASSERT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 1);
}

TEST(PercentileDiscAccumulator_Expressions_Test, OneHundredPercentile) {
	PercentileDiscAccumulator acc(1.0);
	for (int v : {1, 2, 3, 4, 5}) {
		acc.update(PropertyValue(static_cast<int64_t>(v)));
	}
	auto result = acc.getResult();
	ASSERT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 5);
}

TEST(PercentileDiscAccumulator_Expressions_Test, EmptyReturnsNull) {
	PercentileDiscAccumulator acc(0.5);
	auto result = acc.getResult();
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// PercentileContAccumulator tests
// ============================================================================

TEST(PercentileContAccumulator_Expressions_Test, InterpolateTwoValues) {
	PercentileContAccumulator acc(0.5);
	acc.update(PropertyValue(static_cast<int64_t>(1)));
	acc.update(PropertyValue(static_cast<int64_t>(2)));
	auto result = acc.getResult();
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 1.5, 0.001);
}

TEST(PercentileContAccumulator_Expressions_Test, MedianOfThreeValues) {
	PercentileContAccumulator acc(0.5);
	for (int v : {1, 2, 3}) {
		acc.update(PropertyValue(static_cast<int64_t>(v)));
	}
	auto result = acc.getResult();
	// rank = 0.5 * 2 = 1.0, lo == hi == 1, returns sorted[1] = 2
	ASSERT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 2);
}

TEST(PercentileContAccumulator_Expressions_Test, EmptyReturnsNull) {
	PercentileContAccumulator acc(0.5);
	auto result = acc.getResult();
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// createAccumulator factory tests for statistical types
// ============================================================================

TEST(CreateAccumulatorStatisticalTest, StDevFactory) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_STDEV);
	ASSERT_NE(acc, nullptr);
	// Sample stdev: single value should return null (n < 2)
	acc->update(PropertyValue(static_cast<int64_t>(5)));
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
}

TEST(CreateAccumulatorStatisticalTest, StDevPFactory) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_STDEVP);
	ASSERT_NE(acc, nullptr);
	// Population stdev: single value should return 0.0 (n >= 1)
	acc->update(PropertyValue(static_cast<int64_t>(5)));
	auto result = acc->getResult();
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 0.0);
}

TEST(CreateAccumulatorStatisticalTest, PercentileDiscFactory) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_PERCENTILE_DISC, false, 0.75);
	ASSERT_NE(acc, nullptr);
	for (int v : {1, 2, 3, 4}) {
		acc->update(PropertyValue(static_cast<int64_t>(v)));
	}
	auto result = acc->getResult();
	ASSERT_EQ(result.getType(), PropertyType::INTEGER);
	// floor(0.75 * 3) = floor(2.25) = 2 -> sorted[2] = 3
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 3);
}

TEST(CreateAccumulatorStatisticalTest, PercentileContFactory) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_PERCENTILE_CONT, false, 0.25);
	ASSERT_NE(acc, nullptr);
	for (int v : {1, 2, 3, 4}) {
		acc->update(PropertyValue(static_cast<int64_t>(v)));
	}
	auto result = acc->getResult();
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	// rank = 0.25 * 3 = 0.75, lo=0, hi=1, frac=0.75
	// 1.0 + 0.75 * (2.0 - 1.0) = 1.75
	EXPECT_NEAR(std::get<double>(result.getVariant()), 1.75, 0.001);
}
