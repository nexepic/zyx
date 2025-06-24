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

#include <string>
#include <vector>
#include <memory>
#include <shared_mutex>
#include "graph/core/IndexTreeManager.hpp"

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
		// Constants for this index type
		static constexpr uint32_t LABEL_INDEX_TYPE = 1;

		// State key for persisting rootId
		static constexpr char STATE_KEY_ROOT_ID[] = "label_index.root_id";

		/**
		 * Constructor
		 *
		 * @param dataManager Pointer to data manager for persistence
		 */
		explicit LabelIndex(const std::shared_ptr<storage::DataManager>& dataManager);
		~LabelIndex() = default;

		/**
		 * Adds a node to a label index
		 *
		 * @param nodeId ID of the node to add
		 * @param label Label to index the node under
		 */
		void addNode(int64_t nodeId, const std::string& label);

		/**
		 * Removes a node from a label index
		 *
		 * @param nodeId ID of the node to remove
		 * @param label Label to remove the node from
		 */
		void removeNode(int64_t nodeId, const std::string& label);

		/**
		 * Finds all nodes with a specific label
		 *
		 * @param label Label to search for
		 * @return Vector of node IDs with the label
		 */
		std::vector<int64_t> findNodes(const std::string& label) const;

		/**
		 * Checks if a node has a specific label
		 *
		 * @param nodeId ID of the node to check
		 * @param label Label to check for
		 * @return true if the node has the label
		 */
		bool hasLabel(int64_t nodeId, const std::string& label) const;

		/**
		 * Initializes the index
		 */
		void initialize();

		bool isEmpty() const;

		/**
		 * Clears all index data
		 */
		void clear();

		/**
		 * Ensures persistence of index data
		 */
		void flush();

		void saveState();

	private:
		std::shared_ptr<IndexTreeManager> treeManager_;
		mutable std::shared_mutex mutex_;
		int64_t rootId_ = 0;

		void loadRootId();
	};

} // namespace graph::query::indexes