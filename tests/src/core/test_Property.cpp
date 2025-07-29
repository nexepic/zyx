/**
 * @file test_Property.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/28
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include <sstream>
#include "graph/core/Property.hpp"
#include "graph/core/PropertyValue.hpp"

class PropertyTest : public ::testing::Test {
protected:
	void SetUp() override {
		// No setup needed for direct Property testing
	}

	void TearDown() override {
		// No cleanup needed
	}
};

TEST_F(PropertyTest, DefaultConstructor) {
	graph::Property property;

	EXPECT_EQ(property.getMetadata().id, 0);
	EXPECT_EQ(property.getMetadata().entityId, 0);
	EXPECT_EQ(property.getMetadata().entityType, 0u);
	EXPECT_TRUE(property.getMetadata().isActive);
	EXPECT_TRUE(property.isActive());
	EXPECT_EQ(property.getPropertyValues().size(), 0);
}

TEST_F(PropertyTest, ParameterizedConstructor) {
	constexpr int64_t id = 123;
	constexpr int64_t entityId = 456;
	constexpr uint32_t entityType = 1; // Edge type

	graph::Property property(id, entityId, entityType);

	EXPECT_EQ(property.getMetadata().id, id);
	EXPECT_EQ(property.getMetadata().entityId, entityId);
	EXPECT_EQ(property.getMetadata().entityType, entityType);
	EXPECT_TRUE(property.getMetadata().isActive);
	EXPECT_TRUE(property.isActive());
}

TEST_F(PropertyTest, AddPropertyValue) {
	graph::Property property;
	const std::string key = "test_key";
	const graph::PropertyValue value("test_value");

	property.addPropertyValue(key, value);

	EXPECT_TRUE(property.hasPropertyValue(key));
	EXPECT_EQ(property.getPropertyValue(key), value);
	EXPECT_EQ(property.getPropertyValues().size(), 1);
}

TEST_F(PropertyTest, AddMultiplePropertyValues) {
	graph::Property property;
	const std::string key1 = "name";
	const std::string key2 = "age";
	const std::string key3 = "score";
	const graph::PropertyValue value1("Alice");
	const graph::PropertyValue value2(30);
	const graph::PropertyValue value3(95.5);

	property.addPropertyValue(key1, value1);
	property.addPropertyValue(key2, value2);
	property.addPropertyValue(key3, value3);

	EXPECT_TRUE(property.hasPropertyValue(key1));
	EXPECT_TRUE(property.hasPropertyValue(key2));
	EXPECT_TRUE(property.hasPropertyValue(key3));
	EXPECT_EQ(property.getPropertyValue(key1), value1);
	EXPECT_EQ(property.getPropertyValue(key2), value2);
	EXPECT_EQ(property.getPropertyValue(key3), value3);
	EXPECT_EQ(property.getPropertyValues().size(), 3);
}

TEST_F(PropertyTest, OverwritePropertyValue) {
	graph::Property property;
	const std::string key = "changeable";
	const graph::PropertyValue value1("original");
	const graph::PropertyValue value2("updated");

	property.addPropertyValue(key, value1);
	EXPECT_EQ(property.getPropertyValue(key), value1);

	property.addPropertyValue(key, value2);
	EXPECT_EQ(property.getPropertyValue(key), value2);
	EXPECT_EQ(property.getPropertyValues().size(), 1);
}

TEST_F(PropertyTest, HasPropertyValue) {
	graph::Property property;
	const std::string existingKey = "exists";
	const std::string nonexistentKey = "does_not_exist";

	property.addPropertyValue(existingKey, graph::PropertyValue(42));

	EXPECT_TRUE(property.hasPropertyValue(existingKey));
	EXPECT_FALSE(property.hasPropertyValue(nonexistentKey));
}

TEST_F(PropertyTest, GetPropertyValueException) {
	const graph::Property property;

	EXPECT_THROW(static_cast<void>(property.getPropertyValue("nonexistent")), std::out_of_range);
}

TEST_F(PropertyTest, RemovePropertyValue) {
	graph::Property property;
	const std::string key = "removable";
	const graph::PropertyValue value("test");

	property.addPropertyValue(key, value);
	EXPECT_TRUE(property.hasPropertyValue(key));
	EXPECT_EQ(property.getPropertyValues().size(), 1);

	property.removePropertyValue(key);
	EXPECT_FALSE(property.hasPropertyValue(key));
	EXPECT_EQ(property.getPropertyValues().size(), 0);
}

TEST_F(PropertyTest, RemoveNonexistentPropertyValue) {
	graph::Property property;

	// Should not throw when removing non-existent key
	property.removePropertyValue("nonexistent");
	EXPECT_EQ(property.getPropertyValues().size(), 0);
}

TEST_F(PropertyTest, SetProperties) {
	graph::Property property;
	std::unordered_map<std::string, graph::PropertyValue> newProperties;
	newProperties["key1"] = graph::PropertyValue("value1");
	newProperties["key2"] = graph::PropertyValue(100);
	newProperties["key3"] = graph::PropertyValue(true);

	property.setProperties(newProperties);

	EXPECT_EQ(property.getPropertyValues().size(), 3);
	EXPECT_TRUE(property.hasPropertyValue("key1"));
	EXPECT_TRUE(property.hasPropertyValue("key2"));
	EXPECT_TRUE(property.hasPropertyValue("key3"));
	EXPECT_EQ(property.getPropertyValue("key1"), graph::PropertyValue("value1"));
	EXPECT_EQ(property.getPropertyValue("key2"), graph::PropertyValue(100));
	EXPECT_EQ(property.getPropertyValue("key3"), graph::PropertyValue(true));
}

TEST_F(PropertyTest, MarkInactive) {
	graph::Property property(1, 2, 0);

	EXPECT_TRUE(property.isActive());

	property.markInactive();
	EXPECT_FALSE(property.isActive());
	EXPECT_FALSE(property.getMetadata().isActive);
}

TEST_F(PropertyTest, SerializeDeserializeEmpty) {
	graph::Property originalProperty(100, 200, 1);
	std::stringstream ss;

	originalProperty.serialize(ss);
	graph::Property deserializedProperty = graph::Property::deserialize(ss);

	EXPECT_EQ(deserializedProperty.getMetadata().id, originalProperty.getMetadata().id);
	EXPECT_EQ(deserializedProperty.getMetadata().entityId, originalProperty.getMetadata().entityId);
	EXPECT_EQ(deserializedProperty.getMetadata().isActive, originalProperty.getMetadata().isActive);
	EXPECT_EQ(deserializedProperty.getMetadata().entityType, originalProperty.getMetadata().entityType);
	EXPECT_EQ(deserializedProperty.getMetadata().isActive, originalProperty.getMetadata().isActive);
	EXPECT_EQ(deserializedProperty.getPropertyValues().size(), 0);
}

TEST_F(PropertyTest, SerializeDeserializeWithValues) {
	graph::Property originalProperty(300, 400, 0);
	originalProperty.addPropertyValue("name", graph::PropertyValue("Test"));
	originalProperty.addPropertyValue("count", graph::PropertyValue(static_cast<int64_t>(42)));
	originalProperty.addPropertyValue("active", graph::PropertyValue(true));
	originalProperty.addPropertyValue("ratio", graph::PropertyValue(3.14));
	originalProperty.markInactive();

	std::stringstream ss;
	originalProperty.serialize(ss);
	graph::Property deserializedProperty = graph::Property::deserialize(ss);

	EXPECT_EQ(deserializedProperty.getMetadata().id, originalProperty.getMetadata().id);
	EXPECT_EQ(deserializedProperty.getMetadata().entityId, originalProperty.getMetadata().entityId);
	EXPECT_EQ(deserializedProperty.getMetadata().entityType, originalProperty.getMetadata().entityType);
	EXPECT_EQ(deserializedProperty.getPropertyValue("count"), graph::PropertyValue(static_cast<int64_t>(42)));
	EXPECT_EQ(deserializedProperty.getPropertyValue("active"), graph::PropertyValue(true));
	EXPECT_EQ(deserializedProperty.getPropertyValue("ratio"), graph::PropertyValue(3.14));
}

TEST_F(PropertyTest, GetSerializedSizeEmpty) {
	const graph::Property property(1, 2, 0);

	size_t expectedSize = 0;
	expectedSize += sizeof(int64_t); // id
	expectedSize += sizeof(int64_t); // entityId
	expectedSize += sizeof(uint32_t); // entityType
	expectedSize += sizeof(bool); // isActive
	expectedSize += sizeof(uint32_t); // property count (0)

	EXPECT_EQ(property.getSerializedSize(), expectedSize);
}

TEST_F(PropertyTest, GetSerializedSizeWithValues) {
	graph::Property property(1, 2, 0);
	property.addPropertyValue("short", graph::PropertyValue(1));
	property.addPropertyValue("longer_key_name", graph::PropertyValue("string_value"));

	size_t expectedSize = 0;
	expectedSize += sizeof(int64_t); // id
	expectedSize += sizeof(int64_t); // entityId
	expectedSize += sizeof(uint32_t); // entityType
	expectedSize += sizeof(bool); // isActive
	expectedSize += sizeof(uint32_t); // property count

	// First property
	expectedSize += sizeof(uint32_t); // key length prefix
	expectedSize += std::string("short").size(); // key content
	expectedSize += graph::property_utils::getPropertyValueSize(graph::PropertyValue(1));

	// Second property
	expectedSize += sizeof(uint32_t); // key length prefix
	expectedSize += std::string("longer_key_name").size(); // key content
	expectedSize += graph::property_utils::getPropertyValueSize(graph::PropertyValue("string_value"));

	EXPECT_EQ(property.getSerializedSize(), expectedSize);
}

TEST_F(PropertyTest, Constants) {
	EXPECT_EQ(graph::Property::getTotalSize(), 256u);
	EXPECT_EQ(graph::Property::TOTAL_PROPERTY_SIZE, 256u);

	size_t expectedMetadataSize = 0;
	expectedMetadataSize += sizeof(int64_t) * 2;
	expectedMetadataSize += sizeof(uint32_t);
	expectedMetadataSize += sizeof(bool);

	EXPECT_EQ(graph::Property::METADATA_SIZE, expectedMetadataSize);
	EXPECT_EQ(graph::Property::typeId, graph::toUnderlying(graph::EntityType::Property));
}

TEST_F(PropertyTest, MetadataAccess) {
	graph::Property property(42, 84, 1);

	const auto &metadata = property.getMetadata();
	EXPECT_EQ(metadata.id, 42);
	EXPECT_EQ(metadata.entityId, 84);
	EXPECT_EQ(metadata.entityType, 1u);
	EXPECT_TRUE(metadata.isActive);

	auto &mutableMetadata = property.getMutableMetadata();
	mutableMetadata.entityId = 168;
	EXPECT_EQ(property.getMetadata().entityId, 168);
}

TEST_F(PropertyTest, PropertyValueTypes) {
	graph::Property property;

	// Test different PropertyValue types
	property.addPropertyValue("int_val", graph::PropertyValue(42));
	property.addPropertyValue("double_val", graph::PropertyValue(3.14159));
	property.addPropertyValue("string_val", graph::PropertyValue("hello"));
	property.addPropertyValue("bool_val", graph::PropertyValue(true));

	EXPECT_EQ(property.getPropertyValues().size(), 4);
	EXPECT_EQ(property.getPropertyValue("int_val"), graph::PropertyValue(42));
	EXPECT_EQ(property.getPropertyValue("double_val"), graph::PropertyValue(3.14159));
	EXPECT_EQ(property.getPropertyValue("string_val"), graph::PropertyValue("hello"));
	EXPECT_EQ(property.getPropertyValue("bool_val"), graph::PropertyValue(true));
}

TEST_F(PropertyTest, EmptyStringKeys) {
	graph::Property property;
	const std::string emptyKey;
	const graph::PropertyValue value("test");

	property.addPropertyValue(emptyKey, value);
	EXPECT_TRUE(property.hasPropertyValue(emptyKey));
	EXPECT_EQ(property.getPropertyValue(emptyKey), value);
}

TEST_F(PropertyTest, TypeIdConstant) {
	// Verify that typeId matches the expected entity type
	EXPECT_EQ(graph::Property::typeId, graph::toUnderlying(graph::EntityType::Property));
}
