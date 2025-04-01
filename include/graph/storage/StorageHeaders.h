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

    constexpr char FILE_HEADER_MAGIC_STRING[] = "METRIXDB";
    constexpr uint32_t PAGE_SIZE = 4096;
    constexpr uint32_t SEGMENT_SIZE = PAGE_SIZE * 32; // 128K per segment
    constexpr uint32_t NODES_PER_SEGMENT = SEGMENT_SIZE / sizeof(Node);
    constexpr uint32_t EDGES_PER_SEGMENT = SEGMENT_SIZE / sizeof(Edge);
    constexpr uint32_t MAX_INLINE_PROPERTY_SIZE = 2; // 16KB max inline property
    constexpr uint32_t MAX_SEGMENT_PROPERTY_SIZE = 1 * 512; // 512 bytes per property segment
    constexpr uint32_t BLOB_SECTION_SIZE = SEGMENT_SIZE * 4; // 512K per blob section
    constexpr uint32_t PROPERTY_SEGMENT_SIZE = SEGMENT_SIZE * 2; // 256K per property segment
    constexpr uint32_t SMALL_PROPERTY_THRESHOLD = 64;

    constexpr size_t FILE_HEADER_SIZE = 128;
    constexpr size_t SEGMENT_HEADER_SIZE = 64;

    // File header
    struct alignas(FILE_HEADER_SIZE) FileHeader {
        char magic[8]{};
        uint64_t node_segment_head = 0; // Offset of the first node segment
        uint64_t edge_segment_head = 0; // Offset of the first edge segment
        uint64_t property_segment_head = 0; // Offset of the first property segment
        uint64_t blob_section_head = 0; // Offset of the first blob section
        uint64_t free_pool_head = 0; // Head of the free pool list

        // Statistics
        uint64_t max_node_id = 0;
        uint64_t max_edge_id = 0;
        uint64_t max_blob_id = 0;

        uint32_t page_size = PAGE_SIZE; // Storage block alignment size
        uint32_t segment_count = 0; // Total number of segments

        uint32_t version = 0x00000001; // File format version

        // Checksum
        uint32_t header_crc = 0;

        FileHeader() {
		    std::copy(std::begin(FILE_HEADER_MAGIC_STRING), std::end(FILE_HEADER_MAGIC_STRING) - 1, magic);
		}
    };

    // Segment metadata
    struct alignas(SEGMENT_HEADER_SIZE) SegmentHeader {
        uint64_t next_segment_offset = 0; // File offset of the next segment
        uint64_t start_id = 0; // Starting ID
        uint32_t capacity = 0; // Total capacity of the segment (in items)
        uint32_t used = 0; // Number of used items
        uint32_t data_type = 0; // Data type 0: Node 1: Edge
        uint32_t segment_crc = 0; // CRC checksum of the segment data
    };

    // Property segment header information
    struct alignas(SEGMENT_HEADER_SIZE) PropertySegmentHeader {
        uint64_t next_segment_offset = 0;  // Offset of the next property segment
        uint32_t capacity = 0;             // Total capacity of the segment (bytes)
        uint32_t used = 0;                 // Number of used bytes
        uint32_t segment_crc = 0;          // CRC checksum of the segment data
    };

    // Property entry header information - stored before each property set in the property segment
    struct PropertyEntryHeader {
        uint64_t entity_id = 0;     // ID of the associated entity
        uint8_t entity_type = 0;    // Entity type (0=Node, 1=Edge)
        uint32_t data_size = 0;     // Property data size
        uint32_t property_count = 0; // Number of properties
    };

} // namespace graph::storage