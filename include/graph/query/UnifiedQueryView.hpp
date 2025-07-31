/**
 * @file UnifiedQueryView.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/30
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <functional>
#include <memory>
#include <vector>
#include "graph/storage/data/DataManager.hpp"

namespace graph::query {

	/**
	 * @class UnifiedQueryView
	 * @brief A helper class to merge query results from persistent indexes with in-memory dirty entities.
	 *
	 * This class provides a unified view for queries by taking an initial set of entity IDs
	 * (typically from an index) and augmenting it with changes from the DataManager's dirty cache.
	 * It correctly handles entities that are ADDED, MODIFIED, or DELETED but not yet saved to disk.
	 */
	class UnifiedQueryView {
	public:
		/**
		 * @brief Constructs a UnifiedQueryView.
		 * @param dataManager A shared pointer to the DataManager to access dirty entity collections.
		 */
		explicit UnifiedQueryView(std::shared_ptr<storage::DataManager> dataManager);

		/**
		 * @brief Merges a list of entity IDs with dirty entities based on a predicate.
		 *
		 * This is the core method that implements the read-your-writes capability. It modifies the
		 * initial list of IDs to reflect the most current state of the data.
		 *
		 * @tparam EntityType The type of entity (e.g., Node, Edge).
		 * @param initialIds A vector of entity IDs from a persistent source (e.g., index). This vector will be modified in place.
		 * @param predicate A function that takes an entity and returns true if it matches the query criteria.
		 */
		template<typename EntityType>
		void mergeDirtyEntities(std::vector<int64_t> &initialIds,
								const std::function<bool(const EntityType &)> &predicate) const;

	private:
		std::shared_ptr<storage::DataManager> dataManager_;
	};

} // namespace graph::query