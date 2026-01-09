/**
 * @file EntityManagerInterface.hpp
 * @author Nexepic
 * @date 2025/7/24
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

#include <vector>

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
		virtual void add(EntityType &entity) = 0;
		virtual void update(const EntityType &entity) = 0;
		virtual void remove(EntityType &entity) = 0;
		virtual EntityType get(int64_t id) = 0;

		// Batch operations
		virtual std::vector<EntityType> getBatch(const std::vector<int64_t> &ids) = 0;
		virtual std::vector<EntityType> getInRange(int64_t startId, int64_t endId, size_t limit) = 0;

		// Cache management
		virtual void addToCache(const EntityType &entity) = 0;
		virtual void clearCache() = 0;
	};

} // namespace graph::storage
