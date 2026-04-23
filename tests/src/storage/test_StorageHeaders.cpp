/**
 * @file test_StorageHeaders.cpp
 * @author Nexepic
 * @date 2025/3/25
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
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/EntityDispatch.hpp"

TEST(StorageHeadersTest, FileHeaderInitialization) {
	const graph::storage::FileHeader fileHeader;

	EXPECT_EQ(std::string(fileHeader.magic, 8), graph::storage::FILE_HEADER_MAGIC_STRING);
	EXPECT_EQ(fileHeader.node_segment_head, 0u);
	EXPECT_EQ(fileHeader.edge_segment_head, 0u);
	EXPECT_EQ(fileHeader.max_node_id, 0u);
	EXPECT_EQ(fileHeader.max_edge_id, 0u);
	EXPECT_EQ(fileHeader.data_crc, 0u);
	EXPECT_EQ(fileHeader.version, 0x0003u);
}

TEST(StorageHeadersTest, SegmentHeaderInitialization) {
	constexpr graph::storage::SegmentHeader segmentHeader;

	EXPECT_EQ(segmentHeader.next_segment_offset, 0u);
	EXPECT_EQ(segmentHeader.start_id, 0u);
	EXPECT_EQ(segmentHeader.capacity, 0u);
	EXPECT_EQ(segmentHeader.used, 0u);
	EXPECT_EQ(segmentHeader.data_type, 0u);
}

TEST(StorageHeadersTest, FragmentationRatioHandlesZeroCapacity) {
	graph::storage::SegmentHeader header;
	header.capacity = 0;
	header.used = 10;
	header.inactive_count = 2;

	EXPECT_EQ(graph::storage::calculateFragmentationRatio(header), 0.0);
}

// ============================================================================
// EntityDispatch.hpp: getEntitySize() for all entity types
// ============================================================================

using namespace graph::storage;

TEST(EntityDispatchTest, GetEntitySizeNode) {
	// Exercises dispatchByType for EntityType::Node
	size_t sz = getEntitySize(toUnderlying(graph::EntityType::Node));
	EXPECT_GT(sz, 0u);
}

TEST(EntityDispatchTest, GetEntitySizeEdge) {
	size_t sz = getEntitySize(toUnderlying(graph::EntityType::Edge));
	EXPECT_GT(sz, 0u);
}

TEST(EntityDispatchTest, GetEntitySizeProperty) {
	size_t sz = getEntitySize(toUnderlying(graph::EntityType::Property));
	EXPECT_GT(sz, 0u);
}

TEST(EntityDispatchTest, GetEntitySizeBlob) {
	size_t sz = getEntitySize(toUnderlying(graph::EntityType::Blob));
	EXPECT_GT(sz, 0u);
}

TEST(EntityDispatchTest, GetEntitySizeIndex) {
	size_t sz = getEntitySize(toUnderlying(graph::EntityType::Index));
	EXPECT_GT(sz, 0u);
}

TEST(EntityDispatchTest, GetEntitySizeState) {
	size_t sz = getEntitySize(toUnderlying(graph::EntityType::State));
	EXPECT_GT(sz, 0u);
}

TEST(EntityDispatchTest, GetEntitySizeInvalidTypeThrows) {
	// Covers the default branch in dispatchByType
	EXPECT_THROW(getEntitySize(999u), std::invalid_argument);
}

// ============================================================================
// StorageHeaders: getTotalFreeSpace, itemsPerSegment, getSegmentHead default
// ============================================================================

TEST(StorageHeadersTest, GetTotalFreeSpace_Basic) {
	graph::storage::SegmentHeader header;
	header.capacity = 100;
	header.used = 70;
	header.inactive_count = 20;
	// activeCount = used - inactive_count = 70 - 20 = 50
	// freeSpace = capacity - activeCount = 100 - 50 = 50
	EXPECT_EQ(header.getTotalFreeSpace(), 50u);
}

TEST(StorageHeadersTest, GetTotalFreeSpace_ZeroFree) {
	graph::storage::SegmentHeader header;
	header.capacity = 50;
	header.used = 50;
	header.inactive_count = 0;
	EXPECT_EQ(header.getTotalFreeSpace(), 0u);
}

TEST(StorageHeadersTest, GetTotalFreeSpace_InactiveExceedsUsed_Clamps) {
	// calculateActiveCount clamps to 0 when inactive_count > used
	graph::storage::SegmentHeader header;
	header.capacity = 100;
	header.used = 5;
	header.inactive_count = 10; // More inactive than used -> activeCount = 0
	EXPECT_EQ(header.getTotalFreeSpace(), 100u);
}

TEST(StorageHeadersTest, ItemsPerSegment_AllEntityTypes) {
	// Covers itemsPerSegment<T>() for all 6 entity types
	EXPECT_EQ(graph::storage::itemsPerSegment<graph::Node>(),    graph::storage::NODES_PER_SEGMENT);
	EXPECT_EQ(graph::storage::itemsPerSegment<graph::Edge>(),    graph::storage::EDGES_PER_SEGMENT);
	EXPECT_EQ(graph::storage::itemsPerSegment<graph::Property>(), graph::storage::PROPERTIES_PER_SEGMENT);
	EXPECT_EQ(graph::storage::itemsPerSegment<graph::Blob>(),    graph::storage::BLOBS_PER_SEGMENT);
	EXPECT_EQ(graph::storage::itemsPerSegment<graph::Index>(),   graph::storage::INDEXES_PER_SEGMENT);
	EXPECT_EQ(graph::storage::itemsPerSegment<graph::State>(),   graph::storage::STATES_PER_SEGMENT);
}

TEST(StorageHeadersTest, GetSegmentHead_AllValidTypes) {
	graph::storage::FileHeader h;
	h.node_segment_head     = 10;
	h.edge_segment_head     = 20;
	h.property_segment_head = 30;
	h.blob_segment_head     = 40;
	h.index_segment_head    = 50;
	h.state_segment_head    = 60;

	EXPECT_EQ(graph::storage::getSegmentHead(h, 0), 10u);
	EXPECT_EQ(graph::storage::getSegmentHead(h, 1), 20u);
	EXPECT_EQ(graph::storage::getSegmentHead(h, 2), 30u);
	EXPECT_EQ(graph::storage::getSegmentHead(h, 3), 40u);
	EXPECT_EQ(graph::storage::getSegmentHead(h, 4), 50u);
	EXPECT_EQ(graph::storage::getSegmentHead(h, 5), 60u);
}

TEST(StorageHeadersTest, GetSegmentHead_DefaultThrows) {
	// Covers the default: throw branch in getSegmentHead
	graph::storage::FileHeader h;
	EXPECT_THROW(graph::storage::getSegmentHead(h, 6), std::invalid_argument);
	EXPECT_THROW(graph::storage::getSegmentHead(h, 99), std::invalid_argument);
}
