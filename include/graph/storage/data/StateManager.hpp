/**
 * @file StateManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include "graph/core/State.hpp"
#include "graph/storage/data/BaseEntityManager.hpp"

namespace graph {
	class StateChainManager; // Forward declaration
}

namespace graph::storage {

	class StateManager : public BaseEntityManager<State> {
	public:
		StateManager(const std::shared_ptr<DataManager> &dataManager,
					 std::shared_ptr<StateChainManager> stateChainManager,
					 std::shared_ptr<DeletionManager> deletionManager);

		void add(State &state) override;

		// State-specific methods
		State findByKey(const std::string &key);
		std::vector<State> getAllHeadStates();
		void addStateProperties(const std::string &stateKey,
								const std::unordered_map<std::string, PropertyValue> &properties);
		std::unordered_map<std::string, PropertyValue> getStateProperties(const std::string &stateKey);
		void removeState(const std::string &stateKey);

		// Chain management methods that delegate to StateChainManager
		[[nodiscard]] std::string readStateChain(int64_t headStateId) const;
		[[nodiscard]] std::vector<State> createStateChain(const std::string &key, const std::string &data) const;
		[[nodiscard]] std::vector<State> updateStateChain(int64_t headStateId, const std::string &newData) const;
		void deleteStateChain(int64_t headStateId) const;

		// Helper methods
		static bool isChainHeadState(const State &state);

	protected:
		int64_t doAllocateId() override;

		void doRemove(State &state) override;

	private:
		std::shared_ptr<StateChainManager> stateChainManager_;
		std::unordered_map<std::string, int64_t> stateKeyToIdMap_;
	};

} // namespace graph::storage
