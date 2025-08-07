/**
 * @file test_Edge.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/28
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include <sstream>
#include "graph/core/Edge.hpp"
#include "graph/core/PropertyTypes.hpp"

class EdgeTest : public ::testing::Test {
protected:
	void SetUp() override {
		// No setup needed for direct Edge testing
	}

	void TearDown() override {
		// No cleanup needed
	}
};

TEST_F(EdgeTest, DefaultConstructor) {
	graph::Edge edge;

	EXPECT_EQ(edge.getId(), 0);
	EXPECT_EQ(edge.getSourceNodeId(), 0);
	EXPECT_EQ(edge.getTargetNodeId(), 0);
	EXPECT_EQ(edge.getNextOutEdgeId(), 0);
	EXPECT_EQ(edge.getPrevOutEdgeId(), 0);
	EXPECT_EQ(edge.getNextInEdgeId(), 0);
	EXPECT_EQ(edge.getPrevInEdgeId(), 0);
	EXPECT_EQ(edge.getPropertyEntityId(), 0);
	EXPECT_TRUE(edge.isActive());
	EXPECT_EQ(edge.getLabel(), "");
}

TEST_F(EdgeTest, ParameterizedConstructor) {
	constexpr int64_t id = 123;
	constexpr int64_t sourceId = 456;
	constexpr int64_t targetId = 789;
	const std::string label = "CONNECTED_TO";

	const graph::Edge edge(id, sourceId, targetId, label);

	EXPECT_EQ(edge.getId(), id);
	EXPECT_EQ(edge.getSourceNodeId(), sourceId);
	EXPECT_EQ(edge.getTargetNodeId(), targetId);
	EXPECT_EQ(edge.getLabel(), label);
	EXPECT_TRUE(edge.isActive());
	EXPECT_EQ(edge.getNextOutEdgeId(), 0);
	EXPECT_EQ(edge.getPrevOutEdgeId(), 0);
	EXPECT_EQ(edge.getNextInEdgeId(), 0);
	EXPECT_EQ(edge.getPrevInEdgeId(), 0);
}

TEST_F(EdgeTest, SetGetLabel) {
	graph::Edge edge;
	const std::string testLabel = "TEST_RELATIONSHIP";

	edge.setLabel(testLabel);

	EXPECT_EQ(edge.getLabel(), testLabel);
}

TEST_F(EdgeTest, SetLabelTruncation) {
	graph::Edge edge;
	// Create a label longer than LABEL_BUFFER_SIZE
	const std::string longLabel(graph::Edge::LABEL_BUFFER_SIZE + 10, 'X');

	edge.setLabel(longLabel);

	// Should be truncated to fit in buffer
	std::string retrievedLabel = edge.getLabel();
	EXPECT_LT(retrievedLabel.size(), longLabel.size());
	EXPECT_EQ(retrievedLabel.size(), graph::Edge::LABEL_BUFFER_SIZE - 1);
}

TEST_F(EdgeTest, NodeRelationshipSetters) {
	graph::Edge edge;

	edge.setSourceNodeId(100);
	edge.setTargetNodeId(200);

	EXPECT_EQ(edge.getSourceNodeId(), 100);
	EXPECT_EQ(edge.getTargetNodeId(), 200);
}

TEST_F(EdgeTest, EdgeLinkingSetters) {
	graph::Edge edge;

	edge.setNextOutEdgeId(301);
	edge.setPrevOutEdgeId(302);
	edge.setNextInEdgeId(303);
	edge.setPrevInEdgeId(304);

	EXPECT_EQ(edge.getNextOutEdgeId(), 301);
	EXPECT_EQ(edge.getPrevOutEdgeId(), 302);
	EXPECT_EQ(edge.getNextInEdgeId(), 303);
	EXPECT_EQ(edge.getPrevInEdgeId(), 304);
}

TEST_F(EdgeTest, PropertyOperations) {
	graph::Edge edge;
	const std::string key1 = "weight";
	const std::string key2 = "type";
	const graph::PropertyValue value1(42.5);
	const graph::PropertyValue value2("important");

	// Add properties
	edge.addProperty(key1, value1);
	edge.addProperty(key2, value2);

	// Check existence
	EXPECT_TRUE(edge.hasProperty(key1));
	EXPECT_TRUE(edge.hasProperty(key2));
	EXPECT_FALSE(edge.hasProperty("nonexistent"));

	// Get properties
	EXPECT_EQ(edge.getProperty(key1), value1);
	EXPECT_EQ(edge.getProperty(key2), value2);

	// Get all properties
	const auto &properties = edge.getProperties();
	EXPECT_EQ(properties.size(), 2);
	EXPECT_EQ(properties.at(key1), value1);
	EXPECT_EQ(properties.at(key2), value2);
}

TEST_F(EdgeTest, PropertyRemoval) {
	graph::Edge edge;
	const std::string key = "removable";
	const graph::PropertyValue value("test");

	edge.addProperty(key, value);
	EXPECT_TRUE(edge.hasProperty(key));

	edge.removeProperty(key);
	EXPECT_FALSE(edge.hasProperty(key));

	// Removing non-existent property should not throw
	edge.removeProperty("nonexistent");
}

TEST_F(EdgeTest, PropertyException) {
	const graph::Edge edge;

	EXPECT_THROW(static_cast<void>(edge.getProperty("nonexistent")), std::out_of_range);
}

TEST_F(EdgeTest, ClearProperties) {
	graph::Edge edge;
	edge.addProperty("key1", graph::PropertyValue(1));
	edge.addProperty("key2", graph::PropertyValue(2));

	EXPECT_EQ(edge.getProperties().size(), 2);

	edge.clearProperties();
	EXPECT_EQ(edge.getProperties().size(), 0);
}

TEST_F(EdgeTest, PropertyEntityManagement) {
	graph::Edge edge;

	EXPECT_FALSE(edge.hasPropertyEntity());
	EXPECT_EQ(edge.getPropertyEntityId(), 0);
	EXPECT_EQ(edge.getPropertyStorageType(), graph::PropertyStorageType::NONE);

	edge.setPropertyEntityId(999, graph::PropertyStorageType::PROPERTY_ENTITY);

	EXPECT_TRUE(edge.hasPropertyEntity());
	EXPECT_EQ(edge.getPropertyEntityId(), 999);
	EXPECT_EQ(edge.getPropertyStorageType(), graph::PropertyStorageType::PROPERTY_ENTITY);
}

TEST_F(EdgeTest, ActiveState) {
	graph::Edge edge;

	EXPECT_TRUE(edge.isActive());

	edge.markInactive();
	EXPECT_FALSE(edge.isActive());

	edge.markInactive(true);
	EXPECT_TRUE(edge.isActive());
}

TEST_F(EdgeTest, SerializeDeserializeEmpty) {
	graph::Edge originalEdge(100, 200, 300, "");
	std::stringstream ss;

	originalEdge.serialize(ss);
	graph::Edge deserializedEdge = graph::Edge::deserialize(ss);

	EXPECT_EQ(deserializedEdge.getId(), originalEdge.getId());
	EXPECT_EQ(deserializedEdge.getSourceNodeId(), originalEdge.getSourceNodeId());
	EXPECT_EQ(deserializedEdge.getTargetNodeId(), originalEdge.getTargetNodeId());
	EXPECT_EQ(deserializedEdge.getLabel(), originalEdge.getLabel());
	EXPECT_EQ(deserializedEdge.isActive(), originalEdge.isActive());
}

TEST_F(EdgeTest, SerializeDeserializeWithData) {
	graph::Edge originalEdge(500, 600, 700, "RELATIONSHIP");
	originalEdge.setNextOutEdgeId(800);
	originalEdge.setPrevInEdgeId(900);
	originalEdge.setPropertyEntityId(1000, graph::PropertyStorageType::PROPERTY_ENTITY);
	originalEdge.markInactive(false);

	std::stringstream ss;
	originalEdge.serialize(ss);
	graph::Edge deserializedEdge = graph::Edge::deserialize(ss);

	EXPECT_EQ(deserializedEdge.getId(), originalEdge.getId());
	EXPECT_EQ(deserializedEdge.getSourceNodeId(), originalEdge.getSourceNodeId());
	EXPECT_EQ(deserializedEdge.getTargetNodeId(), originalEdge.getTargetNodeId());
	EXPECT_EQ(deserializedEdge.getLabel(), originalEdge.getLabel());
	EXPECT_EQ(deserializedEdge.getNextOutEdgeId(), originalEdge.getNextOutEdgeId());
	EXPECT_EQ(deserializedEdge.getPrevInEdgeId(), originalEdge.getPrevInEdgeId());
	EXPECT_EQ(deserializedEdge.getPropertyEntityId(), originalEdge.getPropertyEntityId());
	EXPECT_EQ(deserializedEdge.getPropertyStorageType(), originalEdge.getPropertyStorageType());
	EXPECT_EQ(deserializedEdge.isActive(), originalEdge.isActive());
}

TEST_F(EdgeTest, GetSerializedSize) {
	const std::string testLabel = "TEST_LABEL";
	const graph::Edge edge(1, 2, 3, testLabel);

	size_t expectedSize = 0;
	expectedSize += sizeof(int64_t) * 8; // 8 int64_t fields
	expectedSize += sizeof(uint32_t); // propertyStorageType
	expectedSize += sizeof(bool); // isActive
	expectedSize += sizeof(uint32_t); // String length prefix
	expectedSize += testLabel.size(); // String content

	EXPECT_EQ(edge.getSerializedSize(), expectedSize);
}

TEST_F(EdgeTest, GetSerializedSizeEmptyLabel) {
	const graph::Edge edge(1, 2, 3, "");

	size_t expectedSize = 0;
	expectedSize += sizeof(int64_t) * 8; // 8 int64_t fields
	expectedSize += sizeof(uint32_t); // propertyStorageType
	expectedSize += sizeof(bool); // isActive
	expectedSize += sizeof(uint32_t); // String length prefix
	// No string content for empty label

	EXPECT_EQ(edge.getSerializedSize(), expectedSize);
}

TEST_F(EdgeTest, GetTotalPropertySize) {
	graph::Edge edge;

	EXPECT_EQ(edge.getTotalPropertySize(), 0);

	edge.addProperty("key1", graph::PropertyValue(42));
	edge.addProperty("longer_key_name", graph::PropertyValue("string_value"));

	size_t expectedSize = 0;
	expectedSize += std::string("key1").size();
	expectedSize += std::string("longer_key_name").size();
	expectedSize += graph::property_utils::getPropertyValueSize(graph::PropertyValue(42));
	expectedSize += graph::property_utils::getPropertyValueSize(graph::PropertyValue("string_value"));

	EXPECT_EQ(edge.getTotalPropertySize(), expectedSize);
}

TEST_F(EdgeTest, Constants) {
	EXPECT_EQ(graph::Edge::getTotalSize(), 256u);
	EXPECT_EQ(graph::Edge::TOTAL_EDGE_SIZE, 256u);

	size_t expectedMetadataSize = 0;
	expectedMetadataSize += sizeof(int64_t) * 8;
	expectedMetadataSize += sizeof(uint32_t);
	expectedMetadataSize += sizeof(bool);

	EXPECT_EQ(graph::Edge::METADATA_SIZE, expectedMetadataSize);
	EXPECT_EQ(graph::Edge::LABEL_BUFFER_SIZE, graph::Edge::TOTAL_EDGE_SIZE - graph::Edge::METADATA_SIZE);
	EXPECT_EQ(graph::Edge::typeId, graph::toUnderlying(graph::EntityType::Edge));
}

TEST_F(EdgeTest, TypeIdConstant) {
	// Verify that typeId matches the expected entity type
	EXPECT_EQ(graph::Edge::typeId, graph::toUnderlying(graph::EntityType::Edge));
}
