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

#include <string>
#include <unordered_map>
#include "Types.hpp"
#include "graph/core/Entity.hpp"

namespace graph {

	/**
	 * State entity for storing configuration and persistent variables.
	 * Supports both inline storage for small data and chain-based storage
	 * for larger data volumes.
	 */
	class State : public EntityBase<State>, public ChainableMixin<State> {
	public:
		static constexpr size_t MAX_KEY_LENGTH = 32;

		struct Metadata {
			int64_t id = 0; // Unique identifier
			int64_t nextStateId = 0; // Next state in chain
			int64_t prevStateId = 0; // Previous state in chain
			uint32_t dataSize = 0; // Size of data in current chunk
			int32_t chainPosition = 0; // Position in the chain
			char key[MAX_KEY_LENGTH] = {}; // Unique key for state identification
			bool isActive = true; // Whether this state is active
		};

		static constexpr size_t TOTAL_STATE_SIZE = 256;
		static constexpr size_t METADATA_SIZE = offsetof(Metadata, isActive) + sizeof(Metadata::isActive);
		static constexpr size_t CHUNK_SIZE = TOTAL_STATE_SIZE - METADATA_SIZE;
		static constexpr uint32_t typeId = toUnderlying(EntityType::State);

		// Constructors
		State() = default;
		State(int64_t id, const std::string &key, const std::string &data);

		// Metadata access for CRTP
		[[nodiscard]] const Metadata &getMetadata() const { return metadata; }
		[[nodiscard]] Metadata &getMutableMetadata() { return metadata; }

		// Adapter methods for ChainableMixin
		[[nodiscard]] int64_t getNextChainId() const { return metadata.nextStateId; }
		[[nodiscard]] int64_t getPrevChainId() const { return metadata.prevStateId; }
		void setNextChainId(int64_t id) { metadata.nextStateId = id; }
		void setPrevChainId(int64_t id) { metadata.prevStateId = id; }

		// Legacy methods for backward compatibility
		[[nodiscard]] int64_t getNextStateId() const { return getNextId(); }
		[[nodiscard]] int64_t getPrevStateId() const { return getPrevId(); }
		void setNextStateId(int64_t id) { setNextId(id); }
		void setPrevStateId(int64_t id) { setPrevId(id); }

		// Key management
		[[nodiscard]] std::string getKey() const;
		void setKey(const std::string &newKey);

		// Data management
		[[nodiscard]] const char *getData() const { return dataBuffer; }
		[[nodiscard]] std::string getDataAsString() const;
		[[nodiscard]] uint32_t getSize() const { return metadata.dataSize; }
		void setData(const std::string &newData);
		[[nodiscard]] static bool canFitData(uint32_t dataSize);

		// Serialization
		void serialize(std::ostream &os) const;
		static State deserialize(std::istream &is);
		[[nodiscard]] size_t getSerializedSize() const;

		static constexpr size_t getTotalSize() { return TOTAL_STATE_SIZE; }

	private:
		Metadata metadata;
		char dataBuffer[CHUNK_SIZE > 0 ? CHUNK_SIZE : 1] = {};
	};

} // namespace graph
