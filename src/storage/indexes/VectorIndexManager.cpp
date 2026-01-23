/**
 * @file VectorIndexManager.cpp
 * @author Nexepic
 * @date 2026/1/22
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include "graph/storage/indexes/VectorIndexManager.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/state/SystemStateManager.hpp"
#include "graph/vector/VectorIndexRegistry.hpp"
#include "graph/vector/index/DiskANNIndex.hpp"

namespace graph::query::indexes {

	VectorIndexManager::VectorIndexManager(std::shared_ptr<storage::DataManager> dm,
										   std::shared_ptr<storage::state::SystemStateManager> sm) :
		dataManager_(std::move(dm)), stateManager_(std::move(sm)) {}

	VectorIndexManager::~VectorIndexManager() = default;

	std::shared_ptr<vector::DiskANNIndex> VectorIndexManager::getIndex(const std::string &name) {
		std::lock_guard<std::mutex> lock(mutex_);

		// Check cache first
		if (indexes_.find(name) == indexes_.end()) {
			// Not in memory, instantiate it.
			// VectorIndexRegistry handles loading state from disk.
			auto registry = std::make_shared<vector::VectorIndexRegistry>(dataManager_, stateManager_, name);

			// Load config from registry to initialize DiskANN params
			auto cfg = registry->getConfig();

			// Note: If cfg.dimension == 0, it means the index is new/uninitialized.
			// DiskANNIndex handles this gracefully (e.g. requires train() call).

			vector::DiskANNConfig daCfg;
			daCfg.dim = cfg.dimension;
			// Map other params if needed (maxDegree, beamWidth)

			indexes_[name] = std::make_shared<vector::DiskANNIndex>(registry, daCfg);
		}
		return indexes_[name];
	}

	void VectorIndexManager::createIndex(const std::string &name, int dim) {
		std::lock_guard<std::mutex> lock(mutex_);

		// Create registry to persist initial config
		auto registry = std::make_shared<vector::VectorIndexRegistry>(dataManager_, stateManager_, name);

		vector::VectorIndexConfig cfg;
		cfg.dimension = dim;
		registry->updateConfig(cfg);

		// Initialize in-memory instance
		vector::DiskANNConfig daCfg;
		daCfg.dim = dim;
		indexes_[name] = std::make_shared<vector::DiskANNIndex>(registry, daCfg);
	}

	bool VectorIndexManager::hasIndex(const std::string &name) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (indexes_.count(name))
			return true;

		// Check disk state via Registry without caching the instance
		try {
			// Create a temporary registry to read state
			auto registry = std::make_shared<vector::VectorIndexRegistry>(dataManager_, stateManager_, name);
			return registry->getConfig().dimension > 0;
		} catch (...) {
			return false;
		}
	}

} // namespace graph::query::indexes
