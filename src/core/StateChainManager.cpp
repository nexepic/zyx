/**
 * @file StateChainManager.cpp
 * @author Nexepic
 * @date 2025/6/17
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/core/StateChainManager.hpp"
#include <stdexcept>
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph {

	StateChainManager::StateChainManager(std::shared_ptr<storage::DataManager> dataManager) :
		dataManager_(std::move(dataManager)) {}

	std::vector<State> StateChainManager::createStateChain(const std::string &key,
														   const std::string &data,
														   bool useBlobStorage) const {
		int64_t stateId = dataManager_->getIdAllocator(EntityType::State)->allocate();
		State headState(stateId, key, "");

		std::vector<State> chain;
		setupStorageForState(headState, data, useBlobStorage, chain);

		for (const auto& state : chain) {
			dataManager_->addStateEntity(const_cast<State&>(state));
		}

		return chain;
	}

	std::string StateChainManager::readStateChain(const int64_t headStateId) const {
		const State headState = dataManager_->getState(headStateId);
		if (headState.getId() == 0 || !headState.isActive()) {
			throw std::runtime_error("Head state not found or inactive");
		}

		if (headState.isBlobStorage()) {
			if (headState.getExternalId() == 0) return "";
			return dataManager_->getBlobManager()->readBlobChain(headState.getExternalId());
		}

		if (!headState.isChained()) {
			return headState.getDataAsString();
		}

		std::string reassembledData;
		std::vector<int64_t> visitedIds;
		int64_t currentStateId = headStateId;
		int32_t expectedPosition = 0;

		while (currentStateId != 0) {
			if (std::ranges::find(visitedIds, currentStateId) != visitedIds.end()) {
				throw std::runtime_error("Circular reference detected");
			}
			visitedIds.push_back(currentStateId);

			State currentState = dataManager_->getState(currentStateId);
			if (currentState.getId() == 0 || !currentState.isActive()) {
				throw std::runtime_error("State chain corrupted");
			}
			if (currentState.getChainPosition() != expectedPosition) {
				throw std::runtime_error("Chain position mismatch");
			}

			reassembledData.append(currentState.getDataAsString());
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
                                                           const std::string &newData,
                                                           bool useBlobStorage) const {
        // 1. Same data check
        if (isDataSame(headStateId, newData)) {
            // Even if data is same, if storage mode differs, we might want to convert?
            // For now, assuming isDataSame implies no change needed.
            // If strict mode switch is required, remove this check or enhance it.
            const auto chainIds = getStateChainIds(headStateId);
            std::vector<State> existingChain;
            for (auto stateId: chainIds) {
                State state = dataManager_->getState(stateId);
                existingChain.push_back(state);
            }
            return existingChain;
        }

        // 2. Fetch Head
        State headState = dataManager_->getState(headStateId);
        if (headState.getId() == 0 || !headState.isActive()) {
            throw std::runtime_error("Head state not found or inactive");
        }

        // 3. Clean up OLD storage
        // Critical Fix: Ensure we are reading the *current* state of storage
        if (headState.isBlobStorage()) {
            if (headState.getExternalId() != 0) {
                dataManager_->getBlobManager()->deleteBlobChain(headState.getExternalId());
            }
        } else {
            // Delete internal chain
            int64_t nextId = headState.getNextStateId();
            while (nextId != 0) {
                State nextState = dataManager_->getState(nextId);
                int64_t tempId = nextState.getNextStateId();
                dataManager_->deleteState(nextState);
                nextId = tempId;
            }
        }

        // 4. Setup NEW storage
        std::vector<State> newChain;
        setupStorageForState(headState, newData, useBlobStorage, newChain);

        // 5. Apply Updates
        // Update Head
        dataManager_->updateStateEntity(newChain[0]);

        // Add Tail states (if any)
        for (size_t i = 1; i < newChain.size(); i++) {
            dataManager_->addStateEntity(const_cast<State&>(newChain[i]));
        }

        return newChain;
    }

	void StateChainManager::deleteStateChain(const int64_t headStateId) const {
		const State headState = dataManager_->getState(headStateId);
		if (headState.getId() == 0 || !headState.isActive()) return;

		if (headState.isBlobStorage()) {
			if (headState.getExternalId() != 0) {
				dataManager_->getBlobManager()->deleteBlobChain(headState.getExternalId());
			}
			State mutableState = headState;
			dataManager_->deleteState(mutableState);
			return;
		}

		for (const auto chainIds = getStateChainIds(headStateId); auto stateId: chainIds) {
			State state = dataManager_->getState(stateId);
			dataManager_->deleteState(state);
		}
	}

	std::vector<int64_t> StateChainManager::getStateChainIds(const int64_t headStateId) const {
		std::vector<int64_t> chainIds;
		int64_t currentStateId = headStateId;

		State head = dataManager_->getState(headStateId);
		if (head.isBlobStorage()) {
			if (head.isActive()) chainIds.push_back(headStateId);
			return chainIds;
		}

		while (currentStateId != 0) {
			State currentState = dataManager_->getState(currentStateId);
			if (!currentState.isActive()) break;
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

		// Always return at least one chunk, even for empty data
		if (data.empty()) {
			chunks.push_back(std::string());
			return chunks;
		}

		for (size_t offset = 0; offset < data.size(); offset += State::CHUNK_SIZE) {
			size_t chunkSize = std::min(State::CHUNK_SIZE, data.size() - offset);
			chunks.push_back(data.substr(offset, chunkSize));
		}

		return chunks;
	}

	void StateChainManager::setupStorageForState(State &headState,
												 const std::string &data,
												 bool useBlobStorage,
												 std::vector<State> &outChainEntities) const {
		// Reset storage metadata
		headState.setExternalId(0);
		headState.setNextStateId(0);
		headState.setPrevStateId(0);
		headState.setChainPosition(0);
		headState.setData("");

		outChainEntities.push_back(headState);

		if (useBlobStorage) {
			// --- Blob Mode ---
			auto blobChain = dataManager_->getBlobManager()->createBlobChain(
				headState.getId(), State::typeId, data
			);

			if (!blobChain.empty()) {
				headState.setExternalId(blobChain[0].getId());
			}
			outChainEntities[0] = headState;
		} else {
			// --- Internal Mode ---
			auto chunks = splitData(data);
			headState.setData(chunks[0]);
			outChainEntities[0] = headState;

			int64_t prevId = headState.getId();

			for (size_t i = 1; i < chunks.size(); i++) {
				int64_t newId = dataManager_->getIdAllocator(EntityType::State)->allocate();
				State chunkState(newId, "", chunks[i]);

				chunkState.setChainPosition(static_cast<int32_t>(i));
				chunkState.setPrevStateId(prevId);
				chunkState.setExternalId(0);

				outChainEntities.push_back(chunkState);

				if (i == 1) {
					outChainEntities[0].setNextStateId(newId);
				} else {
					outChainEntities[i-1].setNextStateId(newId);
				}
				prevId = newId;
			}
		}
	}

} // namespace graph
