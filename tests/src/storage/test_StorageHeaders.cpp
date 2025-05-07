/**
 * @file test_StorageHeaders.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/25
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include "graph/storage/StorageHeaders.h"

TEST(StorageHeadersTest, FileHeaderInitialization) {
    graph::storage::FileHeader fileHeader;

    EXPECT_EQ(std::string(fileHeader.magic, 8), graph::storage::FILE_HEADER_MAGIC_STRING);
    EXPECT_EQ(fileHeader.node_segment_head, 0u);
    EXPECT_EQ(fileHeader.edge_segment_head, 0u);
    EXPECT_EQ(fileHeader.max_node_id, 0u);
    EXPECT_EQ(fileHeader.max_edge_id, 0u);
    EXPECT_EQ(fileHeader.header_crc, 0u);
    EXPECT_EQ(fileHeader.version, 0x0001u);
}

TEST(StorageHeadersTest, SegmentHeaderInitialization) {
    graph::storage::SegmentHeader segmentHeader;

    EXPECT_EQ(segmentHeader.next_segment_offset, 0u);
    EXPECT_EQ(segmentHeader.start_id, 0u);
    EXPECT_EQ(segmentHeader.capacity, 0u);
    EXPECT_EQ(segmentHeader.used, 0u);
    EXPECT_EQ(segmentHeader.segment_crc, 0u);
    EXPECT_EQ(segmentHeader.data_type, 0u);
}