/**
 * @file StateManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/data/StateManager.hpp"
#include <graph/core/StateRegistry.hpp>
#include <ranges>
#include <sstream>
#include <utility>
#include "graph/core/StateChainManager.hpp"

namespace graph::storage {

	StateManager::StateManager(const std::shared_ptr<DataManager> &dataManager,
							   std::shared_ptr<PropertyManager> propertyManager,
							   std::shared_ptr<StateChainManager> stateChainManager,
							   std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager(dataManager, std::move(propertyManager), std::move(deletionManager)),
		stateChainManager_(std::move(stateChainManager)) {}

	void StateManager::add(State &state) {
		BaseEntityManager::add(state);
		if (!state.getKey().empty()) {
			stateKeyToIdMap_[state.getKey()] = state.getId();
			// Update StateRegistry cache if initialized
			StateRegistry::setString(state.getKey(), state.getDataAsString());
		}
	}

	void StateManager::doRemove(State &state) {
		// Remove from key mapping if needed
		if (!state.getKey().empty()) {
			stateKeyToIdMap_.erase(state.getKey());
			StateRegistry::remove(state.getKey());
		}

		deletionManager_->deleteState(state);
	}

	State StateManager::findByKey(const std::string &key) {
		// Check key-to-id map first (most efficient)
		auto it = stateKeyToIdMap_.find(key);
		if (it != stateKeyToIdMap_.end()) {
			State state = get(it->second);
			if (state.getId() != 0 && state.isActive()) {
				return state;
			}
		}

		// Not found
		return {};
	}

	std::vector<State> StateManager::getAllHeadStates() {
		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return {};

		std::vector<State> allHeadStates;
		std::unordered_set<int64_t> processedIds;

		// First collect all in-memory states
		for (const auto &stateId: std::views::values(stateKeyToIdMap_)) {
			State state = get(stateId);
			if (state.getId() != 0 && state.isActive() && isChainHeadState(state)) {
				allHeadStates.push_back(state);
				processedIds.insert(state.getId());
			}
		}

		// Then check all states from disk
		auto &segmentIndices = EntityTraits<State>::getSegmentIndex(dataManager.get());
		for (const auto &segmentIndex: segmentIndices) {
			auto states = dataManager->getEntitiesInRange<State>(segmentIndex.startId, segmentIndex.endId,
																 segmentIndex.endId - segmentIndex.startId + 1);

			for (const auto &state: states) {
				if (isChainHeadState(state) && !processedIds.contains(state.getId())) {
					allHeadStates.push_back(state);
					processedIds.insert(state.getId());

					// Cache for future use
					if (!state.getKey().empty()) {
						stateKeyToIdMap_[state.getKey()] = state.getId();
						addToCache(state);
					}
				}
			}
		}

		return allHeadStates;
	}

	void StateManager::addStateProperties(const std::string &stateKey,
										  const std::unordered_map<std::string, PropertyValue> &properties) {
		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return;

		// Serialize the properties
		std::stringstream ss;
		if (propertyManager_) {
			PropertyManager::serializeProperties(ss, properties);
		}
		std::string serializedData = ss.str();

		// Find or create the state
		State state = findByKey(stateKey);
		if (state.getId() == 0) {
			// Create a new state chain
			auto stateChain = createStateChain(stateKey, serializedData);
		} else {
			// Update existing state chain
			auto stateChain = updateStateChain(state.getId(), serializedData);
		}
	}

	std::unordered_map<std::string, PropertyValue> StateManager::getStateProperties(const std::string &stateKey) {
		auto dataManager = dataManager_.lock();
		if (!dataManager || !propertyManager_)
			return {};

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
		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return;

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

	std::vector<State> StateManager::createStateChain(const std::string &key, const std::string &data) const {
		// Delegate to StateChainManager
		return stateChainManager_->createStateChain(key, data);
	}

	std::vector<State> StateManager::updateStateChain(int64_t headStateId, const std::string &newData) const {
		// Delegate to StateChainManager
		return stateChainManager_->updateStateChain(headStateId, newData);
	}

	void StateManager::deleteStateChain(int64_t headStateId) const {
		// Delegate to StateChainManager
		stateChainManager_->deleteStateChain(headStateId);
	}

	bool StateManager::isChainHeadState(const State &state) {
		return state.getId() != 0 && state.isActive() && state.getPrevStateId() == 0;
	}

	int64_t StateManager::doAllocateId() {
		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			throw std::runtime_error("DataManager is not available for ID allocation.");
		}
		return dataManager->getIdAllocator()->allocateId(State::typeId);
	}

} // namespace graph::storage
