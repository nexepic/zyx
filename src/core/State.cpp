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

	std::string State::getKey() const { return {metadata.key}; }

	void State::setKey(const std::string &newKey) {
		if (newKey.length() >= MAX_KEY_LENGTH) {
			throw std::runtime_error("State key exceeds maximum length of " + std::to_string(MAX_KEY_LENGTH - 1) +
									 " characters");
		}
		std::fill_n(metadata.key, MAX_KEY_LENGTH, 0);
		strncpy(metadata.key, newKey.c_str(), MAX_KEY_LENGTH - 1);
		metadata.key[MAX_KEY_LENGTH - 1] = '\0';
	}

	std::string State::getDataAsString() const { return {dataBuffer, metadata.dataSize}; }

	void State::setData(const std::string &newData) {
		metadata.dataSize = std::min(newData.size(), CHUNK_SIZE);
		memcpy(dataBuffer, newData.data(), metadata.dataSize);
	}

	bool State::canFitData(uint32_t dataSize) { return dataSize <= CHUNK_SIZE; }

	void State::serialize(std::ostream &os) const {
		utils::Serializer::writePOD(os, metadata.id);
		utils::Serializer::writePOD(os, metadata.nextStateId);
		utils::Serializer::writePOD(os, metadata.prevStateId);
		utils::Serializer::writePOD(os, metadata.dataSize);
		utils::Serializer::writePOD(os, metadata.chainPosition);
		utils::Serializer::writeBuffer(os, metadata.key, MAX_KEY_LENGTH);
		utils::Serializer::writePOD(os, metadata.isActive);

		if (metadata.dataSize > 0) {
			utils::Serializer::writeBuffer(os, dataBuffer, metadata.dataSize);
		}
	}

	State State::deserialize(std::istream &is) {
		State state;

		state.metadata.id = utils::Serializer::readPOD<int64_t>(is);
		state.metadata.nextStateId = utils::Serializer::readPOD<int64_t>(is);
		state.metadata.prevStateId = utils::Serializer::readPOD<int64_t>(is);
		state.metadata.dataSize = utils::Serializer::readPOD<uint32_t>(is);
		state.metadata.chainPosition = utils::Serializer::readPOD<int32_t>(is);
		utils::Serializer::readBuffer(is, state.metadata.key, MAX_KEY_LENGTH);
		state.metadata.key[MAX_KEY_LENGTH - 1] = '\0'; // Ensure null-termination for safety
		state.metadata.isActive = utils::Serializer::readPOD<bool>(is);

		if (state.metadata.dataSize > 0) {
			if (state.metadata.dataSize > CHUNK_SIZE) {
				throw std::runtime_error("Deserialization error: dataSize " + std::to_string(state.metadata.dataSize) +
										 " exceeds CHUNK_SIZE " + std::to_string(CHUNK_SIZE));
			}
			utils::Serializer::readBuffer(is, state.dataBuffer, state.metadata.dataSize);
		}
		return state;
	}

	size_t State::getSerializedSize() const {
		size_t size = 0;
		size += METADATA_SIZE; // Metadata size
		size += metadata.dataSize; // Actual data size

		return size;
	}

} // namespace graph
