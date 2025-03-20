/**
 * @file test_FileStorage.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/19
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <fstream>
#include <graph/utils/ChecksumUtils.h>
#include <gtest/gtest.h>
#include <zlib.h>
#include "graph/storage/FileStorage.h"

// Test fixture for FileStorage
class FileStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Set up code here, e.g., create a temporary file
        testFilePath = "test_db_file.dat";
    }

    void TearDown() override {
        // Clean up code here, e.g., remove the temporary file
        std::remove(testFilePath.c_str());
    }

    std::string testFilePath;
};

TEST(CalculateCrcTest, BasicTest) {
	const char *data = "CRC32 Test";
	size_t length = strlen(data);
	uint32_t expectedCrc = crc32(0L, reinterpret_cast<const Bytef *>(data), length);

	ASSERT_EQ(graph::utils::calculateCrc(data, length), expectedCrc);
}

// Test case for FileStorage::allocateSegment
TEST_F(FileStorageTest, AllocateSegment) {
    // Create a FileStorage instance
    graph::storage::FileStorageTest storage(testFilePath);

    // Open the file for writing
    std::fstream file(testFilePath, std::ios::binary | std::ios::out | std::ios::in | std::ios::trunc);
    ASSERT_TRUE(file.is_open());

    // Allocate a segment
    uint64_t segmentOffset = storage.allocateSegment(file, 0, 10);

    // Verify the segment was allocated correctly
    file.seekg(static_cast<std::streamoff>(segmentOffset));
    graph::storage::SegmentHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    ASSERT_EQ(header.data_type, static_cast<unsigned int>(0));
    ASSERT_EQ(header.capacity, static_cast<unsigned int>(10));
    ASSERT_EQ(header.used, static_cast<unsigned int>(0));
    ASSERT_EQ(header.next_segment_offset, static_cast<unsigned long long>(0));
    ASSERT_EQ(header.start_id, static_cast<unsigned long long>(0));

    // Verify the segment data area is correctly initialized
    file.seekg(static_cast<std::streamoff>(segmentOffset + sizeof(graph::storage::SegmentHeader)));
    std::vector<char> data(header.capacity * sizeof(graph::Node), 0);
    std::vector<char> fileData(data.size());
    file.read(fileData.data(), static_cast<std::streamsize>(fileData.size()));
    ASSERT_EQ(data, fileData);
}