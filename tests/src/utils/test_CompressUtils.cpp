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

// ============================================================================
// Additional tests to improve branch coverage
// ============================================================================

TEST_F(CompressUtilsTest, ZlibCompressExtremelyLargeData) {
	// Test with extremely large data to stress memory allocation
	std::string hugeData(1000000, 'M'); // 1 MB of data
	std::string compressed = graph::utils::zlibCompress(hugeData);
	EXPECT_GT(compressed.size(), 0UL);

	std::string decompressed = graph::utils::zlibDecompress(compressed, hugeData.size());
	EXPECT_EQ(decompressed, hugeData);
}

TEST_F(CompressUtilsTest, ZlibCompressSingleByte) {
	// Test with single byte
	std::string singleByte = "X";
	std::string compressed = graph::utils::zlibCompress(singleByte);

	std::string decompressed = graph::utils::zlibDecompress(compressed, singleByte.size());
	EXPECT_EQ(decompressed, singleByte);
}

TEST_F(CompressUtilsTest, ZlibCompressAllByteValues) {
	// Test with all possible byte values (0-255)
	std::string allBytes;
	for (int i = 0; i < 256; ++i) {
		allBytes += static_cast<char>(i);
	}

	std::string compressed = graph::utils::zlibCompress(allBytes);
	std::string decompressed = graph::utils::zlibDecompress(compressed, allBytes.size());

	EXPECT_EQ(decompressed, allBytes);
	EXPECT_EQ(decompressed.size(), 256UL);
}

TEST_F(CompressUtilsTest, ZlibCompressRepeatingPattern) {
	// Test with repeating pattern that compresses well
	std::string pattern = "ABCD";
	std::string repeatingData;
	for (int i = 0; i < 10000; ++i) {
		repeatingData += pattern;
	}

	std::string compressed = graph::utils::zlibCompress(repeatingData);
	EXPECT_LT(compressed.size(), repeatingData.size()); // Should compress significantly

	std::string decompressed = graph::utils::zlibDecompress(compressed, repeatingData.size());
	EXPECT_EQ(decompressed, repeatingData);
}

TEST_F(CompressUtilsTest, ZlibCompressAlternatingPattern) {
	// Test with alternating pattern
	std::string alternatingData;
	for (int i = 0; i < 5000; ++i) {
		alternatingData += "AB";
	}

	std::string compressed = graph::utils::zlibCompress(alternatingData);
	std::string decompressed = graph::utils::zlibDecompress(compressed, alternatingData.size());

	EXPECT_EQ(decompressed, alternatingData);
}

TEST_F(CompressUtilsTest, ZlibCompressSequentialData) {
	// Test with sequential data (0, 1, 2, ..., 255, 0, 1, ...)
	std::string sequentialData;
	for (int i = 0; i < 10000; ++i) {
		sequentialData += static_cast<char>(i % 256);
	}

	std::string compressed = graph::utils::zlibCompress(sequentialData);
	std::string decompressed = graph::utils::zlibDecompress(compressed, sequentialData.size());

	EXPECT_EQ(decompressed, sequentialData);
}

TEST_F(CompressUtilsTest, ZlibCompressVerySmallStrings) {
	// Test various very small strings
	std::vector<std::string> smallStrings = {"", "a", "ab", "abc", "abcd"};

	for (const auto &str: smallStrings) {
		std::string compressed = graph::utils::zlibCompress(str);
		EXPECT_GT(compressed.size(), 0UL);

		std::string decompressed = graph::utils::zlibDecompress(compressed, str.size());
		EXPECT_EQ(decompressed, str);
	}
}

TEST_F(CompressUtilsTest, ZlibDecompressInvalidCompressedFormat) {
	// Create a completely invalid compressed data (not zlib format)
	std::string invalidData = "NOT_ZLIB_FORMAT_DATA_HERE!!!";

	EXPECT_THROW(graph::utils::zlibDecompress(invalidData, 100), std::runtime_error);
}

TEST_F(CompressUtilsTest, ZlibDecompressPartialHeader) {
	// Create valid compressed data then truncate to only part of header
	std::string originalData = "Test data";
	std::string compressed = graph::utils::zlibCompress(originalData);

	// Truncate to only 1 byte (partial header)
	if (compressed.size() > 1) {
		std::string partialHeader = compressed.substr(0, 1);
		EXPECT_THROW(graph::utils::zlibDecompress(partialHeader, originalData.size()), std::runtime_error);
	}
}

TEST_F(CompressUtilsTest, ZlibDecompressOnlyHeader) {
	// Test with only zlib header bytes (no actual compressed data)
	std::string originalData = "Header only test";
	std::string compressed = graph::utils::zlibCompress(originalData);

	// Typical zlib header is 2 bytes, truncate to just header
	if (compressed.size() > 2) {
		std::string onlyHeader = compressed.substr(0, 2);
		EXPECT_THROW(graph::utils::zlibDecompress(onlyHeader, originalData.size()), std::runtime_error);
	}
}

TEST_F(CompressUtilsTest, ZlibDecompressExtraDataAfterEnd) {
	// Test with extra garbage data appended to valid compressed data
	std::string originalData = "Valid data";
	std::string compressed = graph::utils::zlibCompress(originalData);

	// Append extra data
	std::string withExtra = compressed + "EXTRA_GARBAGE_DATA_HERE";

	// zlib should decompress successfully and ignore extra data
	std::string decompressed = graph::utils::zlibDecompress(withExtra, originalData.size());
	EXPECT_EQ(decompressed, originalData);
}

// ============================================================================
// Tests attempting to trigger compress() failures
// Note: These tests are unlikely to trigger actual failures because zlib's
// compress() is extremely stable, but they exercise different code paths
// ============================================================================

TEST_F(CompressUtilsTest, ZlibCompressMultipleLargeAllocations) {
	// Try to stress memory allocation by compressing multiple large datasets
	std::vector<std::string> compressedResults;

	for (int i = 0; i < 100; ++i) {
		std::string largeData(100000, 'A' + (i % 26)); // 100KB each iteration
		std::string compressed = graph::utils::zlibCompress(largeData);
		compressedResults.push_back(compressed);

		// Verify each compression worked
		std::string decompressed = graph::utils::zlibDecompress(compressed, largeData.size());
		EXPECT_EQ(decompressed, largeData);
	}

	// If we get here, all compressions succeeded (which is expected)
	EXPECT_EQ(compressedResults.size(), 100UL);
}

TEST_F(CompressUtilsTest, ZlibCompressMaximumSizeData) {
	// Try to compress very large data that might stress the system
	// Note: Using smaller size to avoid actual memory issues in tests
	std::string maxSizeData(5000000, 'Z'); // 5 MB

	std::string compressed = graph::utils::zlibCompress(maxSizeData);
	EXPECT_GT(compressed.size(), 0UL);

	// Verify round-trip
	std::string decompressed = graph::utils::zlibDecompress(compressed, maxSizeData.size());
	EXPECT_EQ(decompressed, maxSizeData);
}

TEST_F(CompressUtilsTest, ZlibCompressVaryingSizes) {
	// Test compression with many different sizes to hit different internal code paths
	std::vector<size_t> sizes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
	                              15, 16, 17, 31, 32, 33,
	                              63, 64, 65, 127, 128, 129,
	                              255, 256, 257, 511, 512, 513,
	                              1023, 1024, 1025, 2047, 2048, 2049,
	                              4095, 4096, 4097, 8191, 8192, 8193,
	                              16383, 16384, 16385};

	for (size_t size : sizes) {
		std::string data(size, 'X');
		std::string compressed = graph::utils::zlibCompress(data);
		EXPECT_GT(compressed.size(), 0UL);

		std::string decompressed = graph::utils::zlibDecompress(compressed, data.size());
		EXPECT_EQ(decompressed, data);
	}
}

TEST_F(CompressUtilsTest, ZlibCompressIncompressibleRandomData) {
	// Generate random data that doesn't compress well
	std::mt19937 rng(42); // Fixed seed for reproducibility
	std::uniform_int_distribution<int> dist(0, 255);

	std::string randomData;
	for (int i = 0; i < 100000; ++i) {
		randomData += static_cast<char>(dist(rng));
	}

	std::string compressed = graph::utils::zlibCompress(randomData);
	EXPECT_GT(compressed.size(), 0UL);

	// Random data might actually expand slightly due to zlib overhead
	std::string decompressed = graph::utils::zlibDecompress(compressed, randomData.size());
	EXPECT_EQ(decompressed, randomData);
}

TEST_F(CompressUtilsTest, ZlibCompressHighlyCompressibleData) {
	// Test data that compresses extremely well
	std::string highlyCompressible(1000000, 'A'); // 1 MB of same character

	std::string compressed = graph::utils::zlibCompress(highlyCompressible);

	// Should compress to a very small size
	EXPECT_LT(compressed.size(), 10000UL); // Should be less than 1% of original

	std::string decompressed = graph::utils::zlibDecompress(compressed, highlyCompressible.size());
	EXPECT_EQ(decompressed, highlyCompressible);
}

TEST_F(CompressUtilsTest, ZlibCompressWavePattern) {
	// Create wave pattern data
	std::string waveData;
	for (int i = 0; i < 10000; ++i) {
		waveData += static_cast<char>(i % 256);
	}

	std::string compressed = graph::utils::zlibCompress(waveData);
	std::string decompressed = graph::utils::zlibDecompress(compressed, waveData.size());

	EXPECT_EQ(decompressed, waveData);
}

TEST_F(CompressUtilsTest, ZlibCompressZeroBytesRepeated) {
	// Test data with all zeros
	std::string zeroData(10000, '\0');

	std::string compressed = graph::utils::zlibCompress(zeroData);
	EXPECT_GT(compressed.size(), 0UL);

	std::string decompressed = graph::utils::zlibDecompress(compressed, zeroData.size());
	EXPECT_EQ(decompressed, zeroData);
}

