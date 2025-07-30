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
#include <string>
#include <variant>
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
		// Supported key types
		using KeyType = std::variant<std::string, int64_t, double>;

		// Constants for B+Tree configuration
		static uint32_t MAX_KEYS_PER_NODE;

		/**
		 * Constructor
		 *
		 * @param dataManager Pointer to the data manager for entity persistence
		 * @param indexType The type identifier for this index (e.g., LABEL_INDEX_TYPE)
		 */
		IndexTreeManager(std::shared_ptr<storage::DataManager> dataManager, uint32_t indexType);
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
		int64_t insert(int64_t rootId, const KeyType &key, int64_t value);

		/**
		 * Removes a key-value pair from the tree
		 *
		 * @param rootId ID of the root node
		 * @param key The key to remove
		 * @param value The value to remove
		 * @return true if the pair was found and removed
		 */
		bool remove(int64_t rootId, const KeyType &key, int64_t value) const;

		/**
		 * Finds all values associated with a key
		 *
		 * @param rootId ID of the root node
		 * @param key The key to search for
		 * @return Vector of values associated with the key
		 */
		std::vector<int64_t> find(int64_t rootId, const KeyType &key) const;

		/**
		 * Finds all values within a range (for numeric keys)
		 *
		 * @param rootId ID of the root node
		 * @param minKey The lower bound of the range
		 * @param maxKey The upper bound of the range
		 * @return Vector of values within the range
		 */
		std::vector<int64_t> findRange(int64_t rootId, const KeyType &minKey, const KeyType &maxKey) const;

		int64_t findLeafNode(int64_t rootId, const KeyType &key) const;

		std::shared_ptr<storage::DataManager> getDataManager() const { return dataManager_; }

	private:
		std::shared_ptr<storage::DataManager> dataManager_;
		mutable std::shared_mutex mutex_;
		uint32_t indexType_;

		// Helper methods for B+Tree operations
		int64_t createNewNode(Index::NodeType type) const;

		// Key comparison helper
		static bool compareKeys(const KeyType &a, const KeyType &b);
		static std::string keyToString(const KeyType &key);

		// Insert helpers
		bool insertIntoLeaf(int64_t leafId, const KeyType &key, int64_t value) const;
		void splitLeaf(int64_t rootId, int64_t leafId, const KeyType &key, int64_t value, int64_t &newRootId);
		void insertIntoParent(int64_t rootId, int64_t nodeId, const KeyType &key, int64_t newNodeId,
							  int64_t &newRootId);

		// Get all key-values from a node
		struct KeyValueEntry {
			KeyType key;
			std::vector<int64_t> values;
		};
		std::vector<KeyValueEntry> getAllKeyValuesFromNode(const Index &node) const;
		void setAllKeyValuesToNode(Index &node, const std::vector<KeyValueEntry> &entries) const;

		// Convert between variant key types and specific types
		static void setEntityKey(Index &entity, const KeyType &key, int64_t childOrValue);
		static std::vector<int64_t> getEntityValues(const Index &entity, const KeyType &key);
	};

} // namespace graph::query::indexes
