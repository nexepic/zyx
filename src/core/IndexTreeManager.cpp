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

	std::function<bool(const PropertyValue &, const PropertyValue &)> makeComparator(PropertyType type) {
		switch (type) {
			case PropertyType::INTEGER:
				return [](const auto &a, const auto &b) {
					return std::get<int64_t>(a.getVariant()) < std::get<int64_t>(b.getVariant());
				};
			case PropertyType::DOUBLE:
				return [](const auto &a, const auto &b) {
					return std::get<double>(a.getVariant()) < std::get<double>(b.getVariant());
				};
			case PropertyType::STRING:
				return [](const auto &a, const auto &b) {
					return std::get<std::string>(a.getVariant()) < std::get<std::string>(b.getVariant());
				};
			case PropertyType::BOOLEAN:
				return [](const auto &a, const auto &b) {
					return std::get<bool>(a.getVariant()) < std::get<bool>(b.getVariant());
				};
			default:
				throw std::logic_error("Unsupported key type for comparison.");
		}
	}

	IndexTreeManager::IndexTreeManager(std::shared_ptr<storage::DataManager> dataManager, uint32_t indexType,
									   PropertyType keyDataType) :
		dataManager_(std::move(dataManager)), indexType_(indexType), keyDataType_(keyDataType),
		keyComparator_(makeComparator(keyDataType)) {}

	int64_t IndexTreeManager::initialize() const {
		std::unique_lock lock(mutex_);

		// Create a new root node as a leaf
		return createNewNode(Index::NodeType::LEAF);
	}

	void IndexTreeManager::clear(const int64_t rootId) const {
		if (rootId == 0)
			return;

		std::vector<int64_t> toDelete;
		toDelete.push_back(rootId);

		// Use an index-based loop because the vector grows during iteration.
		for (size_t i = 0; i < toDelete.size(); ++i) {
			auto entity = dataManager_->getIndex(toDelete[i]);
			if (entity.getId() == 0)
				continue;

			// For any node, we must clean up blobs it might own.
			if (entity.isLeaf()) {
				auto allEntries = entity.getAllEntries(dataManager_);
				for (const auto &entry: allEntries) {
					// Clean up blobs for both keys and values, not just values.
					if (entry.keyBlobId != 0) {
						dataManager_->getBlobManager()->deleteBlobChain(entry.keyBlobId);
					}
					if (entry.valuesBlobId != 0) {
						dataManager_->getBlobManager()->deleteBlobChain(entry.valuesBlobId);
					}
				}
			} else { // Internal node
				auto allChildren = entity.getAllChildren(dataManager_);
				for (const auto &child: allChildren) {
					if (child.keyBlobId != 0) {
						dataManager_->getBlobManager()->deleteBlobChain(child.keyBlobId);
					}
					toDelete.push_back(child.childId);
				}
			}
			dataManager_->deleteIndex(entity); // Finally, delete the index node itself
		}
	}

	int64_t IndexTreeManager::createNewNode(Index::NodeType type) const {
		Index entity(0, type, indexType_);
		dataManager_->addIndexEntity(entity);
		return entity.getId();
	}

	int64_t IndexTreeManager::findLeafNode(int64_t rootId, const PropertyValue &key) const {
		if (rootId == 0)
			return 0;
		int64_t currentId = rootId;
		while (true) {
			auto current = dataManager_->getIndex(currentId);
			if (current.isLeaf())
				return currentId;
			// Pass dataManager to findChild
			int64_t childId = current.findChild(key, dataManager_, keyComparator_);
			if (childId == 0)
				return 0; // Should not happen in a healthy tree
			currentId = childId;
		}
	}

	void IndexTreeManager::splitLeaf(Index &leaf, const PropertyValue &newKey, int64_t newValue, int64_t &rootId) {
		// 1. Create a temporary, sorted list of all entries including the new one.
		auto allEntries = leaf.getAllEntries(dataManager_);
		auto it = std::lower_bound(
				allEntries.begin(), allEntries.end(), newKey,
				[&](const Index::Entry &a, const PropertyValue &b) { return keyComparator_(a.key, b); });
		// IMPORTANT: The new entry to be inserted must also be default-initialized
		allEntries.insert(it, {newKey, {newValue}, 0, 0});

		// 2. Create the new sibling leaf node.
		int64_t newLeafId = createNewNode(Index::NodeType::LEAF);
		auto newLeaf = dataManager_->getIndex(newLeafId);
		newLeaf.setLevel(leaf.getLevel());

		// 3. Find the split point.
		// The key at the midpoint is COPIED UP to the parent.
		// The entry itself becomes the FIRST entry of the new right leaf.
		size_t midPointIdx = allEntries.size() / 2;
		PropertyValue promotedKey = allEntries[midPointIdx].key; // This key is COPIED up.

		// 4. Distribute entries between the old and new leaves.
		// The original leaf gets all entries BEFORE the midpoint.
		std::vector<Index::Entry> leftEntries(std::make_move_iterator(allEntries.begin()),
											  std::make_move_iterator(allEntries.begin() + midPointIdx));
		leaf.setAllEntries(leftEntries, dataManager_);

		// The new leaf gets all entries FROM the midpoint to the end.
		std::vector<Index::Entry> rightEntries(std::make_move_iterator(allEntries.begin() + midPointIdx),
											   std::make_move_iterator(allEntries.end()));
		newLeaf.setAllEntries(rightEntries, dataManager_);

		// 5. Update the doubly linked list of leaves.
		newLeaf.setNextLeafId(leaf.getNextLeafId());
		newLeaf.setPrevLeafId(leaf.getId());
		if (leaf.getNextLeafId() != 0) {
			auto nextLeaf = dataManager_->getIndex(leaf.getNextLeafId());
			nextLeaf.setPrevLeafId(newLeafId);
			dataManager_->updateIndexEntity(nextLeaf);
		}
		leaf.setNextLeafId(newLeafId);

		// 6. Save changes and insert the promoted key into the parent.
		dataManager_->updateIndexEntity(leaf);
		dataManager_->updateIndexEntity(newLeaf);
		insertIntoParent(leaf, promotedKey, newLeafId, rootId);
	}

	void IndexTreeManager::insertIntoParent(Index &leftNode, const PropertyValue &key, int64_t rightNodeId,
											int64_t &rootId) {
		// Case 1: The left node was the root. A new root must be created.
		if (leftNode.getParentId() == 0) {
			int64_t newRootId = createNewNode(Index::NodeType::INTERNAL);
			auto newRoot = dataManager_->getIndex(newRootId);
			rootId = newRootId;
			newRoot.setLevel(leftNode.getLevel() + 1);

			auto leftNodeFromDb = dataManager_->getIndex(leftNode.getId());
			auto rightNodeFromDb = dataManager_->getIndex(rightNodeId);

			leftNodeFromDb.setParentId(newRootId);
			rightNodeFromDb.setParentId(newRootId);

			// The new child entry to be inserted into the new root.
			Index::ChildEntry newChildEntry{key, rightNodeId, 0};

			// The first child has an implicit, "negative infinity" key.
			Index::ChildEntry firstChild{PropertyValue(std::monostate{}), leftNode.getId(), 0};

			std::vector<Index::ChildEntry> children = {firstChild, newChildEntry};
			// Let replaceAllChildren handle blob creation for `newChildEntry.key` if needed.
			newRoot.setAllChildren(children, dataManager_);

			dataManager_->updateIndexEntity(leftNodeFromDb);
			dataManager_->updateIndexEntity(rightNodeFromDb);
			dataManager_->updateIndexEntity(newRoot);
			return;
		}

		// Case 2: Parent exists.
		auto parent = dataManager_->getIndex(leftNode.getParentId());
		Index::ChildEntry newChildEntry{key, rightNodeId, 0};

		// If parent is not full, just add the new child.
		if (!parent.wouldInternalOverflowOnAddChild(newChildEntry, dataManager_)) {
			parent.addChild(newChildEntry, dataManager_, keyComparator_);
			auto rightNode = dataManager_->getIndex(rightNodeId);
			rightNode.setParentId(parent.getId());
			dataManager_->updateIndexEntity(parent);
			dataManager_->updateIndexEntity(rightNode);
		} else {
			// Case 3: Parent is full and must also be split ("Push-Up").
			auto allChildren = parent.getAllChildren(dataManager_);
			auto it = std::lower_bound(
					allChildren.begin() + 1, allChildren.end(), key,
					[&](const Index::ChildEntry &a, const PropertyValue &b) { return keyComparator_(a.key, b); });
			allChildren.insert(it, newChildEntry);

			// The median entry is PUSHED UP. Its key goes to the grandparent,
			// and it is REMOVED from this level.
			size_t midPointIdx = allChildren.size() / 2;
			Index::ChildEntry promotedEntry = std::move(allChildren[midPointIdx]);

			// Create the new sibling internal node.
			int64_t newInternalId = createNewNode(Index::NodeType::INTERNAL);
			auto newInternalNode = dataManager_->getIndex(newInternalId);
			newInternalNode.setParentId(parent.getParentId());
			newInternalNode.setLevel(parent.getLevel());

			// The new sibling gets the children to the RIGHT of the promoted entry.
			// The first child of the new sibling is the one pointed to by the promoted entry.
			std::vector<Index::ChildEntry> rightChildren;
			rightChildren.reserve(allChildren.size() - midPointIdx);
			// The first child is special: it's the one from the promoted entry, but with an implicit key.
			Index::ChildEntry firstRightChild{PropertyValue(std::monostate{}), promotedEntry.childId, 0};
			rightChildren.push_back(firstRightChild);
			// The rest of the children are moved from the original list.
			std::move(allChildren.begin() + midPointIdx + 1, allChildren.end(), std::back_inserter(rightChildren));

			// This will correctly handle any blobs for keys in rightChildren.
			newInternalNode.setAllChildren(rightChildren, dataManager_);

			// The old node (parent) now contains the children BEFORE the midpoint.
			allChildren.erase(allChildren.begin() + midPointIdx, allChildren.end());
			parent.setAllChildren(allChildren, dataManager_);

			// Re-parent the children that were moved.
			for (const auto &childEntry: newInternalNode.getAllChildren(dataManager_)) {
				auto child = dataManager_->getIndex(childEntry.childId);
				child.setParentId(newInternalId);
				dataManager_->updateIndexEntity(child);
			}

			dataManager_->updateIndexEntity(parent);
			dataManager_->updateIndexEntity(newInternalNode);

			// Recursively call insert on the grandparent, using the key from the promoted entry.
			insertIntoParent(parent, promotedEntry.key, newInternalId, rootId);
		}
	}

	int64_t IndexTreeManager::insert(int64_t rootId, const PropertyValue &key, const int64_t value) {
		std::unique_lock lock(mutex_);

		if (rootId == 0) {
			rootId = initialize();
		}

		int64_t leafId = findLeafNode(rootId, key);
		if (leafId == 0) {
			rootId = initialize();
			leafId = rootId;
		}

		auto leaf = dataManager_->getIndex(leafId);

		if (leaf.wouldLeafOverflowOnInsert(key, value, dataManager_, keyComparator_)) {
			splitLeaf(leaf, key, value, rootId);
		} else {
			leaf.insertEntry(key, value, dataManager_, keyComparator_);
			dataManager_->updateIndexEntity(leaf);
		}

		return rootId;
	}

	bool IndexTreeManager::remove(int64_t rootId, const PropertyValue &key, int64_t value) {
		std::unique_lock lock(mutex_);
		if (rootId == 0)
			return false;
		int64_t leafId = findLeafNode(rootId, key);
		if (leafId == 0)
			return false;

		auto leaf = dataManager_->getIndex(leafId);
		bool removed = leaf.removeEntry(key, value, dataManager_, keyComparator_);

		if (removed) {
			dataManager_->updateIndexEntity(leaf);
			// TODO: A full implementation would check if the leaf is now under-full
			// and trigger a merge or redistribution with its siblings.
		}
		return removed;
	}

	std::vector<int64_t> IndexTreeManager::find(int64_t rootId, const PropertyValue &key) {
		std::shared_lock lock(mutex_);
		if (rootId == 0)
			return {};
		int64_t leafId = findLeafNode(rootId, key);
		if (leafId == 0)
			return {};
		auto leaf = dataManager_->getIndex(leafId);
		return leaf.findValues(key, dataManager_, keyComparator_);
	}

	std::vector<int64_t> IndexTreeManager::findRange(int64_t rootId, const PropertyValue &minKey,
													 const PropertyValue &maxKey) {
		std::shared_lock lock(mutex_);
		if (rootId == 0)
			return {};

		int64_t leafId = findLeafNode(rootId, minKey);
		if (leafId == 0)
			return {};

		std::vector<int64_t> results;
		bool continueScan = true;

		while (leafId != 0 && continueScan) {
			auto leaf = dataManager_->getIndex(leafId);
			auto allEntries = leaf.getAllEntries(dataManager_);

			for (const auto &entry: allEntries) {
				if (keyComparator_(entry.key, minKey))
					continue; // Skip if less than minKey
				if (keyComparator_(maxKey, entry.key)) { // Stop if greater than maxKey
					continueScan = false;
					break;
				}
				results.insert(results.end(), entry.values.begin(), entry.values.end());
			}
			if (continueScan)
				leafId = leaf.getNextLeafId();
		}
		return results;
	}

} // namespace graph::query::indexes
