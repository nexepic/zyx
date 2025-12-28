/**
 * @file IndexTreeManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/13
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <shared_mutex>
#include <vector>
#include "Index.hpp"

namespace graph::storage {
	class DataManager;
}

namespace graph::query::indexes {

	/**
	 * Manages B+Tree operations for index structures
	 * Handles node creation, splitting, merging and traversal
	 */
	class IndexTreeManager {
	public:
		/**
		 * Constructor
		 *
		 * @param dataManager Pointer to the data manager for entity persistence
		 * @param indexType The type identifier for this index (e.g., LABEL_INDEX_TYPE)
		 */
		IndexTreeManager(std::shared_ptr<storage::DataManager> dataManager, uint32_t indexType,
						 PropertyType keyDataType);
		~IndexTreeManager() = default;

		/**
		 * Initialize the B+Tree structure
		 * Creates a root node if not already present
		 *
		 * @return The ID of the root node
		 */
		int64_t initialize() const;

		/**
		 * Clears the entire tree structure
		 * Deletes all nodes and creates a new empty root
		 *
		 * @param rootId ID of the current root node
		 * @return ID of the new root node
		 */
		void clear(int64_t rootId) const;

		/**
		 * Inserts a key-value pair into the tree
		 *
		 * @param rootId ID of the root node
		 * @param key The key to insert
		 * @param value The value to associate with the key
		 * @return ID of the new root (may change due to splits)
		 */
		int64_t insert(int64_t rootId, const PropertyValue &key, int64_t value);

		int64_t insertBatch(int64_t rootId, const std::vector<std::pair<PropertyValue, int64_t>>& entries);

		/**
		 * Removes a key-value pair from the tree
		 *
		 * @param rootId ID of the root node
		 * @param key The key to remove
		 * @param value The value to remove
		 * @return true if the pair was found and removed
		 */
		bool remove(int64_t rootId, const PropertyValue &key, int64_t value);

		/**
		 * Finds all values associated with a key
		 *
		 * @param rootId ID of the root node
		 * @param key The key to search for
		 * @return Vector of values associated with the key
		 */
		std::vector<int64_t> find(int64_t rootId, const PropertyValue &key);

		/**
		 * Finds all values within a range (for numeric keys)
		 *
		 * @param rootId ID of the root node
		 * @param minKey The lower bound of the range
		 * @param maxKey The upper bound of the range
		 * @return Vector of values within the range
		 */
		std::vector<int64_t> findRange(int64_t rootId, const PropertyValue &minKey, const PropertyValue &maxKey);

		int64_t findLeafNode(int64_t rootId, const PropertyValue &key) const;

		std::shared_ptr<storage::DataManager> getDataManager() const { return dataManager_; }

	private:
		std::shared_ptr<storage::DataManager> dataManager_;
		mutable std::shared_mutex mutex_;
		uint32_t indexType_;
		PropertyType keyDataType_;
		std::function<bool(const PropertyValue &, const PropertyValue &)> keyComparator_;

		static constexpr double UNDERFLOW_THRESHOLD_RATIO = 0.4;

		// Helper methods for B+Tree operations
		int64_t createNewNode(Index::NodeType type) const;

		void splitLeaf(Index &leaf, const PropertyValue &newKey, int64_t newValue, int64_t &rootId);
		void insertIntoParent(Index &leftNode, const PropertyValue &key, int64_t rightNodeId, int64_t &rootId);

		/**
		 * @brief Handles a node that may be under-full after a deletion.
		 * @param node The node to check (can be leaf or internal).
		 * @param rootId Reference to the root ID, as it may change.
		 */
		void handleUnderflow(Index &node, int64_t &rootId);

		/**
		 * @brief Attempts to redistribute entries from a sibling node.
		 * @param node The under-full node.
		 * @param sibling The sibling node to borrow from.
		 * @param parent The parent of the two nodes.
		 * @param isLeftSibling True if the sibling is to the left of the node.
		 */
		void redistribute(Index &node, Index &sibling, Index &parent, bool isLeftSibling);

		/**
		 * @brief Merges a node with its sibling.
		 * @param leftNode The node on the left.
		 * @param rightNode The node on the right.
		 * @param parent The parent of the two nodes.
		 * @param separatorKey The key in the parent that separates the two nodes.
		 * @param rootId Reference to the root ID, as it may change.
		 */
		void mergeNodes(Index &leftNode, Index &rightNode, Index &parent, const PropertyValue &separatorKey,
						int64_t &rootId);
	};

} // namespace graph::query::indexes
