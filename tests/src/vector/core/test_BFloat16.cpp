/**
 * @file test_BFloat16.cpp
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
#include <cmath>
#include "graph/vector/core/BFloat16.hpp"

using namespace graph::vector;

TEST(BFloat16Test, ConversionAccuracy) {
	std::vector<float> input = {1.0f, -1.0f, 0.5f, 3.14159f, 0.0f};
	auto bf16 = toBFloat16(input);
	auto output = toFloat(bf16);

	ASSERT_EQ(input.size(), output.size());

	for (size_t i = 0; i < input.size(); ++i) {
		// BFloat16 has less precision, so exact equality is not guaranteed for all floats.
		// But for small numbers it should be close.
		EXPECT_NEAR(input[i], output[i], 0.05f)
			<< "Mismatch at index " << i << ": " << input[i] << " vs " << output[i];
	}
}

TEST(BFloat16Test, ZeroAndInfinity) {
	float inf = std::numeric_limits<float>::infinity();
	std::vector<float> special = {0.0f, -0.0f, inf, -inf};
	auto bf = toBFloat16(special);
	auto out = toFloat(bf);

	EXPECT_EQ(out[0], 0.0f);
	EXPECT_EQ(out[1], 0.0f); // -0.0 == 0.0
	EXPECT_TRUE(std::isinf(out[2]));
	EXPECT_TRUE(std::isinf(out[3]));
}