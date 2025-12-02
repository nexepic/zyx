/**
 * @file EntityTraits.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/12
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <optional>
#include <vector>
#include "graph/storage/CacheManager.hpp"
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
		static auto &getCache(const DataManager *dm) { return dm->getNodeCache(); }
		static auto loadFromDisk(const DataManager *dm, int64_t id) { return dm->loadNodeFromDisk(id); }
		static const auto &getSegmentIndex(const DataManager *dm) {
			return dm->getSegmentIndexManager()->getNodeSegmentIndex();
		}
	};

	// --- Edge Specialization ---
	template<>
	struct DataManagerAccess<Edge> {
		static auto &getCache(const DataManager *dm) { return dm->getEdgeCache(); }
		static auto loadFromDisk(const DataManager *dm, int64_t id) { return dm->loadEdgeFromDisk(id); }
		static const auto &getSegmentIndex(const DataManager *dm) {
			return dm->getSegmentIndexManager()->getEdgeSegmentIndex();
		}
	};

	// --- Property Specialization ---
	template<>
	struct DataManagerAccess<Property> {
		static auto &getCache(const DataManager *dm) { return dm->getPropertyCache(); }
		static auto loadFromDisk(const DataManager *dm, int64_t id) { return dm->loadPropertyFromDisk(id); }
		static const auto &getSegmentIndex(const DataManager *dm) {
			return dm->getSegmentIndexManager()->getPropertySegmentIndex();
		}
	};

	// --- Blob Specialization ---
	template<>
	struct DataManagerAccess<Blob> {
		static auto &getCache(const DataManager *dm) { return dm->getBlobCache(); }
		static auto loadFromDisk(const DataManager *dm, int64_t id) { return dm->loadBlobFromDisk(id); }
		static const auto &getSegmentIndex(const DataManager *dm) {
			return dm->getSegmentIndexManager()->getBlobSegmentIndex();
		}
	};

	// --- Index Specialization ---
	template<>
	struct DataManagerAccess<Index> {
		static auto &getCache(const DataManager *dm) { return dm->getIndexCache(); }
		static auto loadFromDisk(const DataManager *dm, int64_t id) { return dm->loadIndexFromDisk(id); }
		static const auto &getSegmentIndex(const DataManager *dm) {
			return dm->getSegmentIndexManager()->getIndexSegmentIndex();
		}
	};

	// --- State Specialization ---
	template<>
	struct DataManagerAccess<State> {
		static auto &getCache(const DataManager *dm) { return dm->getStateCache(); }
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
		using CacheType = LRUCache<int64_t, EntityType>;
		using Access = DataManagerAccess<EntityType>;

		static constexpr uint32_t typeId = EntityType::typeId;

		/* --- Core Retrieval --- */

		static EntityType get(DataManager *manager, int64_t id) {
			// This method is generic in DataManager, so no adapter needed
			return manager->getEntityFromMemoryOrDisk<EntityType>(id);
		}

		static EntityType loadFromDisk(const DataManager *manager, int64_t id) {
			// Call specific method via Adapter
			return Access::loadFromDisk(manager, id);
		}

		/* --- Cache Management --- */

		static void addToCache(const DataManager *manager, const EntityType &entity) {
			Access::getCache(const_cast<DataManager *>(manager)).put(entity.getId(), entity);
		}

		static void removeFromCache(const DataManager *manager, int64_t id) {
			Access::getCache(const_cast<DataManager *>(manager)).remove(id);
		}

		static CacheType &getCache(const DataManager *manager) {
			return Access::getCache(const_cast<DataManager *>(manager));
		}

		/* --- Dirty Data Management --- */
		// These methods in DataManager are already generic templates, so no Adapter needed!

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
