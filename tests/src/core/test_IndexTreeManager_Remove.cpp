/**
 * @file test_IndexTreeManager_Remove.cpp
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

#include "core/IndexTreeManagerTestFixture.hpp"

// ============================================================================
// Remove, Merge & Redistribute Tests
// ============================================================================

TEST_F(IndexTreeManagerTest, AdvancedRemoveScenarios) {
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("test_key"), 1);
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("test_key"), 2);
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("test_key"), 3);

	ASSERT_FALSE(stringTreeManager_->remove(stringRootId_, PropertyValue("test_key"), 99));
	ASSERT_EQ(stringTreeManager_->find(stringRootId_, PropertyValue("test_key")).size(), 3UL);

	ASSERT_TRUE(stringTreeManager_->remove(stringRootId_, PropertyValue("test_key"), 2));
	auto results1 = stringTreeManager_->find(stringRootId_, PropertyValue("test_key"));
	ASSERT_EQ(results1.size(), 2UL);
	ASSERT_EQ(std::ranges::find(results1, 2), results1.end());
	ASSERT_TRUE(stringTreeManager_->remove(stringRootId_, PropertyValue("test_key"), 1));
	ASSERT_TRUE(stringTreeManager_->remove(stringRootId_, PropertyValue("test_key"), 3));
	ASSERT_TRUE(stringTreeManager_->find(stringRootId_, PropertyValue("test_key")).empty());

	ASSERT_FALSE(stringTreeManager_->remove(stringRootId_, PropertyValue("non_existent_key"), 1));
}

TEST_F(IndexTreeManagerTest, RemoveCausesLeafRedistributionFromRight) {
	// Setup: Insert keys to cause a split. We need precise control.
	// Assuming a split promotes the median key, we can craft the insertions.
	// For simplicity, let's just insert enough to cause a split, then add more to the right.
	for (int i = 0; i < 300; ++i) { // Creates at least one split
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(i), i);
	}

	// At this point, keys are likely distributed among several leaves.
	// Find two adjacent leaves. Let's find leaf for key 50.
	int64_t leftLeafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(50));
	auto leftLeaf = dataManager_->getIndex(leftLeafId);
	int64_t rightLeafId = leftLeaf.getNextLeafId();
	ASSERT_NE(rightLeafId, 0) << "Setup failed: Could not find two adjacent leaf nodes.";

	// To reliably trigger redistribution, let's empty the left leaf and fill the right one.
	auto leftEntries = leftLeaf.getAllEntries(dataManager_);
	auto rightLeaf = dataManager_->getIndex(rightLeafId);
	auto rightEntries = rightLeaf.getAllEntries(dataManager_);

	// Move all but one entry from left to right.
	rightEntries.insert(rightEntries.begin(), std::make_move_iterator(leftEntries.begin() + 1),
						std::make_move_iterator(leftEntries.end()));
	leftEntries.resize(1);

	leftLeaf.setAllEntries(leftEntries, dataManager_);
	rightLeaf.setAllEntries(rightEntries, dataManager_);
	dataManager_->updateIndexEntity(leftLeaf);
	dataManager_->updateIndexEntity(rightLeaf);

	// Action: Delete the last entry from the left leaf to trigger underflow.
	int64_t keyToDelete = std::get<int64_t>(leftEntries[0].key.getVariant());
	ASSERT_TRUE(intTreeManager_->remove(intRootId_, PropertyValue(keyToDelete), keyToDelete));

	// Verification
	auto finalLeftLeaf = dataManager_->getIndex(leftLeafId);
	auto finalRightLeaf = dataManager_->getIndex(rightLeafId);

	ASSERT_GT(finalLeftLeaf.getEntryCount(), 0u) << "Left leaf should have received an entry.";
	ASSERT_LT(finalRightLeaf.getEntryCount(), rightEntries.size()) << "Right leaf should have given an entry.";

	// Check that the data is still correct.
	auto result = intTreeManager_->find(intRootId_, PropertyValue(keyToDelete + 1));
	ASSERT_FALSE(result.empty()) << "Data should still be findable after redistribution.";
}

TEST_F(IndexTreeManagerTest, RemoveCausesLeafMerge) {
	// Start with a fresh tree
	intTreeManager_->clear(intRootId_);
	intRootId_ = intTreeManager_->initialize();

	// 1. Create a simple tree with a small, controlled set of entries
	// Use a distribution that forces multiple leaf nodes
	std::vector<int64_t> values;
	// First leaf should contain values 10-50
	for (int i = 10; i <= 50; i += 10) {
		values.push_back(i);
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Force a split by adding more values
	// Second leaf should contain values 60-100
	for (int i = 60; i <= 100; i += 10) {
		values.push_back(i);
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// 2. Verify the tree structure - we should have at least two leaf nodes
	int64_t firstLeafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(10)));
	auto firstLeaf = dataManager_->getIndex(firstLeafId);
	ASSERT_NE(firstLeaf.getNextLeafId(), 0) << "Test requires at least two leaf nodes";

	int64_t secondLeafId = firstLeaf.getNextLeafId();
	auto secondLeaf = dataManager_->getIndex(secondLeafId);

	// Both leaves should have the same parent
	ASSERT_EQ(firstLeaf.getParentId(), secondLeaf.getParentId());
	int64_t parentId = firstLeaf.getParentId();
	auto parent = dataManager_->getIndex(parentId);

	// Save parent's child count before operation
	size_t parentChildCount_before = parent.getChildCount();
	ASSERT_GE(parentChildCount_before, 2UL) << "Parent must have at least 2 children for merge test";

	// 3. Now remove all entries from first leaf to force a merge
	for (int i = 10; i <= 50; i += 10) {
		ASSERT_TRUE(intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i));
	}

	// 4. Verification logic must handle Root Collapse
	auto currentRoot = dataManager_->getIndex(intRootId_);

	if (currentRoot.getId() == parentId) {
		// Case A: Root didn't collapse (e.g. if parent had > 2 children originally)
		ASSERT_EQ(currentRoot.getChildCount(), parentChildCount_before - 1)
				<< "Parent should have one less child after merge";
	} else {
		// Case B: Root collapsed! (This is what actually happens)
		// The new root should be the merged leaf itself.
		// The old parent is gone.
		ASSERT_NE(currentRoot.getId(), parentId);
		ASSERT_TRUE(currentRoot.isLeaf()) << "New root should be the merged leaf";
		// We can't check parent.getChildCount() because parent is deleted.
	}

	// 5. Verify the merged node contains the remaining values
	// If collapsed, intRootId_ IS the remaining leaf.
	// If not collapsed, findLeafNode still works.
	int64_t remainingLeafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(60)));
	auto remainingLeaf = dataManager_->getIndex(remainingLeafId);
	auto entries = remainingLeaf.getAllEntries(dataManager_);

	// Should have entries for 60, 70, 80, 90, 100
	ASSERT_EQ(entries.size(), 5UL) << "Merged leaf should contain all remaining entries";

	// Verify each expected value is present
	for (int i = 60; i <= 100; i += 10) {
		bool found = false;
		for (const auto &entry: entries) {
			if (std::get<int64_t>(entry.key.getVariant()) == i) {
				found = true;
				break;
			}
		}
		ASSERT_TRUE(found) << "Value " << i << " not found in merged leaf";
	}
}

TEST_F(IndexTreeManagerTest, RemoveCausesRecursiveMerge) {
	// Setup: This requires creating a very specific tree structure.
	// We need a tree of height > 1, with at least two internal nodes at the same level,
	// both of which have the minimum number of children (e.g., 2), and those children
	// are also minimally filled leaves.

	// This setup can be very sensitive to the fanout factor of nodes.
	// We'll insert and then delete strategically to create the desired state.
	constexpr int initial_insert_count = 5000;
	for (int i = 0; i < initial_insert_count; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(i), i);
	}

	auto root_before = dataManager_->getIndex(intRootId_);
	uint8_t height_before = root_before.getLevel();
	ASSERT_GE(height_before, 2u) << "Test requires a tree of at least height 2.";

	// Now, delete a large chunk of keys from the middle, which tends to cause
	// many merges and can trigger recursive merges.
	for (int i = 100; i < 4800; ++i) {
		intTreeManager_->remove(intRootId_, PropertyValue(i), i);
	}

	// Verification
	auto root_after = dataManager_->getIndex(intRootId_);
	uint8_t height_after = root_after.getLevel();

	// It's possible the tree height decreased.
	ASSERT_LE(height_after, height_before) << "Tree height should not increase after massive deletions.";

	// The most important check: data integrity. The remaining data should be findable.
	auto result1 = intTreeManager_->find(intRootId_, PropertyValue(50));
	ASSERT_EQ(result1.size(), 1UL);
	ASSERT_EQ(result1[0], 50);

	auto result2 = intTreeManager_->find(intRootId_, PropertyValue(4900));
	ASSERT_EQ(result2.size(), 1UL);
	ASSERT_EQ(result2[0], 4900);

	// A specific check for a key that was deleted.
	auto result3 = intTreeManager_->find(intRootId_, PropertyValue(2500));
	ASSERT_TRUE(result3.empty());
}

TEST_F(IndexTreeManagerTest, RootCollapse_HeightReduction) {
	// 1. Build a tree with height at least 1 (Root -> Leaves)
	// We need enough entries to force a split at the root.
	// Assuming fanout is small, 50 entries usually triggers splits.
	for (int i = 0; i < 50; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
	}

	auto rootBefore = dataManager_->getIndex(stringRootId_);
	uint8_t initialLevel = rootBefore.getLevel();
	ASSERT_GE(initialLevel, 1) << "Setup failed: Tree must have height > 0 (internal root).";

	// 2. Delete almost everything, leaving just enough to fit in one node
	// We keep keys 0 and 1.
	for (int i = 2; i < 50; ++i) {
		stringTreeManager_->remove(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
	}

	// 3. Verify Height Reduction
	auto rootAfter = dataManager_->getIndex(stringRootId_);
	ASSERT_LT(rootAfter.getLevel(), initialLevel) << "Root should have collapsed to a lower level.";
	ASSERT_TRUE(rootAfter.isLeaf()) << "With only 2 items, the root should now be a leaf.";

	// 4. Verify Data Integrity
	auto res0 = stringTreeManager_->find(stringRootId_, PropertyValue(generatePaddedKey(0)));
	ASSERT_EQ(res0.size(), 1UL);
	auto res1 = stringTreeManager_->find(stringRootId_, PropertyValue(generatePaddedKey(1)));
	ASSERT_EQ(res1.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, LeafRedistributeFromLeft) {
	// 1. Manually construct two leaves to control the state precisely.
	// We assume the implementation uses the standard logic where we can create this state via inserts.

	// Insert 0..30. Assuming capacity allows this to split into at least two nodes.
	for (int i = 0; i <= 30; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Identify the leaf containing '30' (The rightmost leaf)
	int64_t rightLeafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(30)));
	auto rightLeaf = dataManager_->getIndex(rightLeafId);

	int64_t leftLeafId = rightLeaf.getPrevLeafId();
	ASSERT_NE(leftLeafId, 0) << "Setup failed: Right leaf must have a left sibling.";

	// 2. Manipulate state to force Left-to-Right redistribution condition.
	// We want Left Leaf to be FULL and Right Leaf to be almost EMPTY.

	// Remove almost all entries from Right Leaf except '30'.
	// We iterate backwards from 29 down to whatever is in that leaf.
	// Note: This relies on knowing the split point, but we can just blindly remove
	// numbers and check if they were in the right leaf.
	for (int i = 29; i >= 15; --i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Now, verify Right Leaf has very few items (likely just 30)
	// And Left Leaf should still have plenty (0..14 or so).

	// 3. Trigger Underflow on Right Leaf
	// Remove '30'. The right leaf becomes empty/underflow.
	// It should look to its Left Sibling. Left sibling has data.
	ASSERT_TRUE(intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(30)), 30));

	// 4. Verify Data Integrity
	// If redistribution worked, the Right Leaf (or what replaced it) should now contain
	// the largest value from the Left Leaf (e.g., 14).
	// The key '14' should still be findable.
	auto res = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(14)));
	ASSERT_EQ(res.size(), 1UL);
	ASSERT_EQ(res[0], 14);
}

TEST_F(IndexTreeManagerTest, InternalNodeMerge) {
	// 1. Create a 3-level tree (Root -> Internal -> Leaf)
	// Insert enough data to ensure we have multiple internal nodes at level 1.
	constexpr int count = 1000;
	for (int i = 0; i < count; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_GE(root.getLevel(), 2) << "Setup failed: Need a tree with at least 3 levels (Height 2).";

	// 2. Mass delete from a specific range to hollow out a subtree.
	// This forces leaves to merge, which forces their parent (Internal Node) to underflow.
	// If neighbors are also sparse, the Internal Nodes will merge.

	// Delete the first 80% of data. This usually causes cascading merges from left to right.
	for (int i = 0; i < 800; ++i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// 3. Verify the tree didn't collapse into an invalid state
	// We should still find the remaining data.
	auto res = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(999)));
	ASSERT_EQ(res.size(), 1UL);
	ASSERT_EQ(res[0], 999);

	// Verify root is still valid (it might have lowered in height)
	auto rootAfter = dataManager_->getIndex(intRootId_);
	ASSERT_GT(rootAfter.getEntryCount() + rootAfter.getChildCount(), 0u);
}

TEST_F(IndexTreeManagerTest, RemoveReturnsFalseForNonExistentKey) {
	// Tests remove returns false for non-existent keys
	bool removed = stringTreeManager_->remove(stringRootId_, PropertyValue("non_existent"), 999);
	ASSERT_FALSE(removed);
}

TEST_F(IndexTreeManagerTest, RemoveFromZeroRootId) {
	// Tests remove with rootId = 0 returns false
	int64_t zeroRootId = 0;
	bool removed = stringTreeManager_->remove(zeroRootId, PropertyValue("key"), 1);
	ASSERT_FALSE(removed);
}

TEST_F(IndexTreeManagerTest, RemoveAllFromRootLeaf) {
	// Insert a single entry into root
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("only_key"), 1);

	// Remove it - root leaf becomes empty
	bool removed = stringTreeManager_->remove(stringRootId_, PropertyValue("only_key"), 1);
	ASSERT_TRUE(removed);

	// Root should still exist but be empty
	auto root = dataManager_->getIndex(stringRootId_);
	ASSERT_TRUE(root.isLeaf());
	ASSERT_EQ(root.getEntryCount(), 0u);
}

TEST_F(IndexTreeManagerTest, RemoveFromRootLeafNoUnderflow) {
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("key1"), 1);
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("key2"), 2);
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("key3"), 3);

	// Remove one - root still has entries, no underflow
	bool removed = stringTreeManager_->remove(stringRootId_, PropertyValue("key2"), 2);
	ASSERT_TRUE(removed);

	auto root = dataManager_->getIndex(stringRootId_);
	ASSERT_TRUE(root.isLeaf());
	ASSERT_EQ(root.getEntryCount(), 2u);

	// Verify remaining entries
	auto r1 = stringTreeManager_->find(stringRootId_, PropertyValue("key1"));
	ASSERT_EQ(r1.size(), 1UL);
	auto r3 = stringTreeManager_->find(stringRootId_, PropertyValue("key3"));
	ASSERT_EQ(r3.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, HandleUnderflowIntegrity) {
	// Build a tree with enough data to trigger underflow handling
	for (int i = 0; i < 50; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Remove most entries, triggering various underflow scenarios
	for (int i = 5; i < 45; ++i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Verify data integrity after underflow handling
	for (int i = 0; i < 5; ++i) {
		auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(i)));
		ASSERT_EQ(results.size(), 1UL) << "Key " << i << " should still exist";
	}
	for (int i = 45; i < 50; ++i) {
		auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(i)));
		ASSERT_EQ(results.size(), 1UL) << "Key " << i << " should still exist";
	}
}

TEST_F(IndexTreeManagerTest, MergeNodesWithNextLeafLink) {
	// Create at least 3 leaves by inserting enough data
	for (int i = 0; i < 60; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_FALSE(root.isLeaf()) << "Need at least 2 leaves for this test";

	// Find a middle leaf that has both prev and next
	int64_t midLeafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(30)));
	auto midLeaf = dataManager_->getIndex(midLeafId);

	// Verify it has both neighbors (for the merge to exercise nextLeafId update)
	bool hasNext = midLeaf.getNextLeafId() != 0;
	bool hasPrev = midLeaf.getPrevLeafId() != 0;

	if (hasNext && hasPrev) {
		// Remove entries from mid leaf to trigger merge
		auto entries = midLeaf.getAllEntries(dataManager_);
		for (const auto &entry : entries) {
			int64_t key = std::get<int64_t>(entry.key.getVariant());
			for (int64_t val : entry.values) {
				intTreeManager_->remove(intRootId_, PropertyValue(key), val);
			}
		}
	}

	// Verify remaining data is still accessible
	auto result = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(0)));
	ASSERT_EQ(result.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, InternalNodeMergeUpdatesChildParents) {
	// Create a tree with enough data for multi-level
	for (int i = 0; i < 80; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	// The tree should have at least 2 levels
	ASSERT_GE(root.getLevel(), 1) << "Need height >= 1 for internal merge test";

	// Mass delete to trigger internal node merge
	for (int i = 10; i < 70; ++i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Verify remaining data
	auto r1 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(5)));
	ASSERT_EQ(r1.size(), 1UL);

	auto r2 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(75)));
	ASSERT_EQ(r2.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, RedistributeFromRightInternal) {
	// Build a tree and then selectively delete to force redistribute
	for (int i = 0; i < 60; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Delete from the left side to trigger redistribute from right
	for (int i = 0; i < 15; ++i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Verify data integrity after redistribution
	auto result = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(30)));
	ASSERT_EQ(result.size(), 1UL);
	ASSERT_EQ(result[0], 30);
}

TEST_F(IndexTreeManagerTest, RedistributeFromLeftInternal) {
	// Build a tree and then selectively delete from right side
	for (int i = 0; i < 60; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Delete from the right side to trigger redistribute from left
	for (int i = 45; i < 60; ++i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Verify data integrity
	auto result = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(20)));
	ASSERT_EQ(result.size(), 1UL);
	ASSERT_EQ(result[0], 20);
}

TEST_F(IndexTreeManagerTest, RemoveEntryWithBlobValues) {
	// Insert a key with many values to force values into blob storage
	const std::string testKey = "blob_values_key";
	size_t valueCount = (Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t)) + 5;

	for (size_t i = 0; i < valueCount; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(testKey), static_cast<int64_t>(i));
	}

	// Verify values are in blob
	int64_t leafId = stringTreeManager_->findLeafNode(stringRootId_, PropertyValue(testKey));
	ASSERT_NE(leafId, 0);
	auto leaf = dataManager_->getIndex(leafId);
	auto entries = leaf.getAllEntries(dataManager_);
	ASSERT_EQ(entries.size(), 1UL);
	ASSERT_NE(entries[0].valuesBlobId, 0) << "Values should be in blob storage";

	// Now remove ALL values one by one - the last removal should clean up the valuesBlobId
	for (size_t i = 0; i < valueCount; ++i) {
		stringTreeManager_->remove(stringRootId_, PropertyValue(testKey), static_cast<int64_t>(i));
	}

	// After removing all values, the entry should be gone
	auto results = stringTreeManager_->find(stringRootId_, PropertyValue(testKey));
	ASSERT_TRUE(results.empty());
}

TEST_F(IndexTreeManagerTest, RemoveEntryWithBlobKey) {
	const std::string longKey(Index::LEAF_KEY_INLINE_THRESHOLD + 5, 'X');

	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(longKey), 42);

	// Verify key is in blob
	int64_t leafId = stringTreeManager_->findLeafNode(stringRootId_, PropertyValue(longKey));
	auto leaf = dataManager_->getIndex(leafId);
	auto entries = leaf.getAllEntries(dataManager_);
	ASSERT_EQ(entries.size(), 1UL);
	ASSERT_NE(entries[0].keyBlobId, 0) << "Key should be in blob storage";

	// Remove the entry
	bool removed = stringTreeManager_->remove(stringRootId_, PropertyValue(longKey), 42);
	ASSERT_TRUE(removed);

	auto results = stringTreeManager_->find(stringRootId_, PropertyValue(longKey));
	ASSERT_TRUE(results.empty());
}

TEST_F(IndexTreeManagerTest, MergeWithLeftSiblingOnly) {
	// Build a tree with multiple leaves
	for (int i = 0; i < 30; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	if (!root.isLeaf()) {
		// Find rightmost leaf (has no right sibling)
		int64_t leafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(29)));
		auto leaf = dataManager_->getIndex(leafId);

		// If this leaf is the rightmost, it has no right sibling
		if (leaf.getNextLeafId() == 0 && leaf.getPrevLeafId() != 0) {
			// Delete entries from this rightmost leaf to trigger underflow
			auto entries = leaf.getAllEntries(dataManager_);
			for (const auto &entry : entries) {
				int64_t key = std::get<int64_t>(entry.key.getVariant());
				for (int64_t val : entry.values) {
					intTreeManager_->remove(intRootId_, PropertyValue(key), val);
				}
			}
		}
	}

	// Verify tree is still valid
	auto res = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(0)));
	ASSERT_EQ(res.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, RemoveTriggeringLeftRedistribute) {
	// Build tree with keys 100-2400 (avoiding key 0 complications)
	for (int i = 1; i <= 25; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i * 100)), i);
	}
	// Remove several keys from the high end to trigger underflow + left redistribute
	for (int i = 25; i >= 21; --i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i * 100)), i);
	}
	// Verify some remaining keys are still findable (tree is functional)
	auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(100)));
	ASSERT_FALSE(results.empty()) << "Key 100 not found after redistribute";
	auto results2 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(1000)));
	ASSERT_FALSE(results2.empty()) << "Key 1000 not found after redistribute";
}

TEST_F(IndexTreeManagerTest, RemoveTriggeringMerge) {
	// Insert keys 100-2000
	for (int i = 1; i <= 20; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i * 100)), i);
	}
	// Remove enough keys to trigger merge rather than redistribute
	for (int i = 20; i >= 11; --i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i * 100)), i);
	}
	// Verify remaining keys still accessible
	auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(100)));
	ASSERT_FALSE(results.empty()) << "Key 100 not found after merge";
	auto results2 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(500)));
	ASSERT_FALSE(results2.empty()) << "Key 500 not found after merge";
}

TEST_F(IndexTreeManagerTest, HandleUnderflowRootInternalMultipleChildren) {
	// Build a tree with enough data so root has 3+ children
	for (int i = 0; i < 40; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_FALSE(root.isLeaf());
	size_t rootChildCount = root.getChildCount();
	ASSERT_GE(rootChildCount, 3u) << "Need root with 3+ children";

	// Find the leftmost leaf and delete enough entries from it to trigger
	// a merge that removes one child from root, but root still has >1 children
	int64_t leafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(0)));
	auto leaf = dataManager_->getIndex(leafId);
	auto entries = leaf.getAllEntries(dataManager_);

	// Delete entries from leftmost leaf
	for (const auto &entry : entries) {
		int64_t key = std::get<int64_t>(entry.key.getVariant());
		for (int64_t val : entry.values) {
			intTreeManager_->remove(intRootId_, PropertyValue(key), val);
		}
	}

	// Root should still be valid, possibly with fewer children
	auto rootAfter = dataManager_->getIndex(intRootId_);
	ASSERT_GT(rootAfter.getEntryCount() + rootAfter.getChildCount(), 0u);

	// Verify some data is still accessible
	auto res = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(39)));
	ASSERT_EQ(res.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, HandleUnderflowRootLeaf) {
	// Insert 2 entries so root stays a leaf
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(1)), 1);
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(2)), 2);

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_TRUE(root.isLeaf()) << "Root should be a leaf with only 2 entries";

	// Remove one entry. Since root is a leaf with parentId=0, the handleUnderflow
	// path for root-leaf should be exercised (line 237-252, node.isLeaf() = true
	// so the if at line 238 is false).
	ASSERT_TRUE(intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(1)), 1));

	auto res = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(2)));
	ASSERT_EQ(res.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, MergeNodesParentUnderflowNotRoot) {
	// Build a 3+ level tree
	for (int i = 0; i < 200; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_GE(root.getLevel(), 2u) << "Need 3-level tree for parent-not-root underflow";

	// Delete a contiguous range from the middle to trigger cascading merges
	// where intermediate parents (not root) underflow
	for (int i = 50; i < 150; ++i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Verify data integrity
	auto r1 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(10)));
	ASSERT_EQ(r1.size(), 1UL);
	auto r2 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(190)));
	ASSERT_EQ(r2.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, InternalRedistributeFromRight) {
	// Build a multi-level tree with enough data
	for (int i = 0; i < 150; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_GE(root.getLevel(), 1u);

	// Delete from the left side to starve the left internal node
	// The right internal node should redistribute
	for (int i = 0; i < 40; ++i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Verify remaining data
	auto res = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(100)));
	ASSERT_EQ(res.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, InternalRedistributeFromLeft) {
	// Build a multi-level tree
	for (int i = 0; i < 150; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_GE(root.getLevel(), 1u);

	// Delete from the right side to starve the right internal node
	for (int i = 149; i >= 110; --i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Verify remaining data
	auto res = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(50)));
	ASSERT_EQ(res.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, HandleUnderflowRootStillHasMultipleChildren) {
	// Create a tree where the root has many children
	// We insert enough data to get 4+ children on root, then remove from
	// just the second leaf to trigger a merge that leaves root with 3+ children
	for (int i = 0; i < 50; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_FALSE(root.isLeaf());
	size_t originalChildCount = root.getChildCount();

	if (originalChildCount >= 4) {
		// Find the second leaf and empty it to trigger a merge
		int64_t firstLeafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(0)));
		auto firstLeaf = dataManager_->getIndex(firstLeafId);
		int64_t secondLeafId = firstLeaf.getNextLeafId();
		if (secondLeafId != 0) {
			auto secondLeaf = dataManager_->getIndex(secondLeafId);
			auto entries = secondLeaf.getAllEntries(dataManager_);
			for (const auto &entry : entries) {
				int64_t key = std::get<int64_t>(entry.key.getVariant());
				for (int64_t val : entry.values) {
					intTreeManager_->remove(intRootId_, PropertyValue(key), val);
				}
			}
		}
	}

	// Verify tree is still valid
	auto rootAfter = dataManager_->getIndex(intRootId_);
	ASSERT_GT(rootAfter.getEntryCount() + rootAfter.getChildCount(), 0u);
}

TEST_F(IndexTreeManagerTest, RemoveNoUnderflow) {
	// Insert enough to create multi-leaf tree
	for (int i = 0; i < 30; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_FALSE(root.isLeaf());

	// Remove just one entry - should not cause underflow
	ASSERT_TRUE(intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(15)), 15));

	// Verify the removed key is gone
	auto res = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(15)));
	ASSERT_TRUE(res.empty());

	// Verify other keys still exist
	auto res2 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(14)));
	ASSERT_EQ(res2.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, HandleUnderflowRootCollapseExactlyOneChild) {
	// Build tree, then delete until root has exactly 1 child
	for (int i = 0; i < 20; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	if (!root.isLeaf()) {
		// Delete from the high end to force merges down to 1 child
		for (int i = 19; i >= 2; --i) {
			intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
		}

		// After sufficient deletions, the root should have collapsed
		// Tree should still be functional
		auto res = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(0)));
		ASSERT_EQ(res.size(), 1UL);
	}
}

TEST_F(IndexTreeManagerTest, RemoveNonMatchingValueFromMultiLeafTree) {
	// Build a multi-leaf tree
	for (int i = 0; i < 30; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_FALSE(root.isLeaf());

	// Try to remove with wrong value - key exists but value doesn't
	bool removed = intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(15)), 9999);
	ASSERT_FALSE(removed);

	// Verify the original entry is untouched
	auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(15)));
	ASSERT_EQ(results.size(), 1UL);
	ASSERT_EQ(results[0], 15);
}

TEST_F(IndexTreeManagerTest, RemoveFromFirstChildLeafNoLeftSibling) {
	// Build a multi-leaf tree
	for (int i = 0; i < 40; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_FALSE(root.isLeaf());

	// Find the FIRST (leftmost) leaf - it has no left sibling
	int64_t firstLeafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(0)));
	auto firstLeaf = dataManager_->getIndex(firstLeafId);
	ASSERT_EQ(firstLeaf.getPrevLeafId(), 0) << "First leaf should have no left sibling";

	auto entries = firstLeaf.getAllEntries(dataManager_);
	// Remove all but 1 entry from the first leaf to trigger underflow
	// handleUnderflow will try right sibling first (leftSiblingId == 0)
	for (size_t i = 0; i < entries.size() - 1; ++i) {
		int64_t key = std::get<int64_t>(entries[i].key.getVariant());
		for (int64_t val : entries[i].values) {
			intTreeManager_->remove(intRootId_, PropertyValue(key), val);
		}
	}

	// Now remove the last entry to trigger underflow with no left sibling
	int64_t lastKey = std::get<int64_t>(entries.back().key.getVariant());
	for (int64_t val : entries.back().values) {
		intTreeManager_->remove(intRootId_, PropertyValue(lastKey), val);
	}

	// Verify remaining data is intact
	auto res = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(39)));
	ASSERT_EQ(res.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, InternalMergeChildrenAlreadyOwnedByLeft) {
	// Build a large tree with 3+ levels
	for (int i = 0; i < 300; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_GE(root.getLevel(), 2u) << "Need 3-level tree for internal merge";

	// Delete a large contiguous range to trigger internal merges
	// During internal merge, leftNode's existing children already have
	// parentId == leftNode.getId(), so the if-check on line 417 is false
	for (int i = 100; i < 250; ++i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Verify remaining data
	auto r1 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(50)));
	ASSERT_EQ(r1.size(), 1UL);
	auto r2 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(280)));
	ASSERT_EQ(r2.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, InternalMergeRightNodeCleanup) {
	// Build a 3-level tree
	for (int i = 0; i < 500; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_GE(root.getLevel(), 2u) << "Need multi-level tree";

	// Delete from the center to cause cascading internal merges
	// This should trigger the rightNode cleanup path at line 441-444
	for (int i = 150; i < 400; ++i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Verify data integrity at extremes
	auto r1 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(0)));
	ASSERT_EQ(r1.size(), 1UL);
	auto r2 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(499)));
	ASSERT_EQ(r2.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, MergeNodesCausesNonRootParentUnderflow) {
	// Build a 3-level tree with many entries
	for (int i = 0; i < 400; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_GE(root.getLevel(), 2u) << "Need at least 3 levels";

	// Strategically delete to cause leaf merges that cascade to
	// non-root internal node underflow
	// Delete from the first quarter to starve one internal subtree
	for (int i = 0; i < 100; ++i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}
	// Delete from the third quarter too
	for (int i = 200; i < 300; ++i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Verify remaining data
	auto r1 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(150)));
	ASSERT_EQ(r1.size(), 1UL);
	auto r2 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(350)));
	ASSERT_EQ(r2.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, HandleUnderflowRootCollapseViaSpecificDeletion) {
	// Build a tree with exactly 2 children at root level
	for (int i = 0; i < 25; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_FALSE(root.isLeaf());

	// Delete entries from one side until root has only 1 child
	// This triggers the root collapse: root is internal with 1 child
	for (int i = 0; i < 20; ++i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// After collapsing, verify the root is either a leaf or has > 0 children
	auto rootAfter = dataManager_->getIndex(intRootId_);
	if (rootAfter.isLeaf()) {
		ASSERT_GT(rootAfter.getEntryCount(), 0u);
	} else {
		ASSERT_GT(rootAfter.getChildCount(), 1u);
	}

	// Verify remaining data
	for (int i = 20; i < 25; ++i) {
		auto res = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(i)));
		ASSERT_EQ(res.size(), 1UL) << "Key " << i << " should still exist after root collapse";
	}
}

TEST_F(IndexTreeManagerTest, MergeLastTwoLeavesNoNextPointer) {
	// Build a tree with a few leaves
	for (int i = 0; i < 30; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_FALSE(root.isLeaf());

	// Find the last leaf (getNextLeafId() == 0)
	int64_t leafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(29)));
	auto leaf = dataManager_->getIndex(leafId);

	// If this is truly the last leaf, delete its entries to trigger merge
	if (leaf.getNextLeafId() == 0) {
		auto entries = leaf.getAllEntries(dataManager_);
		for (const auto &entry : entries) {
			int64_t key = std::get<int64_t>(entry.key.getVariant());
			for (int64_t val : entry.values) {
				intTreeManager_->remove(intRootId_, PropertyValue(key), val);
			}
		}
	}

	// Verify tree is still valid
	auto res = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(0)));
	ASSERT_EQ(res.size(), 1UL);
}
