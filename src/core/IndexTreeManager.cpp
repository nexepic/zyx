/**
 * @file IndexTreeManager.cpp
 * @author Nexepic
 * @date 2025/6/13
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
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

	void IndexTreeManager::splitLeaf(Index &leaf, std::vector<Index::Entry> &allEntries, int64_t &rootId) {
		// 1. Create Sibling Node
		int64_t newLeafId = createNewNode(Index::NodeType::LEAF);
		Index newLeaf = dataManager_->getIndex(newLeafId);

		// 2. Distribute Entries (50/50 split)
		size_t mid = allEntries.size() / 2;

		// Entries for old leaf (Left)
		std::vector<Index::Entry> leftEntries(allEntries.begin(), allEntries.begin() + mid);
		// Entries for new leaf (Right)
		std::vector<Index::Entry> rightEntries(allEntries.begin() + mid, allEntries.end());

		// 3. Update Leaf Links
		newLeaf.setNextLeafId(leaf.getNextLeafId());
		newLeaf.setPrevLeafId(leaf.getId());

		// If there was a next leaf, update its prev pointer
		if (leaf.getNextLeafId() != 0) {
			Index nextLeaf = dataManager_->getIndex(leaf.getNextLeafId());
			nextLeaf.setPrevLeafId(newLeafId);
			dataManager_->updateIndexEntity(nextLeaf);
		}
		leaf.setNextLeafId(newLeafId);

		// 4. Write Data to Nodes
		// Note: setAllEntries handles serialization and blob management
		leaf.setAllEntries(leftEntries, dataManager_);
		newLeaf.setAllEntries(rightEntries, dataManager_);

		// 5. Propagate to Parent
		// The separator key is the first key of the right node
		PropertyValue separatorKey = rightEntries.front().key;

		// Ensure new node knows its parent
		newLeaf.setParentId(leaf.getParentId());

		// Save changes
		dataManager_->updateIndexEntity(leaf);
		dataManager_->updateIndexEntity(newLeaf);

		// 6. Insert into Parent
		insertIntoParent(leaf, separatorKey, newLeafId, rootId);
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

	std::vector<Index::Entry>
	coalesceRawEntries(const std::vector<std::pair<PropertyValue, int64_t>> &rawEntries,
					   const std::function<bool(const PropertyValue &, const PropertyValue &)> &comparator) {
		if (rawEntries.empty())
			return {};

		// 1. Sort raw entries
		auto sortedEntries = rawEntries;
		std::ranges::sort(sortedEntries, [&](const auto &a, const auto &b) {
			if (comparator(a.first, b.first))
				return true;
			if (comparator(b.first, a.first))
				return false;
			return a.second < b.second; // Secondary sort by value
		});

		// 2. Coalesce
		std::vector<Index::Entry> coalesced;
		coalesced.reserve(sortedEntries.size());

		for (const auto &[key, value]: sortedEntries) {
			if (!coalesced.empty()) {
				auto &last = coalesced.back();
				// Check if key matches last key (Using !less(a,b) && !less(b,a) for equality)
				if (!comparator(last.key, key) && !comparator(key, last.key)) {
					// Avoid duplicate values for the same key
					if (last.values.empty() || last.values.back() != value) {
						last.values.push_back(value);
					}
					continue;
				}
			}
			// New key
			coalesced.push_back({key, {value}, 0, 0});
		}
		return coalesced;
	}

	int64_t IndexTreeManager::insertBatch(int64_t rootId,
										  const std::vector<std::pair<PropertyValue, int64_t>> &entries) {
		if (entries.empty())
			return rootId;
		std::unique_lock lock(mutex_);
		if (rootId == 0)
			rootId = initialize();

		// --- STEP 1: Pre-process (Sort & Coalesce) ---
		std::vector<Index::Entry> coalescedEntries = coalesceRawEntries(entries, keyComparator_);

		// --- STEP 2: Group by target Leaf Node ---
		// Map: LeafID -> List of NEW Entries to be inserted there
		std::map<int64_t, std::vector<Index::Entry>> entriesByLeaf;

		for (const auto &entry: coalescedEntries) {
			int64_t leafId = findLeafNode(rootId, entry.key);
			if (leafId == 0)
				leafId = rootId; // Should catch root init cases
			entriesByLeaf[leafId].push_back(entry);
		}

		// --- STEP 3: Process each leaf ---
		for (auto &[leafId, newEntries]: entriesByLeaf) {
			auto leaf = dataManager_->getIndex(leafId);

			// A. Load ALL existing entries
			auto allEntries = leaf.getAllEntries(dataManager_);

			// B. Merge NEW into EXISTING (Memory Merge)
			// Since both vectors are sorted by key, we can do a linear merge.
			// We also need to merge values if keys match.
			std::vector<Index::Entry> merged;
			merged.reserve(allEntries.size() + newEntries.size());

			auto existIt = allEntries.begin();
			auto newIt = newEntries.begin();

			while (existIt != allEntries.end() || newIt != newEntries.end()) {
				if (existIt != allEntries.end() &&
					(newIt == newEntries.end() || keyComparator_(existIt->key, newIt->key))) {
					merged.push_back(std::move(*existIt));
					++existIt;
				} else if (newIt != newEntries.end() &&
						   (existIt == allEntries.end() || keyComparator_(newIt->key, existIt->key))) {
					merged.push_back(std::move(*newIt));
					++newIt;
				} else {
					// Keys are equal: Merge values
					existIt->values.insert(existIt->values.end(), newIt->values.begin(), newIt->values.end());
					// Deduplicate values (optional but recommended)
					std::ranges::sort(existIt->values);
					auto last = std::ranges::unique(existIt->values).begin();
					existIt->values.erase(last, existIt->values.end());

					merged.push_back(std::move(*existIt));
					++existIt;
					++newIt;
				}
			}

			// C. Calculate Size & Check Overflow
			size_t totalSize = 0;
			for (const auto &e: merged)
				totalSize += Index::getEntrySerializedSize(e);

			if (totalSize <= Index::DATA_SIZE) {
				// Happy Path: Everything fits
				leaf.setAllEntries(merged, dataManager_);
				dataManager_->updateIndexEntity(leaf);
			} else {
				// --- STEP 4: Bulk Split (Fixes "Massive Split" / "Sorted Batch" failures) ---
				// We have too much data for one node. We need to distribute 'merged' into N nodes.

				// 1. Rewrite the current leaf (leafId) with the first chunk
				size_t currentSize = 0;
				auto it = merged.begin();
				std::vector<Index::Entry> chunk;

				// Fill current leaf ~75% full to allow future inserts, or until full
				// NOTE: Being too aggressive with filling (100%) can cause immediate splits on next insert.
				// Using a fill factor helps.
				size_t fillLimit = static_cast<size_t>(Index::DATA_SIZE * 0.9);

				while (it != merged.end()) {
					size_t entrySize = Index::getEntrySerializedSize(*it);
					if (currentSize + entrySize > fillLimit && !chunk.empty()) {
						break;
					}
					currentSize += entrySize;
					chunk.push_back(std::move(*it));
					++it;
				}

				leaf.setAllEntries(chunk, dataManager_);
				dataManager_->updateIndexEntity(leaf);

				// 2. Create new leaves for the rest
				int64_t prevLeafId = leaf.getId();

				while (it != merged.end()) {
					chunk.clear();
					currentSize = 0;

					// The first key of the new node is the separator/promoted key
					PropertyValue separatorKey = it->key;

					// Create new leaf
					int64_t newLeafId = createNewNode(Index::NodeType::LEAF);
					auto newLeaf = dataManager_->getIndex(newLeafId);
					newLeaf.setLevel(leaf.getLevel());
					newLeaf.setParentId(leaf.getParentId()); // Initially share parent

					// Fill new leaf
					while (it != merged.end()) {
						size_t entrySize = Index::getEntrySerializedSize(*it);
						if (currentSize + entrySize > fillLimit && !chunk.empty()) {
							break;
						}
						currentSize += entrySize;
						chunk.push_back(std::move(*it));
						++it;
					}

					newLeaf.setAllEntries(chunk, dataManager_);

					// Linking:
					// newLeaf points back to prevLeafId
					newLeaf.setPrevLeafId(prevLeafId);

					// If this is the last chunk, it inherits the original leaf's next ptr
					// But wait, the original leaf's 'next' is currently pointing to "OldNext".
					// The first new leaf we create is inserted *after* the original leaf.

					// Let's refine the linking logic:
					// Current sequence: Leaf -> [Original Next]
					// We want: Leaf -> NewLeaf1 -> NewLeaf2 -> [Original Next]

					// Read 'prev' node to set its next pointer
					auto prevNode = dataManager_->getIndex(prevLeafId);
					int64_t oldNextId = prevNode.getNextLeafId();

					prevNode.setNextLeafId(newLeafId);
					newLeaf.setNextLeafId(oldNextId); // Tentatively point to old next

					if (oldNextId != 0) {
						auto oldNext = dataManager_->getIndex(oldNextId);
						oldNext.setPrevLeafId(newLeafId);
						dataManager_->updateIndexEntity(oldNext);
					}

					dataManager_->updateIndexEntity(prevNode);
					dataManager_->updateIndexEntity(newLeaf);

					// Insert into Parent
					// Note: 'separatorKey' is the first key of 'newLeaf'.
					// In B+ Tree, for leaf splits, we copy the key up.
					insertIntoParent(prevNode, separatorKey, newLeafId, rootId);

					// Move cursor
					prevLeafId = newLeafId;

					// Important: After insertIntoParent, 'rootId' might change (root split),
					// or 'prevNode' parent might change (internal split).
					// We need to ensure we fetch fresh parent info if we loop again?
					// Actually, insertIntoParent handles the parent logic recursively.
					// The 'newLeaf' we just created has the correct parentId set?
					// insertIntoParent *might* split the parent. If it does, 'prevNode' and 'newLeaf'
					// parentIds are updated inside insertIntoParent.
					// However, for the *next* iteration of this while loop (NewLeaf2),
					// we need to make sure we attach to the correct parent.
					// Since NewLeaf2 is split from NewLeaf1, logically it shares NewLeaf1's parent.
					// But insertIntoParent updates parent pointers. So we are good.
				}
			}
		}
		return rootId;
	}

	int64_t IndexTreeManager::insert(int64_t rootId, const PropertyValue &key, int64_t value) {
		// 1. Acquire Shared Lock on the Tree Structure (handled by caller usually, but good to note)
		// In the context of the provided code, the caller (LabelIndex/PropertyIndex) holds a unique_lock.

		if (rootId == 0) {
			rootId = initialize();
		}

		// 2. Traverse to Leaf
		// We use a simple path stack if we need to propagate splits up.
		// However, standard B+Tree insert usually recurses or uses a parent-pointer loop.
		// Since your Index entity stores `parentId`, we can iterate down and bubble up using IDs.

		int64_t currentNodeId = rootId;
		Index currentNode = dataManager_->getIndex(currentNodeId);

		// Traverse down to the correct leaf
		while (!currentNode.isLeaf()) {
			int64_t childId = currentNode.findChild(key, dataManager_, keyComparator_);
			if (childId == 0) {
				// Should not happen in a valid tree unless empty root
				// Fallback to first child if key < all separators
				auto children = currentNode.getChildIds();
				if (!children.empty())
					childId = children[0];
				else
					throw std::runtime_error("Corrupt internal node: no children");
			}
			currentNodeId = childId;
			currentNode = dataManager_->getIndex(currentNodeId);
		}

		// 3. Attempt Insert at Leaf
		// Use tryInsertEntry to optimize: deserialize -> check/modify -> calculate size -> (serialize OR return
		// overflow)
		auto result = currentNode.tryInsertEntry(key, value, dataManager_, keyComparator_);

		if (result.success) {
			// Fits in node, update persistence and cache
			dataManager_->updateIndexEntity(currentNode);
			return rootId; // Root hasn't changed
		}

		// 4. Handle Split (Overflow)
		// result.overflowEntries contains the sorted entries that didn't fit.
		// We need to split this data between `currentNode` and a `newNode`.
		splitLeaf(currentNode, result.overflowEntries, rootId);

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
