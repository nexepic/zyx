/**
 * @file test_FileTruncator.cpp
 * @author Nexepic
 * @date 2025/12/4
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
#include <memory>

#include "graph/core/Node.hpp"
#include "graph/storage/FileHeaderManager.hpp"
#include "graph/storage/FileTruncator.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentAllocator.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageHeaders.hpp"

using namespace graph::storage;
using namespace graph;

class FileTruncatorTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	FileHeader header;
	std::shared_ptr<SegmentTracker> segmentTracker;
	std::shared_ptr<FileHeaderManager> fileHeaderManager;
	std::shared_ptr<IDAllocator> idAllocator;
	std::shared_ptr<SegmentAllocator> segmentAllocator;
	std::shared_ptr<FileTruncator> truncator;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("trunc_test_" + to_string(uuid) + ".dat");
		file = std::make_shared<std::fstream>(testFilePath,
			std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		file->write(reinterpret_cast<char*>(&header), sizeof(FileHeader));
		file->flush();

		segmentTracker = std::make_shared<SegmentTracker>(file, header);
		fileHeaderManager = std::make_shared<FileHeaderManager>(file, header);
		idAllocator = std::make_shared<IDAllocator>(
			file, segmentTracker, fileHeaderManager->getMaxNodeIdRef(), fileHeaderManager->getMaxEdgeIdRef(),
			fileHeaderManager->getMaxPropIdRef(), fileHeaderManager->getMaxBlobIdRef(),
			fileHeaderManager->getMaxIndexIdRef(), fileHeaderManager->getMaxStateIdRef());
		segmentAllocator = std::make_shared<SegmentAllocator>(file, segmentTracker, fileHeaderManager, idAllocator);
		truncator = std::make_shared<FileTruncator>(file, testFilePath.string(), segmentTracker);
	}

	void TearDown() override {
		truncator.reset();
		segmentAllocator.reset();
		idAllocator.reset();
		fileHeaderManager.reset();
		segmentTracker.reset();
		if (file && file->is_open()) file->close();
		file.reset();
		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
	}

	uint64_t getFileSize() const { return std::filesystem::file_size(testFilePath); }
};

// 1. No segments allocated — nothing to truncate, returns false.
TEST_F(FileTruncatorTest, TruncateFile_EmptyDatabase) {
	uint64_t sizeBefore = getFileSize();
	EXPECT_EQ(sizeBefore, sizeof(FileHeader));

	bool result = truncator->truncateFile();
	EXPECT_FALSE(result);

	uint64_t sizeAfter = getFileSize();
	EXPECT_EQ(sizeAfter, sizeBefore);
}

// 2. One active segment — truncation has nothing free to remove.
TEST_F(FileTruncatorTest, TruncateFile_NoFreeSegments) {
	uint64_t offset = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
	file->flush();
	EXPECT_GT(offset, 0u);

	uint64_t sizeBefore = getFileSize();
	EXPECT_EQ(sizeBefore, sizeof(FileHeader) + TOTAL_SEGMENT_SIZE);

	bool result = truncator->truncateFile();
	EXPECT_FALSE(result);

	uint64_t sizeAfter = getFileSize();
	EXPECT_EQ(sizeAfter, sizeBefore);
}

// 3. Allocate 2 segments, deallocate the last one — truncation reclaims it.
TEST_F(FileTruncatorTest, TruncateFile_FreeTrailingSegment) {
	uint64_t offsetA = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
	uint64_t offsetB = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
	file->flush();

	EXPECT_EQ(getFileSize(), sizeof(FileHeader) + 2 * TOTAL_SEGMENT_SIZE);

	segmentAllocator->deallocateSegment(offsetB);

	bool result = truncator->truncateFile();
	EXPECT_TRUE(result);

	uint64_t sizeAfter = getFileSize();
	EXPECT_EQ(sizeAfter, sizeof(FileHeader) + TOTAL_SEGMENT_SIZE);
}

// 4. Allocate 3 segments, deallocate the middle — file size unchanged (only trailing free segments truncated).
TEST_F(FileTruncatorTest, TruncateFile_FreeMiddleSegment_NotTruncated) {
	uint64_t offsetA = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
	uint64_t offsetB = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
	uint64_t offsetC = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
	file->flush();

	uint64_t sizeBefore = getFileSize();
	EXPECT_EQ(sizeBefore, sizeof(FileHeader) + 3 * TOTAL_SEGMENT_SIZE);

	// Deallocate the middle segment — it is not at the tail.
	segmentAllocator->deallocateSegment(offsetB);

	bool result = truncator->truncateFile();
	EXPECT_FALSE(result);

	uint64_t sizeAfter = getFileSize();
	EXPECT_EQ(sizeAfter, sizeBefore);
}

// 5. Allocate 3 segments, deallocate last 2 — truncation reclaims both.
TEST_F(FileTruncatorTest, TruncateFile_MultipleFreeTrailingSegments) {
	uint64_t offsetA = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
	uint64_t offsetB = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
	uint64_t offsetC = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
	file->flush();

	EXPECT_EQ(getFileSize(), sizeof(FileHeader) + 3 * TOTAL_SEGMENT_SIZE);

	segmentAllocator->deallocateSegment(offsetB);
	segmentAllocator->deallocateSegment(offsetC);

	bool result = truncator->truncateFile();
	EXPECT_TRUE(result);

	uint64_t sizeAfter = getFileSize();
	EXPECT_EQ(sizeAfter, sizeof(FileHeader) + TOTAL_SEGMENT_SIZE);
}

// 6. Allocate 1 segment, deallocate it — file truncates but respects minimum (FileHeader size).
TEST_F(FileTruncatorTest, TruncateFile_ProtectsMinimumFileSize) {
	uint64_t offset = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
	file->flush();

	EXPECT_EQ(getFileSize(), sizeof(FileHeader) + TOTAL_SEGMENT_SIZE);

	segmentAllocator->deallocateSegment(offset);

	bool result = truncator->truncateFile();
	EXPECT_TRUE(result);

	uint64_t sizeAfter = getFileSize();
	// The file must not shrink below the minimum protected size.
	EXPECT_GE(sizeAfter, sizeof(FileHeader));
}

// 7. Allocate A, B, C — deallocate C (trailing), B is active — only C is reclaimed.
TEST_F(FileTruncatorTest, TruncateFile_ActiveSegmentBlocksTruncation) {
	uint64_t offsetA = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
	uint64_t offsetB = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
	uint64_t offsetC = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
	file->flush();

	EXPECT_EQ(getFileSize(), sizeof(FileHeader) + 3 * TOTAL_SEGMENT_SIZE);

	// Only deallocate C (the tail segment); A and B remain active.
	segmentAllocator->deallocateSegment(offsetC);

	bool result = truncator->truncateFile();
	EXPECT_TRUE(result);

	// B is the last active segment, so only C is removed.
	uint64_t sizeAfter = getFileSize();
	EXPECT_EQ(sizeAfter, sizeof(FileHeader) + 2 * TOTAL_SEGMENT_SIZE);
}

// 8. Allocate 2, deallocate both — truncation reduces file but respects minimum size.
TEST_F(FileTruncatorTest, TruncateFile_AllSegmentsFreed) {
	uint64_t offsetA = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
	uint64_t offsetB = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
	file->flush();

	EXPECT_EQ(getFileSize(), sizeof(FileHeader) + 2 * TOTAL_SEGMENT_SIZE);

	segmentAllocator->deallocateSegment(offsetA);
	segmentAllocator->deallocateSegment(offsetB);

	bool result = truncator->truncateFile();
	EXPECT_TRUE(result);

	uint64_t sizeAfter = getFileSize();
	// Both segments are free and trailing — file shrinks to the header.
	EXPECT_GE(sizeAfter, sizeof(FileHeader));
	EXPECT_LT(sizeAfter, sizeof(FileHeader) + 2 * TOTAL_SEGMENT_SIZE);
}
