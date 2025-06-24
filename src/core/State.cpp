/**
 * @file State.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/17
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/State.hpp"
#include <cstring>
#include <stdexcept>
#include "graph/storage/IDAllocator.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph {

	State::State(int64_t id, const std::string &key, const std::string &data) {
		metadata.id = id;
		setKey(key);
		setData(data);
	}

	void State::setKey(const std::string &newKey) {
		if (newKey.size() > MAX_KEY_LENGTH - 1) { // Leave space for null terminator
			throw std::runtime_error("State key exceeds maximum length of " + std::to_string(MAX_KEY_LENGTH - 1) +
									 " characters");
		}
		metadata.key = newKey;
	}

	std::string State::getDataAsString() const { return {dataBuffer, metadata.dataSize}; }

	void State::setData(const std::string &newData) {
		metadata.dataSize = std::min(newData.size(), CHUNK_SIZE);
		memcpy(dataBuffer, newData.data(), metadata.dataSize);
	}

	bool State::canFitData(uint32_t dataSize) { return dataSize <= CHUNK_SIZE; }

	bool State::hasTemporaryId() const { return storage::IDAllocator::isTemporaryId(metadata.id); }

	void State::setPermanentId(int64_t permanentId) {
		if (!hasTemporaryId()) {
			throw std::runtime_error("Cannot set permanent ID for state that already has one");
		}
		metadata.id = permanentId;
	}

	void State::serialize(std::ostream &os) const {
		utils::Serializer::writePOD(os, metadata.id);
		utils::Serializer::writePOD(os, metadata.nextStateId);
		utils::Serializer::writePOD(os, metadata.prevStateId);
		utils::Serializer::writePOD(os, metadata.dataSize);
		utils::Serializer::writePOD(os, metadata.chainPosition);
		utils::Serializer::writePOD(os, metadata.isActive);
		utils::Serializer::writeString(os, metadata.key);

		if (metadata.dataSize > 0) {
			os.write(dataBuffer, metadata.dataSize);
		}
	}

	State State::deserialize(std::istream &is) {
		State state;
		state.metadata.id = utils::Serializer::readPOD<int64_t>(is);
		state.metadata.nextStateId = utils::Serializer::readPOD<int64_t>(is);
		state.metadata.prevStateId = utils::Serializer::readPOD<int64_t>(is);
		state.metadata.dataSize = utils::Serializer::readPOD<uint32_t>(is);
		state.metadata.chainPosition = utils::Serializer::readPOD<int32_t>(is);
		state.metadata.isActive = utils::Serializer::readPOD<bool>(is);
		state.metadata.key = utils::Serializer::readString(is);

		if (state.metadata.dataSize > 0) {
			is.read(state.dataBuffer, state.metadata.dataSize);
		}
		return state;
	}

	size_t State::getSerializedSize() const {
		size_t size = 0;
		size += sizeof(metadata.id);
		size += sizeof(metadata.nextStateId);
		size += sizeof(metadata.prevStateId);
		size += sizeof(metadata.dataSize);
		size += sizeof(metadata.chainPosition);
		size += sizeof(metadata.isActive);
		size += sizeof(uint32_t) + metadata.key.size(); // String size + content
		size += metadata.dataSize; // Actual data size

		return size;
	}

} // namespace graph
