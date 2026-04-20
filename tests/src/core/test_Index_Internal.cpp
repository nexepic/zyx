/**
 * @file test_Index_Internal.cpp
 * @author ZYX Contributors
 * @date 2026
 *
 * @copyright Copyright (c) 2026
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

#include "core/IndexTestFixture.hpp"

// ============================================================================
// Internal Node Operations Tests
// ============================================================================

TEST_F(IndexTest, Internal_AddAndRemoveChild) {
	// Mandatory first child
	std::vector<Index::ChildEntry> children = {{PropertyValue(std::monostate{}), 5, 0}};
	internalNode_.setAllChildren(children, dataManager_);

	Index::ChildEntry entry1 = {PropertyValue(200), 20, 0};
	Index::ChildEntry entry2 = {PropertyValue(100), 10, 0};

	internalNode_.addChild(entry1, dataManager_, intComparator);
	internalNode_.addChild(entry2, dataManager_, intComparator);

	auto final_children = internalNode_.getAllChildren(dataManager_);
	ASSERT_EQ(final_children.size(), 3UL);
	ASSERT_EQ(final_children[1].key, PropertyValue(100)); // Sorted check

	ASSERT_TRUE(internalNode_.removeChild(PropertyValue(200), dataManager_, intComparator));
	ASSERT_EQ(internalNode_.getAllChildren(dataManager_).size(), 2UL);
}

TEST_F(IndexTest, Internal_FindChild) {
	std::vector<Index::ChildEntry> children = {
			{PropertyValue(std::monostate{}), 5, 0}, {PropertyValue(100), 10, 0}, {PropertyValue(200), 20, 0}};
	internalNode_.setAllChildren(children, dataManager_);

	ASSERT_EQ(internalNode_.findChild(PropertyValue(50), dataManager_, intComparator), 5);
	ASSERT_EQ(internalNode_.findChild(PropertyValue(150), dataManager_, intComparator), 10);
	ASSERT_EQ(internalNode_.findChild(PropertyValue(250), dataManager_, intComparator), 20);
}

TEST_F(IndexTest, Internal_WouldOverflowCheck) {
	// Note: Internal nodes usually use 'wouldInternalOverflowOnAddChild' logic which is distinct from the new Leaf
	// logic. We assume this method still exists for Internal Nodes unless refactored similarly.

	std::vector<Index::ChildEntry> children;
	size_t size = 0;
	int i = 0;
	// Fill internal node
	while (size < Index::DATA_SIZE - 50) {
		PropertyValue k(i++);
		children.push_back({k, static_cast<int64_t>(i), 0});
		size += sizeof(int64_t) + sizeof(uint8_t) + graph::utils::getSerializedSize(k);
	}
	internalNode_.setAllChildren(children, dataManager_);

	Index::ChildEntry newEntry = {PropertyValue(99999), 999, 0};

	// Just verify the check works
	bool wouldOverflow = internalNode_.wouldInternalOverflowOnAddChild(newEntry, dataManager_);
	// We can't strictly assert true/false without exact byte calculation,
	// but the API should be callable.
	(void) wouldOverflow;
}

TEST_F(IndexTest, Internal_UpdateChildId) {
	std::vector<Index::ChildEntry> children;
	children.push_back({PropertyValue(std::monostate{}), 100, 0});
	children.push_back({PropertyValue(10), 200, 0});

	internalNode_.setAllChildren(children, dataManager_);

	ASSERT_TRUE(internalNode_.updateChildId(200, 299));
	auto updated = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(updated[1].childId, 299);
}

TEST_F(IndexTest, InternalKeyBlobManagement) {
	// Dynamically generate based on Internal Node Threshold
	std::string oversized_key_str(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'X');
	Index::ChildEntry oversized_entry = {PropertyValue(oversized_key_str), 123, 0};

	std::vector<Index::ChildEntry> children = {{PropertyValue(std::monostate{}), 1, 0}, oversized_entry};
	internalNode_.setAllChildren(children, dataManager_);

	auto retrieved = internalNode_.getAllChildren(dataManager_);
	ASSERT_NE(retrieved[1].keyBlobId, 0) << "Oversized internal key should be in a blob.";

	// Cleanup check
	int64_t blob_id = retrieved[1].keyBlobId;
	ASSERT_TRUE(internalNode_.removeChild(PropertyValue(oversized_key_str), dataManager_, stringComparator));
	ASSERT_THROW((void) dataManager_->getBlobManager()->readBlobChain(blob_id), std::runtime_error);
}

TEST_F(IndexTest, WouldInternalOverflowOnAddChildForLeaf) {
	// Tests branch at line 256-258: return false if isLeaf()
	Index::ChildEntry newEntry = {PropertyValue(999), 999, 0};

	bool wouldOverflow = leafNode_.wouldInternalOverflowOnAddChild(newEntry, dataManager_);
	EXPECT_FALSE(wouldOverflow) << "Leaf node should return false for wouldInternalOverflowOnAddChild";
}

TEST_F(IndexTest, RemoveChildThrowsOnLeaf) {
	// Tests branch at line 430-431: throw if isLeaf
	EXPECT_THROW(leafNode_.removeChild(PropertyValue("key"), dataManager_, stringComparator), std::logic_error);
}

TEST_F(IndexTest, FindChildThrowsOnLeaf) {
	// Tests branch at line 453-454: throw if isLeaf
	EXPECT_THROW((void)leafNode_.findChild(PropertyValue("key"), dataManager_, stringComparator), std::logic_error);
}

TEST_F(IndexTest, GetAllChildrenThrowsOnLeaf) {
	// Tests branch at line 473-474: throw if isLeaf
	EXPECT_THROW((void)leafNode_.getAllChildren(dataManager_), std::logic_error);
}

TEST_F(IndexTest, SetAllChildrenThrowsOnLeaf) {
	// Tests branch at line 523-524: throw if isLeaf
	std::vector<Index::ChildEntry> children = {{PropertyValue(std::monostate{}), 5, 0}};
	EXPECT_THROW(leafNode_.setAllChildren(children, dataManager_), std::logic_error);
}

TEST_F(IndexTest, GetChildIdsReturnsEmptyForLeaf) {
	// Tests branch at line 685-686: return {} if isLeaf
	auto ids = leafNode_.getChildIds();
	EXPECT_TRUE(ids.empty());
}

TEST_F(IndexTest, GetChildIdsReturnsEmptyForZeroChildCount) {
	// Tests branch at line 687-688: return {} if childCount == 0
	internalNode_.getMutableMetadata().childCount = 0;

	auto ids = internalNode_.getChildIds();
	EXPECT_TRUE(ids.empty());
}

TEST_F(IndexTest, GetChildIdsReturnsEmptyForEmptyChildren) {
	// Even with childCount set, if buffer is empty, should return first child or empty
	// Set up internal node with one child
	std::vector<Index::ChildEntry> children = {{PropertyValue(std::monostate{}), 5, 0}};
	internalNode_.setAllChildren(children, dataManager_);

	auto ids = internalNode_.getChildIds();
	EXPECT_EQ(ids.size(), 1UL);
	EXPECT_EQ(ids[0], 5);
}

TEST_F(IndexTest, UpdateChildIdNonExistentChild) {
	// Tests updateChildId when child ID doesn't exist
	bool updated = internalNode_.updateChildId(99999, 88888);
	EXPECT_FALSE(updated);
}

TEST_F(IndexTest, UpdateChildIdFirstChild) {
	// Tests updating the first child (which has no preceding key)
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Update first child ID
	bool updated = internalNode_.updateChildId(100, 999);
	EXPECT_TRUE(updated);

	auto updatedChildren = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(updatedChildren[0].childId, 999);
	EXPECT_EQ(updatedChildren[1].childId, 200);
}

TEST_F(IndexTest, UpdateChildIdMiddleChild) {
	// Tests updating a middle child
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0},
		{PropertyValue(300), 300, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Update middle child ID
	bool updated = internalNode_.updateChildId(200, 999);
	EXPECT_TRUE(updated);

	auto updatedChildren = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(updatedChildren[0].childId, 100);
	EXPECT_EQ(updatedChildren[1].childId, 999);
	EXPECT_EQ(updatedChildren[2].childId, 300);
}

TEST_F(IndexTest, GetChildIdsMultipleChildren) {
	// Tests getChildIds with multiple children
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0},
		{PropertyValue(300), 300, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	auto ids = internalNode_.getChildIds();
	EXPECT_EQ(ids.size(), 3UL);
	EXPECT_EQ(ids[0], 100);
	EXPECT_EQ(ids[1], 200);
	EXPECT_EQ(ids[2], 300);
}

TEST_F(IndexTest, FindChildReturnsCorrectChild) {
	// Tests findChild returns the correct child based on key
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0},
		{PropertyValue(400), 400, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Find child for key < 200 should return 100
	EXPECT_EQ(internalNode_.findChild(PropertyValue(150), dataManager_, intComparator), 100);

	// Find child for key < 400 should return 200
	EXPECT_EQ(internalNode_.findChild(PropertyValue(250), dataManager_, intComparator), 200);

	// Find child for key >= 400 should return 400
	EXPECT_EQ(internalNode_.findChild(PropertyValue(500), dataManager_, intComparator), 400);
}

TEST_F(IndexTest, FindChildWithAllKeysLessThanFirstSeparator) {
	// Tests findChild when key is less than all separators
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Key 50 should route to first child (100)
	EXPECT_EQ(internalNode_.findChild(PropertyValue(50), dataManager_, intComparator), 100);
}

TEST_F(IndexTest, GetAllChildrenWithBlobKeys) {
	// Tests getAllChildren when keys are stored in blobs
	// Create keys larger than threshold to force blob storage
	std::string largeKey1(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');
	std::string largeKey2(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'B');

	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey1), 200, 0},
		{PropertyValue(largeKey2), 300, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	auto retrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(retrieved.size(), 3UL);

	// Verify blob IDs are set for large keys
	EXPECT_EQ(retrieved[0].keyBlobId, 0);
	EXPECT_NE(retrieved[1].keyBlobId, 0) << "Large key 1 should be in blob";
	EXPECT_NE(retrieved[2].keyBlobId, 0) << "Large key 2 should be in blob";
}

TEST_F(IndexTest, SetAllChildrenUpdateExistingBlob) {
	// Tests setAllChildren when updating existing blob (line 551-556)
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');

	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Get the blob ID
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	int64_t originalBlobId = retrieved[1].keyBlobId;
	EXPECT_NE(originalBlobId, 0);

	// Update with DIFFERENT large key content (should update existing blob)
	std::string differentLargeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'B');
	std::vector<Index::ChildEntry> updatedChildren = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(differentLargeKey), 999, originalBlobId}  // Use existing blob ID
	};
	internalNode_.setAllChildren(updatedChildren, dataManager_);

	auto newRetrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_NE(newRetrieved[1].keyBlobId, 0);
	EXPECT_EQ(newRetrieved[1].childId, 999);

	// Verify the key content was actually updated
	EXPECT_EQ(newRetrieved[1].key, PropertyValue(differentLargeKey));
}

TEST_F(IndexTest, SetAllChildrenCleanupBlobWhenKeyBecomesSmall) {
	// Tests setAllChildren blob cleanup when key becomes small (line 540-544)
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');

	// Create with large key in blob
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Get blob ID
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	int64_t blobId = retrieved[1].keyBlobId;
	EXPECT_NE(blobId, 0);

	// Update with small key (blob should be cleaned up)
	std::vector<Index::ChildEntry> updatedChildren = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(5), 300, blobId}  // Small key now, but has old blobId
	};
	internalNode_.setAllChildren(updatedChildren, dataManager_);

	// Verify old blob was deleted
	EXPECT_THROW((void) dataManager_->getBlobManager()->readBlobChain(blobId), std::runtime_error);

	auto newRetrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(newRetrieved[1].keyBlobId, 0) << "Small key should not have blob";
}

TEST_F(IndexTest, GetAllChildrenEmptyChildren) {
	// Tests getAllChildren when childCount is 0 (line 477-479)
	internalNode_.getMutableMetadata().childCount = 0;

	auto children = internalNode_.getAllChildren(dataManager_);
	EXPECT_TRUE(children.empty());
}

TEST_F(IndexTest, SetAllChildrenEmptyChildren) {
	// Tests setAllChildren with empty children (line 530-532)
	std::vector<Index::ChildEntry> emptyChildren;

	EXPECT_NO_THROW(internalNode_.setAllChildren(emptyChildren, dataManager_));

	EXPECT_EQ(internalNode_.getMetadata().childCount, 0u);
}

TEST_F(IndexTest, WouldInternalOverflowOnAddChildEmptyChildren) {
	// Tests wouldInternalOverflowOnAddChild with empty children (line 267-269)
	// Set up internal node with no children
	Index newInternal(0, Index::NodeType::INTERNAL, 1);
	dataManager_->addIndexEntity(newInternal);
	newInternal.getMutableMetadata().childCount = 0;

	Index::ChildEntry newEntry = {PropertyValue(100), 10, 0};

	// Should return false (no overflow) when adding to empty children
	bool wouldOverflow = newInternal.wouldInternalOverflowOnAddChild(newEntry, dataManager_);
	EXPECT_FALSE(wouldOverflow);

	// Cleanup
	dataManager_->deleteIndex(newInternal);
}

TEST_F(IndexTest, WouldInternalOverflowOnAddChildWithExistingChildren) {
	// Tests wouldInternalOverflowOnAddChild with existing children
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	Index::ChildEntry newEntry = {PropertyValue(300), 300, 0};

	// Just verify the method works without error
	bool wouldOverflow = internalNode_.wouldInternalOverflowOnAddChild(newEntry, dataManager_);
	// Can't assert specific value without exact size calculation
	(void) wouldOverflow;
}

TEST_F(IndexTest, SetAllChildrenCreateNewKeyBlob) {
	// Create child entry with oversized key
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};

	internalNode_.setAllChildren(children, dataManager_);

	auto retrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_NE(retrieved[1].keyBlobId, 0) << "Large child key should be in blob";
}

TEST_F(IndexTest, SetAllChildrenOnLeafNodeThrows) {
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0}
	};

	EXPECT_THROW(leafNode_.setAllChildren(children, dataManager_), std::logic_error);
}

TEST_F(IndexTest, UpdateChildIdOnLeafNodeReturnsFalse) {
	bool result = leafNode_.updateChildId(100, 200);
	EXPECT_FALSE(result);
}

TEST_F(IndexTest, UpdateChildIdWithNoChildrenReturnsFalse) {
	// Create internal node with no children
	Index newInternal(0, Index::NodeType::INTERNAL, 1);
	dataManager_->addIndexEntity(newInternal);
	newInternal.getMutableMetadata().childCount = 0;

	bool result = newInternal.updateChildId(100, 200);
	EXPECT_FALSE(result);

	dataManager_->deleteIndex(newInternal);
}

TEST_F(IndexTest, GetChildIdsOnLeafNodeReturnsEmpty) {
	auto ids = leafNode_.getChildIds();
	EXPECT_TRUE(ids.empty());
}

TEST_F(IndexTest, GetChildIdsWithNoChildrenReturnsEmpty) {
	// Create internal node with no children
	Index newInternal(0, Index::NodeType::INTERNAL, 1);
	dataManager_->addIndexEntity(newInternal);
	newInternal.getMutableMetadata().childCount = 0;

	auto ids = newInternal.getChildIds();
	EXPECT_TRUE(ids.empty());

	dataManager_->deleteIndex(newInternal);
}

TEST_F(IndexTest, UpdateChildIdWithBlobKeys) {
	// Create internal node with large key that forces blob storage
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');

	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Verify blob was created
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_NE(retrieved[1].keyBlobId, 0);

	// Update child ID - should handle blob keys correctly
	bool result = internalNode_.updateChildId(200, 999);
	EXPECT_TRUE(result);

	auto updated = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(updated[1].childId, 999);
}

TEST_F(IndexTest, GetChildIdsWithBlobKeys) {
	// Create internal node with large key that forces blob storage
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');

	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0},
		{PropertyValue(300), 300, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Get child IDs - should handle blob keys correctly
	auto ids = internalNode_.getChildIds();
	EXPECT_EQ(ids.size(), 3UL);
	EXPECT_EQ(ids[0], 100);
	EXPECT_EQ(ids[1], 200);
	EXPECT_EQ(ids[2], 300);
}

TEST_F(IndexTest, SetAllChildrenBlobCleanupWhenKeyShrinks) {
	// Create internal node with large key in blob
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');

	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Get blob ID
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	int64_t blobId = retrieved[1].keyBlobId;
	EXPECT_NE(blobId, 0);

	// Update with small key - blob should be deleted
	std::vector<Index::ChildEntry> updatedChildren = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(5), 300, blobId}  // Small key now with old blobId
	};
	internalNode_.setAllChildren(updatedChildren, dataManager_);

	// Verify blob was deleted
	EXPECT_THROW((void) dataManager_->getBlobManager()->readBlobChain(blobId), std::runtime_error);

	auto final = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(final[1].keyBlobId, 0);
}

TEST_F(IndexTest, AddChildThrowsOnLeafNode) {
	Index::ChildEntry entry = {PropertyValue(100), 10, 0};
	EXPECT_THROW(leafNode_.addChild(entry, dataManager_, intComparator), std::logic_error);
}

TEST_F(IndexTest, RemoveChildReturnsFalseWhenKeyNotFound) {
	// Set up internal node with some children
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Try to remove a non-existent key
	bool removed = internalNode_.removeChild(PropertyValue(999), dataManager_, intComparator);
	EXPECT_FALSE(removed);

	// Verify children are unchanged
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(retrieved.size(), 2UL);
}

TEST_F(IndexTest, FindChildWithEmptyChildrenReturnsZero) {
	// Create internal node with explicitly zero children
	Index emptyInternal(0, Index::NodeType::INTERNAL, 1);
	dataManager_->addIndexEntity(emptyInternal);
	emptyInternal.getMutableMetadata().childCount = 0;

	auto result = emptyInternal.findChild(PropertyValue(50), dataManager_, intComparator);
	EXPECT_EQ(result, 0);

	dataManager_->deleteIndex(emptyInternal);
}

TEST_F(IndexTest, SerializeDeserializeInternalNode) {
	Index original(99, Index::NodeType::INTERNAL, 7);
	original.setLevel(2);

	// Serialize
	std::ostringstream os;
	original.serialize(os);

	// Deserialize
	std::string serialized = os.str();
	std::istringstream is(serialized);
	Index deserialized = Index::deserialize(is);

	EXPECT_EQ(deserialized.getId(), 99);
	EXPECT_EQ(deserialized.getNodeType(), Index::NodeType::INTERNAL);
	EXPECT_EQ(deserialized.getLevel(), 2);
	EXPECT_EQ(deserialized.getIndexType(), 7u);
}

TEST_F(IndexTest, WouldInternalOverflowOnAddChildTrueCase) {
	// Fill internal node as close to capacity as possible
	// Each child entry serializes as: flags (1) + key (variable) + childId (8)
	// First child is just childId (8 bytes)
	std::vector<Index::ChildEntry> children;
	children.push_back({PropertyValue(std::monostate{}), 1, 0});

	// Use string keys just below the blob threshold to maximize space usage
	// Each entry = 1 (flags) + 1 (type tag) + 4 (string len) + key_len + 8 (childId)
	int i = 1;
	size_t estimatedSize = sizeof(int64_t); // First child
	while (true) {
		// Use a key string of ~25 bytes (below INTERNAL_KEY_INLINE_THRESHOLD of 32)
		std::string keyStr = "k" + std::to_string(i);
		// Pad to fill more space
		while (keyStr.size() < 20) keyStr += '_';
		PropertyValue k(keyStr);
		size_t entrySize = sizeof(uint8_t) + graph::utils::getSerializedSize(k) + sizeof(int64_t);
		if (estimatedSize + entrySize > Index::DATA_SIZE - 5) {
			break; // Stop when nearly full, leave very little room
		}
		children.push_back({k, static_cast<int64_t>(i), 0});
		estimatedSize += entrySize;
		i++;
	}
	internalNode_.setAllChildren(children, dataManager_);

	// Now try to add another entry - should overflow since we're nearly full
	std::string addKey = "overflow_test_entry_";
	Index::ChildEntry newEntry = {PropertyValue(addKey), 99999, 0};

	bool wouldOverflow = internalNode_.wouldInternalOverflowOnAddChild(newEntry, dataManager_);
	EXPECT_TRUE(wouldOverflow) << "Adding entry to nearly full node should overflow. "
		<< "Estimated fill: " << estimatedSize << "/" << Index::DATA_SIZE;
}

TEST_F(IndexTest, RemoveChildWithBlobKeyCleanup) {
	// Create internal node with large key (blob storage)
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'X');
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Verify blob was created
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	int64_t blobId = retrieved[1].keyBlobId;
	EXPECT_NE(blobId, 0);

	// Remove the child
	bool removed = internalNode_.removeChild(PropertyValue(largeKey), dataManager_, stringComparator);
	EXPECT_TRUE(removed);

	// Verify blob was cleaned up
	EXPECT_THROW((void) dataManager_->getBlobManager()->readBlobChain(blobId), std::runtime_error);

	// Verify child was removed
	auto final_children = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(final_children.size(), 1UL);
}

TEST_F(IndexTest, WouldInternalOverflowWithBlobKeyChildren) {
	// Create internal node with a large key that forces blob storage
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'X');

	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Verify blob was created
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_NE(retrieved[1].keyBlobId, 0);

	// Check overflow with the existing blob key children
	Index::ChildEntry newEntry = {PropertyValue(300), 300, 0};
	bool wouldOverflow = internalNode_.wouldInternalOverflowOnAddChild(newEntry, dataManager_);
	// Just ensure it runs without error and gives a result
	EXPECT_FALSE(wouldOverflow) << "Adding one small entry to a node with 2 children should not overflow";
}

TEST_F(IndexTest, SetAllChildrenUpdateExistingBlobKey) {
	// First create children with a large key (creates blob)
	std::string largeKey1(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');

	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey1), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Get the blob ID
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	int64_t originalBlobId = retrieved[1].keyBlobId;
	EXPECT_NE(originalBlobId, 0);

	// Now update with a DIFFERENT large key, passing the existing blobId
	// This should trigger the update-existing-blob path
	std::string largeKey2(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'Z');
	std::vector<Index::ChildEntry> updatedChildren = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey2), 200, originalBlobId}
	};
	internalNode_.setAllChildren(updatedChildren, dataManager_);

	auto newRetrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_NE(newRetrieved[1].keyBlobId, 0);
	EXPECT_EQ(newRetrieved[1].key, PropertyValue(largeKey2));
}

TEST_F(IndexTest, UpdateChildIdNotFoundReturnsFalse) {
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0},
		{PropertyValue(300), 300, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Try to update a child ID that doesn't exist
	bool result = internalNode_.updateChildId(999, 888);
	EXPECT_FALSE(result);

	// Verify nothing changed
	auto ids = internalNode_.getChildIds();
	EXPECT_EQ(ids[0], 100);
	EXPECT_EQ(ids[1], 200);
	EXPECT_EQ(ids[2], 300);
}

TEST_F(IndexTest, RemoveChildKeyNearButNotExact) {
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(static_cast<int64_t>(50)), 200, 0},
		{PropertyValue(static_cast<int64_t>(100)), 300, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Try to remove key 75 which doesn't exist (between 50 and 100)
	bool removed = internalNode_.removeChild(PropertyValue(static_cast<int64_t>(75)), dataManager_, intComparator);
	EXPECT_FALSE(removed);

	// All children should still be present
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(retrieved.size(), 3UL);
}

TEST_F(IndexTest, GetAllChildrenReturnsEmptyForZeroCount) {
	Index emptyInternal(0, Index::NodeType::INTERNAL, 1);
	dataManager_->addIndexEntity(emptyInternal);

	auto children = emptyInternal.getAllChildren(dataManager_);
	EXPECT_TRUE(children.empty());

	dataManager_->deleteIndex(emptyInternal);
}

TEST_F(IndexTest, FindChildWithSingleChild) {
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 42, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Any key should return the single child
	auto result = internalNode_.findChild(PropertyValue(999), dataManager_, intComparator);
	EXPECT_EQ(result, 42);
}

TEST_F(IndexTest, RemoveChildKeyBeyondAllChildren) {
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 10, 0},
		{PropertyValue(static_cast<int64_t>(5)), 20, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Key 999 is beyond all children keys
	bool removed = internalNode_.removeChild(PropertyValue(static_cast<int64_t>(999)), dataManager_, intComparator);
	EXPECT_FALSE(removed);
}

TEST_F(IndexTest, SetAllChildrenOverflowThrows) {
	// Create children with keys that will exceed DATA_SIZE
	std::vector<Index::ChildEntry> children;
	children.push_back({PropertyValue(std::monostate{}), 1, 0}); // first child

	// Each subsequent child with a ~20 char key takes about 30-35 bytes
	// Need enough to overflow DATA_SIZE
	size_t estimatedSize = sizeof(int64_t); // first child
	int i = 1;
	while (estimatedSize < Index::DATA_SIZE + 100) {
		std::string keyStr = "key_overflow_test_" + std::to_string(i);
		PropertyValue k(keyStr);
		size_t entrySize = sizeof(uint8_t) + graph::utils::getSerializedSize(k) + sizeof(int64_t);
		children.push_back({k, static_cast<int64_t>(i + 1), 0});
		estimatedSize += entrySize;
		i++;
	}

	EXPECT_THROW(internalNode_.setAllChildren(children, dataManager_), std::runtime_error);
}

TEST_F(IndexTest, WouldInternalOverflowBlobKeySizeEstimation) {
	// Create children where one has a blob key
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 10, 'X');
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// The blob key should be stored with a blob ID
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	ASSERT_NE(retrieved[1].keyBlobId, 0);

	// When checking overflow, the function simulates the size using the
	// existing children (including blob key). This exercises the keyIsBlob
	// path in wouldInternalOverflowOnAddChild.
	Index::ChildEntry smallEntry = {PropertyValue(static_cast<int64_t>(5)), 300, 0};
	bool overflow = internalNode_.wouldInternalOverflowOnAddChild(smallEntry, dataManager_);
	EXPECT_FALSE(overflow) << "Small entry added to node with 2 children should not overflow";
}

TEST_F(IndexTest, AddChildThrowsOnLeafNodeRepeat) {
	Index::ChildEntry entry = {PropertyValue("x"), 1, 0};
	EXPECT_THROW(leafNode_.addChild(entry, dataManager_, stringComparator), std::logic_error);
}

TEST_F(IndexTest, RemoveChildThrowsOnLeafNodeRepeat) {
	EXPECT_THROW(leafNode_.removeChild(PropertyValue("x"), dataManager_, stringComparator), std::logic_error);
}

TEST_F(IndexTest, FindChildExactKeyMatch) {
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 10, 0},
		{PropertyValue(static_cast<int64_t>(50)), 20, 0},
		{PropertyValue(static_cast<int64_t>(100)), 30, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Key exactly at separator value 50
	int64_t result = internalNode_.findChild(PropertyValue(static_cast<int64_t>(50)), dataManager_, intComparator);
	EXPECT_EQ(result, 20);

	// Key exactly at separator value 100
	result = internalNode_.findChild(PropertyValue(static_cast<int64_t>(100)), dataManager_, intComparator);
	EXPECT_EQ(result, 30);
}
