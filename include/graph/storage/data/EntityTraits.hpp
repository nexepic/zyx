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

#include "DataManager.hpp"
#include "graph/storage/CacheManager.hpp"
#include "graph/storage/SegmentIndexManager.hpp"

namespace graph::storage {

	template<typename EntityType>
	struct EntityTraits {
		// Will be specialized for each entity type
	};

	// Node specialization
	template<>
	struct EntityTraits<Node> {
		using CacheType = LRUCache<int64_t, Node>;
		using DirtyMapType = std::unordered_map<int64_t, DirtyEntityInfo<Node>>;

		static constexpr uint32_t typeId = Node::typeId;

		static Node get(DataManager *manager, int64_t id) { return manager->getEntityFromMemoryOrDisk<Node>(id); }

		static Node loadFromDisk(const DataManager *manager, int64_t id) { return manager->loadNodeFromDisk(id); }

		static void addToCache(const DataManager *manager, const Node &entity) {
			manager->getNodeCache().put(entity.getId(), entity);
		}

		static void removeFromCache(const DataManager *manager, int64_t id) { manager->getNodeCache().remove(id); }

		static CacheType &getCache(const DataManager *manager) { return manager->getNodeCache(); }

		static DirtyMapType &getDirtyMap(DataManager *manager) { return manager->getDirtyNodes(); }

		static const std::vector<SegmentIndexManager::SegmentIndex> &getSegmentIndex(const DataManager *manager) {
			return manager->getSegmentIndexManager()->getNodeSegmentIndex();
		}
	};

	// Edge specialization
	template<>
	struct EntityTraits<Edge> {
		using CacheType = LRUCache<int64_t, Edge>;
		using DirtyMapType = std::unordered_map<int64_t, DirtyEntityInfo<Edge>>;

		static constexpr uint32_t typeId = Edge::typeId;

		static Edge get(DataManager *manager, int64_t id) { return manager->getEntityFromMemoryOrDisk<Edge>(id); }

		static Edge loadFromDisk(const DataManager *manager, int64_t id) { return manager->loadEdgeFromDisk(id); }

		static void addToCache(const DataManager *manager, const Edge &entity) {
			manager->getEdgeCache().put(entity.getId(), entity);
		}

		static void removeFromCache(const DataManager *manager, int64_t id) { manager->getEdgeCache().remove(id); }

		static CacheType &getCache(const DataManager *manager) { return manager->getEdgeCache(); }

		static DirtyMapType &getDirtyMap(DataManager *manager) { return manager->getDirtyEdges(); }

		static const std::vector<SegmentIndexManager::SegmentIndex> &getSegmentIndex(const DataManager *manager) {
			return manager->getSegmentIndexManager()->getEdgeSegmentIndex();
		}
	};

	// Property specialization
	template<>
	struct EntityTraits<Property> {
		using CacheType = LRUCache<int64_t, Property>;
		using DirtyMapType = std::unordered_map<int64_t, DirtyEntityInfo<Property>>;

		static constexpr uint32_t typeId = Property::typeId;

		static Property get(DataManager *manager, int64_t id) {
			return manager->getEntityFromMemoryOrDisk<Property>(id);
		}

		static Property loadFromDisk(const DataManager *manager, int64_t id) {
			return manager->loadPropertyFromDisk(id);
		}

		static void addToCache(const DataManager *manager, const Property &entity) {
			manager->getPropertyCache().put(entity.getId(), entity);
		}

		static void removeFromCache(const DataManager *manager, int64_t id) { manager->getPropertyCache().remove(id); }

		static CacheType &getCache(const DataManager *manager) { return manager->getPropertyCache(); }

		static DirtyMapType &getDirtyMap(DataManager *manager) { return manager->getDirtyProperties(); }

		static const std::vector<SegmentIndexManager::SegmentIndex> &getSegmentIndex(const DataManager *manager) {
			return manager->getSegmentIndexManager()->getPropertySegmentIndex();
		}
	};

	// Blob specialization
	template<>
	struct EntityTraits<Blob> {
		using CacheType = LRUCache<int64_t, Blob>;
		using DirtyMapType = std::unordered_map<int64_t, DirtyEntityInfo<Blob>>;

		static constexpr uint32_t typeId = Blob::typeId;

		static Blob get(DataManager *manager, int64_t id) { return manager->getEntityFromMemoryOrDisk<Blob>(id); }

		static Blob loadFromDisk(const DataManager *manager, int64_t id) { return manager->loadBlobFromDisk(id); }

		static void addToCache(const DataManager *manager, const Blob &entity) {
			manager->getBlobCache().put(entity.getId(), entity);
		}

		static void removeFromCache(const DataManager *manager, int64_t id) { manager->getBlobCache().remove(id); }

		static CacheType &getCache(const DataManager *manager) { return manager->getBlobCache(); }

		static DirtyMapType &getDirtyMap(DataManager *manager) { return manager->getDirtyBlobs(); }

		static const std::vector<SegmentIndexManager::SegmentIndex> &getSegmentIndex(const DataManager *manager) {
			return manager->getSegmentIndexManager()->getBlobSegmentIndex();
		}
	};

	// IndexEntity specialization
	template<>
	struct EntityTraits<Index> {
		using CacheType = LRUCache<int64_t, Index>;
		using DirtyMapType = std::unordered_map<int64_t, DirtyEntityInfo<Index>>;

		static constexpr uint32_t typeId = Index::typeId;

		static Index get(DataManager *manager, int64_t id) { return manager->getEntityFromMemoryOrDisk<Index>(id); }

		static Index loadFromDisk(const DataManager *manager, int64_t id) { return manager->loadIndexFromDisk(id); }

		static void addToCache(const DataManager *manager, const Index &entity) {
			manager->getIndexCache().put(entity.getId(), entity);
		}

		static void removeFromCache(const DataManager *manager, int64_t id) { manager->getIndexCache().remove(id); }

		static CacheType &getCache(const DataManager *manager) { return manager->getIndexCache(); }

		static DirtyMapType &getDirtyMap(DataManager *manager) { return manager->getDirtyIndexes(); }

		static const std::vector<SegmentIndexManager::SegmentIndex> &getSegmentIndex(const DataManager *manager) {
			return manager->getSegmentIndexManager()->getIndexSegmentIndex();
		}
	};

	// StateEntity specialization
	template<>
	struct EntityTraits<State> {
		using CacheType = LRUCache<int64_t, State>;
		using DirtyMapType = std::unordered_map<int64_t, DirtyEntityInfo<State>>;

		static constexpr uint32_t typeId = State::typeId;

		static State get(DataManager *manager, int64_t id) { return manager->getEntityFromMemoryOrDisk<State>(id); }

		static State loadFromDisk(const DataManager *manager, int64_t id) { return manager->loadStateFromDisk(id); }

		static void addToCache(const DataManager *manager, const State &entity) {
			manager->getStateCache().put(entity.getId(), entity);
		}

		static void removeFromCache(const DataManager *manager, int64_t id) { manager->getStateCache().remove(id); }

		static CacheType &getCache(const DataManager *manager) { return manager->getStateCache(); }

		static DirtyMapType &getDirtyMap(DataManager *manager) { return manager->getDirtyStates(); }

		static const std::vector<SegmentIndexManager::SegmentIndex> &getSegmentIndex(const DataManager *manager) {
			return manager->getSegmentIndexManager()->getStateSegmentIndex();
		}
	};

} // namespace graph::storage
