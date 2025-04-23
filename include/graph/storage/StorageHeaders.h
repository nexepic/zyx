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
    // constexpr uint32_t SEGMENT_SIZE = PAGE_SIZE * 32; // 128K per segment
    constexpr uint32_t SEGMENT_SIZE = 1024;
    constexpr uint32_t NODES_PER_SEGMENT = SEGMENT_SIZE / sizeof(Node);
    constexpr uint32_t EDGES_PER_SEGMENT = SEGMENT_SIZE / sizeof(Edge);
    constexpr uint32_t MAX_INLINE_PROPERTY_SIZE = 2; // 16KB max inline property
    constexpr uint32_t MAX_SEGMENT_PROPERTY_SIZE = 1 * 512; // 512 bytes per property segment
    constexpr uint32_t BLOB_SECTION_SIZE = SEGMENT_SIZE; // Keep BLOB section size same as segment size
    constexpr uint32_t PROPERTY_SEGMENT_SIZE = SEGMENT_SIZE; // Keep property segment size same as segment size

    constexpr size_t FILE_HEADER_SIZE = 128;
    constexpr size_t SEGMENT_HEADER_SIZE = 128;
    constexpr uint32_t TOTAL_SEGMENT_SIZE = SEGMENT_SIZE + SEGMENT_HEADER_SIZE;

    constexpr uint32_t MAX_BITMAP_SIZE = 64; // 64 bytes = 512 bits

    // File header
    struct alignas(FILE_HEADER_SIZE) FileHeader {
        char magic[8]{};
        uint64_t node_segment_head = 0; // Offset of the first node segment
        uint64_t edge_segment_head = 0; // Offset of the first edge segment
        uint64_t property_segment_head = 0; // Offset of the first property segment
        uint64_t blob_section_head = 0; // Offset of the first blob section
        uint64_t free_pool_head = 0; // Head of the free pool list

        // Statistics
        int64_t max_node_id = 0;
        int64_t max_edge_id = 0;
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

    template <typename HeaderType>
    uint32_t calculateActiveCount(const HeaderType &header) {
        return (header.used > header.inactive_count) ? (header.used - header.inactive_count) : 0;
    }

    template <typename HeaderType>
    uint32_t calculateTotalFreeSpace(const HeaderType &header) {
        return header.capacity - calculateActiveCount(header);
    }

    template <typename HeaderType>
    double calculateFragmentationRatio(const HeaderType &header) {
        return (header.capacity > 0) ? static_cast<double>(calculateTotalFreeSpace(header)) / header.capacity : 0.0;
    }

    // Segment metadata
    struct alignas(SEGMENT_HEADER_SIZE) SegmentHeader {
        uint64_t next_segment_offset = 0; // File offset of the next segment
        uint64_t prev_segment_offset = 0; // File offset of the previous segment
        int64_t start_id = 0; // Starting ID
        uint32_t capacity = 0; // Total capacity of the segment (in items)
        uint32_t used = 0; // Number of used items
        uint32_t inactive_count = 0; // Number of inactive items
        uint32_t data_type = 0; // Data type 0: Node 1: Edge
        uint32_t segment_crc = 0; // CRC checksum of the segment data
        uint32_t bitmap_size = 0; // Size of the activity bitmap in bytes
        uint8_t activity_bitmap[MAX_BITMAP_SIZE] = {}; // Bitmap tracking active/inactive status (1=active, 0=inactive)

        uint32_t getActiveCount() const {
            return calculateActiveCount(*this);
        }

        uint32_t getTotalFreeSpace() const {
            return calculateTotalFreeSpace(*this);
        }

        double getFragmentationRatio() const {
            return calculateFragmentationRatio(*this);
        }
    };

    // Property segment header information
    struct alignas(SEGMENT_HEADER_SIZE) PropertySegmentHeader {
        uint64_t next_segment_offset = 0;  // Offset of the next property segment
        uint64_t prev_segment_offset = 0;  // Offset of the previous property segment
        uint32_t capacity = 0;             // Total capacity of the segment (bytes)
        uint32_t used = 0;                 // Number of used bytes
        uint32_t inactive_count = 0;       // Number of inactive bytes
        uint32_t segment_crc = 0;          // CRC checksum of the segment data
        uint32_t bitmap_size = 0;          // Size of the activity bitmap in bytes
        uint8_t activity_bitmap[MAX_BITMAP_SIZE] = {}; // Bitmap tracking active/inactive areas

        uint32_t getActiveCount() const {
            return calculateActiveCount(*this);
        }

        uint32_t getTotalFreeSpace() const {
            return calculateTotalFreeSpace(*this);
        }

        double getFragmentationRatio() const {
            return calculateFragmentationRatio(*this);
        }
    };

    // Property entry header information - stored before each property set in the property segment
    struct PropertyEntryHeader {
        int64_t entity_id = 0;     // ID of the associated entity
        uint8_t entity_type = 0;    // Entity type (0=Node, 1=Edge)
        uint64_t entity_offset = 0; // File offset of the entity
        uint32_t data_size = 0;     // Property data size
        uint32_t property_count = 0; // Number of properties
    };

    namespace bitmap {
        // Set a bit in a bitmap
        inline void setBit(uint8_t* bitmap, size_t index, bool value) {
            size_t byteIndex = index / 8;
            size_t bitIndex = index % 8;
            if (value) {
                bitmap[byteIndex] |= (1 << bitIndex);
            } else {
                bitmap[byteIndex] &= ~(1 << bitIndex);
            }
        }

        // Get a bit from a bitmap
        inline bool getBit(const uint8_t* bitmap, size_t index) {
            size_t byteIndex = index / 8;
            size_t bitIndex = index % 8;
            return (bitmap[byteIndex] & (1 << bitIndex)) != 0;
        }

        // Calculate required bitmap size in bytes for a given number of elements
        inline uint32_t calculateBitmapSize(uint32_t elementCount) {
            return (elementCount + 7) / 8; // Round up to nearest byte
        }
    }

} // namespace graph::storage