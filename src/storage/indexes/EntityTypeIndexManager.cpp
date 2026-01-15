/**
 * @file EntityTypeIndexManager.cpp
 * @author Nexepic
 * @date 2025/8/4
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

#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/log/Log.hpp"

namespace graph::query::indexes {

	EntityTypeIndexManager::EntityTypeIndexManager(
			std::shared_ptr<storage::DataManager> dataManager,
			const std::shared_ptr<storage::state::SystemStateManager>& systemStateManager, uint32_t labelIndexType,
			const std::string &labelStateKey, uint32_t propertyIndexType, const std::string &propertyStateKeyPrefix) :
		dataManager_(std::move(dataManager)) {
		labelIndex_ = std::make_shared<LabelIndex>(dataManager_, systemStateManager, labelIndexType, labelStateKey);
		propertyIndex_ = std::make_shared<PropertyIndex>(dataManager_, systemStateManager, propertyIndexType,
														 propertyStateKeyPrefix);
	}

	std::shared_ptr<LabelIndex> EntityTypeIndexManager::getLabelIndex() const { return labelIndex_; }

	std::shared_ptr<PropertyIndex> EntityTypeIndexManager::getPropertyIndex() const { return propertyIndex_; }

	bool EntityTypeIndexManager::createLabelIndex(const std::function<bool()> &buildFunc) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		labelIndex_->createIndex();
		return buildFunc();
	}

	bool EntityTypeIndexManager::createPropertyIndex(const std::string &key,
													 const std::function<bool()> &buildFunc) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (propertyIndex_->hasKeyIndexed(key)) {
			return false; // Already exists
		}
		propertyIndex_->createIndex(key);
		return buildFunc();
	}

	bool EntityTypeIndexManager::dropIndex(const std::string &indexType, const std::string &key) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (indexType == "label") {
			labelIndex_->drop();
			return true;
		}
		if (indexType == "property") {
			propertyIndex_->dropKey(key);
			return true;
		}
		return false;
	}

	void EntityTypeIndexManager::persistState() const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (labelIndex_)
			labelIndex_->flush();
		if (propertyIndex_)
			propertyIndex_->flush();
	}

	template<typename T>
	void EntityTypeIndexManager::updateLabelIndex(const T &entity, const std::string &oldLabel, bool isDeleted) const {
		// Only update the index if it has been built (i.e., not empty).
		if (labelIndex_->isEmpty() || entity.getId() == 0)
			return;

		const int64_t entityId = entity.getId();
		std::string newLabel;
		if (entity.getLabelId() != 0) {
			newLabel = dataManager_->resolveLabel(entity.getLabelId());
		}

		if (isDeleted) {
			if (!newLabel.empty()) labelIndex_->removeNode(entityId, newLabel);
		} else {
			// Handle changes
			if (!oldLabel.empty() && oldLabel != newLabel) {
				labelIndex_->removeNode(entityId, oldLabel);
			}
			if (!newLabel.empty() && oldLabel != newLabel) {
				labelIndex_->addNode(entityId, newLabel);
			}
		}
	}

	void EntityTypeIndexManager::updatePropertyIndexes(
			int64_t entityId, const std::unordered_map<std::string, PropertyValue> &oldProps,
			const std::unordered_map<std::string, PropertyValue> &newProps) const {
		if (propertyIndex_->isEmpty())
			return;

		// OPTIMIZATION STRATEGY:
		// Instead of checking every property against the index (Iterate Props -> Check Index),
		// we should iterate the Index Keys and check if the Entity has them (Iterate Index -> Check Props).
		// This is better IF (Number of Indexes << Number of Entity Properties).
		// usually, a graph has few indexes compared to raw data.

		const auto &indexedKeys = propertyIndex_->getIndexedKeys(); // Need a lightweight getter for this

		if (indexedKeys.empty())
			return;

		std::vector<std::tuple<int64_t, std::string, PropertyValue>> addBatch;
		// Reserve assuming worst case: all indexed keys are present
		addBatch.reserve(indexedKeys.size());

		for (const auto &key: indexedKeys) {
			// Look up this indexed key in the entity's properties
			auto newIt = newProps.find(key);
			auto oldIt = oldProps.find(key);

			bool inNew = (newIt != newProps.end());
			bool inOld = (oldIt != oldProps.end());

			// 1. Handle Removals
			if (inOld) {
				// If removed in new, or value changed
				if (!inNew || newIt->second != oldIt->second) {
					propertyIndex_->removeProperty(entityId, key, oldIt->second);
				}
			}

			// 2. Handle Additions
			if (inNew) {
				// If new (not in old), or value changed
				if (!inOld || newIt->second != oldIt->second) {
					// Batch the addition
					addBatch.emplace_back(entityId, key, newIt->second);
				}
			}
		}

		if (!addBatch.empty()) {
			propertyIndex_->addPropertiesBatch(addBatch);
		}
	}

	template<typename T>
	void EntityTypeIndexManager::onEntityAdded(const T &entity) const {
		// Update Label Index
		updateLabelIndex(entity, "", false);

		// Update Property Indexes (Batch optimized)
		updatePropertyIndexes(entity.getId(), {}, entity.getProperties());
	}

	template<typename T>
	void EntityTypeIndexManager::onEntitiesAdded(const std::vector<T> &entities) const {
		if (entities.empty())
			return;

		// Move lock scope: Only lock when actually invoking the index methods.
		// Preparing the data does not require the mutex if 'entities' is local.

		// --- 1. Prepare Label Index Batch ---
		// Optimization: Group by LabelID first to minimize string resolution lookups
		std::unordered_map<int64_t, std::vector<int64_t>> nodesByLabelId;

		// --- 2. Prepare Property Index Batch ---
		std::vector<std::tuple<int64_t, std::string, PropertyValue>> propBatch;
		// Tuning reserve: Assume avg 3 props per entity
		propBatch.reserve(entities.size() * 3);

		for (const auto &entity: entities) {
			// A. Label (Group by Integer ID first)
			if (entity.getLabelId() != 0) {
				nodesByLabelId[entity.getLabelId()].push_back(entity.getId());
			}

			// B. Properties (Unchanged logic)
			const auto &props = entity.getProperties();
			if (!props.empty()) {
				for (const auto &[key, value]: props) {
					// Optimization: Quick check if we should even bother
					if (propertyIndex_->hasKeyIndexed(key)) {
						propBatch.emplace_back(entity.getId(), key, value);
					}
				}
			}
		}

		// --- 3. Execute Batch Updates (NOW we lock) ---
		{
			std::lock_guard<std::recursive_mutex> lock(mutex_);

			// Process Label Index: Convert ID groups to String groups
			if (!nodesByLabelId.empty() && !labelIndex_->isEmpty()) {
				std::unordered_map<std::string, std::vector<int64_t>> nodesByLabelStr;

				for (auto& [labelId, ids] : nodesByLabelId) {
					// Resolve Label String ONCE per label group
					std::string labelStr = dataManager_->resolveLabel(labelId);
					if (!labelStr.empty()) {
						// Move the vector to avoid copying
						nodesByLabelStr[labelStr] = std::move(ids);
					}
				}

				if (!nodesByLabelStr.empty()) {
					labelIndex_->addNodesBatch(nodesByLabelStr);
				}
			}

			if (!propBatch.empty() && !propertyIndex_->isEmpty()) {
				propertyIndex_->addPropertiesBatch(propBatch);
			}

			persistState();
		}
	}

	template<typename T>
	void EntityTypeIndexManager::onEntityUpdated(const T &oldEntity, const T &newEntity) const {
		// Update Label Index
		std::string oldLabelStr;
		if (oldEntity.getLabelId() != 0) {
			oldLabelStr = dataManager_->resolveLabel(oldEntity.getLabelId());
		}

		updateLabelIndex(newEntity, oldLabelStr, false);

		// Update Property Indexes
		updatePropertyIndexes(newEntity.getId(), oldEntity.getProperties(), newEntity.getProperties());
	}

	template<typename T>
	void EntityTypeIndexManager::onEntityDeleted(const T &entity) const {
		// Update Label Index
		std::string labelStr;
		if (entity.getLabelId() != 0) {
			labelStr = dataManager_->resolveLabel(entity.getLabelId());
		}
		updateLabelIndex(entity, labelStr, true);

		// Update Property Indexes
		updatePropertyIndexes(entity.getId(), entity.getProperties(), {});
	}

	// Explicit template instantiations
	template void EntityTypeIndexManager::onEntityAdded<Node>(const Node &) const;
	template void EntityTypeIndexManager::onEntityUpdated<Node>(const Node &, const Node &) const;
	template void EntityTypeIndexManager::onEntityDeleted<Node>(const Node &) const;
	template void EntityTypeIndexManager::onEntityAdded<Edge>(const Edge &) const;
	template void EntityTypeIndexManager::onEntityUpdated<Edge>(const Edge &, const Edge &) const;
	template void EntityTypeIndexManager::onEntityDeleted<Edge>(const Edge &) const;

	template void EntityTypeIndexManager::onEntitiesAdded<Node>(const std::vector<Node> &) const;
	template void EntityTypeIndexManager::onEntitiesAdded<Edge>(const std::vector<Edge> &) const;

} // namespace graph::query::indexes
