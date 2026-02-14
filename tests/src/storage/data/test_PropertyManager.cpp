/**
 * @file test_PropertyManager.cpp
 * @author Nexepic
 * @date 2025/8/15
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

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include "boost/uuid/uuid.hpp"
#include "boost/uuid/uuid_generators.hpp"
#include "boost/uuid/uuid_io.hpp"
#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/EntityPropertyTraits.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/EdgeManager.hpp"
#include "graph/storage/data/NodeManager.hpp"
#include "graph/storage/data/PropertyManager.hpp"

class PropertyManagerTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::storage::PropertyManager> propertyManager;
	std::shared_ptr<graph::storage::NodeManager> nodeManager;
	std::shared_ptr<graph::storage::EdgeManager> edgeManager;

	// Test entities
	graph::Node testNode;
	graph::Edge testEdge;

	void SetUp() override {
		// Create a unique temporary database file
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_propertyManager_" + to_string(uuid) + ".dat");

		// Initialize database and get managers from the DataManager
		db = std::make_unique<graph::Database>(testFilePath.string());
		db->open();
		storage = db->getStorage();
		dataManager = storage->getDataManager();
		// Get the manager instances that were created by the DataManager
		propertyManager = dataManager->getPropertyManager();
		nodeManager = dataManager->getNodeManager();
		edgeManager = dataManager->getEdgeManager();

		// Create test entities using the correct API
		testNode.setLabelId(dataManager->getOrCreateLabelId("TestNode"));
		nodeManager->add(testNode);

		graph::Node targetNode;
		targetNode.setLabelId(dataManager->getOrCreateLabelId("TargetNode"));
		nodeManager->add(targetNode);

		testEdge.setSourceNodeId(testNode.getId());
		testEdge.setTargetNodeId(targetNode.getId());
		testEdge.setLabelId(dataManager->getOrCreateLabelId("CONNECTS_TO"));
		edgeManager->add(testEdge);
	}

	void TearDown() override {
		// Release resources and clean up the file
		db->close();
		db.reset();

		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
	}

	// Helper to create a large property map for testing blob storage
	static std::unordered_map<std::string, graph::PropertyValue> createLargePropertyMap() {
		std::unordered_map<std::string, graph::PropertyValue> properties;
		// The threshold for using a blob is internal to the PropertyManager,
		// but a large value should trigger it.
		std::string largeValue(500, 'X');

		for (int i = 0; i < 5; i++) {
			properties["large_key_" + std::to_string(i)] = graph::PropertyValue(largeValue + "_" + std::to_string(i));
		}

		return properties;
	}
};

// Test basic property entity CRUD operations
TEST_F(PropertyManagerTest, PropertyEntityCRUD) {
	// Create a property entity
	graph::Property property;
	property.setEntityId(testNode.getId());
	property.setEntityType(graph::Node::typeId);
	property.setProperties({{"name", graph::PropertyValue("test_property")}, {"value", graph::PropertyValue(123)}});

	// Add the property entity
	propertyManager->add(property);
	EXPECT_NE(property.getId(), 0) << "Property should have a non-zero ID after adding";

	// Get the property entity
	graph::Property retrievedProperty = propertyManager->get(property.getId());
	EXPECT_EQ(retrievedProperty.getId(), property.getId()) << "Retrieved property should have the same ID";

	auto props = retrievedProperty.getPropertyValues();
	EXPECT_EQ(std::get<std::string>(props.at("name").getVariant()), "test_property");
	EXPECT_EQ(std::get<int64_t>(props.at("value").getVariant()), 123);

	// Update the property entity
	property.setProperties({{"name", graph::PropertyValue("updated_property")}, {"value", graph::PropertyValue(456)}});
	propertyManager->update(property);

	// Get the updated property entity
	retrievedProperty = propertyManager->get(property.getId());
	props = retrievedProperty.getPropertyValues();
	EXPECT_EQ(std::get<std::string>(props.at("name").getVariant()), "updated_property");
	EXPECT_EQ(std::get<int64_t>(props.at("value").getVariant()), 456);

	// Remove the property entity
	propertyManager->remove(property);

	// Clear cache to ensure it's gone from disk
	dataManager->clearCache();

	// Verify the property entity was removed
	graph::Property removedProperty = propertyManager->get(property.getId());
	EXPECT_EQ(removedProperty.getId(), 0) << "Property should not be retrievable after removal";
}


// Test adding and retrieving entity properties (Node)
TEST_F(PropertyManagerTest, NodeEntityProperties) {
	// Get entity properties (should be empty initially)
	auto initialProps = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_TRUE(initialProps.empty()) << "Initial properties should be empty";

	// Add properties to the node
	std::unordered_map<std::string, graph::PropertyValue> properties;
	properties["name"] = graph::PropertyValue("John Doe");
	properties["age"] = graph::PropertyValue(30);
	properties["active"] = graph::PropertyValue(true);

	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), properties);

	// Get the properties
	auto retrievedProps = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_EQ(retrievedProps.size(), 3UL) << "Should retrieve 3 properties";
	EXPECT_EQ(std::get<std::string>(retrievedProps["name"].getVariant()), "John Doe");
	EXPECT_EQ(std::get<int64_t>(retrievedProps["age"].getVariant()), 30);
	EXPECT_EQ(std::get<bool>(retrievedProps["active"].getVariant()), true);

	// Remove a property
	propertyManager->removeEntityProperty<graph::Node>(testNode.getId(), "age");

	// Verify property was removed
	retrievedProps = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_EQ(retrievedProps.size(), 2UL) << "Should have 2 properties after removal";
	EXPECT_EQ(retrievedProps.count("age"), 0UL) << "Age property should be removed";
}

// Test adding and retrieving entity properties (Edge)
TEST_F(PropertyManagerTest, EdgeEntityProperties) {
	// Add properties to the edge
	std::unordered_map<std::string, graph::PropertyValue> properties;
	properties["weight"] = graph::PropertyValue(5.5);
	properties["timestamp"] = graph::PropertyValue(1628097645LL);

	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), properties);

	// Get the properties
	auto retrievedProps = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(retrievedProps.size(), 2UL) << "Should retrieve 2 properties";
	EXPECT_DOUBLE_EQ(std::get<double>(retrievedProps["weight"].getVariant()), 5.5);
	EXPECT_EQ(std::get<int64_t>(retrievedProps["timestamp"].getVariant()), 1628097645LL);
}

// Test large property storage (which should trigger Blob storage)
TEST_F(PropertyManagerTest, LargePropertyStorage) {
	// Create large property map
	auto largeProperties = createLargePropertyMap();

	// Add large properties to the node
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), largeProperties);

	// Get the properties back
	auto retrievedProps = propertyManager->getEntityProperties<graph::Node>(testNode.getId());

	// Verify properties were stored and retrieved correctly
	EXPECT_EQ(retrievedProps.size(), largeProperties.size()) << "Should retrieve all large properties";

	for (int i = 0; i < 5; i++) {
		std::string key = "large_key_" + std::to_string(i);
		std::string expectedValue = std::string(500, 'X') + "_" + std::to_string(i);
		EXPECT_EQ(std::get<std::string>(retrievedProps[key].getVariant()), expectedValue)
				<< "Large property " << i << " should match";
	}

	// Test removing one large property
	propertyManager->removeEntityProperty<graph::Node>(testNode.getId(), "large_key_2");

	// Verify property was removed
	retrievedProps = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_EQ(retrievedProps.size(), 4UL) << "Should have 4 properties after removal";
	EXPECT_EQ(retrievedProps.count("large_key_2"), 0UL) << "Removed property should not exist";
}

// Test error handling and edge cases
TEST_F(PropertyManagerTest, ErrorHandlingAndEdgeCases) {
	// Try to get properties for non-existent entity
	auto nonExistentProps = propertyManager->getEntityProperties<graph::Node>(999999);
	EXPECT_TRUE(nonExistentProps.empty()) << "Properties for non-existent entity should be empty";

	// Add properties to a valid entity
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), {{"key", graph::PropertyValue("value")}});

	// Try to remove a property that does not exist from an entity that does exist.
	// This should not throw an exception; it should just do nothing.
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<graph::Node>(testNode.getId(), "non_existent_key"));

	// Verify that the original property is still there
	auto props = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_EQ(props.size(), 1UL);
}

TEST_F(PropertyManagerTest, HasExternalPropertyCheck) {
	// 1. Test with no external properties (inline only if supported, or just empty)
	EXPECT_FALSE(propertyManager->hasExternalProperty<graph::Node>(testNode, "any_key"));

	// 2. Test with Property Entity storage (small properties)
	std::unordered_map<std::string, graph::PropertyValue> smallProps;
	smallProps["small_key"] = graph::PropertyValue("small_value");

	// Add properties which should be stored in a Property entity (external to Node)
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), smallProps);

	// Refresh node from DB to get updated flags/pointers
	graph::Node nodeWithProps = nodeManager->get(testNode.getId());

	// Check existing key
	EXPECT_TRUE(propertyManager->hasExternalProperty<graph::Node>(nodeWithProps, "small_key"));
	// Check non-existing key
	EXPECT_FALSE(propertyManager->hasExternalProperty<graph::Node>(nodeWithProps, "missing_key"));

	// 3. Test with Blob storage (large properties)
	// Create a new node for clean slate
	graph::Node blobNode;
	blobNode.setLabelId(dataManager->getOrCreateLabelId("BlobNode"));
	nodeManager->add(blobNode);

	auto largeProps = createLargePropertyMap();
	// Add a specific key we will check
	largeProps["blob_key"] = graph::PropertyValue("blob_value");

	propertyManager->addEntityProperties<graph::Node>(blobNode.getId(), largeProps);

	// Refresh node
	graph::Node nodeWithBlob = nodeManager->get(blobNode.getId());

	// Check blob-stored key
	EXPECT_TRUE(propertyManager->hasExternalProperty<graph::Node>(nodeWithBlob, "blob_key"));
	// Check non-existing key in blob
	EXPECT_FALSE(propertyManager->hasExternalProperty<graph::Node>(nodeWithBlob, "missing_blob_key"));
}

TEST_F(PropertyManagerTest, CalculateTotalPropertySize) {
	// 1. Initial size (empty)
	size_t size0 = propertyManager->calculateEntityTotalPropertySize<graph::Node>(testNode.getId());
	EXPECT_EQ(size0, 0UL);

	// 2. Add properties (should be stored externally in Property Entity due to Node design usually)
	// Or if Node supports inline, it covers that too.
	std::unordered_map<std::string, graph::PropertyValue> props;
	std::string key1 = "key1";
	std::string val1 = "value1";
	props[key1] = graph::PropertyValue(val1);

	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), props);

	size_t size1 = propertyManager->calculateEntityTotalPropertySize<graph::Node>(testNode.getId());

	// We check > 0 to be safe against implementation details of getPropertyValueSize,
	// or use >= key.size() + val.size().
	EXPECT_GT(size1, key1.size() + val1.size());

	// 3. Add Large properties (Blob Storage)
	graph::Node blobNode;
	blobNode.setLabelId(dataManager->getOrCreateLabelId("BlobNode"));
	nodeManager->add(blobNode);

	auto largeProps = createLargePropertyMap();
	propertyManager->addEntityProperties<graph::Node>(blobNode.getId(), largeProps);

	size_t sizeBlob = propertyManager->calculateEntityTotalPropertySize<graph::Node>(blobNode.getId());

	// Calculate expected size manually
	size_t expectedBlobSize = 0;
	for(const auto& [k, v] : largeProps) {
		expectedBlobSize += k.size();
		expectedBlobSize += graph::property_utils::getPropertyValueSize(v);
	}

	EXPECT_EQ(sizeBlob, expectedBlobSize);

	// 4. Non-existent entity
	size_t sizeInvalid = propertyManager->calculateEntityTotalPropertySize<graph::Node>(99999);
	EXPECT_EQ(sizeInvalid, 0UL);
}

TEST_F(PropertyManagerTest, UnsupportedEntityTypes) {
	// Assuming Blob entity type does not support arbitrary user properties
	// We need a valid ID to pass the initial checks in functions
	graph::Blob blob;
	blob.setData("some_data");
	dataManager->addBlobEntity(blob);

	// Test calculateEntityTotalPropertySize for Blob
	// Should return 0 immediately due to trait check
	size_t size = propertyManager->calculateEntityTotalPropertySize<graph::Blob>(blob.getId());
	EXPECT_EQ(size, static_cast<decltype(size)>(0));

	// Test hasExternalProperty for Blob
	// Should return false immediately
	bool hasProp = propertyManager->hasExternalProperty<graph::Blob>(blob, "any");
	EXPECT_FALSE(hasProp);

	// Test addEntityProperties for Blob -> Should throw
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["a"] = graph::PropertyValue(1);
	EXPECT_THROW(propertyManager->addEntityProperties<graph::Blob>(blob.getId(), props), std::runtime_error);
}

TEST_F(PropertyManagerTest, CorruptedStorageRecovery) {
	// 1. Create node with blob properties
	auto largeProps = createLargePropertyMap();
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), largeProps);

	// 2. Manually corrupt it by deleting the blob chain directly
	graph::Node node = nodeManager->get(testNode.getId());
	int64_t blobId = graph::storage::EntityPropertyTraits<graph::Node>::getPropertyEntityId(node);

	// Access blob manager to delete
	auto blobManager = dataManager->getBlobManager();
	blobManager->deleteBlobChain(blobId);

	// 3. Try to access properties
	// getPropertiesFromBlob should catch exception/fail gracefully and return empty
	auto props = propertyManager->getEntityProperties<graph::Node>(testNode.getId());

	// Should return empty map or at least not crash
	EXPECT_TRUE(props.empty());

	// 4. Try hasExternalProperty
	// deserialization inside hasExternalProperty might fail, should return false
	EXPECT_FALSE(propertyManager->hasExternalProperty<graph::Node>(node, "large_key_0"));
}

// Test storing empty properties (should return early without storing anything)
TEST_F(PropertyManagerTest, EmptyPropertiesStorage) {
	// Get initial node state
	graph::Node nodeBefore = nodeManager->get(testNode.getId());
	int64_t propertyIdBefore = graph::storage::EntityPropertyTraits<graph::Node>::getPropertyEntityId(nodeBefore);

	// Try to add empty properties
	std::unordered_map<std::string, graph::PropertyValue> emptyProps;
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), emptyProps);

	// Verify no property entity was created
	graph::Node nodeAfter = nodeManager->get(testNode.getId());
	int64_t propertyIdAfter = graph::storage::EntityPropertyTraits<graph::Node>::getPropertyEntityId(nodeAfter);

	EXPECT_EQ(propertyIdAfter, propertyIdBefore) << "Empty properties should not create external storage";
	EXPECT_EQ(propertyIdAfter, 0) << "Should have no external property entity";

	// Verify getEntityProperties returns empty for the node
	auto props = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_TRUE(props.empty()) << "Should have no properties";
}

// Test removing all properties from blob (should clear blob reference)
TEST_F(PropertyManagerTest, RemoveAllPropertiesFromBlob) {
	// Create node with large properties (stored in blob)
	auto largeProps = createLargePropertyMap();
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), largeProps);

	// Verify properties exist
	auto propsBefore = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_GT(propsBefore.size(), 0UL);

	// Get blob reference before removal
	graph::Node nodeBefore = nodeManager->get(testNode.getId());
	int64_t blobIdBefore = graph::storage::EntityPropertyTraits<graph::Node>::getPropertyEntityId(nodeBefore);
	EXPECT_NE(blobIdBefore, 0) << "Should have blob reference";

	// Remove all properties one by one
	std::vector<std::string> keys;
	for (const auto &key: propsBefore | std::views::keys) {
		keys.push_back(key);
	}

	for (const auto& key : keys) {
		propertyManager->removeEntityProperty<graph::Node>(testNode.getId(), key);
	}

	// Verify blob reference was cleared
	graph::Node nodeAfter = nodeManager->get(testNode.getId());
	int64_t blobIdAfter = graph::storage::EntityPropertyTraits<graph::Node>::getPropertyEntityId(nodeAfter);
	EXPECT_EQ(blobIdAfter, 0) << "Blob reference should be cleared after removing all properties";

	// Verify all properties are gone
	auto propsAfter = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_TRUE(propsAfter.empty()) << "Should have no properties after removing all";
}

// Test removing properties from blob causes blob recreation with remaining properties
TEST_F(PropertyManagerTest, BlobRecreationAfterPropertyRemoval) {
	// Create node with large properties (stored in blob)
	auto largeProps = createLargePropertyMap();
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), largeProps);

	// Verify initial properties
	auto propsBefore = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_EQ(propsBefore.size(), largeProps.size());

	// Remove one property
	propertyManager->removeEntityProperty<graph::Node>(testNode.getId(), "large_key_2");

	// Verify blob reference still exists with remaining properties
	graph::Node nodeAfter = nodeManager->get(testNode.getId());
	int64_t blobIdAfter = graph::storage::EntityPropertyTraits<graph::Node>::getPropertyEntityId(nodeAfter);
	EXPECT_NE(blobIdAfter, 0) << "Should still have blob reference with remaining properties";

	// Verify remaining properties exist and count is correct
	auto propsAfter = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_EQ(propsAfter.size(), largeProps.size() - 1) << "Should have one less property";
	EXPECT_FALSE(propsAfter.contains("large_key_2")) << "Removed key should not exist";

	// Verify all other keys still exist
	EXPECT_TRUE(propsAfter.contains("large_key_0"));
	EXPECT_TRUE(propsAfter.contains("large_key_1"));
	EXPECT_TRUE(propsAfter.contains("large_key_3"));
	EXPECT_TRUE(propsAfter.contains("large_key_4"));
}

// Test serializing and deserializing empty properties
TEST_F(PropertyManagerTest, SerializeDeserializeEmptyProperties) {
	std::unordered_map<std::string, graph::PropertyValue> emptyProps;

	// Serialize empty properties
	std::stringstream ss;
	graph::storage::PropertyManager::serializeProperties(ss, emptyProps);
	std::string serialized = ss.str();

	// Should at least write the count (0)
	EXPECT_GT(serialized.size(), 0UL);

	// Deserialize empty properties
	std::stringstream ssIn(serialized);
	auto deserialized = graph::storage::PropertyManager::deserializeProperties(ssIn);

	EXPECT_TRUE(deserialized.empty());
}

// Test serializing and deserializing single property
TEST_F(PropertyManagerTest, SerializeDeserializeSingleProperty) {
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["key1"] = graph::PropertyValue("value1");

	// Serialize
	std::stringstream ss;
	graph::storage::PropertyManager::serializeProperties(ss, props);

	// Deserialize
	auto deserialized = graph::storage::PropertyManager::deserializeProperties(ss);

	EXPECT_EQ(deserialized.size(), 1UL);
	EXPECT_TRUE(deserialized.contains("key1"));
	EXPECT_EQ(std::get<std::string>(deserialized["key1"].getVariant()), "value1");
}

// Test adding properties multiple times to different entities
TEST_F(PropertyManagerTest, AddPropertiesToMultipleEntities) {
	// Create another node
	graph::Node node2;
	node2.setLabelId(dataManager->getOrCreateLabelId("Node2"));
	nodeManager->add(node2);

	// Add different properties to each node
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), {{"name", graph::PropertyValue("Node1")}});
	propertyManager->addEntityProperties<graph::Node>(node2.getId(), {{"name", graph::PropertyValue("Node2")}});

	// Verify each node has its own properties
	auto props1 = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	auto props2 = propertyManager->getEntityProperties<graph::Node>(node2.getId());

	EXPECT_EQ(props1.size(), 1UL);
	EXPECT_EQ(props2.size(), 1UL);
	EXPECT_EQ(std::get<std::string>(props1["name"].getVariant()), "Node1");
	EXPECT_EQ(std::get<std::string>(props2["name"].getVariant()), "Node2");
}

// Test calculateEntityTotalPropertySize with mixed inline and external properties
TEST_F(PropertyManagerTest, CalculateSizeMixedProperties) {
	// Start with small properties (stored in Property Entity)
	std::unordered_map<std::string, graph::PropertyValue> smallProps;
	smallProps["key1"] = graph::PropertyValue("value1");
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), smallProps);

	size_t sizeSmall = propertyManager->calculateEntityTotalPropertySize<graph::Node>(testNode.getId());
	EXPECT_GT(sizeSmall, 0UL);

	// Now add large properties (should trigger blob storage and replace Property Entity)
	auto largeProps = createLargePropertyMap();
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), largeProps);

	size_t sizeLarge = propertyManager->calculateEntityTotalPropertySize<graph::Node>(testNode.getId());
	EXPECT_GT(sizeLarge, sizeSmall) << "Size should increase with large properties";

	// Calculate expected size
	size_t expectedSize = 0;
	for (const auto& [k, v] : largeProps) {
		expectedSize += k.size();
		expectedSize += graph::property_utils::getPropertyValueSize(v);
	}
	EXPECT_EQ(sizeLarge, expectedSize);
}

// Test removeEntityProperty with non-existent entity (idempotent operation)
TEST_F(PropertyManagerTest, RemovePropertyFromNonExistentEntity) {
	// Should not throw, just return silently
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<graph::Node>(99999, "any_key"));
}

// Test removeEntityProperty with inactive entity
TEST_F(PropertyManagerTest, RemovePropertyFromInactiveEntity) {
	// Create a node
	graph::Node tempNode;
	tempNode.setLabelId(dataManager->getOrCreateLabelId("TempNode"));
	nodeManager->add(tempNode);

	// Add properties
	propertyManager->addEntityProperties<graph::Node>(tempNode.getId(), {{"key", graph::PropertyValue("value")}});

	// Remove (mark as inactive) the node
	graph::Node node = nodeManager->get(tempNode.getId());
	nodeManager->remove(node);

	// Try to remove property from inactive node (should not throw)
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<graph::Node>(tempNode.getId(), "key"));
}

// Test Edge with empty properties (covers branch at line 110:7 for Edge)
TEST_F(PropertyManagerTest, EdgeWithEmptyProperties) {
	// Get initial edge state
	graph::Edge edgeBefore = edgeManager->get(testEdge.getId());
	int64_t propertyIdBefore = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edgeBefore);

	// Try to add empty properties to edge
	std::unordered_map<std::string, graph::PropertyValue> emptyProps;
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), emptyProps);

	// Verify no property entity was created
	graph::Edge edgeAfter = edgeManager->get(testEdge.getId());
	int64_t propertyIdAfter = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edgeAfter);

	EXPECT_EQ(propertyIdAfter, propertyIdBefore) << "Empty properties should not create external storage for Edge";
	EXPECT_EQ(propertyIdAfter, 0) << "Edge should have no external property entity with empty properties";

	// Verify getEntityProperties returns empty for the edge
	auto props = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_TRUE(props.empty()) << "Edge should have no properties";
}

// Test Edge with large properties triggering BLOB storage (covers branches at line 119:8 and 146:7 for Edge)
TEST_F(PropertyManagerTest, EdgeWithLargePropertiesInBlob) {
	// Create large property map for edge (larger than PROPERTY_ENTITY_PAYLOAD_SIZE)
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 10; i++) {
		std::string largeValue(1000, 'E'); // Large string value
		largeProps["edge_large_key_" + std::to_string(i)] = graph::PropertyValue(largeValue);
	}

	// Add large properties to the edge (should trigger BLOB storage)
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	// Verify properties were stored
	auto retrievedProps = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(retrievedProps.size(), largeProps.size()) << "Should retrieve all large edge properties";

	// Verify the edge has a BLOB_ENTITY reference
	graph::Edge edgeWithBlob = edgeManager->get(testEdge.getId());
	int64_t propertyId = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edgeWithBlob);
	EXPECT_NE(propertyId, 0) << "Edge should have external property reference";

	auto storageType = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeWithBlob);
	EXPECT_EQ(storageType, graph::PropertyStorageType::BLOB_ENTITY)
		<< "Large edge properties should be stored in BLOB";

	// Verify content
	for (int i = 0; i < 10; i++) {
		std::string key = "edge_large_key_" + std::to_string(i);
		std::string expectedValue = std::string(1000, 'E');
		EXPECT_TRUE(retrievedProps.contains(key)) << "Should contain key " << key;
		EXPECT_EQ(std::get<std::string>(retrievedProps[key].getVariant()), expectedValue)
			<< "Edge large property " << i << " should match";
	}
}

// Test removing blob-stored properties from edge (covers blob cleanup path)
TEST_F(PropertyManagerTest, RemoveBlobPropertiesFromEdge) {
	// Add large properties to edge (triggers BLOB storage)
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 5; i++) {
		std::string largeValue(800, 'E');
		largeProps["edge_blob_key_" + std::to_string(i)] = graph::PropertyValue(largeValue);
	}

	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	// Verify blob reference exists
	graph::Edge edgeBefore = edgeManager->get(testEdge.getId());
	int64_t blobIdBefore = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edgeBefore);
	EXPECT_NE(blobIdBefore, 0) << "Edge should have blob reference";

	// Remove all properties
	std::vector<std::string> keys;
	for (const auto &key: largeProps | std::views::keys) {
		keys.push_back(key);
	}

	for (const auto& key : keys) {
		propertyManager->removeEntityProperty<graph::Edge>(testEdge.getId(), key);
	}

	// Verify blob reference was cleared (cleanupExternalProperties with BLOB_ENTITY should execute)
	graph::Edge edgeAfter = edgeManager->get(testEdge.getId());
	int64_t blobIdAfter = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edgeAfter);
	EXPECT_EQ(blobIdAfter, 0) << "Blob reference should be cleared after removing all edge properties";

	// Verify all properties are gone
	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_TRUE(propsAfter.empty()) << "Edge should have no properties after removing all";
}

// Test calculateEntityTotalPropertySize with inactive entity (covers line 292)
TEST_F(PropertyManagerTest, CalculateSizeInactiveEntity) {
	// Create a node
	graph::Node tempNode;
	tempNode.setLabelId(dataManager->getOrCreateLabelId("TempNode"));
	nodeManager->add(tempNode);

	// Add properties
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["key1"] = graph::PropertyValue("value1");
	props["key2"] = graph::PropertyValue(123);
	propertyManager->addEntityProperties<graph::Node>(tempNode.getId(), props);

	// Get size before deactivation
	size_t sizeBefore = propertyManager->calculateEntityTotalPropertySize<graph::Node>(tempNode.getId());
	EXPECT_GT(sizeBefore, 0UL);

	// Remove (deactivate) the node
	graph::Node node = nodeManager->get(tempNode.getId());
	nodeManager->remove(node);

	// Try to calculate size for inactive node (should return 0)
	size_t sizeAfter = propertyManager->calculateEntityTotalPropertySize<graph::Node>(tempNode.getId());
	EXPECT_EQ(sizeAfter, static_cast<decltype(sizeAfter)>(0)) << "Inactive entity should have 0 property size";
}

// Test calculateEntityTotalPropertySize with non-existent entity (covers line 290)
TEST_F(PropertyManagerTest, CalculateSizeNonExistentEntity) {
	// Test with completely non-existent entity ID
	size_t size = propertyManager->calculateEntityTotalPropertySize<graph::Node>(999999);
	EXPECT_EQ(size, static_cast<decltype(size)>(0)) << "Non-existent entity should have 0 property size";
}

// Test calculateEntityTotalPropertySize after clearing all properties
TEST_F(PropertyManagerTest, CalculateSizeAfterClearingProperties) {
	// Add properties to node
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["key1"] = graph::PropertyValue("value1");
	props["key2"] = graph::PropertyValue("value2");
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), props);

	// Verify size is calculated
	size_t sizeWithProps = propertyManager->calculateEntityTotalPropertySize<graph::Node>(testNode.getId());
	EXPECT_GT(sizeWithProps, 0UL);

	// Remove all properties
	propertyManager->removeEntityProperty<graph::Node>(testNode.getId(), "key1");
	propertyManager->removeEntityProperty<graph::Node>(testNode.getId(), "key2");

	// Verify size is 0 after clearing
	size_t sizeWithoutProps = propertyManager->calculateEntityTotalPropertySize<graph::Node>(testNode.getId());
	EXPECT_EQ(sizeWithoutProps, static_cast<decltype(sizeWithoutProps)>(0)) << "Size should be 0 after removing all properties";
}

// Test hasExternalProperty with entity that has properties but key doesn't exist
TEST_F(PropertyManagerTest, HasExternalPropertyMissingKey) {
	// Add properties to node
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["existing_key"] = graph::PropertyValue("value");
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), props);

	// Refresh node
	graph::Node node = nodeManager->get(testNode.getId());

	// Check for non-existing key
	EXPECT_FALSE(propertyManager->hasExternalProperty<graph::Node>(node, "non_existing_key"))
		<< "Should return false for non-existing key even when entity has properties";
}

// Test property update from small to blob storage (covers transition paths)
TEST_F(PropertyManagerTest, UpdatePropertiesSmallToBlob) {
	// Add small properties first (stored in Property Entity)
	std::unordered_map<std::string, graph::PropertyValue> smallProps;
	smallProps["key1"] = graph::PropertyValue("small");
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), smallProps);

	// Verify initial storage type
	graph::Node nodeBefore = nodeManager->get(testNode.getId());
	auto storageTypeBefore = graph::storage::EntityPropertyTraits<graph::Node>::getPropertyStorageType(nodeBefore);
	EXPECT_EQ(storageTypeBefore, graph::PropertyStorageType::PROPERTY_ENTITY);

	// Add large properties (should trigger blob storage)
	auto largeProps = createLargePropertyMap();
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), largeProps);

	// Verify transition to blob storage
	graph::Node nodeAfter = nodeManager->get(testNode.getId());
	auto storageTypeAfter = graph::storage::EntityPropertyTraits<graph::Node>::getPropertyStorageType(nodeAfter);
	EXPECT_EQ(storageTypeAfter, graph::PropertyStorageType::BLOB_ENTITY);

	// Verify properties are correct
	auto retrievedProps = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_EQ(retrievedProps.size(), largeProps.size());
}

// Test property update from blob to small storage
TEST_F(PropertyManagerTest, UpdatePropertiesBlobToSmall) {
	// Add large properties first (stored in Blob)
	auto largeProps = createLargePropertyMap();
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), largeProps);

	// Verify initial blob storage
	graph::Node nodeBefore = nodeManager->get(testNode.getId());
	auto storageTypeBefore = graph::storage::EntityPropertyTraits<graph::Node>::getPropertyStorageType(nodeBefore);
	EXPECT_EQ(storageTypeBefore, graph::PropertyStorageType::BLOB_ENTITY);

	// Update with small properties (should transition back to Property Entity)
	std::unordered_map<std::string, graph::PropertyValue> smallProps;
	smallProps["key1"] = graph::PropertyValue("small");
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), smallProps);

	// Verify transition to Property Entity
	graph::Node nodeAfter = nodeManager->get(testNode.getId());
	auto storageTypeAfter = graph::storage::EntityPropertyTraits<graph::Node>::getPropertyStorageType(nodeAfter);
	EXPECT_EQ(storageTypeAfter, graph::PropertyStorageType::PROPERTY_ENTITY);

	// Verify only new properties exist
	auto retrievedProps = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_EQ(retrievedProps.size(), 1UL);
	EXPECT_TRUE(retrievedProps.contains("key1"));
}

// Test multiple sequential property updates
TEST_F(PropertyManagerTest, MultipleSequentialPropertyUpdates) {
	// First update
	std::unordered_map<std::string, graph::PropertyValue> props1;
	props1["key1"] = graph::PropertyValue("value1");
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), props1);

	auto propsAfter1 = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_EQ(propsAfter1.size(), 1UL);

	// Second update (should replace, not append)
	std::unordered_map<std::string, graph::PropertyValue> props2;
	props2["key2"] = graph::PropertyValue("value2");
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), props2);

	auto propsAfter2 = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_EQ(propsAfter2.size(), 1UL) << "New properties should replace old ones";
	EXPECT_TRUE(propsAfter2.contains("key2"));
	EXPECT_FALSE(propsAfter2.contains("key1"));
}
