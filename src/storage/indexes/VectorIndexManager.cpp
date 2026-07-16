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
#include <stdexcept>
#include <unordered_map>
#include "graph/log/Log.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexMeta.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"
#include "graph/storage/state/SystemStateManager.hpp"
#include "graph/vector/VectorIndexRegistry.hpp"
#include "graph/vector/index/DiskANNIndex.hpp"

namespace graph::query::indexes {
	namespace {
		std::vector<float> toVectorValues(const PropertyValue &value, const std::string &propertyName) {
			if (value.getType() != PropertyType::LIST)
				throw std::invalid_argument("Vector-indexed property '" + propertyName + "' must be a numeric list");

			const auto &propertyVector = value.getList();
			std::vector<float> vector;
			vector.reserve(propertyVector.size());
			for (const auto &element: propertyVector) {
				if (element.getType() == PropertyType::DOUBLE)
					vector.push_back(static_cast<float>(std::get<double>(element.getVariant())));
				else if (element.getType() == PropertyType::INTEGER)
					vector.push_back(static_cast<float>(std::get<int64_t>(element.getVariant())));
				else
					throw std::invalid_argument("Vector-indexed property '" + propertyName +
											"' contains a non-numeric element");
			}
			return vector;
		}
	} // namespace

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
			if (threadPool_)
				indexes_[name]->setThreadPool(threadPool_);
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
		if (threadPool_)
			indexes_[name]->setThreadPool(threadPool_);

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

		if (!indexMapLoaded_)
			loadIndexMap();

		if (label.empty())
			return;

		for (const auto &[key, val]: props) {
			const auto mapIt = indexMap_.find(label + ":" + key);
			if (mapIt == indexMap_.end())
				continue;
			getIndex(mapIt->second)->insert(node.getId(), toVectorValues(val, key));
		}
	}

	void VectorIndexManager::updateIndexDelta(const Node &oldNode, const Node &newNode, const std::string &label) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (!indexMapLoaded_)
			loadIndexMap();
		if (label.empty())
			return;

		const auto &oldProperties = oldNode.getProperties();
		const auto &newProperties = newNode.getProperties();
		const std::string prefix = label + ":";
		for (const auto &[mapKey, indexName]: indexMap_) {
			if (!mapKey.starts_with(prefix))
				continue;
			const std::string propertyName = mapKey.substr(prefix.size());
			const auto oldIt = oldProperties.find(propertyName);
			const auto newIt = newProperties.find(propertyName);
			if (oldIt != oldProperties.end() && newIt != newProperties.end() && oldIt->second == newIt->second)
				continue;

			auto index = getIndex(indexName);
			if (newIt == newProperties.end()) {
				index->remove(newNode.getId());
				continue;
			}
			index->insert(newNode.getId(), toVectorValues(newIt->second, propertyName));
		}
	}

	void VectorIndexManager::updateIndexBatch(
			const std::vector<std::pair<Node, std::unordered_map<std::string, PropertyValue>>> &nodesWithProps,
			const std::string &label) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		if (!indexMapLoaded_)
			loadIndexMap();

		if (label.empty() || nodesWithProps.empty())
			return;

		// Validate and group all vectors before mutating any index.
		std::unordered_map<std::string, std::vector<std::pair<int64_t, std::vector<float>>>> batchByIndex;

		for (const auto &[node, props]: nodesWithProps) {
			for (const auto &[key, val]: props) {
				std::string mapKey = label + ":" + key;
				auto it = indexMap_.find(mapKey);
				if (it == indexMap_.end())
					continue;

				batchByIndex[it->second].push_back({node.getId(), toVectorValues(val, key)});
			}
		}

		// Execute batch inserts per index
		for (auto &[indexName, batch]: batchByIndex) {
			getIndex(indexName)->batchInsert(batch);
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
				getIndex(indexName)->remove(nodeId);
			}
		}
	}

} // namespace graph::query::indexes
