/**
 * @file EntityTypeIndexManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/8/4
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/indexes/EntityTypeIndexManager.hpp"

#include "graph/log/Log.hpp"

namespace graph::query::indexes {

	EntityTypeIndexManager::EntityTypeIndexManager(
			std::shared_ptr<storage::DataManager> dataManager,
			std::shared_ptr<storage::state::SystemStateManager> systemStateManager, uint32_t labelIndexType,
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
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		// Only update the index if it has been built (i.e., not empty).
		if (labelIndex_->isEmpty() || entity.getId() == 0)
			return;

		const int64_t entityId = entity.getId();
		const std::string &newLabel = entity.getLabel();

		if (isDeleted) {
			// On deletion, remove the entity using its last known label.
			if (!newLabel.empty()) {
				labelIndex_->removeNode(entityId, newLabel);
			}
		} else {
			// Handle label changes for additions or updates.
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
		// No lock needed here yet, we are preparing data.
		// Locking happens inside PropertyIndex methods.

		if (propertyIndex_->isEmpty())
			return;

		// 1. Handle Removals (Old properties that changed or were deleted)
		for (const auto &[key, oldValue]: oldProps) {
			if (!propertyIndex_->hasKeyIndexed(key))
				continue;

			auto newIt = newProps.find(key);
			// If key is missing in new, or value changed, remove old index
			if (newIt == newProps.end() || newIt->second != oldValue) {
				propertyIndex_->removeProperty(entityId, key, oldValue);
			}
		}

		// 2. Handle Additions (New properties or changed values)
		// Optimization: Collect all additions into a batch vector
		// to minimize lock contention and overhead in PropertyIndex.
		std::vector<std::tuple<int64_t, std::string, PropertyValue>> addBatch;
		addBatch.reserve(newProps.size());

		for (const auto &[key, newValue]: newProps) {
			if (!propertyIndex_->hasKeyIndexed(key))
				continue;

			auto oldIt = oldProps.find(key);
			// If key is new, or value changed, add to index
			if (oldIt == oldProps.end() || oldIt->second != newValue) {
				addBatch.emplace_back(entityId, key, newValue);
			}
		}

		if (!addBatch.empty()) {
			// Use batch method even for single entity's properties
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
		std::unordered_map<std::string, std::vector<int64_t>> nodesByLabel;

		// --- 2. Prepare Property Index Batch ---
		std::vector<std::tuple<int64_t, std::string, PropertyValue>> propBatch;
		// Tuning reserve: Assume avg 3 props per entity
		propBatch.reserve(entities.size() * 3);

		for (const auto &entity: entities) {
			// A. Label
			if (!entity.getLabel().empty()) {
				nodesByLabel[entity.getLabel()].push_back(entity.getId());
			}

			// B. Properties
			const auto &props = entity.getProperties();
			if (!props.empty()) {
				// We can check 'hasKeyIndexed' here to filter early,
				// OR let PropertyIndex::addPropertiesBatch handle it.
				// Letting PropertyIndex handle it allows for auto-creation logic if desired.
				// Here we pass all, assuming PropertyIndex filters efficiently.
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

			if (!nodesByLabel.empty() && !labelIndex_->isEmpty()) {
				labelIndex_->addNodesBatch(nodesByLabel);
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
		updateLabelIndex(newEntity, oldEntity.getLabel(), false);

		// Update Property Indexes
		updatePropertyIndexes(newEntity.getId(), oldEntity.getProperties(), newEntity.getProperties());
	}

	template<typename T>
	void EntityTypeIndexManager::onEntityDeleted(const T &entity) const {
		// Update Label Index
		updateLabelIndex(entity, entity.getLabel(), true);

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
