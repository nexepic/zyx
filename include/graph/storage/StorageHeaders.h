/**
 * @file StorageHeaders.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <graph/core/Edge.h>
#include <graph/core/Node.h>

namespace graph::storage {

	constexpr uint32_t PAGE_SIZE = 4096;
	constexpr uint32_t SEGMENT_SIZE = PAGE_SIZE * 256; // 1M per segment
	constexpr uint32_t NODES_PER_SEGMENT = SEGMENT_SIZE / sizeof(Node);
	constexpr uint32_t EDGES_PER_SEGMENT = SEGMENT_SIZE / sizeof(Edge);

	constexpr size_t FILE_HEADER_SIZE = 128;
	constexpr size_t SEGMENT_HEADER_SIZE = 64;

	// File header
	struct FileHeader {
		char magic[8] = {'M', 'E', 'T', 'R', 'I', 'X', 'D', 'B'};
		uint64_t node_segment_head = 0; // Offset of the first node segment
		uint64_t edge_segment_head = 0; // Offset of the first edge segment
		uint64_t free_pool_head = 0; // Head of the free pool list

		// Statistics
		uint64_t max_node_id = 0;
		uint64_t max_edge_id = 0;

		uint32_t page_size = PAGE_SIZE; // Storage block alignment size

		uint32_t segment_count = 0; // Total number of segments

		// Checksum
		uint32_t header_crc = 0;

		uint16_t version = 0x0001; // version

		uint8_t reserved[FILE_HEADER_SIZE -
						 (sizeof(magic) + sizeof(version) + sizeof(page_size) + sizeof(node_segment_head) +
						  sizeof(edge_segment_head) + sizeof(free_pool_head) + sizeof(max_node_id) +
						  sizeof(max_edge_id) + sizeof(segment_count) + sizeof(header_crc))] = {};
	};

	// Segment metadata (32 bytes at the beginning of each segment)
	struct SegmentHeader {
		uint64_t next_segment_offset = 0; // File offset of the next segment
		uint64_t start_id = 0; // Starting ID
		uint32_t capacity = 0; // Total capacity of the segment (in items)
		uint32_t used = 0; // Number of used items
		uint32_t segment_crc = 0; // CRC checksum of the segment data
		uint8_t data_type = 0; // Data type 0: Node 1: Edge
		uint8_t reserved[SEGMENT_HEADER_SIZE - (sizeof(next_segment_offset) + sizeof(data_type) + sizeof(capacity) +
												sizeof(used) + sizeof(start_id) + sizeof(segment_crc))] = {};
	};

} // namespace graph::storage
