/**
 * @file test_ChecksumUtils.cpp
 * @author Nexepic
 * @date 2025/8/1
 *
 * @copyright Copyright (c) 2025 Nexepic
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

#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "graph/utils/ChecksumUtils.hpp"

class ChecksumUtilsTest : public ::testing::Test {
protected:
	void SetUp() override {}
	void TearDown() override {}
};

// Test calculateCrc with various inputs
TEST_F(ChecksumUtilsTest, CalculateCrcEmptyData) {
	uint32_t result = graph::utils::calculateCrc(nullptr, 0);
	EXPECT_EQ(result, 0u); // CRC of empty data should be zero
}

TEST_F(ChecksumUtilsTest, CalculateCrcSingleByte) {
	uint8_t data = 0x42;
	uint32_t result = graph::utils::calculateCrc(&data, sizeof(data));
	EXPECT_NE(result, 0u);
}

TEST_F(ChecksumUtilsTest, CalculateCrcMultipleBytes) {
	std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05};
	uint32_t result = graph::utils::calculateCrc(data.data(), data.size());
	EXPECT_NE(result, 0u);
}

TEST_F(ChecksumUtilsTest, CalculateCrcStringData) {
	const char *testString = "Hello, World!";
	uint32_t result = graph::utils::calculateCrc(testString, strlen(testString));
	EXPECT_NE(result, 0u);
}

TEST_F(ChecksumUtilsTest, CalculateCrcLargeData) {
	std::vector<uint8_t> largeData(1024, 0xAA);
	uint32_t result = graph::utils::calculateCrc(largeData.data(), largeData.size());
	EXPECT_NE(result, 0u);
}

TEST_F(ChecksumUtilsTest, CalculateCrcConsistency) {
	std::vector<uint8_t> data = {0x10, 0x20, 0x30, 0x40};
	uint32_t result1 = graph::utils::calculateCrc(data.data(), data.size());
	uint32_t result2 = graph::utils::calculateCrc(data.data(), data.size());
	EXPECT_EQ(result1, result2);
}

TEST_F(ChecksumUtilsTest, CalculateCrcDifferentData) {
	std::vector<uint8_t> data1 = {0x01, 0x02, 0x03};
	std::vector<uint8_t> data2 = {0x01, 0x02, 0x04};
	uint32_t result1 = graph::utils::calculateCrc(data1.data(), data1.size());
	uint32_t result2 = graph::utils::calculateCrc(data2.data(), data2.size());
	EXPECT_NE(result1, result2);
}

// Test updateCrc with various inputs
TEST_F(ChecksumUtilsTest, UpdateCrcZeroInitial) {
	std::vector<uint8_t> data = {0x01, 0x02, 0x03};
	uint32_t result = graph::utils::updateCrc(0, data.data(), data.size());
	EXPECT_NE(result, 0u);
}

TEST_F(ChecksumUtilsTest, UpdateCrcNonZeroInitial) {
	std::vector<uint8_t> data = {0x01, 0x02, 0x03};
	uint32_t initialCrc = 0x12345678;
	uint32_t result = graph::utils::updateCrc(initialCrc, data.data(), data.size());
	EXPECT_NE(result, initialCrc);
}

TEST_F(ChecksumUtilsTest, UpdateCrcEmptyData) {
	uint32_t initialCrc = 0x12345678;
	uint32_t result = graph::utils::updateCrc(initialCrc, nullptr, 0);
	// zlib crc32 returns 0 when data is nullptr, regardless of initial value
	EXPECT_EQ(result, 0u);
}

TEST_F(ChecksumUtilsTest, UpdateCrcChaining) {
	std::vector<uint8_t> data1 = {0x01, 0x02};
	std::vector<uint8_t> data2 = {0x03, 0x04};

	uint32_t crc1 = graph::utils::calculateCrc(data1.data(), data1.size());
	uint32_t crc2 = graph::utils::updateCrc(crc1, data2.data(), data2.size());

	// Compare with single calculation
	std::vector<uint8_t> combinedData = {0x01, 0x02, 0x03, 0x04};
	uint32_t directCrc = graph::utils::calculateCrc(combinedData.data(), combinedData.size());

	EXPECT_EQ(crc2, directCrc);
}

TEST_F(ChecksumUtilsTest, UpdateCrcMultipleUpdates) {
	std::vector<uint8_t> data = {0xFF};
	uint32_t crc = 0;

	for (int i = 0; i < 5; ++i) {
		crc = graph::utils::updateCrc(crc, data.data(), data.size());
	}

	// Should be different from initial value
	EXPECT_NE(crc, 0u);
}

// Boundary and edge cases
TEST_F(ChecksumUtilsTest, CalculateCrcMaxValues) {
	std::vector<uint8_t> maxData(1, 0xFF);
	uint32_t result = graph::utils::calculateCrc(maxData.data(), maxData.size());
	EXPECT_NE(result, 0u);
}

TEST_F(ChecksumUtilsTest, UpdateCrcMaxInitialValue) {
	std::vector<uint8_t> data = {0x01};
	uint32_t result = graph::utils::updateCrc(UINT32_MAX, data.data(), data.size());
	EXPECT_NE(result, UINT32_MAX);
}

TEST_F(ChecksumUtilsTest, CrcFunctionsEquivalence) {
	std::vector<uint8_t> data = {0x11, 0x22, 0x33, 0x44, 0x55};

	uint32_t directCrc = graph::utils::calculateCrc(data.data(), data.size());
	uint32_t updateCrc = graph::utils::updateCrc(0, data.data(), data.size());

	// Both methods should produce the same result when starting from 0
	EXPECT_EQ(directCrc, updateCrc);
}
