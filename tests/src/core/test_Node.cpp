/**
 * @file test_Node.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/28
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include <sstream>
#include "graph/core/Node.hpp"
#include "graph/core/PropertyTypes.hpp"

class NodeTest : public ::testing::Test {
protected:
	void SetUp() override {
		// No setup needed for direct Node testing
	}

	void TearDown() override {
		// No cleanup needed
	}
};

TEST_F(NodeTest, DefaultConstructor) {
	const graph::Node node;

	EXPECT_EQ(node.getId(), 0);
	EXPECT_EQ(node.getFirstOutEdgeId(), 0);
	EXPECT_EQ(node.getFirstInEdgeId(), 0);
	EXPECT_EQ(node.getPropertyEntityId(), 0);
	EXPECT_TRUE(node.isActive());
	EXPECT_EQ(node.getLabel(), "");
}

TEST_F(NodeTest, ParameterizedConstructor) {
	constexpr int64_t id = 123;
	const std::string label = "TestNode";

	const graph::Node node(id, label);

	EXPECT_EQ(node.getId(), id);
	EXPECT_EQ(node.getLabel(), label);
	EXPECT_EQ(node.getFirstOutEdgeId(), 0);
	EXPECT_EQ(node.getFirstInEdgeId(), 0);
	EXPECT_TRUE(node.isActive());
}

TEST_F(NodeTest, SetGetLabel) {
	graph::Node node;
	const std::string testLabel = "MyNode";

	node.setLabel(testLabel);

	EXPECT_EQ(node.getLabel(), testLabel);
}

TEST_F(NodeTest, SetLabelTruncation) {
	graph::Node node;
	// Create a label longer than LABEL_BUFFER_SIZE
	const std::string longLabel(graph::Node::LABEL_BUFFER_SIZE + 10, 'X');

	node.setLabel(longLabel);

	// Should be truncated to fit in buffer
	std::string retrievedLabel = node.getLabel();
	EXPECT_LT(retrievedLabel.size(), longLabel.size());
	EXPECT_EQ(retrievedLabel.size(), graph::Node::LABEL_BUFFER_SIZE - 1);
}

TEST_F(NodeTest, EdgeRelationshipSetters) {
	graph::Node node;

	node.setFirstOutEdgeId(100);
	node.setFirstInEdgeId(200);

	EXPECT_EQ(node.getFirstOutEdgeId(), 100);
	EXPECT_EQ(node.getFirstInEdgeId(), 200);
}

TEST_F(NodeTest, PropertyOperations) {
	graph::Node node;
	const std::string key1 = "name";
	const std::string key2 = "age";
	const graph::PropertyValue value1("Alice");
	const graph::PropertyValue value2(30);

	// Add properties
	node.addProperty(key1, value1);
	node.addProperty(key2, value2);

	// Check existence
	EXPECT_TRUE(node.hasProperty(key1));
	EXPECT_TRUE(node.hasProperty(key2));
	EXPECT_FALSE(node.hasProperty("nonexistent"));

	// Get properties
	EXPECT_EQ(node.getProperty(key1), value1);
	EXPECT_EQ(node.getProperty(key2), value2);

	// Get all properties
	const auto &properties = node.getProperties();
	EXPECT_EQ(properties.size(), 2);
	EXPECT_EQ(properties.at(key1), value1);
	EXPECT_EQ(properties.at(key2), value2);
}

TEST_F(NodeTest, PropertyRemoval) {
	graph::Node node;
	const std::string key = "removable";
	const graph::PropertyValue value("test");

	node.addProperty(key, value);
	EXPECT_TRUE(node.hasProperty(key));

	node.removeProperty(key);
	EXPECT_FALSE(node.hasProperty(key));

	// Removing non-existent property should not throw
	node.removeProperty("nonexistent");
}

TEST_F(NodeTest, PropertyException) {
	const graph::Node node;

	EXPECT_THROW(static_cast<void>(node.getProperty("nonexistent")), std::out_of_range);
}

TEST_F(NodeTest, ClearProperties) {
	graph::Node node;
	node.addProperty("key1", graph::PropertyValue(1));
	node.addProperty("key2", graph::PropertyValue(2));

	EXPECT_EQ(node.getProperties().size(), 2);

	node.clearProperties();
	EXPECT_EQ(node.getProperties().size(), 0);
}

TEST_F(NodeTest, PropertyEntityManagement) {
	graph::Node node;

	EXPECT_FALSE(node.hasPropertyEntity());
	EXPECT_EQ(node.getPropertyEntityId(), 0);
	EXPECT_EQ(node.getPropertyStorageType(), graph::PropertyStorageType::NONE);

	node.setPropertyEntityId(999, graph::PropertyStorageType::PROPERTY_ENTITY);

	EXPECT_TRUE(node.hasPropertyEntity());
	EXPECT_EQ(node.getPropertyEntityId(), 999);
	EXPECT_EQ(node.getPropertyStorageType(), graph::PropertyStorageType::PROPERTY_ENTITY);
}

TEST_F(NodeTest, ActiveState) {
	graph::Node node;

	EXPECT_TRUE(node.isActive());

	node.markInactive();
	EXPECT_FALSE(node.isActive());

	node.markInactive(true);
	EXPECT_TRUE(node.isActive());
}

TEST_F(NodeTest, SerializeDeserializeEmpty) {
	graph::Node originalNode(100, "");
	std::stringstream ss;

	originalNode.serialize(ss);
	graph::Node deserializedNode = graph::Node::deserialize(ss);

	EXPECT_EQ(deserializedNode.getId(), originalNode.getId());
	EXPECT_EQ(deserializedNode.getLabel(), originalNode.getLabel());
	EXPECT_EQ(deserializedNode.getFirstOutEdgeId(), originalNode.getFirstOutEdgeId());
	EXPECT_EQ(deserializedNode.getFirstInEdgeId(), originalNode.getFirstInEdgeId());
	EXPECT_EQ(deserializedNode.isActive(), originalNode.isActive());
}

TEST_F(NodeTest, SerializeDeserializeWithData) {
	graph::Node originalNode(200, "TestNode");
	originalNode.setFirstOutEdgeId(300);
	originalNode.setFirstInEdgeId(400);
	originalNode.setPropertyEntityId(500, graph::PropertyStorageType::PROPERTY_ENTITY);
	originalNode.markInactive(false);

	std::stringstream ss;
	originalNode.serialize(ss);
	graph::Node deserializedNode = graph::Node::deserialize(ss);

	EXPECT_EQ(deserializedNode.getId(), originalNode.getId());
	EXPECT_EQ(deserializedNode.getLabel(), originalNode.getLabel());
	EXPECT_EQ(deserializedNode.getFirstOutEdgeId(), originalNode.getFirstOutEdgeId());
	EXPECT_EQ(deserializedNode.getFirstInEdgeId(), originalNode.getFirstInEdgeId());
	EXPECT_EQ(deserializedNode.getPropertyEntityId(), originalNode.getPropertyEntityId());
	EXPECT_EQ(deserializedNode.getPropertyStorageType(), originalNode.getPropertyStorageType());
	EXPECT_EQ(deserializedNode.isActive(), originalNode.isActive());
}

TEST_F(NodeTest, GetSerializedSize) {
	const std::string testLabel = "TestLabel";
	const graph::Node node(1, testLabel);

	size_t expectedSize = 0;
	expectedSize += sizeof(int64_t) * 4; // id, firstOutEdgeId, firstInEdgeId, propertyEntityId
	expectedSize += sizeof(uint32_t); // propertyStorageType
	expectedSize += sizeof(bool); // isActive
	expectedSize += sizeof(uint32_t); // String length prefix
	expectedSize += testLabel.size(); // String content

	EXPECT_EQ(node.getSerializedSize(), expectedSize);
}

TEST_F(NodeTest, GetSerializedSizeEmptyLabel) {
	const graph::Node node(1, "");

	size_t expectedSize = 0;
	expectedSize += sizeof(int64_t) * 4; // id, firstOutEdgeId, firstInEdgeId, propertyEntityId
	expectedSize += sizeof(uint32_t); // propertyStorageType
	expectedSize += sizeof(bool); // isActive
	expectedSize += sizeof(uint32_t); // String length prefix (0)
	// No string content for empty label

	EXPECT_EQ(node.getSerializedSize(), expectedSize);
}

TEST_F(NodeTest, GetTotalPropertySize) {
	graph::Node node;

	EXPECT_EQ(node.getTotalPropertySize(), 0);

	node.addProperty("name", graph::PropertyValue("Alice"));
	node.addProperty("age", graph::PropertyValue(25));

	size_t expectedSize = 0;
	expectedSize += std::string("name").size();
	expectedSize += std::string("age").size();
	expectedSize += graph::property_utils::getPropertyValueSize(graph::PropertyValue("Alice"));
	expectedSize += graph::property_utils::getPropertyValueSize(graph::PropertyValue(25));

	EXPECT_EQ(node.getTotalPropertySize(), expectedSize);
}

TEST_F(NodeTest, Constants) {
	EXPECT_EQ(graph::Node::getTotalSize(), 256u);
	EXPECT_EQ(graph::Node::TOTAL_NODE_SIZE, 256u);

	size_t expectedMetadataSize = 0;
	expectedMetadataSize += sizeof(int64_t) * 4;
	expectedMetadataSize += sizeof(uint32_t);
	expectedMetadataSize += sizeof(bool);

	EXPECT_EQ(graph::Node::METADATA_SIZE, expectedMetadataSize);
	EXPECT_EQ(graph::Node::LABEL_BUFFER_SIZE, graph::Node::TOTAL_NODE_SIZE - graph::Node::METADATA_SIZE);
	EXPECT_EQ(graph::Node::typeId, graph::toUnderlying(graph::EntityType::Node));
}

TEST_F(NodeTest, MetadataAccess) {
	graph::Node node(42, "TestNode");

	const auto &metadata = node.getMetadata();
	EXPECT_EQ(metadata.id, 42);
	EXPECT_EQ(metadata.firstOutEdgeId, 0);
	EXPECT_EQ(metadata.firstInEdgeId, 0);
	EXPECT_EQ(metadata.propertyEntityId, 0);
	EXPECT_TRUE(metadata.isActive);

	auto &mutableMetadata = node.getMutableMetadata();
	mutableMetadata.firstOutEdgeId = 100;
	EXPECT_EQ(node.getFirstOutEdgeId(), 100);
}

TEST_F(NodeTest, LabelEmptyString) {
	graph::Node node;
	node.setLabel("");
	EXPECT_EQ(node.getLabel(), "");
}

TEST_F(NodeTest, LabelWithNullCharacters) {
	graph::Node node;
	const std::string labelWithNull = "test\0embedded";
	node.setLabel(labelWithNull);

	// The label should be truncated at the first null character
	EXPECT_EQ(node.getLabel(), "test");
}

TEST_F(NodeTest, TypeIdConstant) {
	// Verify that typeId matches the expected entity type
	EXPECT_EQ(graph::Node::typeId, graph::toUnderlying(graph::EntityType::Node));
}
