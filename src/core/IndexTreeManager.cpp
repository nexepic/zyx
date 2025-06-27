/**
 * @file IndexTreeManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/13
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/IndexTreeManager.hpp"
#include <algorithm>
#include <stdexcept>
#include "graph/storage/DataManager.hpp"

namespace graph::query::indexes {

	uint32_t IndexTreeManager::MAX_KEYS_PER_NODE = 32;

	IndexTreeManager::IndexTreeManager(std::shared_ptr<storage::DataManager> dataManager, uint32_t indexType) :
		dataManager_(std::move(dataManager)), indexType_(indexType) {}

	int64_t IndexTreeManager::initialize() {
		std::unique_lock lock(mutex_);

		// Create a new root node as a leaf
		return createNewNode(storage::Index::NodeType::LEAF);
	}

	void IndexTreeManager::clear(int64_t rootId) {
		std::unique_lock lock(mutex_);

		if (rootId != 0) {
			// Queue of nodes to delete (breadth-first approach)
			std::vector<int64_t> toDelete;
			toDelete.push_back(rootId);

			for (size_t i = 0; i < toDelete.size(); i++) {
				auto entity = dataManager_->getIndex(toDelete[i]);

				// If it's an internal node, add all children to the queue
				if (entity.getNodeType() == storage::Index::NodeType::INTERNAL) {
					auto children = entity.getAllChildren();
					for (const auto &child: children) {
						toDelete.push_back(child.second);
					}
				}

				dataManager_->deleteIndex(entity);
			}
		}

		// // Create a new root node
		// return createNewNode(storage::Index::NodeType::LEAF);
	}

	int64_t IndexTreeManager::createNewNode(storage::Index::NodeType type) {
		// Allocate ID for new node
		int64_t id = dataManager_->reserveTemporaryIndexId();

		// Create node
		storage::Index entity(id, type, indexType_);

		// Save it
		dataManager_->addIndexEntity(entity);

		return id;
	}

	bool IndexTreeManager::compareKeys(const KeyType &a, const KeyType &b) {
		// Compare keys of potentially different types
		if (a.index() != b.index()) {
			return a.index() < b.index(); // Different types - sort by type index
		}

		// Same types, compare values
		if (std::holds_alternative<std::string>(a)) {
			return std::get<std::string>(a) < std::get<std::string>(b);
		} else if (std::holds_alternative<int64_t>(a)) {
			return std::get<int64_t>(a) < std::get<int64_t>(b);
		} else if (std::holds_alternative<double>(a)) {
			return std::get<double>(a) < std::get<double>(b);
		}

		return false; // Should never reach here
	}

	std::string IndexTreeManager::keyToString(const KeyType &key) {
		if (std::holds_alternative<std::string>(key)) {
			return std::get<std::string>(key);
		} else if (std::holds_alternative<int64_t>(key)) {
			return std::to_string(std::get<int64_t>(key));
		} else if (std::holds_alternative<double>(key)) {
			return std::to_string(std::get<double>(key));
		}
		return ""; // Should never reach here
	}

	void IndexTreeManager::setEntityKey(storage::Index &entity, const KeyType &key, int64_t childOrValue) {
		if (entity.isLeaf()) {
			// For leaf nodes, insert values
			entity.insertStringKey(std::get<std::string>(key), childOrValue);
		} else {
			// For internal nodes, add child pointers
			entity.addChild(std::get<std::string>(key), childOrValue);
		}
	}

	std::vector<int64_t> IndexTreeManager::getEntityValues(const storage::Index &entity, const KeyType &key) {
		if (!entity.isLeaf()) {
			return {}; // Only leaf nodes store values
		}

		// Handle only string type keys
		return entity.findStringValues(std::get<std::string>(key));
	}

	std::vector<IndexTreeManager::KeyValueEntry>
	IndexTreeManager::getAllKeyValuesFromNode(const storage::Index &node) const {
		std::vector<KeyValueEntry> result;

		if (!node.isLeaf()) {
			return result; // Only applies to leaf nodes
		}

		// Get all values based on key type
		// For string keys
		auto stringKVs = node.getAllKeyValues(dataManager_); // This should return the public version
		for (const auto &kv: stringKVs) {
			KeyValueEntry entry;
			entry.key = kv.key;
			entry.values = kv.values;
			result.push_back(entry);
		}

		// Similar methods for int and double keys would be needed
		// (For the current implementation focused on strings)

		return result;
	}

	void IndexTreeManager::setAllKeyValuesToNode(storage::Index &node, const std::vector<KeyValueEntry> &entries) {
		if (!node.isLeaf()) {
			return; // Only applies to leaf nodes
		}

		// Prepare entries for string keys
		std::vector<storage::Index::KeyValuePair> stringEntries;

		for (const auto &entry: entries) {
			if (std::holds_alternative<std::string>(entry.key)) {
				storage::Index::KeyValuePair kv;
				kv.key = std::get<std::string>(entry.key);
				kv.values = entry.values;
				stringEntries.push_back(kv);
			}
		}

		// Set values for string keys
		node.setAllKeyValues(stringEntries, dataManager_);
	}

	int64_t IndexTreeManager::findLeafNode(int64_t rootId, const KeyType &key) const {
		if (rootId == 0) {
			return 0;
		}

		int64_t currentId = rootId;
		auto current = dataManager_->getIndex(currentId);

		// Traverse down to leaf
		while (current.getNodeType() == storage::Index::NodeType::INTERNAL) {
			int64_t childId = 0;

			// Find appropriate child based on string key
			childId = current.findChild(std::get<std::string>(key));

			if (childId == 0) {
				break; // No suitable child found
			}

			currentId = childId;
			current = dataManager_->getIndex(currentId);
		}

		return currentId;
	}

	bool IndexTreeManager::insertIntoLeaf(int64_t leafId, const KeyType &key, int64_t value) {
		auto leaf = dataManager_->getIndex(leafId);

		// Check if we're at capacity
		if (leaf.getKeyCount() >= MAX_KEYS_PER_NODE) {
			return false;
		}

		// Insert based on string key
		bool success = leaf.insertStringKey(std::get<std::string>(key), value);

		if (success) {
			dataManager_->updateIndexEntity(leaf);
		}

		return success;
	}

	void IndexTreeManager::splitLeaf(int64_t rootId, int64_t leafId, const KeyType &key, int64_t value,
									 int64_t &newRootId) {
		auto leaf = dataManager_->getIndex(leafId);

		// Create a new leaf
		int64_t newLeafId = createNewNode(storage::Index::NodeType::LEAF);
		auto newLeaf = dataManager_->getIndex(newLeafId);

		// Get all key-value pairs
		auto allValues = getAllKeyValuesFromNode(leaf);

		// Add the new key-value pair
		bool inserted = false;
		for (auto it = allValues.begin(); it != allValues.end(); ++it) {
			if (!inserted && compareKeys(key, it->key)) {
				// Insert the new key-value pair at this position
				KeyValueEntry newEntry;
				newEntry.key = key;
				newEntry.values.push_back(value);
				allValues.insert(it, newEntry);
				inserted = true;
				break;
			}

			if (!inserted && !compareKeys(it->key, key) && !compareKeys(key, it->key)) { // Equal keys
				// Key exists, add value if not already present
				if (std::find(it->values.begin(), it->values.end(), value) == it->values.end()) {
					it->values.push_back(value);
				}
				inserted = true;
				break;
			}
		}

		if (!inserted) {
			// Insert at the end
			KeyValueEntry newEntry;
			newEntry.key = key;
			newEntry.values.push_back(value);
			allValues.push_back(newEntry);
		}

		// Split values between the two leaves
		size_t midPoint = allValues.size() / 2;
		KeyType midKey = allValues[midPoint].key;

		// Clear and reload the nodes
		leaf = dataManager_->getIndex(leafId);

		// Keep first half in original leaf
		std::vector<KeyValueEntry> firstHalf(allValues.begin(), allValues.begin() + midPoint);
		setAllKeyValuesToNode(leaf, firstHalf);

		// Put second half in new leaf
		std::vector<KeyValueEntry> secondHalf(allValues.begin() + midPoint, allValues.end());
		setAllKeyValuesToNode(newLeaf, secondHalf);

		// Update leaf links
		newLeaf.setNextLeafId(leaf.getNextLeafId());
		newLeaf.setPrevLeafId(leafId);

		if (leaf.getNextLeafId() != 0) {
			auto nextLeaf = dataManager_->getIndex(leaf.getNextLeafId());
			nextLeaf.setPrevLeafId(newLeafId);
			dataManager_->updateIndexEntity(nextLeaf);
		}

		leaf.setNextLeafId(newLeafId);

		// Save both leaves
		dataManager_->updateIndexEntity(leaf);
		dataManager_->updateIndexEntity(newLeaf);

		// Update parent
		newRootId = rootId; // Will be modified if root changes
		insertIntoParent(rootId, leafId, midKey, newLeafId, newRootId);
	}

	void IndexTreeManager::insertIntoParent(int64_t rootId, int64_t nodeId, const KeyType &key, int64_t newNodeId,
											int64_t &newRootId) {
		auto node = dataManager_->getIndex(nodeId);

		// If this is the root, create a new root
		if (node.getParentId() == 0) {
			int64_t createdNewRootId = createNewNode(storage::Index::NodeType::INTERNAL);
			auto newRoot = dataManager_->getIndex(createdNewRootId);

			// First child gets empty key (will match everything less than the split key)
			newRoot.addChild("", nodeId);

			// Add the new split key and child
			setEntityKey(newRoot, key, newNodeId);

			// Update parent references
			node.setParentId(createdNewRootId);
			dataManager_->updateIndexEntity(node);

			auto newNode = dataManager_->getIndex(newNodeId);
			newNode.setParentId(createdNewRootId);
			dataManager_->updateIndexEntity(newNode);

			// Return the new root ID
			dataManager_->updateIndexEntity(newRoot);
			return;
		}

		// Get the parent
		int64_t parentId = node.getParentId();
		auto parent = dataManager_->getIndex(parentId);

		// If parent has space, simply insert
		if (parent.getChildCount() < MAX_KEYS_PER_NODE) {
			setEntityKey(parent, key, newNodeId);

			// Update parent reference of new node
			auto newNode = dataManager_->getIndex(newNodeId);
			newNode.setParentId(parentId);
			dataManager_->updateIndexEntity(newNode);

			dataManager_->updateIndexEntity(parent);
			return;
		}

		// Parent is full, need to split the internal node

		// 1. Create a new internal node
		int64_t newParentId = createNewNode(storage::Index::NodeType::INTERNAL);
		auto newParent = dataManager_->getIndex(newParentId);

		// 2. Get all children from parent and prepare for splitting
		struct ChildKeyPair {
			KeyType key;
			int64_t childId;
		};

		std::vector<ChildKeyPair> allChildren;

		// Get existing children from parent
		auto stringChildren = parent.getAllChildren();
		for (const auto &child: stringChildren) {
			allChildren.push_back({child.first, child.second});
		}

		// 3. Add the new child in sorted order
		bool inserted = false;
		for (auto it = allChildren.begin(); it != allChildren.end(); ++it) {
			if (!inserted && compareKeys(key, it->key)) {
				allChildren.insert(it, {key, newNodeId});
				inserted = true;
				break;
			}
		}

		if (!inserted) {
			allChildren.push_back({key, newNodeId});
		}

		// 4. Split children between the two parent nodes
		size_t midPoint = allChildren.size() / 2;
		KeyType midKey = allChildren[midPoint].key;

		// Clear parent nodes of children
		parent = dataManager_->getIndex(parentId);

		// 5. Move first half to original parent (excluding middle key)
		for (size_t i = 0; i < midPoint; i++) {
			setEntityKey(parent, allChildren[i].key, allChildren[i].childId);

			// Update child's parent reference
			auto child = dataManager_->getIndex(allChildren[i].childId);
			child.setParentId(parentId);
			dataManager_->updateIndexEntity(child);
		}

		// 6. Move second half to new parent (excluding middle key)
		for (size_t i = midPoint + 1; i < allChildren.size(); i++) {
			setEntityKey(newParent, allChildren[i].key, allChildren[i].childId);

			// Update child's parent reference
			auto child = dataManager_->getIndex(allChildren[i].childId);
			child.setParentId(newParentId);
			dataManager_->updateIndexEntity(child);
		}

		// 7. Handle the middle child, which should go to the new parent
		auto midChild = dataManager_->getIndex(allChildren[midPoint].childId);
		midChild.setParentId(newParentId);
		dataManager_->updateIndexEntity(midChild);

		// Also add this child to the new parent with minimum key
		newParent.addChild("", allChildren[midPoint].childId);

		// 8. Save both parents
		dataManager_->updateIndexEntity(parent);
		dataManager_->updateIndexEntity(newParent);

		// 9. Recursively update the parent's parent with the middle key
		insertIntoParent(rootId, parentId, midKey, newParentId, newRootId);
	}

	int64_t IndexTreeManager::insert(int64_t rootId, const KeyType &key, int64_t value) {
		std::unique_lock lock(mutex_);

		if (rootId == 0) {
			rootId = initialize();
		}

		// Find the leaf node where this key should be inserted
		int64_t leafId = findLeafNode(rootId, key);

		// Try to insert into the leaf
		if (!insertIntoLeaf(leafId, key, value)) {
			// Leaf is full, need to split
			int64_t newRootId = rootId;
			splitLeaf(rootId, leafId, key, value, newRootId);
			return newRootId; // May have changed if root was split
		}

		return rootId; // Root didn't change
	}

	bool IndexTreeManager::remove(int64_t rootId, const KeyType &key, int64_t value) {
		std::unique_lock lock(mutex_);

		if (rootId == 0) {
			return false;
		}

		// Find the leaf node containing this key
		int64_t leafId = findLeafNode(rootId, key);
		if (leafId == 0) {
			return false;
		}

		auto leaf = dataManager_->getIndex(leafId);
		bool removed = false;

		// Remove based on string key
		if (std::holds_alternative<std::string>(key)) {
			removed = leaf.removeStringKey(std::get<std::string>(key), value);
		}

		if (removed) {
			dataManager_->updateIndexEntity(leaf);

			// Note: In a complete implementation, we would handle merging of leaves
			// when they become underfilled
		}

		return removed;
	}

	std::vector<int64_t> IndexTreeManager::find(int64_t rootId, const KeyType &key) const {
		std::shared_lock lock(mutex_);

		if (rootId == 0) {
			return {};
		}

		// Find the leaf node containing this key
		int64_t leafId = findLeafNode(rootId, key);
		if (leafId == 0) {
			return {};
		}

		auto leaf = dataManager_->getIndex(leafId);
		return getEntityValues(leaf, key);
	}

	std::vector<int64_t> IndexTreeManager::findRange(int64_t rootId, const KeyType &minKey,
													 const KeyType &maxKey) const {
		std::shared_lock lock(mutex_);

		if (rootId == 0) {
			return {};
		}

		// Validate key types match
		if (minKey.index() != maxKey.index()) {
			return {}; // Different types not supported for range queries
		}

		// Start at the minimum key's leaf
		int64_t leafId = findLeafNode(rootId, minKey);
		if (leafId == 0) {
			return {};
		}

		std::vector<int64_t> results;

		// Traverse leaves collecting matching values
		while (leafId != 0) {
			auto leaf = dataManager_->getIndex(leafId);
			auto allEntries = getAllKeyValuesFromNode(leaf);

			for (const auto &entry: allEntries) {
				// Skip if less than minimum key
				if (compareKeys(entry.key, minKey)) {
					continue;
				}

				// Stop if greater than maximum key
				if (compareKeys(maxKey, entry.key)) {
					break;
				}

				// Add matching values
				results.insert(results.end(), entry.values.begin(), entry.values.end());
			}

			// Move to next leaf if we haven't passed maxKey
			if (allEntries.empty() || !compareKeys(maxKey, allEntries.back().key)) {
				leafId = leaf.getNextLeafId();
			} else {
				break;
			}
		}

		return results;
	}

} // namespace graph::query::indexes
