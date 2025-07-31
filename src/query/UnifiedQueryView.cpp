/**
 * @file UnifiedQueryView.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/30
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/UnifiedQueryView.hpp"
#include <unordered_set>

namespace graph::query {

	using namespace graph::storage;

	UnifiedQueryView::UnifiedQueryView(std::shared_ptr<DataManager> dataManager) :
		dataManager_(std::move(dataManager)) {}

	template<typename EntityType>
	void UnifiedQueryView::mergeDirtyEntities(std::vector<int64_t> &initialIds,
											  const std::function<bool(const EntityType &)> &predicate) const {
		if (!dataManager_) {
			return;
		}

		// Get all dirty entities with their full change info using the new method.
		// The return type is now std::vector<DirtyEntityInfo<EntityType>>.
		auto dirtyEntityInfos = dataManager_->getDirtyEntityInfos<EntityType>(
				{EntityChangeType::ADDED, EntityChangeType::MODIFIED, EntityChangeType::DELETED});

		// Use a hash set for efficient addition and removal of IDs.
		std::unordered_set idSet(initialIds.begin(), initialIds.end());

		// The loop variable 'entityInfo' is now of type DirtyEntityInfo<EntityType>,
		// which correctly has 'backup' and 'changeType' members.
		for (const auto &entityInfo: dirtyEntityInfos) {
			// The dirty map must contain a backup of the entity for the predicate to work.
			if (!entityInfo.backup.has_value()) {
				continue;
			}
			const auto &entity = *entityInfo.backup;
			const int64_t entityId = entity.getId();

			// Evaluate if the current state of the entity matches the query predicate.
			const bool matchesPredicate = predicate(entity);

			if (entityInfo.changeType == EntityChangeType::DELETED) {
				// If the entity was deleted, remove it from our result set.
				idSet.erase(entityId);
			} else if (matchesPredicate) {
				// If the entity is new (ADDED) or MODIFIED and it matches the predicate,
				// ensure it is included in the result set.
				idSet.insert(entityId);
			} else {
				// If the entity was MODIFIED and it no longer matches the predicate,
				// ensure it is excluded from the result set, even if the index found its old version.
				idSet.erase(entityId);
			}
		}

		// Update the original vector with the merged results.
		initialIds.assign(idSet.begin(), idSet.end());
	}

	// Explicitly instantiate the template for all entity types that can be queried.
	template void UnifiedQueryView::mergeDirtyEntities<Node>(std::vector<int64_t> &initialIds,
															 const std::function<bool(const Node &)> &predicate) const;

	template void UnifiedQueryView::mergeDirtyEntities<Edge>(std::vector<int64_t> &initialIds,
															 const std::function<bool(const Edge &)> &predicate) const;

} // namespace graph::query
