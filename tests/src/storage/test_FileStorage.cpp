/**
 * @file test_FileStorage.cpp
 * @author Nexepic
 * @date 2025/3/19
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

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <zlib.h>
#include "graph/core/Database.hpp"

class FileStorageTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate a unique temporary file name using UUID
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_db_file_" + to_string(uuid) + ".dat");

		// Create and initialize Database instead of FileStorage directly
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();

		// Get FileStorage from Database
		fileStorage = database->getStorage();
	}

	void TearDown() override {
		// Clean up using Database
		database->close();
		database.reset();
		std::filesystem::remove(testFilePath);
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
};

// Remove redundant open() calls from all test methods
TEST_F(FileStorageTest, AllocateSegment) {
	// Allocate a segment
	const uint64_t segmentOffset = fileStorage->allocateSegment(0, 10);

	// Verify the segment was allocated correctly using SegmentTracker
	const auto segmentTracker = fileStorage->getSegmentTracker();
	const graph::storage::SegmentHeader header = segmentTracker->getSegmentHeader(segmentOffset);

	ASSERT_EQ(header.data_type, static_cast<unsigned int>(0));
	ASSERT_EQ(header.capacity, static_cast<unsigned int>(10));
	ASSERT_EQ(header.used, static_cast<unsigned int>(0));
	ASSERT_EQ(header.next_segment_offset, static_cast<unsigned long long>(0));
	ASSERT_EQ(header.start_id, static_cast<long long>(1));
}

TEST_F(FileStorageTest, TestOpenClose) {
	EXPECT_TRUE(fileStorage->isOpen());
	fileStorage->close();
	EXPECT_FALSE(fileStorage->isOpen());
}

TEST_F(FileStorageTest, SaveDataEmpty) {
	std::unordered_map<int64_t, graph::Node> data;
	uint64_t segmentHead = 0;

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_EQ(segmentHead, 0u);
}

TEST_F(FileStorageTest, SaveDataSingleElement) {
	std::unordered_map<int64_t, graph::Node> data = {{1, graph::Node(1, "Node1")}};
	uint64_t segmentHead = 0;

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_NE(segmentHead, 0u);
}

TEST_F(FileStorageTest, SaveDataFitsInOneSegment) {
	std::unordered_map<int64_t, graph::Node> data;
	for (int64_t i = 1; i <= 50; ++i) {
		data[i] = graph::Node(i, "Node" + std::to_string(i));
	}
	uint64_t segmentHead = 0;
	std::fstream file(testFilePath, std::ios::binary | std::ios::in | std::ios::out);

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_NE(segmentHead, 0u);
}

TEST_F(FileStorageTest, SaveDataMultipleSegments) {
	std::unordered_map<int64_t, graph::Node> data;
	for (int64_t i = 1; i <= 300; ++i) {
		data[i] = graph::Node(i, "Node" + std::to_string(i));
	}
	uint64_t segmentHead = 0;
	std::fstream file(testFilePath, std::ios::binary | std::ios::in | std::ios::out);

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_NE(segmentHead, 0u);
}

TEST_F(FileStorageTest, VerifySegmentLinking) {
	std::unordered_map<int64_t, graph::Node> data;
	for (int64_t i = 1; i <= 300; ++i) {
		data[i] = graph::Node(i, "Node" + std::to_string(i));
	}
	uint64_t segmentHead = 0;

	fileStorage->saveData(data, segmentHead, 100);

	// Use SegmentTracker to check linking
	const auto segmentTracker = fileStorage->getSegmentTracker();
	uint64_t currentOffset = segmentHead;
	int segmentCount = 0;

	while (currentOffset != 0) {
		graph::storage::SegmentHeader header = segmentTracker->getSegmentHeader(currentOffset);
		segmentCount++;
		currentOffset = header.next_segment_offset;
	}
	EXPECT_EQ(segmentCount, 3);
}
