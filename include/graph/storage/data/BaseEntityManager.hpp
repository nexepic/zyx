/**
 * @file BaseEntityManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/25
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <algorithm>
#include <memory>
#include <utility>
#include "DataManager.hpp"
#include "EntityManagerInterface.hpp"
#include "EntityTraits.hpp"
#include "PropertyManager.hpp"
#include "graph/storage/DeletionManager.hpp"

namespace graph::storage {

	/**
	 * Base template for entity managers that leverages EntityTraits
	 * Implements common operations for all entity types
	 */
	template<typename EntityType>
	class BaseEntityManager : public EntityManagerInterface<EntityType> {
	protected:
		std::weak_ptr<DataManager> dataManager_;
		std::shared_ptr<PropertyManager> propertyManager_;
		std::shared_ptr<DeletionManager> deletionManager_;

	public:
		BaseEntityManager(const std::shared_ptr<DataManager> &dataManager,
						  std::shared_ptr<PropertyManager> propertyManager,
						  std::shared_ptr<DeletionManager> deletionManager) :
			dataManager_(dataManager), propertyManager_(std::move(propertyManager)),
			deletionManager_(std::move(deletionManager)) {}

		// Core CRUD operations
		void add(EntityType &entity) override {
			const auto dataManager = dataManager_.lock();
			if (!dataManager)
				return;

			if (entity.getId() == 0) {
				int64_t newId = doAllocateId();
				entity.setId(newId);
			}

			// Add to cache
			EntityTraits<EntityType>::addToCache(dataManager.get(), entity);

			// Mark as dirty
			auto &dirtyMap = EntityTraits<EntityType>::getDirtyMap(dataManager.get());
			dirtyMap[entity.getId()] = DirtyEntityInfo<EntityType>(EntityChangeType::ADDED, entity);

			// Check if auto-flush needed
			dataManager->checkAndTriggerAutoFlush();
		}

		void update(const EntityType &entity) override {
			const auto dataManager = dataManager_.lock();
			if (!dataManager)
				return;

			if (!entity.isActive()) {
				throw std::runtime_error("Cannot update inactive entity: " + std::to_string(entity.getId()));
			}

			// Add to cache
			EntityTraits<EntityType>::addToCache(dataManager.get(), entity);

			auto &dirtyMap = EntityTraits<EntityType>::getDirtyMap(dataManager.get());
			auto it = dirtyMap.find(entity.getId());

			// If already marked as ADDED, keep that state but update the backup
			if (it != dirtyMap.end() && it->second.changeType == EntityChangeType::ADDED) {
				dirtyMap[entity.getId()] = DirtyEntityInfo<EntityType>(EntityChangeType::ADDED, entity);
				return;
			}

			// Otherwise mark as modified
			dirtyMap[entity.getId()] = DirtyEntityInfo<EntityType>(EntityChangeType::MODIFIED, entity);
			dataManager->checkAndTriggerAutoFlush();
		}

		void remove(EntityType &entity) override {
			// Call the virtual method that can be overridden by subclasses
			doRemove(entity);

			// Mark that a deletion has been performed
			if (const auto dataManager = dataManager_.lock()) {
				dataManager->markDeletionPerformed();
			}
		}

		EntityType get(int64_t id) override {
			const auto dataManager = dataManager_.lock();
			if (!dataManager)
				return EntityType();

			return EntityTraits<EntityType>::get(dataManager.get(), id);
		}

		std::vector<EntityType> getBatch(const std::vector<int64_t> &ids) override {
			if (const auto dataManager = dataManager_.lock(); !dataManager)
				return {};

			std::vector<EntityType> result;
			result.reserve(ids.size());

			// TODO: Performance issue? Get batch data from disk?
			for (int64_t id: ids) {
				EntityType entity = get(id);
				if (entity.getId() != 0 && entity.isActive()) {
					result.push_back(entity);
				}
			}

			return result;
		}

		std::vector<EntityType> getInRange(int64_t startId, int64_t endId, size_t limit) override {
			const auto dataManager = dataManager_.lock();
			if (!dataManager)
				return {};

			return dataManager->template getEntitiesInRange<EntityType>(startId, endId, limit);
		}

		std::vector<EntityType> getDirtyWithChangeTypes(const std::vector<EntityChangeType> &types) override {
			const auto dataManager = dataManager_.lock();
			if (!dataManager)
				return {};

			auto &dirtyMap = EntityTraits<EntityType>::getDirtyMap(dataManager.get());
			std::vector<EntityType> result;

			for (const auto &[id, info]: dirtyMap) {
				if (std::find(types.begin(), types.end(), info.changeType) != types.end()) {
					if (info.backup.has_value()) {
						result.push_back(*info.backup);
					} else {
						auto &cache = EntityTraits<EntityType>::getCache(dataManager.get());
						if (cache.contains(id)) {
							result.push_back(cache.peek(id));
						}
					}
				}
			}

			return result;
		}

		void markAllSaved() override {
			const auto dataManager = dataManager_.lock();
			if (!dataManager)
				return;

			EntityTraits<EntityType>::getDirtyMap(dataManager.get()).clear();
		}

		void addToCache(const EntityType &entity) override {
			const auto dataManager = dataManager_.lock();
			if (!dataManager)
				return;

			EntityTraits<EntityType>::addToCache(dataManager.get(), entity);
		}

		void clearCache() override {
			const auto dataManager = dataManager_.lock();
			if (!dataManager)
				return;

			auto &cache = EntityTraits<EntityType>::getCache(dataManager.get());
			cache.clear();
		}

		// Property operations for entities that support properties
		virtual std::unordered_map<std::string, PropertyValue> getProperties(int64_t entityId) {
			if (!propertyManager_)
				return {};
			return propertyManager_->getEntityProperties<EntityType>(entityId);
		}

		virtual void addProperties(int64_t entityId, const std::unordered_map<std::string, PropertyValue> &properties) {
			if (!propertyManager_)
				return;
			propertyManager_->addEntityProperties<EntityType>(entityId, properties);
		}

		virtual void removeProperty(int64_t entityId, const std::string &key) {
			if (!propertyManager_)
				return;
			propertyManager_->removeEntityProperty<EntityType>(entityId, key);
		}

	protected:
		// Abstract method for allocating an ID that can be overridden by subclasses
		virtual int64_t doAllocateId() = 0;

		// Virtual method for removal that can be overridden by subclasses
		virtual void doRemove(EntityType &entity) = 0;
	};

} // namespace graph::storage
