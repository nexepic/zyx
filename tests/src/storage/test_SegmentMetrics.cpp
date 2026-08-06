/**
 * @file test_SegmentMetrics.cpp
 * @brief Tests for Stage 0 current-format segment measurements.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/FileHeaderManager.hpp"
#include "graph/storage/SegmentAllocator.hpp"
#include "graph/storage/SegmentMetrics.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/StorageIO.hpp"

using namespace graph;
using namespace graph::storage;

namespace {

	const SegmentTypeMetrics &metricsFor(const SegmentStorageMetrics &metrics, uint32_t type) {
		return metrics.by_type.at(type);
	}

} // namespace

class SegmentMetricsTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;
	std::shared_ptr<SegmentTracker> tracker;
	std::shared_ptr<FileHeaderManager> fileHeaderManager;
	std::shared_ptr<SegmentAllocator> allocator;
	FileHeader header;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("segment_metrics_" + to_string(uuid) + ".dat");
		file = std::make_shared<std::fstream>(testFilePath,
											  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		file->write(reinterpret_cast<const char *>(&header), sizeof(header));
		file->flush();

		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		tracker = std::make_shared<SegmentTracker>(io, header);
		fileHeaderManager = std::make_shared<FileHeaderManager>(file, header);
		allocator = std::make_shared<SegmentAllocator>(io, tracker, fileHeaderManager);
	}

	void TearDown() override {
		allocator.reset();
		fileHeaderManager.reset();
		tracker.reset();
		io.reset();
		if (file && file->is_open()) {
			file->close();
		}
		file.reset();
		std::error_code error;
		std::filesystem::remove(testFilePath, error);
	}

	void registerSegment(uint64_t offset, uint32_t type, uint32_t capacity, uint32_t used, uint32_t inactive) {
		SegmentHeader segment;
		segment.file_offset = offset;
		segment.data_type = type;
		segment.capacity = capacity;
		segment.used = used;
		segment.inactive_count = inactive;
		segment.bitmap_size = bitmap::calculateBitmapSize(capacity);
		tracker->registerSegment(segment);
	}
};

TEST_F(SegmentMetricsTest, EmptySnapshotContainsEverySubtype) {
	const auto metrics = tracker->getStorageMetrics();

	EXPECT_EQ(metrics.reusable_free_segment_count, 0u);
	EXPECT_EQ(metrics.reusable_free_bytes, 0u);
	for (uint32_t type = 0; type <= getMaxEntityType(); ++type) {
		const auto &entry = metricsFor(metrics, type);
		EXPECT_EQ(entry.data_type, type);
		EXPECT_EQ(entry.segment_count, 0u);
		EXPECT_EQ(entry.physical_bytes, 0u);
		EXPECT_EQ(entry.writes.logical_payload_bytes, 0u);
		EXPECT_EQ(entry.writes.attributed_write_bytes, 0u);
		EXPECT_DOUBLE_EQ(entry.writes.write_amplification, 0.0);
	}
}

TEST_F(SegmentMetricsTest, SnapshotSeparatesLiveUnusedTombstoneAndSlackBytes) {
	registerSegment(FILE_HEADER_SIZE, Node::typeId, 4, 3, 1);
	registerSegment(FILE_HEADER_SIZE + TOTAL_SEGMENT_SIZE, Edge::typeId, 2, 2, 0);

	const auto metrics = tracker->getStorageMetrics();
	const auto &nodes = metricsFor(metrics, Node::typeId);
	EXPECT_EQ(nodes.segment_count, 1u);
	EXPECT_EQ(nodes.physical_bytes, TOTAL_SEGMENT_SIZE);
	EXPECT_EQ(nodes.metadata_bytes, SEGMENT_HEADER_SIZE);
	EXPECT_EQ(nodes.data_region_bytes, SEGMENT_SIZE);
	EXPECT_EQ(nodes.slot_capacity_bytes, 4u * Node::getTotalSize());
	EXPECT_EQ(nodes.live_payload_bytes, 2u * Node::getTotalSize());
	EXPECT_EQ(nodes.never_used_payload_bytes, Node::getTotalSize());
	EXPECT_EQ(nodes.tombstone_payload_bytes, Node::getTotalSize());
	EXPECT_EQ(nodes.layout_slack_bytes, SEGMENT_SIZE - 4u * Node::getTotalSize());
	EXPECT_EQ(nodes.inconsistent_header_count, 0u);

	const auto &edges = metricsFor(metrics, Edge::typeId);
	EXPECT_EQ(edges.segment_count, 1u);
	EXPECT_EQ(edges.live_payload_bytes, 2u * Edge::getTotalSize());
	EXPECT_EQ(edges.never_used_payload_bytes, 0u);
	EXPECT_EQ(edges.tombstone_payload_bytes, 0u);
}

TEST_F(SegmentMetricsTest, SnapshotClampsMalformedHeadersWithoutOverflow) {
	registerSegment(FILE_HEADER_SIZE, Node::typeId, NODES_PER_SEGMENT + 1, NODES_PER_SEGMENT + 2,
					NODES_PER_SEGMENT + 3);
	registerSegment(FILE_HEADER_SIZE + TOTAL_SEGMENT_SIZE, getMaxEntityType() + 1, 1, 1, 0);

	const auto metrics = tracker->getStorageMetrics();
	const auto &nodes = metricsFor(metrics, Node::typeId);
	EXPECT_EQ(nodes.segment_count, 1u);
	EXPECT_EQ(nodes.inconsistent_header_count, 1u);
	EXPECT_EQ(nodes.slot_capacity_bytes, SEGMENT_SIZE);
	EXPECT_EQ(nodes.live_payload_bytes, 0u);
	EXPECT_EQ(nodes.tombstone_payload_bytes, SEGMENT_SIZE);
	EXPECT_EQ(nodes.layout_slack_bytes, 0u);

	for (uint32_t type = 0; type <= getMaxEntityType(); ++type) {
		EXPECT_NE(metricsFor(metrics, type).data_type, getMaxEntityType() + 1);
	}
}

TEST_F(SegmentMetricsTest, InstrumentationTracksWritesAndSaturatesCounters) {
	registerSegment(FILE_HEADER_SIZE, Node::typeId, 1, 0, 0);
	tracker->recordSegmentDataWrite(FILE_HEADER_SIZE, Node::getTotalSize());
	tracker->recordSegmentMetadataWrite(Node::typeId, SEGMENT_HEADER_SIZE);
	tracker->recordSegmentAllocationWrite(Node::typeId, 1);
	tracker->recordSegmentRelocationWrite(Node::typeId, TOTAL_SEGMENT_SIZE);
	tracker->recordSegmentDataWrite(FILE_HEADER_SIZE, 0);
	tracker->recordSegmentMetadataWrite(Node::typeId, 0);
	tracker->recordSegmentAllocationWrite(Node::typeId, 0);
	tracker->recordSegmentRelocationWrite(Node::typeId, 0);

	auto writes = metricsFor(tracker->getStorageMetrics(), Node::typeId).writes;
	EXPECT_EQ(writes.logical_payload_bytes, Node::getTotalSize());
	EXPECT_EQ(writes.data_write_bytes, Node::getTotalSize());
	EXPECT_EQ(writes.metadata_write_bytes, SEGMENT_HEADER_SIZE);
	EXPECT_EQ(writes.allocation_write_bytes, 1u);
	EXPECT_EQ(writes.relocation_write_bytes, TOTAL_SEGMENT_SIZE);
	EXPECT_EQ(writes.attributed_write_bytes, Node::getTotalSize() + SEGMENT_HEADER_SIZE + 1 + TOTAL_SEGMENT_SIZE);
	EXPECT_DOUBLE_EQ(writes.write_amplification,
					 static_cast<double>(Node::getTotalSize() + SEGMENT_HEADER_SIZE + 1 + TOTAL_SEGMENT_SIZE) /
							 static_cast<double>(Node::getTotalSize()));

	tracker->recordSegmentDataWrite(FILE_HEADER_SIZE, std::numeric_limits<uint64_t>::max());
	tracker->recordSegmentMetadataWrite(Node::typeId, std::numeric_limits<uint64_t>::max());
	tracker->recordSegmentAllocationWrite(Node::typeId, std::numeric_limits<uint64_t>::max());
	tracker->recordSegmentRelocationWrite(Node::typeId, std::numeric_limits<uint64_t>::max());
	writes = metricsFor(tracker->getStorageMetrics(), Node::typeId).writes;
	EXPECT_EQ(writes.logical_payload_bytes, std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(writes.data_write_bytes, std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(writes.metadata_write_bytes, std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(writes.allocation_write_bytes, std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(writes.relocation_write_bytes, std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(writes.attributed_write_bytes, std::numeric_limits<uint64_t>::max());
}

TEST_F(SegmentMetricsTest, InstrumentationRejectsUnknownSegmentsAndInvalidTypes) {
	EXPECT_THROW(tracker->recordSegmentDataWrite(FILE_HEADER_SIZE, 1), std::runtime_error);
	EXPECT_THROW(tracker->recordSegmentMetadataWrite(getMaxEntityType() + 1, 1), std::invalid_argument);
	EXPECT_THROW(tracker->recordSegmentAllocationWrite(getMaxEntityType() + 1, 1), std::invalid_argument);
	EXPECT_THROW(tracker->recordSegmentRelocationWrite(getMaxEntityType() + 1, 1), std::invalid_argument);
}

TEST_F(SegmentMetricsTest, AllocatorTrackerAndFreeListContributeExpectedMeasurements) {
	const auto offset = allocator->allocateSegment(Node::typeId, 2);
	Node node(1, 1);
	tracker->writeEntity(offset, 0, node, Node::getTotalSize());
	tracker->flushDirtySegments();
	allocator->deallocateSegment(offset);

	const auto metrics = tracker->getStorageMetrics();
	const auto &nodes = metricsFor(metrics, Node::typeId);
	EXPECT_EQ(nodes.segment_count, 0u);
	EXPECT_EQ(nodes.writes.logical_payload_bytes, Node::getTotalSize());
	EXPECT_EQ(nodes.writes.data_write_bytes, Node::getTotalSize());
	EXPECT_EQ(nodes.writes.metadata_write_bytes, 3u * SEGMENT_HEADER_SIZE);
	EXPECT_EQ(nodes.writes.allocation_write_bytes, 1u);
	EXPECT_EQ(metrics.reusable_free_segment_count, 1u);
	EXPECT_EQ(metrics.reusable_free_bytes, TOTAL_SEGMENT_SIZE);
}

TEST_F(SegmentMetricsTest, ReinitializeResetsProcessLifetimeWriteCounters) {
	registerSegment(FILE_HEADER_SIZE, Node::typeId, 1, 0, 0);
	tracker->recordSegmentMetadataWrite(Node::typeId, 1);
	EXPECT_EQ(metricsFor(tracker->getStorageMetrics(), Node::typeId).writes.metadata_write_bytes, 1u);

	tracker->initialize(header);
	EXPECT_EQ(metricsFor(tracker->getStorageMetrics(), Node::typeId).writes.metadata_write_bytes, 0u);
}
