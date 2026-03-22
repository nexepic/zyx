/**
 * @file BaseEntityManager.hpp
 * @author Nexepic
 * @date 2025/7/25
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

#include <algorithm>
#include <memory>
#include <utility>
#include "DataManager.hpp"
#include "EntityManagerInterface.hpp"
#include "graph/storage/DeletionManager.hpp"

namespace graph::storage {

	// Forward declarations instead of includes
	class DataManager;
	class PropertyManager;

	/**
	 * Base template for entity managers that leverages EntityTraits
	 * Implements common operations for all entity types
	 */
	template<typename EntityType>
	class BaseEntityManager : public EntityManagerInterface<EntityType> {
	protected:
		DataManager* dataManager_;
		std::shared_ptr<DeletionManager> deletionManager_;

	public:
		BaseEntityManager(DataManager* dataManager,
						  std::shared_ptr<DeletionManager> deletionManager) :
			dataManager_(dataManager), deletionManager_(std::move(deletionManager)) {}

		// Core CRUD operations - moved implementations to cpp file
		void add(EntityType &entity) override;
		virtual void addBatch(std::vector<EntityType> &entities);
		void update(const EntityType &entity) override;
		void remove(EntityType &entity) override;
		EntityType get(int64_t id) override;
		std::vector<EntityType> getBatch(const std::vector<int64_t> &ids) override;
		std::vector<EntityType> getInRange(int64_t startId, int64_t endId, size_t limit) override;
		void addToCache(const EntityType &entity) override;
		void clearCache() override;

		// Property operations for entities that support properties
		virtual std::unordered_map<std::string, PropertyValue> getProperties(int64_t entityId);
		virtual void addProperties(int64_t entityId, const std::unordered_map<std::string, PropertyValue> &properties);
		virtual void removeProperty(int64_t entityId, const std::string &key);

	protected:
		/**
		 * @brief Get DataManager pointer with invariant guarantee
		 * @return DataManager pointer (guaranteed non-null)
		 *
		 * @note The DataManager's lifetime is guaranteed by design:
		 *       - DataManager holds shared_ptr<BaseEntityManager>
		 *       - BaseEntityManager stores raw DataManager*
		 *       - DataManager destructor destroys BaseEntityManager first
		 *       - Therefore dataManager_ is always valid
		 */
		[[nodiscard]] DataManager* getDataManagerPtr() {
			return dataManager_;
		}

		[[nodiscard]] DataManager* getDataManagerPtr() const {
			return dataManager_;
		}

		// Abstract method for allocating an ID that can be overridden by subclasses
		virtual int64_t doAllocateId() = 0;

		// Virtual method for removal that can be overridden by subclasses
		virtual void doRemove(EntityType &entity) = 0;
	};

} // namespace graph::storage
