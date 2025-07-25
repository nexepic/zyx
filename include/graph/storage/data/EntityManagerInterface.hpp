/**
 * @file EntityManagerInterface.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <optional>
#include <vector>
#include "EntityChangeType.hpp"
#include "graph/core/Types.hpp"

namespace graph::storage {

	/**
	 * Base interface for all entity managers
	 * @tparam EntityType The type of entity this manager handles
	 */
	template<typename EntityType>
	class EntityManagerInterface {
	public:
		virtual ~EntityManagerInterface() = default;

		// Core CRUD operations
		virtual void add(const EntityType &entity) = 0;
		virtual void update(const EntityType &entity) = 0;
		virtual void remove(EntityType &entity) = 0;
		virtual EntityType get(int64_t id) = 0;

		// Batch operations
		virtual std::vector<EntityType> getBatch(const std::vector<int64_t> &ids) = 0;
		virtual std::vector<EntityType> getInRange(int64_t startId, int64_t endId, size_t limit) = 0;

		// ID management
		virtual int64_t reserveTemporaryId() = 0;

		// Dirty entity management
		virtual std::vector<EntityType> getDirtyWithChangeTypes(const std::vector<EntityChangeType> &types) = 0;
		virtual void markAllSaved() = 0;

		// Cache management
		virtual void addToCache(const EntityType &entity) = 0;
		virtual void clearCache() = 0;
	};

} // namespace graph::storage
