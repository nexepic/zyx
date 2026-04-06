/**
 * @file test_StorageBootstrap.cpp
 * @brief Focused branch tests for StorageBootstrap::scanChain().
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>

#include "graph/core/Node.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageBootstrap.hpp"

namespace fs = std::filesystem;
using namespace graph::storage;

namespace {

SegmentHeader makeSegmentHeader(uint64_t offset, int64_t startId, uint32_t used, uint64_t nextOffset) {
	SegmentHeader h;
	h.file_offset = offset;
	h.next_segment_offset = nextOffset;
	h.prev_segment_offset = 0;
	h.start_id = startId;
	h.capacity = 16;
	h.used = used;
	h.inactive_count = 0;
	h.data_type = graph::Node::typeId;
	h.bitmap_size = 0;
	return h;
}

} // namespace

TEST(StorageBootstrapTest, ScanChainCoversUsedAndUnusedSegmentBranches) {
	const auto uuid = boost::uuids::random_generator()();
	const auto filePath = fs::temp_directory_path() / ("test_storage_bootstrap_" + boost::uuids::to_string(uuid) + ".dat");

	{
		auto file = std::make_shared<std::fstream>(
			filePath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());

		FileHeader header;
		auto tracker = std::make_shared<SegmentTracker>(file, header);

		// 100 -> 200 -> 300
		// - First segment sets physicalMaxId.
		// - Second segment has smaller used-end ID, exercising the "not greater" branch.
		// - Third segment is unused (used == 0), exercising both used-related false branches.
		tracker->registerSegment(makeSegmentHeader(100, 100, 3, 200)); // endId=102
		tracker->registerSegment(makeSegmentHeader(200, 10, 2, 300));  // endId=11
		tracker->registerSegment(makeSegmentHeader(300, 500, 0, 0));   // endId=500 (unused segment rule)

		StorageBootstrap bootstrap(tracker);
		auto result = bootstrap.scanChain(100);

		EXPECT_EQ(result.physicalMaxId, 102);
		ASSERT_EQ(result.segmentIndexEntries.size(), 3UL);

		EXPECT_EQ(result.segmentIndexEntries[0].startId, 100);
		EXPECT_EQ(result.segmentIndexEntries[0].endId, 102);
		EXPECT_EQ(result.segmentIndexEntries[1].startId, 10);
		EXPECT_EQ(result.segmentIndexEntries[1].endId, 11);
		EXPECT_EQ(result.segmentIndexEntries[2].startId, 500);
		EXPECT_EQ(result.segmentIndexEntries[2].endId, 500);
	}

	std::error_code ec;
	fs::remove(filePath, ec);
}

