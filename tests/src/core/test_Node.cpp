/**
 * @file test_Node.cpp
 * @author Nexepic
 * @date 2025/7/28
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
	// Changed: Check Label ID instead of string
	EXPECT_EQ(node.getLabelId(), 0);
}

TEST_F(NodeTest, ParameterizedConstructor) {
	constexpr int64_t id = 123;
	constexpr int64_t labelId = 888; // Using ID

	const graph::Node node(id, labelId);

	EXPECT_EQ(node.getId(), id);
	EXPECT_EQ(node.getLabelId(), labelId);
	EXPECT_EQ(node.getFirstOutEdgeId(), 0);
	EXPECT_EQ(node.getFirstInEdgeId(), 0);
	EXPECT_TRUE(node.isActive());
}

TEST_F(NodeTest, SetGetLabelId) {
	graph::Node node;
	const int64_t testLabelId = 777;

	node.setLabelId(testLabelId);

	EXPECT_EQ(node.getLabelId(), testLabelId);
}

// Removed: SetLabelTruncation

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
	EXPECT_EQ(properties.size(), 2UL);
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

	EXPECT_EQ(node.getProperties().size(), 2UL);

	node.clearProperties();
	EXPECT_EQ(node.getProperties().size(), 0UL);
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
	graph::Node originalNode(100, 0); // ID 100, Label ID 0
	std::stringstream ss;

	originalNode.serialize(ss);
	graph::Node deserializedNode = graph::Node::deserialize(ss);

	EXPECT_EQ(deserializedNode.getId(), originalNode.getId());
	EXPECT_EQ(deserializedNode.getLabelId(), originalNode.getLabelId());
	EXPECT_EQ(deserializedNode.getFirstOutEdgeId(), originalNode.getFirstOutEdgeId());
	EXPECT_EQ(deserializedNode.getFirstInEdgeId(), originalNode.getFirstInEdgeId());
	EXPECT_EQ(deserializedNode.isActive(), originalNode.isActive());
}

TEST_F(NodeTest, SerializeDeserializeWithData) {
	graph::Node originalNode(200, 555); // ID 200, Label ID 555
	originalNode.setFirstOutEdgeId(300);
	originalNode.setFirstInEdgeId(400);
	originalNode.setPropertyEntityId(500, graph::PropertyStorageType::PROPERTY_ENTITY);
	originalNode.markInactive(false);

	std::stringstream ss;
	originalNode.serialize(ss);
	graph::Node deserializedNode = graph::Node::deserialize(ss);

	EXPECT_EQ(deserializedNode.getId(), originalNode.getId());
	EXPECT_EQ(deserializedNode.getLabelId(), originalNode.getLabelId());
	EXPECT_EQ(deserializedNode.getFirstOutEdgeId(), originalNode.getFirstOutEdgeId());
	EXPECT_EQ(deserializedNode.getFirstInEdgeId(), originalNode.getFirstInEdgeId());
	EXPECT_EQ(deserializedNode.getPropertyEntityId(), originalNode.getPropertyEntityId());
	EXPECT_EQ(deserializedNode.getPropertyStorageType(), originalNode.getPropertyStorageType());
	EXPECT_EQ(deserializedNode.isActive(), originalNode.isActive());
}

TEST_F(NodeTest, GetSerializedSize) {
	const graph::Node node(1, 100); // ID 1, Label ID 100

	size_t expectedSize = 0;
	// 5 int64_t fields: id, firstOut, firstIn, propEntity, labelId
	expectedSize += sizeof(int64_t) * 5;
	// 1 uint32_t field: propertyStorageType
	expectedSize += sizeof(uint32_t);
	// 1 bool field: isActive
	expectedSize += sizeof(bool);

	EXPECT_EQ(node.getSerializedSize(), expectedSize);
}

TEST_F(NodeTest, GetTotalPropertySize) {
	graph::Node node;

	EXPECT_EQ(node.getTotalPropertySize(), 0UL);

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
	// 5 int64_t + 1 uint32_t + 1 bool
	expectedMetadataSize += offsetof(graph::Node::Metadata, isActive) + sizeof(bool);

	EXPECT_EQ(graph::Node::METADATA_SIZE, expectedMetadataSize);
	// No more LABEL_BUFFER_SIZE check
	EXPECT_EQ(graph::Node::typeId, graph::toUnderlying(graph::EntityType::Node));
}

TEST_F(NodeTest, MetadataAccess) {
	graph::Node node(42, 999);

	const auto &metadata = node.getMetadata();
	EXPECT_EQ(metadata.id, 42);
	EXPECT_EQ(metadata.labelId, 999);
	EXPECT_EQ(metadata.firstOutEdgeId, 0);
	EXPECT_EQ(metadata.firstInEdgeId, 0);
	EXPECT_EQ(metadata.propertyEntityId, 0);
	EXPECT_TRUE(metadata.isActive);

	auto &mutableMetadata = node.getMutableMetadata();
	mutableMetadata.firstOutEdgeId = 100;
	EXPECT_EQ(node.getFirstOutEdgeId(), 100);
}

// Removed: LabelEmptyString, LabelWithNullCharacters tests

TEST_F(NodeTest, TypeIdConstant) {
	// Verify that typeId matches the expected entity type
	EXPECT_EQ(graph::Node::typeId, graph::toUnderlying(graph::EntityType::Node));
}

// Branch coverage: hasPropertyEntity() when storageType != NONE but propertyEntityId == 0
TEST_F(NodeTest, HasPropertyEntityWithNonNoneStorageTypeButZeroId) {
	graph::Node node;
	// Directly set the metadata to have a non-NONE storage type but zero property entity ID
	auto &meta = node.getMutableMetadata();
	meta.propertyStorageType = static_cast<uint32_t>(graph::PropertyStorageType::PROPERTY_ENTITY);
	meta.propertyEntityId = 0;

	// storageType != NONE (true), but propertyEntityId == 0 (false) => hasPropertyEntity() should be false
	EXPECT_FALSE(node.hasPropertyEntity());
	EXPECT_NE(node.getPropertyStorageType(), graph::PropertyStorageType::NONE);
	EXPECT_EQ(node.getPropertyEntityId(), 0);
}
