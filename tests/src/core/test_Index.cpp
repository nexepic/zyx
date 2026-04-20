/**
 * @file test_Index.cpp
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
// Core Index Tests
// ============================================================================

TEST_F(IndexTest, LeafKeyBlobManagement) {
	// 1. Oversized Key
	std::string oversized_key_str(Index::LEAF_KEY_INLINE_THRESHOLD + 5, 'K');
	std::vector<Index::Entry> entries = {{PropertyValue(oversized_key_str), {1}, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_NE(retrieved[0].keyBlobId, 0);
	int64_t old_blob_id = retrieved[0].keyBlobId;

	// 2. Small Key
	// Use a very small string ("s") to ensure it stays inline after adding
	// serialization overhead (4 bytes length). 1+4 = 5 bytes < 32.
	std::string small_key_str = "s";

	std::vector<Index::Entry> updated_entries = {{PropertyValue(small_key_str), {1}, old_blob_id, 0}};
	leafNode_.setAllEntries(updated_entries, dataManager_);

	retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_EQ(retrieved[0].keyBlobId, 0) << "Small key should be stored inline.";

	ASSERT_THROW((void) dataManager_->getBlobManager()->readBlobChain(old_blob_id), std::runtime_error);
}

TEST_F(IndexTest, LeafValuesBlobManagement) {
	// Dynamically calculate value count to exceed threshold
	// Threshold is in bytes, so we divide by int64 size
	size_t threshold_count = Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t);
	size_t oversized_count = threshold_count + 2; // +2 to be safe

	std::vector<int64_t> oversized_values;
	oversized_values.reserve(oversized_count);
	for (size_t i = 0; i < oversized_count; ++i) {
		oversized_values.push_back(static_cast<int64_t>(i));
	}

	// 1. Create
	std::vector<Index::Entry> entries = {{PropertyValue("key"), oversized_values, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_NE(retrieved[0].valuesBlobId, 0)
			<< "Value list of size " << (oversized_count * 8)
			<< " bytes should be blob (Threshold: " << Index::LEAF_VALUES_INLINE_THRESHOLD << ")";

	int64_t old_blob_id = retrieved[0].valuesBlobId;

	// 2. Update to small
	std::vector<Index::Entry> small_entry_list = {{PropertyValue("key"), {999}, 0, old_blob_id}};
	leafNode_.setAllEntries(small_entry_list, dataManager_);

	retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_EQ(retrieved[0].valuesBlobId, 0);

	// Verify deletion
	ASSERT_THROW((void) dataManager_->getBlobManager()->readBlobChain(old_blob_id), std::runtime_error);
}

TEST_F(IndexTest, IsUnderflowRootNode) {
	// Root node (parentId == 0) should never be in underflow
	// Tests branch at line 46-48: if (metadata.parentId == 0) return false
	leafNode_.setParentId(0); // Root node has no parent
	leafNode_.getMutableMetadata().dataUsage = 0; // Even with zero usage

	EXPECT_FALSE(leafNode_.isUnderflow(0.5)) << "Root node should never be in underflow";
}

TEST_F(IndexTest, IsUnderflowNonRootNode) {
	// Non-root node should check data usage
	// Tests branch at line 49: return metadata.dataUsage < threshold
	leafNode_.setParentId(100); // Non-root node
	leafNode_.getMutableMetadata().dataUsage = 10;

	// With 50% threshold, underflow threshold is DATA_SIZE * 0.5
	double threshold = 0.5;
	EXPECT_TRUE(leafNode_.isUnderflow(threshold)) << "Node with low usage should be in underflow";
	EXPECT_FALSE(leafNode_.isUnderflow(0.01)) << "Node with low usage should not be in underflow with very low threshold";
}

TEST_F(IndexTest, SerializeDeserializeRoundTrip) {
	// Create a leaf node with some data
	Index original(42, Index::NodeType::LEAF, 5);
	original.setParentId(10);
	original.setNextLeafId(20);
	original.setPrevLeafId(30);
	original.setLevel(3);

	// Serialize
	std::ostringstream os;
	original.serialize(os);

	// Deserialize
	std::string serialized = os.str();
	std::istringstream is(serialized);
	Index deserialized = Index::deserialize(is);

	EXPECT_EQ(deserialized.getId(), 42);
	EXPECT_EQ(deserialized.getParentId(), 10);
	EXPECT_EQ(deserialized.getNextLeafId(), 20);
	EXPECT_EQ(deserialized.getPrevLeafId(), 30);
	EXPECT_EQ(deserialized.getLevel(), 3);
	EXPECT_EQ(deserialized.getIndexType(), 5u);
	EXPECT_EQ(deserialized.getNodeType(), Index::NodeType::LEAF);
	EXPECT_TRUE(deserialized.isActive());
}

TEST_F(IndexTest, ConstructorSetsLevelCorrectly) {
	Index leaf(0, Index::NodeType::LEAF, 1);
	EXPECT_EQ(leaf.getLevel(), 0) << "Leaf node should have level 0";

	Index internal(0, Index::NodeType::INTERNAL, 1);
	EXPECT_EQ(internal.getLevel(), 1) << "Internal node should have level 1";
}

TEST_F(IndexTest, IsUnderflowNonRootWithData) {
	// Create a node with some actual data usage
	leafNode_.setParentId(50); // Non-root
	leafNode_.insertEntry(PropertyValue("key1"), 1, dataManager_, stringComparator);

	// After insertion, dataUsage > 0
	uint32_t usage = leafNode_.getMetadata().dataUsage;
	EXPECT_GT(usage, 0u);

	// With threshold that makes threshold value > usage, should be underflow
	EXPECT_TRUE(leafNode_.isUnderflow(0.99));

	// With threshold of 0.0, nothing should be underflow
	EXPECT_FALSE(leafNode_.isUnderflow(0.0));
}

TEST_F(IndexTest, ConstructorInitializesMetadata) {
	Index leaf(0, Index::NodeType::LEAF, 42);
	EXPECT_EQ(leaf.getMetadata().entryCount, 0u);
	EXPECT_EQ(leaf.getMetadata().childCount, 0u);
	EXPECT_EQ(leaf.getMetadata().dataUsage, 0u);
	EXPECT_EQ(leaf.getMetadata().indexType, 42u);
	EXPECT_EQ(leaf.getLevel(), 0);
	EXPECT_TRUE(leaf.isLeaf());

	Index internal(0, Index::NodeType::INTERNAL, 99);
	EXPECT_EQ(internal.getLevel(), 1);
	EXPECT_FALSE(internal.isLeaf());
}

TEST_F(IndexTest, IsUnderflowNonRoot_NotUnderflow) {
	leafNode_.setParentId(100); // Non-root
	// Set dataUsage to be above the 50% threshold
	leafNode_.getMutableMetadata().dataUsage = static_cast<uint32_t>(Index::DATA_SIZE);

	EXPECT_FALSE(leafNode_.isUnderflow(0.5)) << "Node at full capacity should not be in underflow";
}
