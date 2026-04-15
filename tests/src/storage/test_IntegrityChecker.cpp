/**
 * @file test_IntegrityChecker.cpp
 * @brief Unit tests for IntegrityChecker diagnostic verification
 *
 * Tests bitmap consistency and CRC integrity verification in isolation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "graph/storage/IntegrityChecker.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;
using namespace graph;

class IntegrityCheckerTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> fileStream;
	std::shared_ptr<StorageIO> storageIO;
	std::shared_ptr<SegmentTracker> tracker;
	std::shared_ptr<IntegrityChecker> checker;

	static uint64_t getSegmentOffset(uint32_t index) {
		return FILE_HEADER_SIZE + (static_cast<uint64_t>(index) * TOTAL_SEGMENT_SIZE);
	}

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() /
			("integrity_checker_test_" + to_string(uuid) + ".dat");

		{
			std::ofstream out(testFilePath, std::ios::binary);
			FileHeader header{};
			header.version = 1;
			out.write(reinterpret_cast<const char *>(&header), sizeof(FileHeader));

			std::vector<char> emptySegment(TOTAL_SEGMENT_SIZE, 0);
			for (int i = 0; i < 5; ++i) {
				out.write(emptySegment.data(), static_cast<std::streamsize>(emptySegment.size()));
			}
		}

		fileStream = std::make_shared<std::fstream>(testFilePath, std::ios::binary | std::ios::in | std::ios::out);
		ASSERT_TRUE(fileStream->is_open());

		FileHeader header{};
		fileStream->seekg(0);
		fileStream->read(reinterpret_cast<char *>(&header), sizeof(FileHeader));

		storageIO = std::make_shared<StorageIO>(fileStream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		tracker = std::make_shared<SegmentTracker>(storageIO, header);
		checker = std::make_shared<IntegrityChecker>(tracker, storageIO);
	}

	void TearDown() override {
		checker.reset();
		tracker.reset();
		storageIO.reset();
		if (fileStream) {
			fileStream->close();
			fileStream.reset();
		}
		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
	}

	SegmentHeader createAndRegisterSegment(uint64_t offset, uint32_t type, uint32_t capacity,
										   int64_t startId = 0) {
		SegmentHeader header{};
		header.file_offset = offset;
		header.data_type = type;
		header.capacity = capacity;
		header.used = 0;
		header.inactive_count = 0;
		header.start_id = startId;
		header.bitmap_size = bitmap::calculateBitmapSize(capacity);
		std::memset(header.activity_bitmap, 0, sizeof(header.activity_bitmap));

		fileStream->seekp(static_cast<std::streamoff>(offset));
		fileStream->write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));
		fileStream->flush();

		tracker->registerSegment(header);
		return header;
	}
};

// ============================================================================
// verifyBitmapConsistency
// ============================================================================

TEST_F(IntegrityCheckerTest, VerifyBitmapConsistency_ZeroOffset) {
	EXPECT_FALSE(checker->verifyBitmapConsistency(0));
}

TEST_F(IntegrityCheckerTest, VerifyBitmapConsistency_EmptySegment) {
	uint64_t offset = getSegmentOffset(0);
	createAndRegisterSegment(offset, static_cast<uint32_t>(EntityType::Node), 100);
	EXPECT_TRUE(checker->verifyBitmapConsistency(offset));
}

TEST_F(IntegrityCheckerTest, VerifyBitmapConsistency_ConsistentBitmap) {
	uint64_t offset = getSegmentOffset(0);
	createAndRegisterSegment(offset, static_cast<uint32_t>(EntityType::Node), 16);

	tracker->updateSegmentUsage(offset, 8, 0);

	// Set 5 active, 3 inactive
	for (uint32_t i = 0; i < 5; ++i) {
		tracker->setEntityActive(offset, i, true);
	}
	// Bits 5-7 remain 0 (inactive) — inactive_count should be 3
	// But setEntityActive increments inactive_count per bit set to false
	// Need to align state: start with all active then deactivate some
	std::vector<bool> bitmap(8, true);
	bitmap[5] = false;
	bitmap[6] = false;
	bitmap[7] = false;
	tracker->updateActivityBitmap(offset, bitmap);

	EXPECT_TRUE(checker->verifyBitmapConsistency(offset));
}

TEST_F(IntegrityCheckerTest, VerifyBitmapConsistency_InconsistentBitmap) {
	uint64_t offset = getSegmentOffset(0);
	createAndRegisterSegment(offset, static_cast<uint32_t>(EntityType::Node), 16);

	tracker->updateSegmentUsage(offset, 4, 0);

	// Set all 4 as active via bitmap
	std::vector<bool> bitmap(4, true);
	tracker->updateActivityBitmap(offset, bitmap);

	// Corrupt: manually set inactive_count to wrong value
	tracker->updateSegmentHeader(offset, [](SegmentHeader &h) {
		h.inactive_count = 99;
	});

	EXPECT_FALSE(checker->verifyBitmapConsistency(offset));
}

// ============================================================================
// verifyIntegrity
// ============================================================================

TEST_F(IntegrityCheckerTest, VerifyIntegrity_NoSegments) {
	auto result = checker->verifyIntegrity();
	EXPECT_TRUE(result.allPassed);
	EXPECT_TRUE(result.segments.empty());
}

TEST_F(IntegrityCheckerTest, VerifyIntegrity_SegmentWithCorrectCrc) {
	uint64_t offset = getSegmentOffset(0);
	createAndRegisterSegment(offset, static_cast<uint32_t>(EntityType::Node), 100, 1);

	// Write some data to the data region
	uint64_t dataOffset = offset + sizeof(SegmentHeader);
	std::vector<char> data(SEGMENT_SIZE, 0);
	data[0] = 'A';
	data[1] = 'B';
	storageIO->writeAt(dataOffset, data.data(), SEGMENT_SIZE);

	// Compute correct CRC and set it on the header
	uint32_t crc = graph::utils::calculateCrc(data.data(), SEGMENT_SIZE);
	tracker->updateSegmentHeader(offset, [crc](SegmentHeader &h) {
		h.segment_crc = crc;
		h.used = 1;
	});

	auto result = checker->verifyIntegrity();
	EXPECT_TRUE(result.allPassed);
	EXPECT_EQ(result.segments.size(), 1u);
	EXPECT_TRUE(result.segments[0].passed);
	EXPECT_EQ(result.segments[0].offset, offset);
	EXPECT_EQ(result.segments[0].capacity, 100u);
}

TEST_F(IntegrityCheckerTest, VerifyIntegrity_SegmentWithBadCrc) {
	uint64_t offset = getSegmentOffset(0);
	createAndRegisterSegment(offset, static_cast<uint32_t>(EntityType::Node), 100, 1);

	// Write data but set wrong CRC
	uint64_t dataOffset = offset + sizeof(SegmentHeader);
	std::vector<char> data(SEGMENT_SIZE, 0);
	data[0] = 'X';
	storageIO->writeAt(dataOffset, data.data(), SEGMENT_SIZE);

	tracker->updateSegmentHeader(offset, [](SegmentHeader &h) {
		h.segment_crc = 0xDEADBEEF; // Wrong CRC
		h.used = 1;
	});

	auto result = checker->verifyIntegrity();
	EXPECT_FALSE(result.allPassed);
	EXPECT_EQ(result.segments.size(), 1u);
	EXPECT_FALSE(result.segments[0].passed);
}

TEST_F(IntegrityCheckerTest, VerifyIntegrity_MultipleSegments) {
	uint64_t off1 = getSegmentOffset(0);
	uint64_t off2 = getSegmentOffset(1);
	createAndRegisterSegment(off1, static_cast<uint32_t>(EntityType::Node), 100, 1);
	createAndRegisterSegment(off2, static_cast<uint32_t>(EntityType::Edge), 50, 1);

	// Set correct CRCs for both
	for (uint64_t off : {off1, off2}) {
		uint64_t dataOffset = off + sizeof(SegmentHeader);
		std::vector<char> data(SEGMENT_SIZE, 0);
		storageIO->readAt(dataOffset, data.data(), SEGMENT_SIZE);
		uint32_t crc = graph::utils::calculateCrc(data.data(), SEGMENT_SIZE);
		tracker->updateSegmentHeader(off, [crc](SegmentHeader &h) {
			h.segment_crc = crc;
			h.used = 1;
		});
	}

	auto result = checker->verifyIntegrity();
	EXPECT_TRUE(result.allPassed);
	EXPECT_EQ(result.segments.size(), 2u);
}
