/**
 * @file test_Index.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/13
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include "graph/core/Index.hpp"

using namespace graph::storage;

class IndexTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Initialize test environment
		leafNode = Index(1, Index::NodeType::LEAF, 0);
		internalNode = Index(2, Index::NodeType::INTERNAL, 0);
	}

	Index leafNode;
	Index internalNode;
};

TEST_F(IndexTest, InsertStringKey) {
	// Test inserting key-value pairs
	ASSERT_TRUE(leafNode.insertStringKey("key1", 100));
	ASSERT_TRUE(leafNode.insertStringKey("key2", 200));
	ASSERT_FALSE(leafNode.insertStringKey("key1", 100)); // Duplicate insertion
	auto values = leafNode.findStringValues("key1");
	ASSERT_EQ(values.size(), 1);
	ASSERT_EQ(values[0], 100);
}

TEST_F(IndexTest, RemoveStringKey) {
	// Test removing key-value pairs
	leafNode.insertStringKey("key1", 100);
	leafNode.insertStringKey("key2", 200);
	ASSERT_TRUE(leafNode.removeStringKey("key1", 100));
	ASSERT_FALSE(leafNode.removeStringKey("key1", 100)); // Removing non-existent key-value pair
	auto values = leafNode.findStringValues("key1");
	ASSERT_TRUE(values.empty());
}

TEST_F(IndexTest, FindStringValues) {
	// Test finding key-value pairs
	leafNode.insertStringKey("key1", 100);
	leafNode.insertStringKey("key2", 200);
	auto values = leafNode.findStringValues("key1");
	ASSERT_EQ(values.size(), 1);
	ASSERT_EQ(values[0], 100);
	values = leafNode.findStringValues("key3"); // Finding non-existent key
	ASSERT_TRUE(values.empty());
}

TEST_F(IndexTest, AddChild) {
	// Test adding child nodes
	ASSERT_TRUE(internalNode.addChild("key1", 100));
	ASSERT_TRUE(internalNode.addChild("key2", 200));
	auto children = internalNode.getAllChildren();
	ASSERT_EQ(children.size(), 2);
	ASSERT_EQ(children[0].first, "key1");
	ASSERT_EQ(children[0].second, 100);
}

TEST_F(IndexTest, RemoveChild) {
	// Test removing child nodes
	internalNode.addChild("key1", 100);
	internalNode.addChild("key2", 200);
	ASSERT_TRUE(internalNode.removeChild("key1"));
	auto children = internalNode.getAllChildren();
	ASSERT_EQ(children.size(), 1);
	ASSERT_EQ(children[0].first, "key2");
	ASSERT_EQ(children[0].second, 200);
}

TEST_F(IndexTest, LeafNodeLinkedList) {
	// Test leaf node linked list structure
	Index leafNode1(1, Index::NodeType::LEAF, 0);
	Index leafNode2(2, Index::NodeType::LEAF, 0);

	leafNode1.setNextLeafId(leafNode2.getId());
	leafNode2.setPrevLeafId(leafNode1.getId());

	ASSERT_EQ(leafNode1.getNextLeafId(), leafNode2.getId());
	ASSERT_EQ(leafNode2.getPrevLeafId(), leafNode1.getId());
}

TEST_F(IndexTest, EmptyTreeOperations) {
	// Test operations on an empty tree
	ASSERT_TRUE(leafNode.insertStringKey("key1", 100));
	ASSERT_TRUE(leafNode.removeStringKey("key1", 100));
	auto values = leafNode.findStringValues("key1");
	ASSERT_TRUE(values.empty());
}

TEST_F(IndexTest, ExtremeKeyValues) {
	// Test extreme key values
	ASSERT_TRUE(leafNode.insertStringKey("key_min", INT64_MIN));
	ASSERT_TRUE(leafNode.insertStringKey("key_max", INT64_MAX));
	auto valuesMin = leafNode.findStringValues("key_min");
	auto valuesMax = leafNode.findStringValues("key_max");
	ASSERT_EQ(valuesMin.size(), 1);
	ASSERT_EQ(valuesMin[0], INT64_MIN);
	ASSERT_EQ(valuesMax.size(), 1);
	ASSERT_EQ(valuesMax[0], INT64_MAX);
}

TEST_F(IndexTest, Serialization) {
	// Test serialization and deserialization
	leafNode.insertStringKey("key1", 100);
	leafNode.insertStringKey("key2", 200);

	std::ostringstream os;
	leafNode.serialize(os);

	std::istringstream is(os.str());
	auto deserializedNode = Index::deserialize(is);

	auto values = deserializedNode.findStringValues("key1");
	ASSERT_EQ(values.size(), 1);
	ASSERT_EQ(values[0], 100);
	values = deserializedNode.findStringValues("key2");
	ASSERT_EQ(values.size(), 1);
	ASSERT_EQ(values[0], 200);
}
