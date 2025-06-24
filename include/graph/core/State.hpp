/**
 * @file State.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/17
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <cstdint>
#include <string>
#include <iostream>
#include <unordered_map>
#include "Types.hpp"

namespace graph {

    /**
     * State entity for storing configuration and persistent variables.
     * Supports both inline storage for small data and chain-based storage
     * for larger data volumes.
     */
    class State {
    public:
        struct Metadata {
            int64_t id = 0;                // Unique identifier
            int64_t nextStateId = 0;       // Next state in chain
            int64_t prevStateId = 0;       // Previous state in chain
            uint32_t dataSize = 0;         // Size of data in current chunk
            int32_t chainPosition = 0;     // Position in the chain
            std::string key;               // Unique key for state identification
            bool isActive = true;          // Whether this state is active
        };

        static constexpr size_t TOTAL_STATE_SIZE = 256;
        static constexpr size_t MAX_KEY_LENGTH = 32;
        static constexpr size_t METADATA_SIZE = sizeof(int64_t) * 3 + sizeof(uint32_t) +
                                               sizeof(int32_t) + sizeof(bool) + MAX_KEY_LENGTH;
        static constexpr size_t CHUNK_SIZE = TOTAL_STATE_SIZE - METADATA_SIZE;
        static constexpr uint32_t typeId = toUnderlying(EntityType::State);

        // Constructors
        State() = default;
        State(int64_t id, const std::string& key, const std::string& data);

        // Basic accessors
        [[nodiscard]] const Metadata& getMetadata() const { return metadata; }
        [[nodiscard]] Metadata& getMutableMetadata() { return metadata; }

        [[nodiscard]] int64_t getId() const { return metadata.id; }
        void setId(int64_t newId) { metadata.id = newId; }

        [[nodiscard]] const std::string& getKey() const { return metadata.key; }
        void setKey(const std::string& newKey);

        [[nodiscard]] const char* getData() const { return dataBuffer; }
        [[nodiscard]] std::string getDataAsString() const;
        [[nodiscard]] uint32_t getSize() const { return metadata.dataSize; }

        void setData(const std::string& newData);
        [[nodiscard]] static bool canFitData(uint32_t dataSize);

        // Chain management
        [[nodiscard]] int64_t getNextStateId() const { return metadata.nextStateId; }
        [[nodiscard]] int64_t getPrevStateId() const { return metadata.prevStateId; }
        void setNextStateId(int64_t id) { metadata.nextStateId = id; }
        void setPrevStateId(int64_t id) { metadata.prevStateId = id; }
        [[nodiscard]] bool isChained() const { return metadata.nextStateId != 0; }
        [[nodiscard]] bool isChainStart() const { return metadata.prevStateId == 0 && metadata.nextStateId != 0; }
        [[nodiscard]] bool isChainEnd() const { return metadata.nextStateId == 0 && metadata.prevStateId != 0; }
        void setChainPosition(int32_t pos) { metadata.chainPosition = pos; }
        [[nodiscard]] int32_t getChainPosition() const { return metadata.chainPosition; }

        // Active state management
        void markInactive() { metadata.isActive = false; }
        [[nodiscard]] bool isActive() const { return metadata.isActive; }

        // ID management
        [[nodiscard]] bool hasTemporaryId() const;
        void setPermanentId(int64_t permanentId);

        // Serialization
        void serialize(std::ostream& os) const;
        static State deserialize(std::istream& is);
        [[nodiscard]] size_t getSerializedSize() const;

        static constexpr size_t getTotalSize() {
            return TOTAL_STATE_SIZE;
        }

    private:
        Metadata metadata;
        char dataBuffer[CHUNK_SIZE] = {};
    };

} // namespace graph