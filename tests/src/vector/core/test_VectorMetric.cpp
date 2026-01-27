/**
 * @file test_VectorMetric.cpp
 * @author Nexepic
 * @date 2026/1/26
 *
 * @copyright Copyright (c) 2026 Nexepic
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
#include <vector>
#include "graph/vector/core/VectorMetric.hpp"
#include "graph/vector/core/BFloat16.hpp"

using namespace graph::vector;

// Test Fixture for Data Setup
class MetricTest : public ::testing::Test {
protected:
    std::vector<float> fA = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}; // Dim 5 (Odd, triggers tail loop)
    std::vector<float> fB = {0.5f, 1.5f, 2.5f, 3.5f, 4.5f};
    // Diff: 0.5 each. Sq: 0.25 * 5 = 1.25.
    // IP: 0.5 + 3 + 7.5 + 14 + 22.5 = 47.5 -> Negated: -47.5

    std::vector<BFloat16> bfA;
    std::vector<BFloat16> bfB;

    void SetUp() override {
        bfA = toBFloat16(fA);
        bfB = toBFloat16(fB);
    }
};

// --- L2 Tests ---

TEST_F(MetricTest, L2_Float_Float) {
    float res = VectorMetric::computeL2Sqr(fA.data(), fB.data(), 5);
    EXPECT_NEAR(res, 1.25f, 0.001f);
}

TEST_F(MetricTest, L2_Float_BF16) {
    float res = VectorMetric::computeL2Sqr(fA.data(), bfB.data(), 5);
    // BF16 precision loss expected
    EXPECT_NEAR(res, 1.25f, 0.1f);
}

TEST_F(MetricTest, L2_BF16_BF16) {
    float res = VectorMetric::computeL2Sqr(bfA.data(), bfB.data(), 5);
    EXPECT_NEAR(res, 1.25f, 0.1f);
}

// --- IP Tests (Targeting missed coverage) ---

TEST_F(MetricTest, IP_Float_Float) {
    float res = VectorMetric::computeIP(fA.data(), fB.data(), 5);
    EXPECT_NEAR(res, -47.5f, 0.001f);
}

TEST_F(MetricTest, IP_Float_BF16) {
    float res = VectorMetric::computeIP(fA.data(), bfB.data(), 5);
    EXPECT_NEAR(res, -47.5f, 0.5f);
}

TEST_F(MetricTest, IP_BF16_BF16) {
    float res = VectorMetric::computeIP(bfA.data(), bfB.data(), 5);
    EXPECT_NEAR(res, -47.5f, 0.5f);
}

// --- Dimension Edge Cases ---

TEST(MetricEdgeTest, TinyDimension) {
    // Dim 1 (Loop unrolling shouldn't run)
    float a = 2.0f, b = 3.0f;
    EXPECT_NEAR(VectorMetric::computeL2Sqr(&a, &b, 1), 1.0f, 0.001f);
}

TEST(MetricEdgeTest, LargeDimension) {
    // Dim 1024 (Loop unrolling runs many times)
    std::vector<float> a(1024, 1.0f);
    std::vector<float> b(1024, 0.0f);
    EXPECT_NEAR(VectorMetric::computeL2Sqr(a.data(), b.data(), 1024), 1024.0f, 0.001f);
}