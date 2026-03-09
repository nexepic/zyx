/**
 * @file BaseEntityManager.cpp
 * @author Nexepic
 * @date 2025/8/14
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

#include "graph/storage/data/BaseEntityManager.hpp"

#include "graph/core/EntityPropertyTraits.hpp"
#include "graph/log/Log.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/EntityTraits.hpp"
#include "graph/storage/data/PropertyManager.hpp"

namespace graph::storage {

	template<typename EntityType>
	void BaseEntityManager<EntityType>::add(EntityType &entity) {
		auto *dataManager = getDataManagerPtr();

		if (entity.getId() == 0) {
			int64_t newId = doAllocateId();
			entity.setId(newId);
		}

		EntityTraits<EntityType>::addToCache(dataManager, entity);

		// REFACTORED: Use addToDirty instead of accessing map
		EntityTraits<EntityType>::addToDirty(dataManager,
											 DirtyEntityInfo<EntityType>(EntityChangeType::CHANGE_ADDED, entity));
	}

	template<typename EntityType>
	void BaseEntityManager<EntityType>::addBatch(std::vector<EntityType> &entities) {
		if (entities.empty())
			return;

		auto *dataManager = getDataManagerPtr();

		// 1. Identify entities needing new IDs
		// We collect pointers to avoid copying large objects
		std::vector<EntityType *> newEntities;
		newEntities.reserve(entities.size());

		for (auto &entity: entities) {
			if (entity.getId() == 0) {
				newEntities.push_back(&entity);
			}
		}

		// 2. Batch ID Allocation (Critical Optimization)
		// One mutex lock for N entities.
		if (!newEntities.empty()) {
			int64_t startId = dataManager->getIdAllocator()->allocateIdBatch(EntityType::typeId, newEntities.size());

			// Assign sequential IDs
			for (size_t i = 0; i < newEntities.size(); ++i) {
				newEntities[i]->setId(startId + static_cast<int64_t>(i));
			}
		}

		// 3. Batch Cache Update
		// Efficiently add to LRU cache
		for (const auto &entity: entities) {
			EntityTraits<EntityType>::addToCache(dataManager, entity);
		}

		// 4. Batch Persistence (Dirty Map)
		// Updates dirty registry and triggers auto-flush check only once
		dataManager->getPersistenceManager()->upsertBatch(entities, EntityChangeType::CHANGE_ADDED);
	}

	template<typename EntityType>
	void BaseEntityManager<EntityType>::update(const EntityType &entity) {
		auto *dataManager = getDataManagerPtr();

		if (entity.getId() == 0) {
			return;
		}

		if (!entity.isActive())
			throw std::runtime_error("Update inactive entity");

		EntityTraits<EntityType>::addToCache(dataManager, entity);

		// REFACTORED: Check logic via Traits wrapper
		auto dirtyInfo = EntityTraits<EntityType>::getDirtyInfo(dataManager, entity.getId());

		if (dirtyInfo.has_value() && dirtyInfo->changeType == EntityChangeType::CHANGE_ADDED) {
			EntityTraits<EntityType>::addToDirty(dataManager,
												 DirtyEntityInfo<EntityType>(EntityChangeType::CHANGE_ADDED, entity));
		} else {
			EntityTraits<EntityType>::addToDirty(dataManager,
												 DirtyEntityInfo<EntityType>(EntityChangeType::CHANGE_MODIFIED, entity));
		}

		dataManager->checkAndTriggerAutoFlush();
	}

	template<typename EntityType>
	void BaseEntityManager<EntityType>::remove(EntityType &entity) {
		if (entity.getId() == 0 || !entity.isActive()) {
			return;
		}
		// Call the virtual method that can be overridden by subclasses
		doRemove(entity);
		log::Log::debug("Entity of type {} with ID {} marked for deletion.",
						std::to_string(EntityTraits<EntityType>::typeId), entity.getId());

		// Mark that a deletion has been performed
		getDataManagerPtr()->markDeletionPerformed();
	}

	template<typename EntityType>
	EntityType BaseEntityManager<EntityType>::get(int64_t id) {
		return EntityTraits<EntityType>::get(getDataManagerPtr(), id);
	}

	template<typename EntityType>
	std::vector<EntityType> BaseEntityManager<EntityType>::getBatch(const std::vector<int64_t> &ids) {
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
		return getDataManagerPtr()->template getEntitiesInRange<EntityType>(startId, endId, limit);
	}

	template<typename EntityType>
	void BaseEntityManager<EntityType>::addToCache(const EntityType &entity) {
		EntityTraits<EntityType>::addToCache(getDataManagerPtr(), entity);
	}

	template<typename EntityType>
	void BaseEntityManager<EntityType>::clearCache() {
		auto *dataManager = getDataManagerPtr();
		auto &cache = EntityTraits<EntityType>::getCache(dataManager);
		cache.clear();
	}

	template<typename EntityType>
	std::unordered_map<std::string, PropertyValue> BaseEntityManager<EntityType>::getProperties(int64_t entityId) {
		// Only process entity types that support properties
		if constexpr (!EntityPropertyTraits<EntityType>::supportsProperties) {
			return {};
		}

		auto *dataManager = getDataManagerPtr();
		auto propertyManager = dataManager->getPropertyManager();
		if (!propertyManager)
			return {};

		return propertyManager->template getEntityProperties<EntityType>(entityId);
	}

	template<typename EntityType>
	void
	BaseEntityManager<EntityType>::addProperties(int64_t entityId,
												 const std::unordered_map<std::string, PropertyValue> &properties) {
		// Only process entity types that support properties
		if constexpr (!EntityPropertyTraits<EntityType>::supportsProperties) {
			return;
		}

		auto *dataManager = getDataManagerPtr();
		auto propertyManager = dataManager->getPropertyManager();
		if (!propertyManager)
			return;

		propertyManager->template addEntityProperties<EntityType>(entityId, properties);
	}

	template<typename EntityType>
	void BaseEntityManager<EntityType>::removeProperty(int64_t entityId, const std::string &key) {
		// Only process entity types that support properties
		if constexpr (!EntityPropertyTraits<EntityType>::supportsProperties) {
			return;
		}

		auto *dataManager = getDataManagerPtr();
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
