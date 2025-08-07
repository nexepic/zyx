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
#include <ranges>
#include <stdexcept>
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::indexes {

	IndexTreeManager::IndexTreeManager(std::shared_ptr<storage::DataManager> dataManager, uint32_t indexType) :
		dataManager_(std::move(dataManager)), indexType_(indexType) {}

	int64_t IndexTreeManager::initialize() const {
		std::unique_lock lock(mutex_);

		// Create a new root node as a leaf
		return createNewNode(Index::NodeType::LEAF);
	}

	void IndexTreeManager::clear(const int64_t rootId) const {
		std::unique_lock lock(mutex_);

		if (rootId == 0) {
			return;
		}

		// Use a queue for a Breadth-First-Search (BFS) traversal to delete all nodes.
		std::vector<int64_t> toDelete;
		toDelete.push_back(rootId);

		for (size_t i = 0; i < toDelete.size(); ++i) {
			int64_t currentId = toDelete[i];
			auto entity = dataManager_->getIndex(currentId);

			if (entity.getId() == 0)
				continue; // Node already deleted or invalid

			// If it's an internal node, add all its children to the deletion queue
			// AND clean up any blobs used for its keys.
			if (entity.getNodeType() == Index::NodeType::INTERNAL) {
				auto children = entity.getAllChildren();

				for (const auto &entry: children) {
					// Add the child node's ID to the queue for future deletion.
					toDelete.push_back(entry.childId);

					// This prevents leaking key data.
					if (entry.keyBlobId != 0) {
						dataManager_->getBlobManager()->deleteBlobChain(entry.keyBlobId);
					}
				}
			}
			// If it's a leaf node and uses blob storage for its *content*,
			// we must delete that blob chain.
			else if (entity.isLeaf() && entity.hasBlobStorage()) {
				dataManager_->getBlobManager()->deleteBlobChain(entity.getBlobId());
			}

			// Finally, delete the index node entity itself.
			dataManager_->deleteIndex(entity);
		}
	}

	int64_t IndexTreeManager::createNewNode(Index::NodeType type) const {
		Index entity(0, type, indexType_);
		dataManager_->addIndexEntity(entity);
		return entity.getId();
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
		if (std::holds_alternative<std::string>(key))
			return std::get<std::string>(key);
		if (std::holds_alternative<int64_t>(key))
			return std::to_string(std::get<int64_t>(key));
		if (std::holds_alternative<double>(key))
			return std::to_string(std::get<double>(key));
		return "";
	}

	std::vector<IndexTreeManager::KeyValueEntry> IndexTreeManager::getAllKeyValuesFromNode(const Index &node) const {
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

	void IndexTreeManager::setAllKeyValuesToNode(Index &node, const std::vector<KeyValueEntry> &entries) const {
		if (!node.isLeaf()) {
			return; // Only applies to leaf nodes
		}

		// Prepare entries for string keys
		std::vector<Index::KeyValuePair> stringEntries;

		for (const auto &entry: entries) {
			if (std::holds_alternative<std::string>(entry.key)) {
				Index::KeyValuePair kv;
				kv.key = std::get<std::string>(entry.key);
				kv.values = entry.values;
				stringEntries.push_back(kv);
			}
		}

		// Set values for string keys
		node.setAllKeyValues(stringEntries, dataManager_);
	}

	int64_t IndexTreeManager::findLeafNode(int64_t rootId, const KeyType &key) const {
		if (rootId == 0)
			return 0;
		int64_t currentId = rootId;
		auto current = dataManager_->getIndex(currentId);
		while (!current.isLeaf()) {
			int64_t childId = current.findChild(keyToString(key), dataManager_);
			if (childId == 0) {
				// Safeguard
				auto children = current.getAllChildren();
				return children.empty() ? 0 : children.front().childId;
			}
			currentId = childId;
			current = dataManager_->getIndex(currentId);
		}
		return currentId;
	}

	void IndexTreeManager::splitLeaf(Index &leaf, const KeyType &newKey, int64_t newValue, int64_t &rootId) {
		// 1. Get all existing KVs from the leaf and add the new one.
		auto allKVs = leaf.getAllKeyValues(dataManager_);

		// Find the correct place and insert the new key-value pair.
		std::string newKeyStr = keyToString(newKey);
		auto it = std::lower_bound(allKVs.begin(), allKVs.end(), newKeyStr,
								   [](const Index::KeyValuePair &a, const std::string &b) { return a.key < b; });

		if (it != allKVs.end() && it->key == newKeyStr) {
			// Key exists, add new value if not present.
			auto &values = it->values;
			if (std::find(values.begin(), values.end(), newValue) == values.end()) {
				values.push_back(newValue);
			}
		} else {
			// Key does not exist, insert new entry.
			allKVs.insert(it, {newKeyStr, {newValue}});
		}

		// 2. Create a new sibling leaf.
		int64_t newLeafId = createNewNode(Index::NodeType::LEAF);
		auto newLeaf = dataManager_->getIndex(newLeafId);

		// 3. Find the midpoint. The key at the midpoint will be promoted to the parent.
		size_t midPointIdx = allKVs.size() / 2;
		// The promoted key is the *first key* of the new (right) sibling node.
		KeyType promotedKey = allKVs[midPointIdx].key;

		// 4. Distribute KVs between the old leaf and the new sibling.
		std::vector<Index::KeyValuePair> firstHalf(allKVs.begin(), allKVs.begin() + midPointIdx);
		leaf.setAllKeyValues(firstHalf, dataManager_);

		std::vector<Index::KeyValuePair> secondHalf(allKVs.begin() + midPointIdx, allKVs.end());
		newLeaf.setAllKeyValues(secondHalf, dataManager_);

		// 5. Update the doubly linked list of leaves.
		newLeaf.setNextLeafId(leaf.getNextLeafId());
		newLeaf.setPrevLeafId(leaf.getId());
		if (leaf.getNextLeafId() != 0) {
			auto nextLeaf = dataManager_->getIndex(leaf.getNextLeafId());
			nextLeaf.setPrevLeafId(newLeafId);
			dataManager_->updateIndexEntity(nextLeaf);
		}
		leaf.setNextLeafId(newLeafId);
		newLeaf.setLevel(leaf.getLevel());

		// 6. Save changes and insert the promoted key into the parent.
		dataManager_->updateIndexEntity(leaf);
		dataManager_->updateIndexEntity(newLeaf);

		insertIntoParent(leaf, promotedKey, newLeafId, rootId);
	}

	void IndexTreeManager::insertIntoParent(Index &leftNode, const KeyType &key, int64_t rightNodeId, int64_t &rootId) {
		std::string separatorKeyStr = keyToString(key);
		Index::ChildEntry newChildEntry;
		newChildEntry.childId = rightNodeId;

		if (separatorKeyStr.length() > Index::INTERNAL_KEY_BLOB_THRESHOLD) {
			auto blobChain = dataManager_->getBlobManager()->createBlobChain(leftNode.getParentId(), Index::typeId,
																			 separatorKeyStr);
			if (blobChain.empty()) {
				throw std::runtime_error("Failed to create blob for oversized index key.");
			}
			newChildEntry.keyBlobId = blobChain.front().getId();
		} else {
			newChildEntry.key = separatorKeyStr;
		}

		// Case 1: The left node was the root. Create a new root.
		if (leftNode.getParentId() == 0) {
			int64_t newRootId = createNewNode(Index::NodeType::INTERNAL);
			auto newRoot = dataManager_->getIndex(newRootId);
			rootId = newRootId; // Update the root ID reference for the caller

			// Set the new root's level. It's one level higher than its children.
			newRoot.setLevel(leftNode.getLevel() + 1);

			// The new root will have two children: the original left node and the new right node.
			// The first child has an implicit key (-infinity).
			Index::ChildEntry firstChildEntry;
			firstChildEntry.childId = leftNode.getId();

			std::vector<Index::ChildEntry> children;
			children.push_back(firstChildEntry);
			children.push_back(newChildEntry); // The new child entry contains the key and rightNodeId
			newRoot.setAllChildren(children);

			// Update parent pointers for both children to point to the new root.
			leftNode.setParentId(newRootId);

			auto rightNode = dataManager_->getIndex(rightNodeId);
			rightNode.setParentId(newRootId);

			// Persist all changes to the database.
			dataManager_->updateIndexEntity(leftNode);
			dataManager_->updateIndexEntity(rightNode);
			dataManager_->updateIndexEntity(newRoot);
			return;
		}

		// Case 2: Parent exists. Try to add the new child.
		auto parent = dataManager_->getIndex(leftNode.getParentId());

		// It no longer passes the unnecessary dataManager_ argument.
		if (parent.tryAddChild(newChildEntry)) {
			parent.addChild(newChildEntry, dataManager_);
			// Update the parent and also the right node, whose parent ID has now been set.
			auto rightNode = dataManager_->getIndex(rightNodeId);
			rightNode.setParentId(parent.getId());
			dataManager_->updateIndexEntity(parent);
			dataManager_->updateIndexEntity(rightNode);
		}
		// Case 3: Parent is full and must be split.
		else {
			auto allChildren = parent.getAllChildren();
			allChildren.push_back(newChildEntry);

			auto blobAwareComparator = [&](const Index::ChildEntry &a, const Index::ChildEntry &b) {
				if (b.key.empty() && b.keyBlobId == 0)
					return false;
				if (a.key.empty() && a.keyBlobId == 0)
					return true;
				std::string keyA =
						(a.keyBlobId != 0) ? dataManager_->getBlobManager()->readBlobChain(a.keyBlobId) : a.key;
				std::string keyB =
						(b.keyBlobId != 0) ? dataManager_->getBlobManager()->readBlobChain(b.keyBlobId) : b.key;
				return keyA < keyB;
			};
			std::sort(allChildren.begin(), allChildren.end(), blobAwareComparator);

			size_t midPointIdx = allChildren.size() / 2;
			Index::ChildEntry promotedEntry = allChildren[midPointIdx];

			int64_t newInternalId = createNewNode(Index::NodeType::INTERNAL);
			auto newInternalNode = dataManager_->getIndex(newInternalId);
			newInternalNode.setParentId(parent.getParentId());
			newInternalNode.setLevel(parent.getLevel());

			std::vector leftChildren(allChildren.begin(), allChildren.begin() + midPointIdx);
			parent.setAllChildren(leftChildren);

			std::vector rightChildren(allChildren.begin() + midPointIdx, allChildren.end());
			if (!rightChildren.empty()) {
				// The first entry in the new right node has its key promoted, so it becomes an implicit key.
				rightChildren[0].key = "";
				rightChildren[0].keyBlobId = 0;
			}
			newInternalNode.setAllChildren(rightChildren);

			// Re-parent the children that were moved to the new internal node.
			for (const auto &childEntry: rightChildren) {
				auto child = dataManager_->getIndex(childEntry.childId);
				child.setParentId(newInternalId);
				dataManager_->updateIndexEntity(child);
			}

			dataManager_->updateIndexEntity(parent);
			dataManager_->updateIndexEntity(newInternalNode);

			KeyType promotedKey = (promotedEntry.keyBlobId != 0)
										  ? dataManager_->getBlobManager()->readBlobChain(promotedEntry.keyBlobId)
										  : promotedEntry.key;

			// Recursively insert the promoted key into the grandparent.
			insertIntoParent(parent, promotedKey, newInternalId, rootId);
		}
	}

	int64_t IndexTreeManager::insert(int64_t rootId, const KeyType &key, const int64_t value) {
		std::unique_lock lock(mutex_);

		std::string keyStr = keyToString(key);
		if (keyStr.length() > Index::ABSOLUTE_MAX_KEY_LENGTH) {
			throw std::runtime_error("Key size exceeds absolute maximum limit.");
		}

		if (rootId == 0) {
			rootId = initialize();
		}

		int64_t leafId = findLeafNode(rootId, key);
		if (leafId == 0) { // Should only happen on a truly empty or corrupt tree
			rootId = initialize();
			leafId = rootId;
		}

		auto leaf = dataManager_->getIndex(leafId);

		auto existingValues = leaf.findStringValues(keyStr, dataManager_);

		if (!existingValues.empty()) {
			// Key exists. Just add the new value. This may trigger the leaf's
			// internal switch to blob storage, but it should NOT cause a split.
			leaf.insertStringKey(keyStr, value, dataManager_);
			dataManager_->updateIndexEntity(leaf);
		} else {
			// This is a NEW key. Now we check if adding it will overfill the node
			// and require a split.
			if (leaf.isFullAfterInsert(keyStr, value, dataManager_)) {
				// The leaf is full of keys, split it.
				splitLeaf(leaf, key, value, rootId);
			} else {
				// The leaf has room for the new key, insert it directly.
				leaf.insertStringKey(keyStr, value, dataManager_);
				dataManager_->updateIndexEntity(leaf);
			}
		}

		return rootId;
	}

	bool IndexTreeManager::remove(int64_t rootId, const KeyType &key, int64_t value) const {
		std::unique_lock lock(mutex_);

		if (rootId == 0) {
			return false;
		}

		int64_t leafId = findLeafNode(rootId, key);
		if (leafId == 0) {
			return false; // Key not found, can't find the leaf.
		}

		auto leaf = dataManager_->getIndex(leafId);

		// The removeStringKey method handles the entire Load->Modify->Save cycle,
		// including updating blob storage if necessary.
		bool removed = leaf.removeStringKey(keyToString(key), value, dataManager_);

		if (removed) {
			dataManager_->updateIndexEntity(leaf);
			// NOTE: A full B+Tree implementation would check if `leaf` is now under-full
			// and trigger a merge or redistribution with sibling nodes. This can
			// recursively affect parent nodes and even change the root.
		}

		return removed;
	}

	std::vector<int64_t> IndexTreeManager::find(const int64_t rootId, const KeyType &key) const {
		std::shared_lock lock(mutex_);
		if (rootId == 0)
			return {};
		int64_t leafId = findLeafNode(rootId, key);
		if (leafId == 0)
			return {};
		auto leaf = dataManager_->getIndex(leafId);
		return leaf.findStringValues(keyToString(key), dataManager_);
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
