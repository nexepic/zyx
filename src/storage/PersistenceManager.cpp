/**
 * @file PersistenceManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/1
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/PersistenceManager.hpp"

namespace graph::storage {

	PersistenceManager::PersistenceManager() {
		nodeRegistry_ = std::make_unique<DirtyEntityRegistry<Node>>();
		edgeRegistry_ = std::make_unique<DirtyEntityRegistry<Edge>>();
		propertyRegistry_ = std::make_unique<DirtyEntityRegistry<Property>>();
		blobRegistry_ = std::make_unique<DirtyEntityRegistry<Blob>>();
		indexRegistry_ = std::make_unique<DirtyEntityRegistry<Index>>();
		stateRegistry_ = std::make_unique<DirtyEntityRegistry<State>>();
	}

	template<typename EntityType>
	void PersistenceManager::upsert(const DirtyEntityInfo<EntityType> &info) {
		if constexpr (std::is_same_v<EntityType, Node>)
			nodeRegistry_->upsert(info);
		else if constexpr (std::is_same_v<EntityType, Edge>)
			edgeRegistry_->upsert(info);
		else if constexpr (std::is_same_v<EntityType, Property>)
			propertyRegistry_->upsert(info);
		else if constexpr (std::is_same_v<EntityType, Blob>)
			blobRegistry_->upsert(info);
		else if constexpr (std::is_same_v<EntityType, Index>)
			indexRegistry_->upsert(info);
		else if constexpr (std::is_same_v<EntityType, State>)
			stateRegistry_->upsert(info);

		checkAndTriggerAutoFlush();
	}

	template<typename EntityType>
	void PersistenceManager::remove(int64_t id) {
		// Assuming DirtyEntityRegistry has a 'remove(id)' or 'erase(id)' method.
		// If your DirtyEntityRegistry wraps a map, this should just call map.erase(id).
		if constexpr (std::is_same_v<EntityType, Node>)
			nodeRegistry_->remove(id);
		else if constexpr (std::is_same_v<EntityType, Edge>)
			edgeRegistry_->remove(id);
		else if constexpr (std::is_same_v<EntityType, Property>)
			propertyRegistry_->remove(id);
		else if constexpr (std::is_same_v<EntityType, Blob>)
			blobRegistry_->remove(id);
		else if constexpr (std::is_same_v<EntityType, Index>)
			indexRegistry_->remove(id);
		else if constexpr (std::is_same_v<EntityType, State>)
			stateRegistry_->remove(id);
	}

	template<typename EntityType>
	std::optional<DirtyEntityInfo<EntityType>> PersistenceManager::getDirtyInfo(int64_t id) const {
		if constexpr (std::is_same_v<EntityType, Node>)
			return nodeRegistry_->getInfo(id);
		else if constexpr (std::is_same_v<EntityType, Edge>)
			return edgeRegistry_->getInfo(id);
		else if constexpr (std::is_same_v<EntityType, Property>)
			return propertyRegistry_->getInfo(id);
		else if constexpr (std::is_same_v<EntityType, Blob>)
			return blobRegistry_->getInfo(id);
		else if constexpr (std::is_same_v<EntityType, Index>)
			return indexRegistry_->getInfo(id);
		else if constexpr (std::is_same_v<EntityType, State>)
			return stateRegistry_->getInfo(id);
		return std::nullopt;
	}

	template<typename EntityType>
	std::vector<DirtyEntityInfo<EntityType>> PersistenceManager::getAllDirtyInfos() const {
		if constexpr (std::is_same_v<EntityType, Node>)
			return nodeRegistry_->getAllDirtyInfos();
		else if constexpr (std::is_same_v<EntityType, Edge>)
			return edgeRegistry_->getAllDirtyInfos();
		else if constexpr (std::is_same_v<EntityType, Property>)
			return propertyRegistry_->getAllDirtyInfos();
		else if constexpr (std::is_same_v<EntityType, Blob>)
			return blobRegistry_->getAllDirtyInfos();
		else if constexpr (std::is_same_v<EntityType, Index>)
			return indexRegistry_->getAllDirtyInfos();
		else if constexpr (std::is_same_v<EntityType, State>)
			return stateRegistry_->getAllDirtyInfos();
		return {};
	}

	template<typename EntityType>
	bool PersistenceManager::isDirty(int64_t id) const {
		if constexpr (std::is_same_v<EntityType, Node>)
			return nodeRegistry_->contains(id);
		else if constexpr (std::is_same_v<EntityType, Edge>)
			return edgeRegistry_->contains(id);
		else if constexpr (std::is_same_v<EntityType, Property>)
			return propertyRegistry_->contains(id);
		else if constexpr (std::is_same_v<EntityType, Blob>)
			return blobRegistry_->contains(id);
		else if constexpr (std::is_same_v<EntityType, Index>)
			return indexRegistry_->contains(id);
		else if constexpr (std::is_same_v<EntityType, State>)
			return stateRegistry_->contains(id);
		return false;
	}

	FlushSnapshot PersistenceManager::createSnapshot() const {
		return FlushSnapshot{nodeRegistry_->createFlushSnapshot(),	   edgeRegistry_->createFlushSnapshot(),
							 propertyRegistry_->createFlushSnapshot(), blobRegistry_->createFlushSnapshot(),
							 indexRegistry_->createFlushSnapshot(),	   stateRegistry_->createFlushSnapshot()};
	}

	void PersistenceManager::commitSnapshot() const {
		nodeRegistry_->commitFlush();
		edgeRegistry_->commitFlush();
		propertyRegistry_->commitFlush();
		blobRegistry_->commitFlush();
		indexRegistry_->commitFlush();
		stateRegistry_->commitFlush();
	}

	bool PersistenceManager::hasUnsavedChanges() const {
		return nodeRegistry_->size() > 0 || edgeRegistry_->size() > 0 || propertyRegistry_->size() > 0 ||
			   blobRegistry_->size() > 0 || indexRegistry_->size() > 0 || stateRegistry_->size() > 0;
	}

	void PersistenceManager::clearAll() const {
		nodeRegistry_->clear();
		edgeRegistry_->clear();
		propertyRegistry_->clear();
		blobRegistry_->clear();
		indexRegistry_->clear();
		stateRegistry_->clear();
	}

	void PersistenceManager::checkAndTriggerAutoFlush() const {
		if (!autoFlushCallback_)
			return;

		// Sum counts from ALL registries to ensure memory safety.
		// If we ignore Blobs or Indexes, a bulk insert of those types could cause OOM.
		size_t total = nodeRegistry_->size() + edgeRegistry_->size() + propertyRegistry_->size() +
					   blobRegistry_->size() + indexRegistry_->size() + stateRegistry_->size();

		if (total >= maxDirtyEntities_) {
			autoFlushCallback_();
		}
	}

	// Explicit instantiations to ensure linking
	template void PersistenceManager::upsert<Node>(const DirtyEntityInfo<Node> &);
	template void PersistenceManager::upsert<Edge>(const DirtyEntityInfo<Edge> &);
	template void PersistenceManager::upsert<Property>(const DirtyEntityInfo<Property> &);
	template void PersistenceManager::upsert<Blob>(const DirtyEntityInfo<Blob> &);
	template void PersistenceManager::upsert<Index>(const DirtyEntityInfo<Index> &);
	template void PersistenceManager::upsert<State>(const DirtyEntityInfo<State> &);

	template void PersistenceManager::remove<Node>(int64_t);
	template void PersistenceManager::remove<Edge>(int64_t);
	template void PersistenceManager::remove<Property>(int64_t);
	template void PersistenceManager::remove<Blob>(int64_t);
	template void PersistenceManager::remove<Index>(int64_t);
	template void PersistenceManager::remove<State>(int64_t);

	template bool PersistenceManager::isDirty<Node>(int64_t) const;
	template bool PersistenceManager::isDirty<Edge>(int64_t) const;
	template bool PersistenceManager::isDirty<Property>(int64_t) const;
	template bool PersistenceManager::isDirty<Blob>(int64_t) const;
	template bool PersistenceManager::isDirty<Index>(int64_t) const;
	template bool PersistenceManager::isDirty<State>(int64_t) const;

	template std::optional<DirtyEntityInfo<Node>> PersistenceManager::getDirtyInfo<Node>(int64_t) const;
	template std::optional<DirtyEntityInfo<Edge>> PersistenceManager::getDirtyInfo<Edge>(int64_t) const;
	template std::optional<DirtyEntityInfo<Property>> PersistenceManager::getDirtyInfo<Property>(int64_t) const;
	template std::optional<DirtyEntityInfo<Blob>> PersistenceManager::getDirtyInfo<Blob>(int64_t) const;
	template std::optional<DirtyEntityInfo<Index>> PersistenceManager::getDirtyInfo<Index>(int64_t) const;
	template std::optional<DirtyEntityInfo<State>> PersistenceManager::getDirtyInfo<State>(int64_t) const;

	template std::vector<DirtyEntityInfo<Node>> PersistenceManager::getAllDirtyInfos<Node>() const;
	template std::vector<DirtyEntityInfo<Edge>> PersistenceManager::getAllDirtyInfos<Edge>() const;
	template std::vector<DirtyEntityInfo<Property>> PersistenceManager::getAllDirtyInfos<Property>() const;
	template std::vector<DirtyEntityInfo<Blob>> PersistenceManager::getAllDirtyInfos<Blob>() const;
	template std::vector<DirtyEntityInfo<Index>> PersistenceManager::getAllDirtyInfos<Index>() const;
	template std::vector<DirtyEntityInfo<State>> PersistenceManager::getAllDirtyInfos<State>() const;
} // namespace graph::storage
