/**
 * @file StateChainManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/17
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/StateChainManager.hpp"
#include <stdexcept>
#include "graph/storage/data/DataManager.hpp"

namespace graph {

	StateChainManager::StateChainManager(std::shared_ptr<storage::DataManager> dataManager) :
		dataManager_(std::move(dataManager)) {}

	std::vector<State> StateChainManager::createStateChain(const std::string &key, const std::string &data) const {
		// Split into chunks
		const auto chunks = splitData(data);
		if (chunks.empty()) {
			return {};
		}

		// Create state entities
		std::vector<State> stateChain;
		stateChain.reserve(chunks.size());

		int64_t prevStateId = 0;

		// Create each state in the chain
		for (size_t i = 0; i < chunks.size(); i++) {
			// Only the head state has the actual key, other states have empty keys
			// as they should only be accessed through the chain
			std::string stateKey = (i == 0) ? key : "";

			State currentState(0, stateKey, chunks[i]);

			// Set chain position
			currentState.setChainPosition(static_cast<int32_t>(i));

			// Set previous state ID
			currentState.setPrevStateId(prevStateId);

			dataManager_->addStateEntity(currentState);

			// Update previous state's next pointer if not the first state
			if (i > 0) {
				// Get a reference to the previous state from our local vector.
				State &prevState = stateChain.back();

				// Set its `next_state_id` to the permanent ID of the current state.
				prevState.setNextStateId(currentState.getId());

				// We modified `prevState` after it was added, so we must tell the DataManager
				// to update its version in the dirty map.
				dataManager_->updateStateEntity(prevState);
			}

			prevStateId = currentState.getId();
			stateChain.push_back(currentState);
		}

		return stateChain;
	}

	std::string StateChainManager::readStateChain(const int64_t headStateId) const {
		// Get the head state
		const State headState = dataManager_->getState(headStateId);
		if (headState.getId() == 0 || !headState.isActive()) {
			throw std::runtime_error("Head state not found or inactive");
		}

		// Check if this is a single state or a chain
		if (!headState.isChained()) {
			// Single state case
			return headState.getDataAsString();
		}

		// Reassemble data from the chain
		std::string reassembledData;
		std::vector<int64_t> visitedIds; // To detect cycles
		int64_t currentStateId = headStateId;
		int32_t expectedPosition = 0;

		while (currentStateId != 0) {
			// Check for cycles
			if (std::ranges::find(visitedIds, currentStateId) != visitedIds.end()) {
				throw std::runtime_error("Circular reference detected in state chain");
			}
			visitedIds.push_back(currentStateId);

			State currentState = dataManager_->getState(currentStateId);

			if (currentState.getId() == 0 || !currentState.isActive()) {
				throw std::runtime_error("State chain corrupted: missing state at position " +
										 std::to_string(expectedPosition));
			}

			// Verify chain position
			if (currentState.getChainPosition() != expectedPosition) {
				throw std::runtime_error("State chain corrupted: expected position " +
										 std::to_string(expectedPosition) + " but found " +
										 std::to_string(currentState.getChainPosition()));
			}

			// Append data
			reassembledData.append(currentState.getDataAsString());

			// Move to next state
			currentStateId = currentState.getNextStateId();
			expectedPosition++;
		}

		return reassembledData;
	}

	bool StateChainManager::isDataSame(const int64_t headStateId, const std::string &newData) const {
		try {
			const std::string currentData = readStateChain(headStateId);
			return currentData == newData;
		} catch (const std::exception &) {
			// If we can't read the current data, assume it's different
			return false;
		}
	}

	std::vector<State> StateChainManager::updateStateChain(const int64_t headStateId,
														   const std::string &newData) const {
		// Check if the data is actually different
		if (isDataSame(headStateId, newData)) {
			// Data is the same, return the existing chain
			const auto chainIds = getStateChainIds(headStateId);
			std::vector<State> existingChain;
			existingChain.reserve(chainIds.size());

			for (auto stateId: chainIds) {
				State state = dataManager_->getState(stateId);
				if (state.getId() != 0 && state.isActive()) {
					existingChain.push_back(state);
				}
			}
			return existingChain;
		}

		// Data is different, proceed with update
		const State headState = dataManager_->getState(headStateId);
		if (headState.getId() == 0 || !headState.isActive()) {
			throw std::runtime_error("Head state not found or inactive");
		}
		const std::string &originalKey = headState.getKey();

		deleteStateChain(headStateId);
		auto updatedChain = createStateChain(originalKey, newData);

		return updatedChain;
	}

	void StateChainManager::deleteStateChain(const int64_t headStateId) const {
		// Delete each state in the chain
		for (const auto chainIds = getStateChainIds(headStateId); auto stateId: chainIds) {
			State state = dataManager_->getState(stateId);
			if (state.getId() != 0 && state.isActive()) {
				dataManager_->deleteState(state);
			}
		}
	}

	std::vector<int64_t> StateChainManager::getStateChainIds(const int64_t headStateId) const {
		std::vector<int64_t> chainIds;
		int64_t currentStateId = headStateId;

		while (currentStateId != 0) {
			State currentState = dataManager_->getState(currentStateId);

			if (currentState.getId() == 0 || !currentState.isActive()) {
				break;
			}

			chainIds.push_back(currentStateId);
			currentStateId = currentState.getNextStateId();
		}

		return chainIds;
	}

	State StateChainManager::findStateByKey(const std::string &key) const {
		// We'll need to implement a method in DataManager to find a state by key
		// For now, this is a placeholder
		return dataManager_->findStateByKey(key);
	}

	std::vector<std::string> StateChainManager::splitData(const std::string &data) {
		std::vector<std::string> chunks;

		for (size_t offset = 0; offset < data.size(); offset += State::CHUNK_SIZE) {
			size_t chunkSize = std::min(State::CHUNK_SIZE, data.size() - offset);
			chunks.push_back(data.substr(offset, chunkSize));
		}

		return chunks;
	}

} // namespace graph
