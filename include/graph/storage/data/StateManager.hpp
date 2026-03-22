/**
 * @file StateManager.hpp
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

	class StateManager final : public BaseEntityManager<State> {
	public:
		StateManager(DataManager* dataManager,
					 std::shared_ptr<StateChainManager> stateChainManager,
					 std::shared_ptr<DeletionManager> deletionManager);

		void add(State &state) override;

		void update(const State &state) override;

		// State-specific methods
		State findByKey(const std::string &key);
		void addStateProperties(const std::string &stateKey,
								const std::unordered_map<std::string, PropertyValue> &properties,
								bool useBlobStorage = false);
		std::unordered_map<std::string, PropertyValue> getStateProperties(const std::string &stateKey);
		void removeState(const std::string &stateKey);

		// Chain management methods that delegate to StateChainManager
		[[nodiscard]] std::string readStateChain(int64_t headStateId) const;

		[[nodiscard]] std::vector<State> createStateChain(const std::string &key,
														  const std::string &data,
														  bool useBlobStorage = false) const;

		[[nodiscard]] std::vector<State> updateStateChain(int64_t headStateId,
														  const std::string &newData,
														  bool useBlobStorage = false) const;

		void deleteStateChain(int64_t headStateId) const;

	protected:
		int64_t doAllocateId() override;

		void doRemove(State &state) override;

	private:
		void populateKeyToIdMap();

		static bool isChainHeadState(const State &state);

		std::shared_ptr<StateChainManager> stateChainManager_;
		std::unordered_map<std::string, int64_t> stateKeyToIdMap_;
	};

} // namespace graph::storage
