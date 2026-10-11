/**
 * @file test_VectorMath.cpp
 * @brief Native common Vector math tests.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "graph/common/VectorMath.hpp"

namespace graph::common {
namespace {

TEST(VectorMathTest, PreservesBFloat16FiniteAndInfiniteConversions) {
	const std::vector<float> input{1.0F, -1.0F, 0.5F, 3.14159F, 0.0F,
									 std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
	const std::vector<float> output = toFloat(toBFloat16(input));
	ASSERT_EQ(output.size(), input.size());
	for (size_t index = 0U; index < 5U; ++index) {
		EXPECT_NEAR(output[index], input[index], 0.05F);
	}
	EXPECT_TRUE(std::isinf(output[5U]));
	EXPECT_TRUE(std::isinf(output[6U]));
	EXPECT_LT(output[6U], 0.0F);
}

TEST(VectorMathTest, ComputesAllMetricPrecisionPairsAndTailDimensions) {
	const std::vector<float> left{1.0F, 2.0F, 3.0F, 4.0F, 5.0F};
	const std::vector<float> right{0.5F, 1.5F, 2.5F, 3.5F, 4.5F};
	const std::vector<BFloat16> bfLeft = toBFloat16(left);
	const std::vector<BFloat16> bfRight = toBFloat16(right);

	EXPECT_NEAR(VectorMetric::computeL2Sqr(left.data(), right.data(), left.size()), 1.25F, 0.001F);
	EXPECT_NEAR(VectorMetric::computeL2Sqr(left.data(), bfRight.data(), left.size()), 1.25F, 0.1F);
	EXPECT_NEAR(VectorMetric::computeL2Sqr(bfLeft.data(), bfRight.data(), left.size()), 1.25F, 0.1F);
	EXPECT_NEAR(VectorMetric::computeIP(left.data(), right.data(), left.size()), -47.5F, 0.001F);
	EXPECT_NEAR(VectorMetric::computeIP(left.data(), bfRight.data(), left.size()), -47.5F, 0.5F);
	EXPECT_NEAR(VectorMetric::computeIP(bfLeft.data(), bfRight.data(), left.size()), -47.5F, 0.5F);

	const float singleLeft = 2.0F;
	const float singleRight = 3.0F;
	EXPECT_NEAR(VectorMetric::computeL2Sqr(&singleLeft, &singleRight, 1U), 1.0F, 0.001F);
}

} // namespace
} // namespace graph::common
