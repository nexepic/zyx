/**
 * @file test_CompressUtils.cpp
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

#include <gtest/gtest.h>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include "graph/utils/CompressUtils.hpp"

class CompressUtilsTest : public ::testing::Test {
protected:
	void SetUp() override {}
	void TearDown() override {}

	// Helper function to generate random string
	static std::string generateRandomString(size_t length) {
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<> dis(0, 255);

		std::string result;
		result.reserve(length);
		for (size_t i = 0; i < length; ++i) {
			result += static_cast<char>(dis(gen));
		}
		return result;
	}
};

// Test zlibCompress with various inputs
TEST_F(CompressUtilsTest, ZlibCompressEmptyString) {
	std::string emptyData = "";
	std::string compressed = graph::utils::zlibCompress(emptyData);
	EXPECT_GT(compressed.size(), 0UL); // Compressed empty data should have some header bytes
}

TEST_F(CompressUtilsTest, ZlibCompressSingleCharacter) {
	std::string singleChar = "A";
	std::string compressed = graph::utils::zlibCompress(singleChar);
	EXPECT_GT(compressed.size(), 0UL);
	EXPECT_NE(compressed, singleChar);
}

TEST_F(CompressUtilsTest, ZlibCompressShortString) {
	std::string shortString = "Hello";
	std::string compressed = graph::utils::zlibCompress(shortString);
	EXPECT_GT(compressed.size(), 0UL);
}

TEST_F(CompressUtilsTest, ZlibCompressRepeatingData) {
	std::string repeatingData(1000, 'A');
	std::string compressed = graph::utils::zlibCompress(repeatingData);
	EXPECT_GT(compressed.size(), 0UL);
	EXPECT_LT(compressed.size(), repeatingData.size()); // Should be compressed
}

TEST_F(CompressUtilsTest, ZlibCompressLargeData) {
	std::string largeData(10000, 'X');
	std::string compressed = graph::utils::zlibCompress(largeData);
	EXPECT_GT(compressed.size(), 0UL);
	EXPECT_LT(compressed.size(), largeData.size());
}

TEST_F(CompressUtilsTest, ZlibCompressRandomData) {
	std::string randomData = generateRandomString(1000);
	std::string compressed = graph::utils::zlibCompress(randomData);
	EXPECT_GT(compressed.size(), 0UL);
}

TEST_F(CompressUtilsTest, ZlibCompressSpecialCharacters) {
	std::string specialChars = "!@#$%^&*()_+-=[]{}|;':\",./<>?`~";
	std::string compressed = graph::utils::zlibCompress(specialChars);
	EXPECT_GT(compressed.size(), 0UL);
}

TEST_F(CompressUtilsTest, ZlibCompressBinaryData) {
	std::string binaryData;
	for (int i = 0; i < 256; ++i) {
		binaryData += static_cast<char>(i);
	}
	std::string compressed = graph::utils::zlibCompress(binaryData);
	EXPECT_GT(compressed.size(), 0UL);
}

// Test zlibDecompress with various inputs
TEST_F(CompressUtilsTest, ZlibDecompressEmptyString) {
	std::string emptyData = "";
	std::string compressed = graph::utils::zlibCompress(emptyData);
	std::string decompressed = graph::utils::zlibDecompress(compressed, emptyData.size());
	EXPECT_EQ(decompressed, emptyData);
}

TEST_F(CompressUtilsTest, ZlibDecompressSingleCharacter) {
	std::string singleChar = "B";
	std::string compressed = graph::utils::zlibCompress(singleChar);
	std::string decompressed = graph::utils::zlibDecompress(compressed, singleChar.size());
	EXPECT_EQ(decompressed, singleChar);
}

TEST_F(CompressUtilsTest, ZlibDecompressShortString) {
	std::string shortString = "World";
	std::string compressed = graph::utils::zlibCompress(shortString);
	std::string decompressed = graph::utils::zlibDecompress(compressed, shortString.size());
	EXPECT_EQ(decompressed, shortString);
}

TEST_F(CompressUtilsTest, ZlibDecompressLargeData) {
	std::string largeData(5000, 'Z');
	std::string compressed = graph::utils::zlibCompress(largeData);
	std::string decompressed = graph::utils::zlibDecompress(compressed, largeData.size());
	EXPECT_EQ(decompressed, largeData);
}

// Test round-trip compression/decompression
TEST_F(CompressUtilsTest, RoundTripCompression) {
	std::string originalData = "This is a test string for round-trip compression testing.";
	std::string compressed = graph::utils::zlibCompress(originalData);
	std::string decompressed = graph::utils::zlibDecompress(compressed, originalData.size());
	EXPECT_EQ(decompressed, originalData);
}

TEST_F(CompressUtilsTest, RoundTripCompressionRandomData) {
	std::string randomData = generateRandomString(500);
	std::string compressed = graph::utils::zlibCompress(randomData);
	std::string decompressed = graph::utils::zlibDecompress(compressed, randomData.size());
	EXPECT_EQ(decompressed, randomData);
}

TEST_F(CompressUtilsTest, RoundTripCompressionMultipleRounds) {
	std::string originalData = "Multiple compression rounds test data.";

	for (int i = 0; i < 3; ++i) {
		std::string compressed = graph::utils::zlibCompress(originalData);
		std::string decompressed = graph::utils::zlibDecompress(compressed, originalData.size());
		EXPECT_EQ(decompressed, originalData);
	}
}

// Test error conditions
TEST_F(CompressUtilsTest, ZlibDecompressInvalidData) {
	std::string invalidData = "This is not compressed data";
	EXPECT_THROW(graph::utils::zlibDecompress(invalidData, 100), std::runtime_error);
}

TEST_F(CompressUtilsTest, ZlibDecompressWrongOriginalSize) {
	std::string originalData = "Test data for wrong size";
	std::string compressed = graph::utils::zlibCompress(originalData);

	// Try with significantly smaller original size (this should fail)
	EXPECT_THROW(graph::utils::zlibDecompress(compressed, originalData.size() / 2), std::runtime_error);
}

TEST_F(CompressUtilsTest, ZlibDecompressZeroOriginalSize) {
	std::string originalData = "Non-empty data";
	std::string compressed = graph::utils::zlibCompress(originalData);

	// Try with zero original size for non-empty data
	EXPECT_THROW(graph::utils::zlibDecompress(compressed, 0), std::runtime_error);
}

TEST_F(CompressUtilsTest, ZlibDecompressCorruptedData) {
	std::string originalData = "Data to be corrupted";
	std::string compressed = graph::utils::zlibCompress(originalData);

	// Corrupt the compressed data
	if (compressed.size() > 5) {
		compressed[compressed.size() / 2] = ~compressed[compressed.size() / 2];
	}

	EXPECT_THROW(graph::utils::zlibDecompress(compressed, originalData.size()), std::runtime_error);
}

// Test edge cases
TEST_F(CompressUtilsTest, ZlibCompressVeryLargeString) {
	std::string veryLargeData(100000, 'L');
	std::string compressed = graph::utils::zlibCompress(veryLargeData);
	std::string decompressed = graph::utils::zlibDecompress(compressed, veryLargeData.size());
	EXPECT_EQ(decompressed, veryLargeData);
}

TEST_F(CompressUtilsTest, ZlibCompressUnicodeString) {
	std::string unicodeData = "Hello 世界 🌍 Мир";
	std::string compressed = graph::utils::zlibCompress(unicodeData);
	std::string decompressed = graph::utils::zlibDecompress(compressed, unicodeData.size());
	EXPECT_EQ(decompressed, unicodeData);
}

TEST_F(CompressUtilsTest, ZlibCompressConsistency) {
	std::string testData = "Consistency test data";
	std::string compressed1 = graph::utils::zlibCompress(testData);
	std::string compressed2 = graph::utils::zlibCompress(testData);

	// Both compressions should produce identical results
	EXPECT_EQ(compressed1, compressed2);
}

// Additional tests to improve branch coverage
TEST_F(CompressUtilsTest, ZlibCompressAllNullBytes) {
	std::string allNulls(1000, '\0');
	std::string compressed = graph::utils::zlibCompress(allNulls);
	EXPECT_GT(compressed.size(), 0UL);

	std::string decompressed = graph::utils::zlibDecompress(compressed, allNulls.size());
	EXPECT_EQ(decompressed, allNulls);
}

TEST_F(CompressUtilsTest, ZlibCompressAllOnes) {
	std::string allOnes(1000, '\xFF');
	std::string compressed = graph::utils::zlibCompress(allOnes);
	EXPECT_GT(compressed.size(), 0UL);

	std::string decompressed = graph::utils::zlibDecompress(compressed, allOnes.size());
	EXPECT_EQ(decompressed, allOnes);
}

TEST_F(CompressUtilsTest, ZlibDecompressTruncatedData) {
	std::string originalData = "Data for truncation test";
	std::string compressed = graph::utils::zlibCompress(originalData);

	// Truncate the compressed data
	if (compressed.size() > 10) {
		std::string truncatedCompressed = compressed.substr(0, compressed.size() / 2);
		EXPECT_THROW(graph::utils::zlibDecompress(truncatedCompressed, originalData.size()), std::runtime_error);
	}
}

TEST_F(CompressUtilsTest, ZlibDecompressEmptyCompressedData) {
	EXPECT_THROW(graph::utils::zlibDecompress("", 100), std::runtime_error);
}

TEST_F(CompressUtilsTest, ZlibDecompressTooLargeOriginalSize) {
	std::string smallData = "Small";
	std::string compressed = graph::utils::zlibCompress(smallData);

	// Request much larger size than original
	// zlib will just decompress to actual size, not throw
	std::string decompressed = graph::utils::zlibDecompress(compressed, 1000000);

	// Verify it decompressed to actual (smaller) size
	EXPECT_EQ(decompressed, smallData);
	EXPECT_LT(decompressed.size(), smallData.size() + 100); // Should not use the full requested size
}

TEST_F(CompressUtilsTest, ZlibCompressSizeReduction) {
	// Test that compression actually reduces size for compressible data
	std::string highlyCompressible(10000, 'A');
	std::string compressed = graph::utils::zlibCompress(highlyCompressible);

	EXPECT_LT(compressed.size(), highlyCompressible.size());
	double compressionRatio = static_cast<double>(compressed.size()) / highlyCompressible.size();
	EXPECT_LT(compressionRatio, 0.1); // Should compress to less than 10%
}

TEST_F(CompressUtilsTest, ZlibCompressIncompressibleData) {
	// Random data should not compress as well
	std::string randomData = generateRandomString(10000);
	std::string compressed = graph::utils::zlibCompress(randomData);

	// Compressed size might be larger due to zlib overhead
	EXPECT_GT(compressed.size(), 0UL);
}

TEST_F(CompressUtilsTest, ZlibDecompressExactSizeMatch) {
	std::string data = "Test exact size match";
	std::string compressed = graph::utils::zlibCompress(data);
	std::string decompressed = graph::utils::zlibDecompress(compressed, data.size());

	EXPECT_EQ(decompressed.size(), data.size());
	EXPECT_EQ(decompressed, data);
}

TEST_F(CompressUtilsTest, ZlibCompressDecompressWithNullsInMiddle) {
	// Create a string with null bytes in the middle
	std::string data = std::string("Before\0After", 14); // "Before" + '\0' + "After" = 6 + 1 + 5 = 12, but we specify 14 to include null terminator
	data = std::string("Before") + '\0' + std::string("After"); // Correct way: 6 + 1 + 5 = 12 bytes

	std::string compressed = graph::utils::zlibCompress(data);
	std::string decompressed = graph::utils::zlibDecompress(compressed, data.size());

	EXPECT_EQ(decompressed.size(), data.size());
	EXPECT_EQ(decompressed, data);
}

TEST_F(CompressUtilsTest, ZlibDecompressModifyCompressedHeader) {
	std::string originalData = "Header modification test";
	std::string compressed = graph::utils::zlibCompress(originalData);

	// Modify the first byte (header)
	if (!compressed.empty()) {
		compressed[0] = ~compressed[0];
		EXPECT_THROW(graph::utils::zlibDecompress(compressed, originalData.size()), std::runtime_error);
	}
}

TEST_F(CompressUtilsTest, ZlibDecompressModifyCompressedTail) {
	std::string originalData = "Tail modification test";
	std::string compressed = graph::utils::zlibCompress(originalData);

	// Modify the last byte (checksum/trailer)
	if (!compressed.empty()) {
		compressed[compressed.size() - 1] = ~compressed[compressed.size() - 1];
		EXPECT_THROW(graph::utils::zlibDecompress(compressed, originalData.size()), std::runtime_error);
	}
}
