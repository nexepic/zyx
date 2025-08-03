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

	IndexTreeManager::IndexTreeManager(std::shared_ptr<storage::DataManager> dataManager, uint32_t indexType,
									 uint32_t maxLeafKeys, uint32_t maxInternalKeys) :
		dataManager_(std::move(dataManager)),
		indexType_(indexType),
		maxKeysPerLeaf_(maxLeafKeys),
		maxKeysPerInternal_(maxInternalKeys) {}

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

			if (entity.getId() == 0) continue; // Node already deleted or invalid

			// If it's an internal node, add all its children to the deletion queue.
			if (entity.getNodeType() == Index::NodeType::INTERNAL) {
				auto children = entity.getAllChildren();
				for (const auto &[key, childId] : children) {
					// CORRECTED LINE: Use the correct member name 'childId'
					toDelete.push_back(childId);
				}
			}
			// If it's a leaf node and uses blob storage, we must delete the blob chain.
			else if (entity.isLeaf() && entity.hasBlobStorage()) {
				dataManager_->getBlobManager()->deleteBlobChain(entity.getBlobId());
			}

			// Finally, delete the index node itself.
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
		if (std::holds_alternative<std::string>(key)) return std::get<std::string>(key);
		if (std::holds_alternative<int64_t>(key)) return std::to_string(std::get<int64_t>(key));
		if (std::holds_alternative<double>(key)) return std::to_string(std::get<double>(key));
		return "";
	}

	std::vector<IndexTreeManager::KeyValueEntry>
	IndexTreeManager::getAllKeyValuesFromNode(const Index &node) const {
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

	void IndexTreeManager::setAllKeyValuesToNode(Index &node,
												 const std::vector<KeyValueEntry> &entries) const {
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
		if (rootId == 0) return 0;
		int64_t currentId = rootId;
		auto current = dataManager_->getIndex(currentId);
		while (!current.isLeaf()) {
			int64_t childId = current.findChild(keyToString(key));
			if (childId == 0) {
				// Should not happen in a well-formed tree, but as a safeguard:
				auto children = current.getAllChildren();
				if (!children.empty()) childId = children.front().childId;
				else return 0; // Tree is corrupted.
			}
			currentId = childId;
			current = dataManager_->getIndex(currentId);
		}
		return currentId;
	}

	void IndexTreeManager::insertIntoLeaf(int64_t leafId, const KeyType &key, int64_t value) {
		auto leaf = dataManager_->getIndex(leafId);
		if (std::holds_alternative<std::string>(key)) {
			leaf.insertStringKey(std::get<std::string>(key), value, dataManager_);
			dataManager_->updateIndexEntity(leaf);
		} else {
			// Placeholder for other key types
			throw std::runtime_error("Numeric key types are not yet implemented for insertion.");
		}
	}

	void IndexTreeManager::splitLeaf(Index& leaf, const KeyType& newKey, int64_t newValue, int64_t& rootId) {
		// 1. Get all key-values from the leaf and add the new one.
		auto allKVs = leaf.getAllKeyValues(dataManager_);
		allKVs.push_back({keyToString(newKey), {newValue}});
		std::sort(allKVs.begin(), allKVs.end(), [](const auto& a, const auto& b) {
			return a.key < b.key;
		});

		// 2. Create a new sibling leaf.
		int64_t newLeafId = createNewNode(Index::NodeType::LEAF);
		auto newLeaf = dataManager_->getIndex(newLeafId);

		// 3. Find midpoint and the key to be promoted to the parent.
		size_t midPoint = allKVs.size() / 2;
		KeyType promotedKey = allKVs[midPoint].key;

		// 4. Distribute KVs between the old leaf and the new leaf.
		std::vector firstHalf(allKVs.begin(), allKVs.begin() + midPoint);
		leaf.setAllKeyValues(firstHalf, dataManager_);

		std::vector secondHalf(allKVs.begin() + midPoint, allKVs.end());
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

		// Leaf and newLeaf levels should be the same
		newLeaf.setLevel(leaf.getLevel());

		// 6. Save changes and insert the promoted key into the parent.
		dataManager_->updateIndexEntity(leaf);
		dataManager_->updateIndexEntity(newLeaf);
		insertIntoParent(leaf, promotedKey, newLeafId, rootId);
	}

	void IndexTreeManager::insertIntoParent(Index& leftNode, const KeyType& key, int64_t rightNodeId, int64_t& rootId) {
		if (leftNode.getParentId() == 0) {
			// The left node was the root, so create a new root.
			int64_t newRootId = createNewNode(Index::NodeType::INTERNAL);
			auto newRoot = dataManager_->getIndex(newRootId);
			rootId = newRootId; // The new root is now the tree's root.

			auto rightNode = dataManager_->getIndex(rightNodeId);
			leftNode.setParentId(newRootId);
			rightNode.setParentId(newRootId);

            // The new root's level is one higher than its children.
            newRoot.setLevel(leftNode.getLevel() + 1);

			// Add children to the new root. First pointer has an empty key (implicit).
			newRoot.addChild("", leftNode.getId());
			newRoot.addChild(keyToString(key), rightNode.getId());

			dataManager_->updateIndexEntity(leftNode);
			dataManager_->updateIndexEntity(rightNode);
			dataManager_->updateIndexEntity(newRoot);
			return;
		}

		// Parent exists, insert the new child pointer there.
		auto parent = dataManager_->getIndex(leftNode.getParentId());
		auto rightNode = dataManager_->getIndex(rightNodeId);
        rightNode.setParentId(parent.getId());
        dataManager_->updateIndexEntity(rightNode);

		if (parent.getChildCount() < maxKeysPerInternal_) {
			// Parent has space, just add the new child.
			parent.addChild(keyToString(key), rightNodeId);
			dataManager_->updateIndexEntity(parent);
		} else {
			// Parent is full, must split it.
            // Use try-catch here as setAllChildren will throw on overflow.
            auto allChildren = parent.getAllChildren();
            allChildren.push_back({keyToString(key), rightNodeId});
            std::sort(allChildren.begin(), allChildren.end(), [](const auto& a, const auto& b){ return a.key < b.key; });

            int64_t newInternalId = createNewNode(Index::NodeType::INTERNAL);
            auto newInternalNode = dataManager_->getIndex(newInternalId);
            newInternalNode.setParentId(parent.getParentId());
            newInternalNode.setLevel(parent.getLevel());

            size_t midPoint = allChildren.size() / 2;
            KeyType promotedKey = allChildren[midPoint].key;

            // Distribute children, promoting the middle key.
            std::vector<Index::ChildEntry> leftChildren(allChildren.begin(), allChildren.begin() + midPoint);
            parent.setAllChildren(leftChildren);

            std::vector<Index::ChildEntry> rightChildren(allChildren.begin() + midPoint, allChildren.end());
            newInternalNode.setAllChildren(rightChildren);

            // Update parent pointers for the children that moved to the new node.
            for (const auto& childEntry : rightChildren) {
                auto child = dataManager_->getIndex(childEntry.childId);
                child.setParentId(newInternalId);
                dataManager_->updateIndexEntity(child);
            }

            dataManager_->updateIndexEntity(parent);
            dataManager_->updateIndexEntity(newInternalNode);

            // Recursively call insertIntoParent for the newly promoted key.
            insertIntoParent(parent, promotedKey, newInternalId, rootId);
		}
	}

	int64_t IndexTreeManager::insert(int64_t rootId, const KeyType &key, const int64_t value) {
		std::unique_lock lock(mutex_);

		if (rootId == 0) {
			rootId = initialize();
		}

		int64_t leafId = findLeafNode(rootId, key);
		auto leaf = dataManager_->getIndex(leafId);

		// If the leaf is full, split it first.
		if (leaf.getKeyCount() >= maxKeysPerLeaf_) {
			splitLeaf(leaf, key, value, rootId);
            // After splitting, we need to find the correct leaf again for the new key.
            // The new key could be in the original leaf or the new one.
            leafId = findLeafNode(rootId, key);
		}

        // Insert into the (now guaranteed not full) leaf.
		insertIntoLeaf(leafId, key, value);
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
		if (rootId == 0) return {};
		int64_t leafId = findLeafNode(rootId, key);
		if (leafId == 0) return {};
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
