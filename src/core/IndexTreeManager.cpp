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
#include <cassert>
#include <ranges>
#include <stdexcept>
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::indexes {

	std::function<bool(const PropertyValue &, const PropertyValue &)> makeComparator(PropertyType type) {
		switch (type) {
			case PropertyType::INTEGER:
				return [](const auto &a, const auto &b) {
					// Ensure we're comparing the actual integer values
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
		// Get all current entries
		auto allEntries = leaf.getAllEntries(dataManager_);

		// Find the position to insert the new entry
		auto it = std::lower_bound(
				allEntries.begin(), allEntries.end(), newKey,
				[&](const Index::Entry &a, const PropertyValue &b) { return keyComparator_(a.key, b); });

		// Check if the key already exists
		bool keyExists =
				(it != allEntries.end() && !keyComparator_(newKey, it->key) && !keyComparator_(it->key, newKey));

		if (keyExists) {
			// Key exists - just add the value if it's not already there
			if (std::find(it->values.begin(), it->values.end(), newValue) == it->values.end()) {
				it->values.push_back(newValue);
			}
		} else {
			// Insert new entry
			allEntries.insert(it, {newKey, {newValue}, 0, 0});
		}

		// Create a new sibling leaf node
		int64_t newLeafId = createNewNode(Index::NodeType::LEAF);
		auto newLeaf = dataManager_->getIndex(newLeafId);
		newLeaf.setLevel(leaf.getLevel());
		newLeaf.setParentId(leaf.getParentId());

		// Find split point
		size_t midPointIdx = allEntries.size() / 2;
		PropertyValue promotedKey = allEntries[midPointIdx].key;

		// Distribute entries
		std::vector<Index::Entry> leftEntries(allEntries.begin(), allEntries.begin() + midPointIdx);
		std::vector<Index::Entry> rightEntries(allEntries.begin() + midPointIdx, allEntries.end());

		// Update both nodes with their new entries
		leaf.setAllEntries(leftEntries, dataManager_);
		newLeaf.setAllEntries(rightEntries, dataManager_);

		// Update the leaf node chain
		newLeaf.setNextLeafId(leaf.getNextLeafId());
		newLeaf.setPrevLeafId(leaf.getId());
		if (leaf.getNextLeafId() != 0) {
			auto nextLeaf = dataManager_->getIndex(leaf.getNextLeafId());
			nextLeaf.setPrevLeafId(newLeafId);
			dataManager_->updateIndexEntity(nextLeaf);
		}
		leaf.setNextLeafId(newLeafId);

		// Save the nodes
		dataManager_->updateIndexEntity(leaf);
		dataManager_->updateIndexEntity(newLeaf);

		// Insert the promoted key into the parent
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

	/**
	 * @brief Handles a node that may be under-full after a deletion.
	 *
	 * This function is the core of the B+Tree rebalancing strategy after a deletion.
	 * It first checks if the root needs to be collapsed, then attempts to redistribute
	 * from a sibling. If redistribution is not possible, it merges the node with a
	 * sibling. This process may propagate recursively up the tree.
	 *
	 * @param node The node to check (can be leaf or internal), passed by reference as it may be modified.
	 * @param rootId Reference to the root ID, as it may change if the root is merged or collapsed.
	 */
	void IndexTreeManager::handleUnderflow(Index &node, int64_t &rootId) {
		// Case 1: The root is the node. The root is allowed to be under-full.
		// However, if it's an internal node with only one child, it's redundant.
		// The root should be removed and its single child becomes the new root.
		if (node.getParentId() == 0) {
			if (!node.isLeaf() && node.getChildCount() == 1) {
				auto children = node.getAllChildren(dataManager_);
				rootId = children[0].childId;

				auto newRoot = dataManager_->getIndex(rootId);
				newRoot.setParentId(0);
				dataManager_->updateIndexEntity(newRoot);

				// FIX: Create a named empty vector to clear owned blobs before deleting.
				std::vector<Index::ChildEntry> emptyChildren;
				node.setAllChildren(emptyChildren, dataManager_);
				dataManager_->deleteIndex(node);
			}
			return;
		}

		// For non-root nodes, get parent and find siblings.
		auto parent = dataManager_->getIndex(node.getParentId());
		auto parentChildren = parent.getAllChildren(dataManager_);

		// Find the node's position within its parent's children array.
		auto it = std::find_if(parentChildren.begin(), parentChildren.end(),
							   [&](const auto &child) { return child.childId == node.getId(); });
		int nodeIndexInParent = std::distance(parentChildren.begin(), it);

		// Try to find a left and right sibling.
		int64_t leftSiblingId = (nodeIndexInParent > 0) ? parentChildren[nodeIndexInParent - 1].childId : 0;
		int64_t rightSiblingId =
				(nodeIndexInParent < parentChildren.size() - 1) ? parentChildren[nodeIndexInParent + 1].childId : 0;

		// Case 2: Try to redistribute from the right sibling.
		if (rightSiblingId != 0) {
			auto rightSibling = dataManager_->getIndex(rightSiblingId);
			if (!rightSibling.isUnderflow(UNDERFLOW_THRESHOLD_RATIO + 0.1)) {
				redistribute(node, rightSibling, parent, false);
				return;
			}
		}

		// Case 3: Try to redistribute from the left sibling.
		if (leftSiblingId != 0) {
			auto leftSibling = dataManager_->getIndex(leftSiblingId);
			if (!leftSibling.isUnderflow(UNDERFLOW_THRESHOLD_RATIO + 0.1)) {
				redistribute(node, leftSibling, parent, true);
				return;
			}
		}

		// Case 4: Redistribution is not possible, so we must merge.
		// Merge with the right sibling if it exists.
		if (rightSiblingId != 0) {
			auto rightSibling = dataManager_->getIndex(rightSiblingId);
			PropertyValue separatorKey = parentChildren[nodeIndexInParent + 1].key;
			mergeNodes(node, rightSibling, parent, separatorKey, rootId);
		} else if (leftSiblingId != 0) {
			// Otherwise, merge with the left sibling.
			auto leftSibling = dataManager_->getIndex(leftSiblingId);
			PropertyValue separatorKey = parentChildren[nodeIndexInParent].key;
			mergeNodes(leftSibling, node, parent, separatorKey, rootId);
		}
	}

	/**
	 * @brief Redistributes entries between two sibling nodes to resolve an underflow.
	 *
	 * This function "borrows" an entry from a sufficiently full sibling node.
	 * The operation involves moving an entry from the sibling, through the parent,
	 * to the under-full node. This is a less costly operation than merging nodes.
	 *
	 * @param node The under-full node that will receive an entry.
	 * @param sibling The sibling node that will provide an entry.
	 * @param parent The common parent of the node and the sibling.
	 * @param isLeftSibling True if the sibling is to the left of the node, false otherwise.
	 */
	void IndexTreeManager::redistribute(Index &node, Index &sibling, Index &parent, bool isLeftSibling) {
		auto parentChildren = parent.getAllChildren(dataManager_);

		if (isLeftSibling) {
			// --- Borrow from the LEFT sibling ---
			auto it = std::find_if(parentChildren.begin(), parentChildren.end(),
								   [&](const auto &c) { return c.childId == node.getId(); });
			PropertyValue separatorKey = it->key;

			if (node.isLeaf()) {
				auto nodeEntries = node.getAllEntries(dataManager_);
				auto siblingEntries = sibling.getAllEntries(dataManager_);

				Index::Entry borrowedEntry = std::move(siblingEntries.back());
				siblingEntries.pop_back();

				// The key of the borrowed entry becomes the new separator in the parent.
				parent.removeChild(separatorKey, dataManager_, keyComparator_);
				Index::ChildEntry newParentEntryForNode = {borrowedEntry.key, node.getId(), 0};
				parent.addChild(newParentEntryForNode, dataManager_, keyComparator_);

				nodeEntries.insert(nodeEntries.begin(), std::move(borrowedEntry));
				node.setAllEntries(nodeEntries, dataManager_);
				sibling.setAllEntries(siblingEntries, dataManager_);
			} else { // Internal node
				auto nodeChildren = node.getAllChildren(dataManager_);
				auto siblingChildren = sibling.getAllChildren(dataManager_);

				Index::ChildEntry borrowedChild = std::move(siblingChildren.back());
				siblingChildren.pop_back();

				// The separator key from the parent comes down and becomes the key for the first child of 'node'.
				// The key for this child was previously implicit.
				nodeChildren.front().key = separatorKey;

				// The borrowed child now becomes the first child of 'node', with an implicit key.
				auto newFirstChild = std::move(borrowedChild);
				newFirstChild.key = PropertyValue(std::monostate{}); // It's the first child, key is implicit.
				nodeChildren.insert(nodeChildren.begin(), std::move(newFirstChild));

				// The key that was with the borrowed child in the sibling moves up to be the new separator.
				parent.removeChild(separatorKey, dataManager_, keyComparator_);
				Index::ChildEntry newParentEntryForNode = {borrowedChild.key, node.getId(), 0};
				parent.addChild(newParentEntryForNode, dataManager_, keyComparator_);

				// Update parent pointer of the moved child.
				auto movedChildNode = dataManager_->getIndex(newFirstChild.childId);
				movedChildNode.setParentId(node.getId());
				dataManager_->updateIndexEntity(movedChildNode);

				node.setAllChildren(nodeChildren, dataManager_);
				sibling.setAllChildren(siblingChildren, dataManager_);
			}
		} else {
			// --- Borrow from the RIGHT sibling ---
			auto it = std::find_if(parentChildren.begin(), parentChildren.end(),
								   [&](const auto &c) { return c.childId == sibling.getId(); });
			PropertyValue separatorKey = it->key;

			if (node.isLeaf()) {
				auto nodeEntries = node.getAllEntries(dataManager_);
				auto siblingEntries = sibling.getAllEntries(dataManager_);

				Index::Entry borrowedEntry = std::move(siblingEntries.front());
				siblingEntries.erase(siblingEntries.begin());

				// The key of the *new* first entry in the right sibling becomes the new parent separator.
				parent.removeChild(separatorKey, dataManager_, keyComparator_);
				Index::ChildEntry newParentEntryForSibling = {siblingEntries.front().key, sibling.getId(), 0};
				parent.addChild(newParentEntryForSibling, dataManager_, keyComparator_);

				// The old separator from the parent comes down with the borrowed entry.
				// The key of the borrowed entry is its own key.
				nodeEntries.push_back(std::move(borrowedEntry));

				node.setAllEntries(nodeEntries, dataManager_);
				sibling.setAllEntries(siblingEntries, dataManager_);
			} else { // Internal node
				auto nodeChildren = node.getAllChildren(dataManager_);
				auto siblingChildren = sibling.getAllChildren(dataManager_);

				Index::ChildEntry borrowedChild = std::move(siblingChildren.front());
				siblingChildren.erase(siblingChildren.begin());

				// The separator from the parent comes down to become the key for the child we are borrowing.
				borrowedChild.key = separatorKey;
				nodeChildren.push_back(std::move(borrowedChild));

				// The key that was with the borrowed child moves up to become the new separator for the sibling.
				// This key is now the key of the new first child of the sibling.
				parent.removeChild(separatorKey, dataManager_, keyComparator_);
				Index::ChildEntry newParentEntryForSibling = {siblingChildren.front().key, sibling.getId(), 0};
				parent.addChild(newParentEntryForSibling, dataManager_, keyComparator_);

				// The key of the new first child of the sibling becomes implicit.
				siblingChildren.front().key = PropertyValue(std::monostate{});

				// Update parent pointer of the moved child.
				auto movedChildNode = dataManager_->getIndex(borrowedChild.childId);
				movedChildNode.setParentId(node.getId());
				dataManager_->updateIndexEntity(movedChildNode);

				node.setAllChildren(nodeChildren, dataManager_);
				sibling.setAllChildren(siblingChildren, dataManager_);
			}
		}

		dataManager_->updateIndexEntity(parent);
		dataManager_->updateIndexEntity(node);
		dataManager_->updateIndexEntity(sibling);
	}

	/**
	 * @brief Merges two adjacent nodes (leftNode and rightNode) into one.
	 *
	 * This function is called when redistribution is not possible. It moves all entries
	 * from the right node into the left node, along with the separating key from the parent.
	 * The right node is then deleted. This operation reduces the number of nodes in the
	 * tree and may cause the parent to underflow, triggering a recursive call to handleUnderflow.
	 *
	 * @param leftNode The left node of the pair, which will receive the merged content.
	 * @param rightNode The right node of the pair, which will be emptied and deleted.
	 * @param parent The common parent of the two nodes.
	 * @param separatorKey The key in the parent that separates the two nodes.
	 * @param rootId Reference to the root ID, as it may change.
	 */
	void IndexTreeManager::mergeNodes(Index &leftNode, Index &rightNode, Index &parent,
									  const PropertyValue &separatorKey, int64_t &rootId) {
		if (leftNode.isLeaf() != rightNode.isLeaf()) {
			throw std::logic_error("Attempting to merge a leaf with a non-leaf node.");
		}

		if (leftNode.isLeaf()) {
			auto leftEntries = leftNode.getAllEntries(dataManager_);
			auto rightEntries = rightNode.getAllEntries(dataManager_);

			leftEntries.insert(leftEntries.end(), std::make_move_iterator(rightEntries.begin()),
							   std::make_move_iterator(rightEntries.end()));

			leftNode.setAllEntries(leftEntries, dataManager_);
			leftNode.setNextLeafId(rightNode.getNextLeafId());

			if (rightNode.getNextLeafId() != 0) {
				auto nextNextLeaf = dataManager_->getIndex(rightNode.getNextLeafId());
				nextNextLeaf.setPrevLeafId(leftNode.getId());
				dataManager_->updateIndexEntity(nextNextLeaf);
			}
		} else {
			auto leftChildren = leftNode.getAllChildren(dataManager_);
			auto rightChildren = rightNode.getAllChildren(dataManager_);

			auto firstChildFromRight = std::move(rightChildren.front());
			rightChildren.erase(rightChildren.begin());
			firstChildFromRight.key = separatorKey;

			leftChildren.push_back(std::move(firstChildFromRight));
			leftChildren.insert(leftChildren.end(), std::make_move_iterator(rightChildren.begin()),
								std::make_move_iterator(rightChildren.end()));

			leftNode.setAllChildren(leftChildren, dataManager_);

			auto finalChildren = leftNode.getAllChildren(dataManager_);
			for (const auto &childEntry: finalChildren) {
				auto childNode = dataManager_->getIndex(childEntry.childId);
				if (childNode.getParentId() != leftNode.getId()) {
					childNode.setParentId(leftNode.getId());
					dataManager_->updateIndexEntity(childNode);
				}
			}
		}

		parent.removeChild(separatorKey, dataManager_, keyComparator_);
		dataManager_->updateIndexEntity(parent);
		dataManager_->updateIndexEntity(leftNode);

		// The right node is now empty. Clean up its owned blobs and then delete the node.
		// FIX: Create a named empty vector to pass to the function.
		if (rightNode.isLeaf()) {
			std::vector<Index::Entry> emptyEntries;
			rightNode.setAllEntries(emptyEntries, dataManager_);
		} else {
			std::vector<Index::ChildEntry> emptyChildren;
			rightNode.setAllChildren(emptyChildren, dataManager_);
		}
		dataManager_->deleteIndex(rightNode);

		// Only handle underflow if the parent is now underfull (and not root)
		if (parent.isUnderflow(UNDERFLOW_THRESHOLD_RATIO) && parent.getParentId() != 0) {
			handleUnderflow(parent, rootId);
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

		// Check if insertion would overflow the leaf
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
		if (rootId == 0) {
			return false;
		}

		int64_t leafId = findLeafNode(rootId, key);
		if (leafId == 0) {
			return false;
		}

		auto leaf = dataManager_->getIndex(leafId);
		bool removed = leaf.removeEntry(key, value, dataManager_, keyComparator_);

		if (removed) {
			dataManager_->updateIndexEntity(leaf);
			// Trigger rebalancing only if the node is under-full and not the root
			if (leaf.getId() != rootId && leaf.isUnderflow(UNDERFLOW_THRESHOLD_RATIO)) {
				handleUnderflow(leaf, rootId);
			}
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
