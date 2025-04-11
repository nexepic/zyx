/**
 * @file BlobStore.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/25
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <string>
#include <fstream>
#include <filesystem>

namespace graph::storage {

    constexpr size_t BLOB_SECTION_HEADER_SIZE = 32;
    constexpr size_t BLOB_ENTRY_HEADER_SIZE = 64;

    // Header for the blob section
    struct alignas(BLOB_SECTION_HEADER_SIZE) BlobSectionHeader {
        uint64_t next_section_offset = 0;
        uint64_t prev_section_offset = 0;
        uint32_t capacity = 0;
        uint32_t used = 0;
        uint32_t section_crc = 0;
    };

    // Individual blob entry header
    struct alignas(BLOB_ENTRY_HEADER_SIZE) BlobEntryHeader {
        uint64_t id = 0;           // Unique identifier for the blob
        uint32_t size = 0;         // Size of uncompressed data
        uint32_t stored_size = 0;  // Size of data as stored (compressed if applicable)
        bool compressed = false;   // Whether the data is compressed
        char content_type[16] = {}; // Hint about content type
        uint32_t entry_crc = 0;     // CRC checksum for this entry
    };

    class BlobStore {
    public:
        explicit BlobStore(std::shared_ptr<std::fstream> file, uint64_t blobSectionOffset = 0);

        // Store a new blob, returns the blob ID
        uint64_t storeBlob(const std::string& data, const std::string& contentType = "text");

        // Retrieve a blob by ID
        std::string getBlob(uint64_t id);

        // Check if compression would be beneficial for this data
        static bool shouldCompress(const std::string& data, const std::string& contentType);

        // Compress/decompress utilities
        static std::string compressData(const std::string& data);
        static std::string decompressData(const std::string& data, uint32_t originalSize);

        // Allocate a new blob section
        uint64_t allocateSection(uint32_t capacity);

        // Flush any changes to disk
        void flush();

        // Check if a blob exists
        bool blobExists(uint64_t id);

    private:
        std::shared_ptr<std::fstream> file_;
        uint64_t blobSectionOffset_;
        uint64_t nextBlobId_ = 1;

        // Helper method to find a blob entry's offset
        uint64_t findBlobEntryOffset(uint64_t id);
    };

} // namespace graph::storage