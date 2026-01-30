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
#include "graph/log/Log.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexMeta.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"
#include "graph/storage/state/SystemStateManager.hpp"
#include "graph/vector/VectorIndexRegistry.hpp"
#include "graph/vector/index/DiskANNIndex.hpp"

namespace graph::query::indexes {

	VectorIndexManager::VectorIndexManager(std::shared_ptr<storage::DataManager> dm,
										   std::shared_ptr<storage::state::SystemStateManager> sm) :
		dataManager_(std::move(dm)), stateManager_(std::move(sm)) {}

	VectorIndexManager::~VectorIndexManager() = default;

	std::shared_ptr<vector::DiskANNIndex> VectorIndexManager::getIndex(const std::string &name) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		if (indexes_.find(name) == indexes_.end()) {
			auto registry = std::make_shared<vector::VectorIndexRegistry>(dataManager_, stateManager_, name);
			auto cfg = registry->getConfig(); // Load from disk

			vector::DiskANNConfig daCfg;
			daCfg.dim = cfg.dimension;

			if (cfg.metricType == 1)
				daCfg.metric = "IP";
			else
				daCfg.metric = "L2";

			indexes_[name] = std::make_shared<vector::DiskANNIndex>(registry, daCfg);
		}
		return indexes_[name];
	}

	void VectorIndexManager::createIndex(const std::string &name, int dim, const std::string &metric) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		auto registry = std::make_shared<vector::VectorIndexRegistry>(dataManager_, stateManager_, name);
		vector::VectorIndexConfig cfg;
		cfg.dimension = dim;

		// Map string to int for storage
		if (metric == "IP" || metric == "Cosine")
			cfg.metricType = 1;
		else
			cfg.metricType = 0; // L2

		registry->updateConfig(cfg);

		vector::DiskANNConfig daCfg;
		daCfg.dim = dim;
		daCfg.metric = metric; // Set runtime config
		indexes_[name] = std::make_shared<vector::DiskANNIndex>(registry, daCfg);

		indexMapLoaded_ = false;
	}

	bool VectorIndexManager::hasIndex(const std::string &name) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
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

	void VectorIndexManager::loadIndexMap() {
		// Load all index definitions from SystemState
		auto allIndexes = stateManager_->getMap<std::string>(storage::state::keys::SYS_INDEXES);

		// Clear existing map to be safe if reloading
		indexMap_.clear();

		for (const auto &[name, rawMeta]: allIndexes) {
			IndexMetadata meta = IndexMetadata::fromString(name, rawMeta);

			// Filter for VECTOR indexes only
			std::string type = meta.indexType;

			if (type == "vector" || type == "VECTOR") {
				// Key format: "Label:Property"
				std::string key = meta.label + ":" + meta.property;
				indexMap_[key] = meta.name;
			}
		}

		indexMapLoaded_ = true;
	}

	void VectorIndexManager::updateIndex(const Node &node, const std::string &label,
										 const std::unordered_map<std::string, PropertyValue> &props) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Force reload for debugging
		// indexMapLoaded_ = false;
		if (!indexMapLoaded_) {
			loadIndexMap();
			if (!indexMap_.empty()) {
				log::Log::info("Loaded Index Map. Count: {}", indexMap_.size());
				for (const auto &[k, v]: indexMap_)
					log::Log::info("Map: {} -> {}", k, v);
			}
		}

		if (label.empty())
			return;

		for (const auto &[key, val]: props) {
			std::string mapKey = label + ":" + key;

			if (indexMap_.find(mapKey) != indexMap_.end()) {
				std::string indexName = indexMap_[mapKey];

				if (val.getType() == PropertyType::LIST) {
					try {
						const auto &vec = val.getList();
						auto index = getIndex(indexName);

						index->insert(node.getId(), vec);

						// DEBUG LOG
						// log::Log::info("Inserted node {} into vector index {}", node.getId(), indexName);
					} catch (const std::exception &e) {
						log::Log::error("Insert failed: {}", e.what());
					}
				} else {
					log::Log::warn("Type mismatch for {}: Expected LIST, got {}", key, (int) val.getType());
				}
			}
		}
	}

	void VectorIndexManager::removeIndex(int64_t nodeId, const std::string &label) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (!indexMapLoaded_)
			loadIndexMap();

		// Iterate all vector indexes. If this node belongs to one (based on label), remove it.
		// Optimization: IndexMap stores "Label:Prop" -> "IndexName".
		// We can find all indexes for this label.

		for (const auto &[key, indexName]: indexMap_) {
			if (key.starts_with(label + ":")) {
				try {
					auto index = getIndex(indexName);
					index->remove(nodeId);
					// log::Log::info("Removed Node {} from Vector Index {}", nodeId, indexName);
				} catch (...) {
				}
			}
		}
	}

} // namespace graph::query::indexes
