/**
 * @file test_Edge.cpp
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
	// Changed: Check Label ID instead of string
	EXPECT_EQ(edge.getLabelId(), 0);
}

TEST_F(EdgeTest, ParameterizedConstructor) {
	constexpr int64_t id = 123;
	constexpr int64_t sourceId = 456;
	constexpr int64_t targetId = 789;
	constexpr int64_t labelId = 999; // Using ID now

	const graph::Edge edge(id, sourceId, targetId, labelId);

	EXPECT_EQ(edge.getId(), id);
	EXPECT_EQ(edge.getSourceNodeId(), sourceId);
	EXPECT_EQ(edge.getTargetNodeId(), targetId);
	// Changed: Check Label ID
	EXPECT_EQ(edge.getLabelId(), labelId);
	EXPECT_TRUE(edge.isActive());
	EXPECT_EQ(edge.getNextOutEdgeId(), 0);
	EXPECT_EQ(edge.getPrevOutEdgeId(), 0);
	EXPECT_EQ(edge.getNextInEdgeId(), 0);
	EXPECT_EQ(edge.getPrevInEdgeId(), 0);
}

TEST_F(EdgeTest, SetGetLabelId) {
	graph::Edge edge;
	const int64_t testLabelId = 555;

	edge.setLabelId(testLabelId);

	EXPECT_EQ(edge.getLabelId(), testLabelId);
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
	EXPECT_EQ(properties.size(), 2UL);
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

	EXPECT_EQ(edge.getProperties().size(), 2UL);

	edge.clearProperties();
	EXPECT_EQ(edge.getProperties().size(), 0UL);
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
	// ID-based constructor: 0 means no label
	graph::Edge originalEdge(100, 200, 300, 0);
	std::stringstream ss;

	originalEdge.serialize(ss);
	graph::Edge deserializedEdge = graph::Edge::deserialize(ss);

	EXPECT_EQ(deserializedEdge.getId(), originalEdge.getId());
	EXPECT_EQ(deserializedEdge.getSourceNodeId(), originalEdge.getSourceNodeId());
	EXPECT_EQ(deserializedEdge.getTargetNodeId(), originalEdge.getTargetNodeId());
	EXPECT_EQ(deserializedEdge.getLabelId(), originalEdge.getLabelId());
	EXPECT_EQ(deserializedEdge.isActive(), originalEdge.isActive());
}

TEST_F(EdgeTest, SerializeDeserializeWithData) {
	graph::Edge originalEdge(500, 600, 700, 42); // Label ID 42
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
	EXPECT_EQ(deserializedEdge.getLabelId(), originalEdge.getLabelId());
	EXPECT_EQ(deserializedEdge.getNextOutEdgeId(), originalEdge.getNextOutEdgeId());
	EXPECT_EQ(deserializedEdge.getPrevInEdgeId(), originalEdge.getPrevInEdgeId());
	EXPECT_EQ(deserializedEdge.getPropertyEntityId(), originalEdge.getPropertyEntityId());
	EXPECT_EQ(deserializedEdge.getPropertyStorageType(), originalEdge.getPropertyStorageType());
	EXPECT_EQ(deserializedEdge.isActive(), originalEdge.isActive());
}

TEST_F(EdgeTest, GetSerializedSize) {
	const graph::Edge edge(1, 2, 3, 10); // Label ID 10

	// Calculate expected size based on POD fields
	size_t expectedSize = 0;
	// 9 int64_t fields: id, source, target, nextOut, prevOut, nextIn, prevIn, propEntity, labelId
	expectedSize += sizeof(int64_t) * 9;
	// 1 uint32_t field: propertyStorageType
	expectedSize += sizeof(uint32_t);
	// 1 bool field: isActive
	expectedSize += sizeof(bool);

	EXPECT_EQ(edge.getSerializedSize(), expectedSize);
}

TEST_F(EdgeTest, GetTotalPropertySize) {
	graph::Edge edge;

	EXPECT_EQ(edge.getTotalPropertySize(), 0UL);

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
	// 9 int64_t + 1 uint32_t + 1 bool
	expectedMetadataSize += offsetof(graph::Edge::Metadata, isActive) + sizeof(bool);

	EXPECT_EQ(graph::Edge::METADATA_SIZE, expectedMetadataSize);
	// No more LABEL_BUFFER_SIZE, checking PADDING_SIZE instead is optional but good practice
	// EXPECT_EQ(graph::Edge::PADDING_SIZE, 128u - expectedMetadataSize);
	EXPECT_EQ(graph::Edge::typeId, graph::toUnderlying(graph::EntityType::Edge));
}

TEST_F(EdgeTest, TypeIdConstant) {
	// Verify that typeId matches the expected entity type
	EXPECT_EQ(graph::Edge::typeId, graph::toUnderlying(graph::EntityType::Edge));
}
