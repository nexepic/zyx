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

TEST(StorageHeadersTest, FileHeaderInitialization) {
	const graph::storage::FileHeader fileHeader;

	EXPECT_EQ(std::string(fileHeader.magic, 8), graph::storage::FILE_HEADER_MAGIC_STRING);
	EXPECT_EQ(fileHeader.node_segment_head, 0u);
	EXPECT_EQ(fileHeader.edge_segment_head, 0u);
	EXPECT_EQ(fileHeader.max_node_id, 0u);
	EXPECT_EQ(fileHeader.max_edge_id, 0u);
	EXPECT_EQ(fileHeader.data_crc, 0u);
	EXPECT_EQ(fileHeader.version, 0x0002u);
}

TEST(StorageHeadersTest, SegmentHeaderInitialization) {
	constexpr graph::storage::SegmentHeader segmentHeader;

	EXPECT_EQ(segmentHeader.next_segment_offset, 0u);
	EXPECT_EQ(segmentHeader.start_id, 0u);
	EXPECT_EQ(segmentHeader.capacity, 0u);
	EXPECT_EQ(segmentHeader.used, 0u);
	EXPECT_EQ(segmentHeader.data_type, 0u);
}
