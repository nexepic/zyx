/**
 * @file EntityTraits.hpp
 * @author Nexepic
 * @date 2025/6/12
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

#include <optional>
#include <vector>
#include "graph/storage/SegmentIndexManager.hpp"

namespace graph::storage {

	// Forward declaration
	class DataManager;

	// ==============================================================================
	// 1. DataManagerAccess Adapter
	//    This layer maps generic operations to specific method names in DataManager.
	// ==============================================================================

	template<typename EntityType>
	struct DataManagerAccess;

	// --- Node Specialization ---
	template<>
	struct DataManagerAccess<Node> {
		static auto loadFromDisk(const DataManager *dm, int64_t id) { return dm->loadNodeFromDisk(id); }
		static const auto &getSegmentIndex(const DataManager *dm) {
			return dm->getSegmentIndexManager()->getNodeSegmentIndex();
		}
	};

	// --- Edge Specialization ---
	template<>
	struct DataManagerAccess<Edge> {
		static auto loadFromDisk(const DataManager *dm, int64_t id) { return dm->loadEdgeFromDisk(id); }
		static const auto &getSegmentIndex(const DataManager *dm) {
			return dm->getSegmentIndexManager()->getEdgeSegmentIndex();
		}
	};

	// --- Property Specialization ---
	template<>
	struct DataManagerAccess<Property> {
		static auto loadFromDisk(const DataManager *dm, int64_t id) { return dm->loadPropertyFromDisk(id); }
		static const auto &getSegmentIndex(const DataManager *dm) {
			return dm->getSegmentIndexManager()->getPropertySegmentIndex();
		}
	};

	// --- Blob Specialization ---
	template<>
	struct DataManagerAccess<Blob> {
		static auto loadFromDisk(const DataManager *dm, int64_t id) { return dm->loadBlobFromDisk(id); }
		static const auto &getSegmentIndex(const DataManager *dm) {
			return dm->getSegmentIndexManager()->getBlobSegmentIndex();
		}
	};

	// --- Index Specialization ---
	template<>
	struct DataManagerAccess<Index> {
		static auto loadFromDisk(const DataManager *dm, int64_t id) { return dm->loadIndexFromDisk(id); }
		static const auto &getSegmentIndex(const DataManager *dm) {
			return dm->getSegmentIndexManager()->getIndexSegmentIndex();
		}
	};

	// --- State Specialization ---
	template<>
	struct DataManagerAccess<State> {
		static auto loadFromDisk(const DataManager *dm, int64_t id) { return dm->loadStateFromDisk(id); }
		static const auto &getSegmentIndex(const DataManager *dm) {
			return dm->getSegmentIndexManager()->getStateSegmentIndex();
		}
	};


	// ==============================================================================
	// 2. EntityTraits (Generic Template)
	//    Now purely generic, utilizing DataManagerAccess<T> for specific mapping.
	// ==============================================================================

	template<typename EntityType>
	struct EntityTraits {
		using Access = DataManagerAccess<EntityType>;

		static constexpr uint32_t typeId = EntityType::typeId;

		/* --- Core Retrieval --- */

		static EntityType get(DataManager *manager, int64_t id) {
			return manager->getEntityFromMemoryOrDisk<EntityType>(id);
		}

		static EntityType loadFromDisk(const DataManager *manager, int64_t id) {
			return Access::loadFromDisk(manager, id);
		}

		/* --- Cache Management (no-op with PageBufferPool) --- */

		static void addToCache(const DataManager * /*manager*/, const EntityType & /*entity*/) {
			// No-op: entities are in dirty registry after add/update,
			// and the page pool is populated on read misses.
		}

		static void removeFromCache(const DataManager * /*manager*/, int64_t /*id*/) {
			// No-op: dirty registry is checked before page pool in read path.
		}

		/* --- Dirty Data Management --- */

		static void addToDirty(DataManager *manager, const DirtyEntityInfo<EntityType> &info) {
			manager->setEntityDirty(info);
		}

		static std::optional<DirtyEntityInfo<EntityType>> getDirtyInfo(DataManager *manager, int64_t id) {
			return manager->getDirtyInfo<EntityType>(id);
		}

		/* --- Segment Index Access --- */

		static const std::vector<SegmentIndexManager::SegmentIndex> &getSegmentIndex(const DataManager *manager) {
			return Access::getSegmentIndex(manager);
		}
	};

} // namespace graph::storage
