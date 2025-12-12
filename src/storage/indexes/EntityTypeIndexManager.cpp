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

namespace graph::query::indexes {

	EntityTypeIndexManager::EntityTypeIndexManager(std::shared_ptr<storage::DataManager> dataManager,
												   uint32_t labelIndexType, const std::string& labelStateKey,
												   uint32_t propertyIndexType, const std::string& propertyStateKeyPrefix) :
		dataManager_(std::move(dataManager))
	{
		labelIndex_ = std::make_shared<LabelIndex>(dataManager_, labelIndexType, labelStateKey);
		propertyIndex_ = std::make_shared<PropertyIndex>(dataManager_, propertyIndexType, propertyStateKeyPrefix);
	}

	std::shared_ptr<LabelIndex> EntityTypeIndexManager::getLabelIndex() const {
		return labelIndex_;
	}

	std::shared_ptr<PropertyIndex> EntityTypeIndexManager::getPropertyIndex() const {
		return propertyIndex_;
	}

	bool EntityTypeIndexManager::buildLabelIndex(const std::function<bool()>& buildFunc) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		return buildFunc();
	}

	bool EntityTypeIndexManager::buildPropertyIndex(const std::string& key, const std::function<bool()>& buildFunc) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		return buildFunc();
	}

	bool EntityTypeIndexManager::buildAllIndexes(const std::function<bool()>& buildFunc) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		return buildFunc();
	}

	bool EntityTypeIndexManager::dropIndex(const std::string& indexType, const std::string& key) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (indexType == "label") {
			labelIndex_->drop();
			return true;
		}
		if (indexType == "property") {
			if (key.empty()) {
				// Drop all property indexes
				propertyIndex_->drop();
			} else {
				// Drop a specific property index
				propertyIndex_->dropKey(key);
			}
			return true;
		}
		return false;
	}

	std::vector<std::pair<std::string, std::string>> EntityTypeIndexManager::listIndexes() const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		std::vector<std::pair<std::string, std::string>> result;

		// An index "exists" if it's not empty (i.e., has persisted data).
		if (!labelIndex_->isEmpty()) {
			result.emplace_back("label", "");
		}

		auto indexedKeys = propertyIndex_->getIndexedKeys();
		for (const auto& key : indexedKeys) {
			result.emplace_back("property", key);
		}

		return result;
	}

	void EntityTypeIndexManager::persistState() const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (labelIndex_) labelIndex_->flush();
		if (propertyIndex_) propertyIndex_->flush();
	}

	template<typename T>
	void EntityTypeIndexManager::updateLabelIndex(const T& entity, const std::string& oldLabel, bool isDeleted) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		// Only update the index if it has been built (i.e., not empty).
		if (labelIndex_->isEmpty() || entity.getId() == 0) return;

		const int64_t entityId = entity.getId();
		const std::string& newLabel = entity.getLabel();

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

	void EntityTypeIndexManager::updatePropertyIndexes(int64_t entityId,
		const std::unordered_map<std::string, PropertyValue>& oldProps,
		const std::unordered_map<std::string, PropertyValue>& newProps) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		if (propertyIndex_->isEmpty()) return;

		// Iterate through old properties to detect removals or changes.
		for (const auto& [key, oldValue] : oldProps) {
			// Check if an index exists for this specific property key.
			if (propertyIndex_->hasKeyIndexed(key)) {
				auto newIt = newProps.find(key);
				// If the key is gone, or the value has changed, remove the old index entry.
				if (newIt == newProps.end() || newIt->second != oldValue) {
					propertyIndex_->removeProperty(entityId, key, oldValue);
				}
			}
		}

		// Iterate through new properties to detect additions or changes.
		for (const auto& [key, newValue] : newProps) {
			// Check if an index exists for this specific property key.
			if (propertyIndex_->hasKeyIndexed(key)) {
				auto oldIt = oldProps.find(key);
				// If the key is new, or the value has changed, add the new index entry.
				if (oldIt == oldProps.end() || oldIt->second != newValue) {
					propertyIndex_->addProperty(entityId, key, newValue);
				}
			}
		}
	}

	template<typename T>
	void EntityTypeIndexManager::onEntityAdded(const T& entity) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		updateLabelIndex(entity, "", false);
		updatePropertyIndexes(entity.getId(), {}, entity.getProperties());
		persistState();
	}

	template<typename T>
	void EntityTypeIndexManager::onEntityUpdated(const T& oldEntity, const T& newEntity) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		updateLabelIndex(newEntity, oldEntity.getLabel(), false);
		updatePropertyIndexes(newEntity.getId(), oldEntity.getProperties(), newEntity.getProperties());
		persistState();
	}

	template<typename T>
	void EntityTypeIndexManager::onEntityDeleted(const T& entity) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		updateLabelIndex(entity, entity.getLabel(), true);
		updatePropertyIndexes(entity.getId(), entity.getProperties(), {});
		persistState();
	}

	// Explicit template instantiations
	template void EntityTypeIndexManager::onEntityAdded<Node>(const Node&) const;
	template void EntityTypeIndexManager::onEntityUpdated<Node>(const Node&, const Node&) const;
	template void EntityTypeIndexManager::onEntityDeleted<Node>(const Node&) const;
	template void EntityTypeIndexManager::onEntityAdded<Edge>(const Edge&) const;
	template void EntityTypeIndexManager::onEntityUpdated<Edge>(const Edge&, const Edge&) const;
	template void EntityTypeIndexManager::onEntityDeleted<Edge>(const Edge&) const;

}