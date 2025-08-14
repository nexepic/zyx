/**
 * @file BaseEntityManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/8/14
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/data/BaseEntityManager.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/EntityTraits.hpp"
#include "graph/storage/data/PropertyManager.hpp"

namespace graph::storage {

	template<typename EntityType>
	void BaseEntityManager<EntityType>::add(EntityType &entity) {
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

	template<typename EntityType>
	void BaseEntityManager<EntityType>::update(const EntityType &entity) {
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

	template<typename EntityType>
	void BaseEntityManager<EntityType>::remove(EntityType &entity) {
		// Call the virtual method that can be overridden by subclasses
		doRemove(entity);

		// Mark that a deletion has been performed
		if (const auto dataManager = dataManager_.lock()) {
			dataManager->markDeletionPerformed();
		}
	}

	template<typename EntityType>
	EntityType BaseEntityManager<EntityType>::get(int64_t id) {
		const auto dataManager = dataManager_.lock();
		if (!dataManager)
			return EntityType();

		return EntityTraits<EntityType>::get(dataManager.get(), id);
	}

	template<typename EntityType>
	std::vector<EntityType> BaseEntityManager<EntityType>::getBatch(const std::vector<int64_t> &ids) {
		if (const auto dataManager = dataManager_.lock(); !dataManager)
			return {};

		std::vector<EntityType> result;
		result.reserve(ids.size());

		for (int64_t id: ids) {
			EntityType entity = get(id);
			if (entity.getId() != 0 && entity.isActive()) {
				result.push_back(entity);
			}
		}

		return result;
	}

	template<typename EntityType>
	std::vector<EntityType> BaseEntityManager<EntityType>::getInRange(int64_t startId, int64_t endId, size_t limit) {
		const auto dataManager = dataManager_.lock();
		if (!dataManager)
			return {};

		return dataManager->template getEntitiesInRange<EntityType>(startId, endId, limit);
	}

	template<typename EntityType>
	std::vector<EntityType>
	BaseEntityManager<EntityType>::getDirtyWithChangeTypes(const std::vector<EntityChangeType> &types) {
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

	template<typename EntityType>
	void BaseEntityManager<EntityType>::markAllSaved() {
		const auto dataManager = dataManager_.lock();
		if (!dataManager)
			return;

		EntityTraits<EntityType>::getDirtyMap(dataManager.get()).clear();
	}

	template<typename EntityType>
	void BaseEntityManager<EntityType>::addToCache(const EntityType &entity) {
		const auto dataManager = dataManager_.lock();
		if (!dataManager)
			return;

		EntityTraits<EntityType>::addToCache(dataManager.get(), entity);
	}

	template<typename EntityType>
	void BaseEntityManager<EntityType>::clearCache() {
		const auto dataManager = dataManager_.lock();
		if (!dataManager)
			return;

		auto &cache = EntityTraits<EntityType>::getCache(dataManager.get());
		cache.clear();
	}

	template<typename EntityType>
	std::unordered_map<std::string, PropertyValue> BaseEntityManager<EntityType>::getProperties(int64_t entityId) {
		const auto dataManager = dataManager_.lock();
		if (!dataManager)
			return {};

		auto propertyManager = dataManager->getPropertyManager();
		if (!propertyManager)
			return {};

		return propertyManager->template getEntityProperties<EntityType>(entityId);
	}

	template<typename EntityType>
	void
	BaseEntityManager<EntityType>::addProperties(int64_t entityId,
												 const std::unordered_map<std::string, PropertyValue> &properties) {
		const auto dataManager = dataManager_.lock();
		if (!dataManager)
			return;

		auto propertyManager = dataManager->getPropertyManager();
		if (!propertyManager)
			return;

		propertyManager->template addEntityProperties<EntityType>(entityId, properties);
	}

	template<typename EntityType>
	void BaseEntityManager<EntityType>::removeProperty(int64_t entityId, const std::string &key) {
		const auto dataManager = dataManager_.lock();
		if (!dataManager)
			return;

		auto propertyManager = dataManager->getPropertyManager();
		if (!propertyManager)
			return;

		propertyManager->template removeEntityProperty<EntityType>(entityId, key);
	}

	// Required template instantiations
	template class BaseEntityManager<Node>;
	template class BaseEntityManager<Edge>;
	template class BaseEntityManager<Property>;
	template class BaseEntityManager<Blob>;
	template class BaseEntityManager<Index>;
	template class BaseEntityManager<State>;

} // namespace graph::storage
