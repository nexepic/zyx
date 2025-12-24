/**
 * @file LabelIndex.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>
#include "graph/core/IndexTreeManager.hpp"
#include "graph/storage/state/SystemStateManager.hpp"

namespace graph::storage {
	class DataManager;
}

namespace graph::query::indexes {

	/**
	 * Index for node labels using B+Tree structure
	 * Uses IndexTreeManager for B+Tree operations
	 */
	class LabelIndex {
	public:
		/**
		 * Constructor
		 *
		 * @param dataManager Pointer to data manager for persistence
		 */
		LabelIndex(std::shared_ptr<storage::DataManager> dataManager,
				   std::shared_ptr<storage::state::SystemStateManager> systemStateManager, uint32_t indexType,
				   std::string stateKey);
		~LabelIndex() = default;

		/**
		 * Adds a node to a label index
		 *
		 * @param nodeId ID of the node to add
		 * @param label Label to index the node under
		 */
		void addNode(int64_t nodeId, const std::string &label);

		/**
		 * Removes a node from a label index
		 *
		 * @param nodeId ID of the node to remove
		 * @param label Label to remove the node from
		 */
		void removeNode(int64_t nodeId, const std::string &label);

		/**
		 * Finds all nodes with a specific label
		 *
		 * @param label Label to search for
		 * @return Vector of node IDs with the label
		 */
		std::vector<int64_t> findNodes(const std::string &label) const;

		/**
		 * Checks if a node has a specific label
		 *
		 * @param entityId ID of the node to check
		 * @param label Label to check for
		 * @return true if the node has the label
		 */
		bool hasLabel(int64_t entityId, const std::string &label) const;

		/**
		 * Initializes the index
		 */
		void initialize();

		/**
		 * @brief Explicitly creates/enables the label index.
		 */
		void createIndex();

		bool isEmpty() const;

		/**
		 * Clears all index data
		 */
		void clear();

		void drop();

		/**
		 * Ensures persistence of index data
		 */
		void flush() const;

		void saveState() const;

		bool isEnabled() const {
			std::shared_lock lock(mutex_);
			return enabled_;
		}

		// Check if the physical B-Tree exists (Root ID is valid)
		bool hasPhysicalData() const {
			std::shared_lock lock(mutex_);
			return rootId_ != 0;
		}

	private:
		std::shared_ptr<storage::DataManager> dataManager_;
		std::shared_ptr<storage::state::SystemStateManager> systemStateManager_;
		std::shared_ptr<IndexTreeManager> treeManager_;
		mutable std::shared_mutex mutex_;
		int64_t rootId_ = 0;
		bool enabled_ = false;
		const std::string stateKey_;

		const std::string STATE_ENABLED_KEY;
	};

} // namespace graph::query::indexes
