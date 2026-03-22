/**
 * @file StateManager.cpp
 * @author Nexepic
 * @date 2025/7/24
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

#include "graph/storage/data/StateManager.hpp"
#include <ranges>
#include <sstream>
#include <utility>
#include "graph/core/StateChainManager.hpp"
#include "graph/storage/data/EntityTraits.hpp"
#include "graph/storage/data/PropertyManager.hpp"

namespace graph::storage {

	StateManager::StateManager(DataManager* dataManager,
							   std::shared_ptr<StateChainManager> stateChainManager,
							   std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager(dataManager, std::move(deletionManager)), stateChainManager_(std::move(stateChainManager)) {

		// Populate the key-to-ID map upon construction, ensuring the manager is
		// immediately ready to serve requests, even after a database restart.
		populateKeyToIdMap();
	}

	void StateManager::add(State &state) {
		BaseEntityManager::add(state);
		if (!state.getKey().empty()) {
			stateKeyToIdMap_[state.getKey()] = state.getId();
		}
	}

	void StateManager::update(const State &state) {
		// To properly update the key-to-ID map, we need the old key.
		// The most reliable way to get the old state is from the DataManager itself,
		// before the update is fully committed.
		State oldState = get(state.getId());

		const std::string &oldKey = oldState.getKey();
		const std::string &newKey = state.getKey();

		// If the key has changed, update our internal map.
		if (oldKey != newKey) {
			if (!oldKey.empty()) {
				stateKeyToIdMap_.erase(oldKey);
			}
			if (!newKey.empty()) {
				stateKeyToIdMap_[newKey] = state.getId();
			}
		}

		BaseEntityManager::update(state);
	}

	void StateManager::doRemove(State &state) {
		// Remove from key mapping if needed
		if (!state.getKey().empty()) {
			stateKeyToIdMap_.erase(state.getKey());
		}

		deletionManager_->deleteState(state);
	}

	void StateManager::populateKeyToIdMap() {
		// At initialization, the map is empty, so we perform a clean scan from disk.
		// We do not need to check in-memory states or use a 'processedIds' set.
		stateKeyToIdMap_.clear();

		// Get the segment index information for the State entity type.
		auto *dataManager = getDataManagerPtr();
		auto &segmentIndices = EntityTraits<State>::getSegmentIndex(dataManager);
		for (const auto &segmentIndex: segmentIndices) {
			// Fetch all states within this segment's range.
			auto states = dataManager->getEntitiesInRange<State>(
					segmentIndex.startId, segmentIndex.endId,
					// Ensure limit is large enough to get all entities in the segment
					segmentIndex.endId - segmentIndex.startId + 1);

			for (const auto &state: states) {
				// Use the existing helper to check if it's a head state.
				if (isChainHeadState(state)) {
					// If it is a head state and has a key, add it to our map.
					if (!state.getKey().empty()) {
						stateKeyToIdMap_[state.getKey()] = state.getId();
					}
				}
			}
		}
	}

	State StateManager::findByKey(const std::string &key) {
		// Check key-to-id map first (most efficient)
		// The map is carefully maintained to only contain valid, active states
		auto it = stateKeyToIdMap_.find(key);
		if (it != stateKeyToIdMap_.end()) {
			return get(it->second);
		}

		// Not found
		return {};
	}

	void StateManager::addStateProperties(const std::string &stateKey,
                                          const std::unordered_map<std::string, PropertyValue> &properties,
                                          bool useBlobStorage) {
        // Serialize the properties
        std::stringstream ss;
        PropertyManager::serializeProperties(ss, properties);
        std::string serializedData = ss.str();

        // Heuristic: If data is larger than ~4KB (16 state chunks), prefer Blob storage
        // This is a simple auto-tuning optimization.
        if (!useBlobStorage && serializedData.size() > 4096) {
            useBlobStorage = true;
        }

        // Find or create the state
        State state = findByKey(stateKey);

        if (state.getId() == 0) {
            // Create a new state chain
            auto stateChain = createStateChain(stateKey, serializedData, useBlobStorage);
        } else {
            // Update existing state chain
            // We pass the useBlobStorage preference.
            auto stateChain = updateStateChain(state.getId(), serializedData, useBlobStorage);
        }
    }

	std::unordered_map<std::string, PropertyValue> StateManager::getStateProperties(const std::string &stateKey) {
		State state = findByKey(stateKey);
		if (state.getId() == 0) {
			return {}; // No properties
		}

		// Read state data
		std::string data = readStateChain(state.getId());

		// Deserialize properties
		std::stringstream ss(data);
		return PropertyManager::deserializeProperties(ss);
	}

	void StateManager::removeState(const std::string &stateKey) {
		// Find the state by key
		State state = findByKey(stateKey);
		if (state.getId() == 0) {
			return; // State does not exist, nothing to remove
		}

		// Delete the entire state chain
		deleteStateChain(state.getId());

		// Remove the state from the key-to-id map
		stateKeyToIdMap_.erase(stateKey);
	}

	std::string StateManager::readStateChain(int64_t headStateId) const {
		// Delegate to StateChainManager
		return stateChainManager_->readStateChain(headStateId);
	}

	std::vector<State> StateManager::createStateChain(const std::string &key, const std::string &data, bool useBlobStorage) const {
		return stateChainManager_->createStateChain(key, data, useBlobStorage);
	}

	std::vector<State> StateManager::updateStateChain(int64_t headStateId, const std::string &newData, bool useBlobStorage) const {
		return stateChainManager_->updateStateChain(headStateId, newData, useBlobStorage);
	}

	void StateManager::deleteStateChain(int64_t headStateId) const {
		// Delegate to StateChainManager
		stateChainManager_->deleteStateChain(headStateId);
	}

	bool StateManager::isChainHeadState(const State &state) {
		return state.getId() != 0 && state.isActive() && state.getPrevStateId() == 0;
	}

	int64_t StateManager::doAllocateId() {
		return getDataManagerPtr()->getIdAllocator()->allocateId(State::typeId);
	}

} // namespace graph::storage
