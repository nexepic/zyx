/**
 * @file Blob.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/25
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <cstdint>
#include <iostream>
#include <string>

namespace graph {

    class alignas(256) Blob {
    public:
        static constexpr uint32_t typeId = 3; // Node = 0, Edge = 1, Property = 2, Blob = 3
        static constexpr size_t MAX_COMPRESSED_SIZE = 5 * 1024 * 1024; // 5MB
        // static constexpr size_t CHUNK_SIZE = 128 * 1024; // 128KB per blob chunk
        static constexpr size_t CHUNK_SIZE = 10;

        Blob(int64_t id, const std::string &data);
        Blob() = default;

        int64_t getId() const;
        void setId(int64_t newId) { id = newId; }
        const std::string &getData() const;
        size_t getSize() const;

        bool hasTemporaryId() const;
        void setPermanentId(int64_t permanentId);

        void markInactive();
        bool isActive() const;

        // Linked blob management
        int64_t getNextBlobId() const { return nextBlobId; }
        int64_t getPrevBlobId() const { return prevBlobId; }
        void setNextBlobId(int64_t id) { nextBlobId = id; }
        void setPrevBlobId(int64_t id) { prevBlobId = id; }
        bool isChained() const { return nextBlobId != 0; }
        bool isChainStart() const { return prevBlobId == 0 && nextBlobId != 0; }
        bool isChainEnd() const { return nextBlobId == 0 && prevBlobId != 0; }
        void setChainPosition(int32_t pos) { chainPosition = pos; }
        int32_t getChainPosition() const { return chainPosition; }

        // Serialization methods
        void serialize(std::ostream &os) const;
        static Blob deserialize(std::istream &is);

        void setEntityId(int64_t newEntityId) {
            entityId = newEntityId;
        }

        void setEntityType(uint32_t newEntityType) {
            entityType = newEntityType;
        }

        int64_t getEntityId() const { return entityId; }
        uint32_t getEntityType() const { return entityType; }

        // setEntityInfo
        void setEntityInfo(int64_t newEntityId, uint32_t newEntityType) {
            entityId = newEntityId;
            entityType = newEntityType;
        }

        // setData
        void setData(const std::string &newData) {
            data = newData;
        }

        // Set compression info
        void setCompressionInfo(uint32_t origSize, bool isCompressed) {
            originalSize = origSize;
            compressed = isCompressed;
        }

        // Get compression info
        uint32_t getOriginalSize() const { return originalSize; }
        bool isCompressed() const { return compressed; }

    private:
        int64_t id = 0;
        int64_t entityId = 0;  // ID of the entity this property belongs to
        uint32_t entityType = 0; // 0 = Node, 1 = Edge
        std::string data;
        bool isActive_ = true;

        // Blob chain management
        int64_t nextBlobId = 0;  // ID of the next blob in the chain
        int64_t prevBlobId = 0;  // ID of the previous blob in the chain
        int32_t chainPosition = 0; // Position in the chain (0 = first blob)

        // Compression information
        uint32_t originalSize = 0; // Original size before compression
        bool compressed = false;   // Whether this blob contains compressed data
    };

} // namespace graph