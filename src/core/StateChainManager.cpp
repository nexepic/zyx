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
#include "graph/storage/DataManager.hpp"

namespace graph {

	StateChainManager::StateChainManager(std::shared_ptr<storage::DataManager> dataManager) :
		dataManager_(std::move(dataManager)) {}

	std::vector<State> StateChainManager::createStateChain(const std::string &key, const std::string &data) const {
		// Split into chunks
		auto chunks = splitData(data);

		// Create state entities
		std::vector<State> stateChain;
		stateChain.reserve(chunks.size());

		int64_t prevStateId = 0;

		// Create each state in the chain
		for (size_t i = 0; i < chunks.size(); i++) {
			// Create new state with temporary ID
			int64_t tempId = dataManager_->reserveTemporaryStateId();

			// Only the head state has the actual key, other states have empty keys
			// as they should only be accessed through the chain
			std::string stateKey = (i == 0) ? key : "";

			State state(tempId, stateKey, chunks[i]);

			// Set chain position
			state.setChainPosition(static_cast<int32_t>(i));

			// Set previous state ID
			state.setPrevStateId(prevStateId);

			// Add to chain
			stateChain.push_back(state);

			// Update previous state's next pointer if not the first state
			if (i > 0) {
				stateChain[i - 1].setNextStateId(tempId);
			}

			prevStateId = tempId;
		}

		return stateChain;
	}

	std::string StateChainManager::readStateChain(int64_t headStateId) const {
		// Get the head state
		State headState = dataManager_->getState(headStateId);
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

	std::vector<State> StateChainManager::updateStateChain(int64_t headStateId, const std::string &newData) const {
		// Delete existing chain
		deleteStateChain(headStateId);

		// Get the original key from the head state
		State headState = dataManager_->getState(headStateId);
		if (headState.getId() == 0 || !headState.isActive()) {
			throw std::runtime_error("Head state not found or inactive");
		}

		// Create new chain with the same key
		auto updatedChain = createStateChain(headState.getKey(), newData);

		// Add each state entity to the system
		for (auto &state: updatedChain) {
			dataManager_->addStateEntity(state);
		}

		return updatedChain;
	}

	void StateChainManager::deleteStateChain(int64_t headStateId) const {
		auto chainIds = getStateChainIds(headStateId);

		// Delete each state in the chain
		for (auto stateId: chainIds) {
			State state = dataManager_->getState(stateId);
			if (state.getId() != 0 && state.isActive()) {
				dataManager_->deleteState(state);
			}
		}
	}

	std::vector<int64_t> StateChainManager::getStateChainIds(int64_t headStateId) const {
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
