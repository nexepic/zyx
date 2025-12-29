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
		return createNewNode(Index::NodeType::LEAF);
	}

	void IndexTreeManager::clear(const int64_t rootId) const {
		if (rootId == 0)
			return;

		std::vector<int64_t> toDelete;
		toDelete.push_back(rootId);

		for (size_t i = 0; i < toDelete.size(); ++i) {
			auto entity = dataManager_->getIndex(toDelete[i]);
			if (entity.getId() == 0)
				continue;

			if (entity.isLeaf()) {
				auto allEntries = entity.getAllEntries(dataManager_);
				for (const auto &entry: allEntries) {
					if (entry.keyBlobId != 0)
						dataManager_->getBlobManager()->deleteBlobChain(entry.keyBlobId);
					if (entry.valuesBlobId != 0)
						dataManager_->getBlobManager()->deleteBlobChain(entry.valuesBlobId);
				}
			} else {
				auto allChildren = entity.getAllChildren(dataManager_);
				for (const auto &child: allChildren) {
					if (child.keyBlobId != 0)
						dataManager_->getBlobManager()->deleteBlobChain(child.keyBlobId);
					toDelete.push_back(child.childId);
				}
			}
			dataManager_->deleteIndex(entity);
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
			int64_t childId = current.findChild(key, dataManager_, keyComparator_);
			if (childId == 0)
				return 0;
			currentId = childId;
		}
	}

	void IndexTreeManager::splitLeaf(Index &leaf, const PropertyValue &newKey, int64_t newValue, int64_t &rootId) {
		auto allEntries = leaf.getAllEntries(dataManager_);
		auto it = std::lower_bound(
				allEntries.begin(), allEntries.end(), newKey,
				[&](const Index::Entry &a, const PropertyValue &b) { return keyComparator_(a.key, b); });

		bool keyExists =
				(it != allEntries.end() && !keyComparator_(newKey, it->key) && !keyComparator_(it->key, newKey));

		if (keyExists) {
			if (std::ranges::find(it->values, newValue) == it->values.end()) {
				it->values.push_back(newValue);
			}
		} else {
			allEntries.insert(it, {newKey, {newValue}, 0, 0});
		}

		int64_t newLeafId = createNewNode(Index::NodeType::LEAF);
		auto newLeaf = dataManager_->getIndex(newLeafId);
		newLeaf.setLevel(leaf.getLevel());
		newLeaf.setParentId(leaf.getParentId());

		size_t midPointIdx = allEntries.size() / 2;
		PropertyValue promotedKey = allEntries[midPointIdx].key;

		std::vector<Index::Entry> leftEntries(allEntries.begin(), allEntries.begin() + midPointIdx);
		std::vector<Index::Entry> rightEntries(allEntries.begin() + midPointIdx, allEntries.end());

		leaf.setAllEntries(leftEntries, dataManager_);
		newLeaf.setAllEntries(rightEntries, dataManager_);

		newLeaf.setNextLeafId(leaf.getNextLeafId());
		newLeaf.setPrevLeafId(leaf.getId());
		if (leaf.getNextLeafId() != 0) {
			auto nextLeaf = dataManager_->getIndex(leaf.getNextLeafId());
			nextLeaf.setPrevLeafId(newLeafId);
			dataManager_->updateIndexEntity(nextLeaf);
		}
		leaf.setNextLeafId(newLeafId);

		dataManager_->updateIndexEntity(leaf);
		dataManager_->updateIndexEntity(newLeaf);

		insertIntoParent(leaf, promotedKey, newLeafId, rootId);
	}

	void IndexTreeManager::insertIntoParent(Index &leftNode, const PropertyValue &key, int64_t rightNodeId,
											int64_t &rootId) {
		if (leftNode.getParentId() == 0) {
			int64_t newRootId = createNewNode(Index::NodeType::INTERNAL);
			auto newRoot = dataManager_->getIndex(newRootId);
			rootId = newRootId;
			newRoot.setLevel(leftNode.getLevel() + 1);

			auto leftNodeFromDb = dataManager_->getIndex(leftNode.getId());
			auto rightNodeFromDb = dataManager_->getIndex(rightNodeId);

			leftNodeFromDb.setParentId(newRootId);
			rightNodeFromDb.setParentId(newRootId);

			Index::ChildEntry newChildEntry{key, rightNodeId, 0};
			Index::ChildEntry firstChild{PropertyValue(std::monostate{}), leftNode.getId(), 0};

			std::vector<Index::ChildEntry> children = {firstChild, newChildEntry};
			newRoot.setAllChildren(children, dataManager_);

			dataManager_->updateIndexEntity(leftNodeFromDb);
			dataManager_->updateIndexEntity(rightNodeFromDb);
			dataManager_->updateIndexEntity(newRoot);
			return;
		}

		auto parent = dataManager_->getIndex(leftNode.getParentId());
		Index::ChildEntry newChildEntry{key, rightNodeId, 0};

		if (!parent.wouldInternalOverflowOnAddChild(newChildEntry, dataManager_)) {
			parent.addChild(newChildEntry, dataManager_, keyComparator_);
			auto rightNode = dataManager_->getIndex(rightNodeId);
			rightNode.setParentId(parent.getId());
			dataManager_->updateIndexEntity(parent);
			dataManager_->updateIndexEntity(rightNode);
		} else {
			auto allChildren = parent.getAllChildren(dataManager_);
			auto it = std::lower_bound(
					allChildren.begin() + 1, allChildren.end(), key,
					[&](const Index::ChildEntry &a, const PropertyValue &b) { return keyComparator_(a.key, b); });
			allChildren.insert(it, newChildEntry);

			size_t midPointIdx = allChildren.size() / 2;
			Index::ChildEntry promotedEntry = std::move(allChildren[midPointIdx]);

			int64_t newInternalId = createNewNode(Index::NodeType::INTERNAL);
			auto newInternalNode = dataManager_->getIndex(newInternalId);
			newInternalNode.setParentId(parent.getParentId());
			newInternalNode.setLevel(parent.getLevel());

			std::vector<Index::ChildEntry> rightChildren;
			rightChildren.reserve(allChildren.size() - midPointIdx);

			Index::ChildEntry firstRightChild{PropertyValue(std::monostate{}), promotedEntry.childId, 0};
			rightChildren.push_back(firstRightChild);
			std::move(allChildren.begin() + midPointIdx + 1, allChildren.end(), std::back_inserter(rightChildren));

			newInternalNode.setAllChildren(rightChildren, dataManager_);

			allChildren.erase(allChildren.begin() + midPointIdx, allChildren.end());
			parent.setAllChildren(allChildren, dataManager_);

			for (const auto &childEntry: newInternalNode.getAllChildren(dataManager_)) {
				auto child = dataManager_->getIndex(childEntry.childId);
				child.setParentId(newInternalId);
				dataManager_->updateIndexEntity(child);
			}

			dataManager_->updateIndexEntity(parent);
			dataManager_->updateIndexEntity(newInternalNode);

			insertIntoParent(parent, promotedEntry.key, newInternalId, rootId);
		}
	}

	void IndexTreeManager::handleUnderflow(Index &node, int64_t &rootId) {
		if (node.getParentId() == 0) {
			if (!node.isLeaf() && node.getChildCount() == 1) {
				auto children = node.getAllChildren(dataManager_);
				int64_t newRootId = children[0].childId;

				auto newRoot = dataManager_->getIndex(newRootId);
				newRoot.setParentId(0);
				dataManager_->updateIndexEntity(newRoot);

				rootId = newRootId;

				std::vector<Index::ChildEntry> emptyChildren;
				node.setAllChildren(emptyChildren, dataManager_);
				dataManager_->deleteIndex(node);
			}
			return;
		}

		auto parent = dataManager_->getIndex(node.getParentId());
		auto parentChildren = parent.getAllChildren(dataManager_);

		auto it =
				std::ranges::find_if(parentChildren, [&](const auto &child) { return child.childId == node.getId(); });

		if (it == parentChildren.end())
			return;

		size_t index = std::distance(parentChildren.begin(), it);

		int64_t leftSiblingId = (index > 0) ? parentChildren[index - 1].childId : 0;
		int64_t rightSiblingId = (index < parentChildren.size() - 1) ? parentChildren[index + 1].childId : 0;

		if (rightSiblingId != 0) {
			auto rightSibling = dataManager_->getIndex(rightSiblingId);
			if (!rightSibling.isUnderflow(UNDERFLOW_THRESHOLD_RATIO + 0.1)) {
				redistribute(node, rightSibling, parent, false);
				return;
			}
		}

		if (leftSiblingId != 0) {
			auto leftSibling = dataManager_->getIndex(leftSiblingId);
			if (!leftSibling.isUnderflow(UNDERFLOW_THRESHOLD_RATIO + 0.1)) {
				redistribute(node, leftSibling, parent, true);
				return;
			}
		}

		if (rightSiblingId != 0) {
			auto rightSibling = dataManager_->getIndex(rightSiblingId);
			PropertyValue separator = parentChildren[index + 1].key;
			mergeNodes(node, rightSibling, parent, separator, rootId);
		} else if (leftSiblingId != 0) {
			auto leftSibling = dataManager_->getIndex(leftSiblingId);
			PropertyValue separator = parentChildren[index].key;
			mergeNodes(leftSibling, node, parent, separator, rootId);
		}
	}

	void IndexTreeManager::redistribute(Index &node, Index &sibling, Index &parent, bool isLeftSibling) {
		auto parentChildren = parent.getAllChildren(dataManager_);

		auto nodeIt = std::ranges::find_if(parentChildren, [&](const auto &c) { return c.childId == node.getId(); });
		size_t nodeIdx = std::distance(parentChildren.begin(), nodeIt);

		if (isLeftSibling) {
			if (node.isLeaf()) {
				auto nodeEntries = node.getAllEntries(dataManager_);
				auto siblingEntries = sibling.getAllEntries(dataManager_);

				Index::Entry borrowed = std::move(siblingEntries.back());
				siblingEntries.pop_back();
				nodeEntries.insert(nodeEntries.begin(), std::move(borrowed));

				parentChildren[nodeIdx].key = nodeEntries.front().key;

				node.setAllEntries(nodeEntries, dataManager_);
				sibling.setAllEntries(siblingEntries, dataManager_);
			} else { // Internal
				auto nodeChildren = node.getAllChildren(dataManager_);
				auto siblingChildren = sibling.getAllChildren(dataManager_);

				Index::ChildEntry borrowedChildFromSibling = std::move(siblingChildren.back());
				siblingChildren.pop_back();

				PropertyValue oldParentSeparator = parentChildren[nodeIdx].key;
				parentChildren[nodeIdx].key = borrowedChildFromSibling.key;

				nodeChildren.front().key = oldParentSeparator;
				borrowedChildFromSibling.key = PropertyValue(std::monostate{});
				nodeChildren.insert(nodeChildren.begin(), std::move(borrowedChildFromSibling));

				auto movedChildNode = dataManager_->getIndex(nodeChildren.front().childId);
				movedChildNode.setParentId(node.getId());
				dataManager_->updateIndexEntity(movedChildNode);

				node.setAllChildren(nodeChildren, dataManager_);
				sibling.setAllChildren(siblingChildren, dataManager_);
			}
		} else {
			size_t rightIdx = nodeIdx + 1;

			if (node.isLeaf()) {
				auto nodeEntries = node.getAllEntries(dataManager_);
				auto siblingEntries = sibling.getAllEntries(dataManager_);

				Index::Entry borrowed = std::move(siblingEntries.front());
				siblingEntries.erase(siblingEntries.begin());
				nodeEntries.push_back(std::move(borrowed));

				parentChildren[rightIdx].key = siblingEntries.front().key;

				node.setAllEntries(nodeEntries, dataManager_);
				sibling.setAllEntries(siblingEntries, dataManager_);
			} else { // Internal
				auto nodeChildren = node.getAllChildren(dataManager_);
				auto siblingChildren = sibling.getAllChildren(dataManager_);

				PropertyValue oldParentSeparator = parentChildren[rightIdx].key;
				Index::ChildEntry borrowedChildFromSibling = std::move(siblingChildren.front());
				siblingChildren.erase(siblingChildren.begin());

				parentChildren[rightIdx].key = siblingChildren.front().key;

				borrowedChildFromSibling.key = oldParentSeparator;
				nodeChildren.push_back(std::move(borrowedChildFromSibling));

				siblingChildren.front().key = PropertyValue(std::monostate{});

				auto movedChildNode = dataManager_->getIndex(nodeChildren.back().childId);
				movedChildNode.setParentId(node.getId());
				dataManager_->updateIndexEntity(movedChildNode);

				node.setAllChildren(nodeChildren, dataManager_);
				sibling.setAllChildren(siblingChildren, dataManager_);
			}
		}

		parent.setAllChildren(parentChildren, dataManager_);
		dataManager_->updateIndexEntity(parent);
		dataManager_->updateIndexEntity(node);
		dataManager_->updateIndexEntity(sibling);
	}

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
				auto nextNext = dataManager_->getIndex(rightNode.getNextLeafId());
				nextNext.setPrevLeafId(leftNode.getId());
				dataManager_->updateIndexEntity(nextNext);
			}
		} else { // Internal Merge
			auto leftChildren = leftNode.getAllChildren(dataManager_);
			auto rightChildren = rightNode.getAllChildren(dataManager_);

			Index::ChildEntry separatorChildEntry{separatorKey, rightChildren.front().childId, 0};
			rightChildren.erase(rightChildren.begin());

			leftChildren.push_back(std::move(separatorChildEntry));
			leftChildren.insert(leftChildren.end(), std::make_move_iterator(rightChildren.begin()),
								std::make_move_iterator(rightChildren.end()));

			leftNode.setAllChildren(leftChildren, dataManager_);

			for (const auto &c: leftNode.getAllChildren(dataManager_)) {
				auto child = dataManager_->getIndex(c.childId);
				if (child.getParentId() != leftNode.getId()) {
					child.setParentId(leftNode.getId());
					dataManager_->updateIndexEntity(child);
				}
			}
		}

		auto parentChildren = parent.getAllChildren(dataManager_);
		auto rightNodeChildIt =
				std::ranges::find_if(parentChildren, [&](const auto &c) { return c.childId == rightNode.getId(); });

		if (rightNodeChildIt != parentChildren.end()) {
			parentChildren.erase(rightNodeChildIt);
			parent.setAllChildren(parentChildren, dataManager_);
			dataManager_->updateIndexEntity(parent);
		} else {
			throw std::logic_error("Parent does not contain right sibling in merge (Severe Corruption)");
		}

		dataManager_->updateIndexEntity(leftNode);

		if (rightNode.isLeaf()) {
			std::vector<Index::Entry> empty;
			rightNode.setAllEntries(empty, dataManager_);
		} else {
			std::vector<Index::ChildEntry> empty;
			rightNode.setAllChildren(empty, dataManager_);
		}
		dataManager_->deleteIndex(rightNode);

		if (parent.getParentId() == 0) {
			handleUnderflow(parent, rootId);
		} else {
			if (parent.isUnderflow(UNDERFLOW_THRESHOLD_RATIO)) {
				handleUnderflow(parent, rootId);
			}
		}
	}

	int64_t IndexTreeManager::insertBatch(int64_t rootId,
										  const std::vector<std::pair<PropertyValue, int64_t>> &entries) {
		if (entries.empty())
			return rootId;
		std::unique_lock lock(mutex_);
		if (rootId == 0)
			rootId = initialize();

		auto sortedEntries = entries;
		std::ranges::sort(sortedEntries, [&](const auto &a, const auto &b) {
			if (keyComparator_(a.first, b.first))
				return true;
			if (keyComparator_(b.first, a.first))
				return false;
			return a.second < b.second;
		});

		for (const auto &[key, value]: sortedEntries) {
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
		}
		return rootId;
	}

	int64_t IndexTreeManager::insert(int64_t rootId, const PropertyValue &key, const int64_t value) {
		std::unique_lock lock(mutex_);
		if (rootId == 0)
			rootId = initialize();

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

	bool IndexTreeManager::remove(int64_t &rootId, const PropertyValue &key, int64_t value) {
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
			// Trigger rebalancing only if the node is under-full and not the root
			if (leaf.getId() != rootId && leaf.isUnderflow(UNDERFLOW_THRESHOLD_RATIO)) {
				handleUnderflow(leaf, rootId);
			} else if (leaf.getId() == rootId) {
				// Special check for Root Leaf becoming empty
				if (leaf.getEntryCount() == 0) {
					// Optional: You might want to keep an empty root, or delete it.
					// For now, keeping it is safer.
				}
			}
		}
		return removed;
	}

	std::vector<int64_t> IndexTreeManager::find(int64_t rootId, const PropertyValue &key) const {
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
													 const PropertyValue &maxKey) const {
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
					continue;
				if (keyComparator_(maxKey, entry.key)) {
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
