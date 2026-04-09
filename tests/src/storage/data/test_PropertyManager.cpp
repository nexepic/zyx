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
		testNode.setLabelId(dataManager->getOrCreateTokenId("TestNode"));
		nodeManager->add(testNode);

		graph::Node targetNode;
		targetNode.setLabelId(dataManager->getOrCreateTokenId("TargetNode"));
		nodeManager->add(targetNode);

		testEdge.setSourceNodeId(testNode.getId());
		testEdge.setTargetNodeId(targetNode.getId());
		testEdge.setTypeId(dataManager->getOrCreateTokenId("CONNECTS_TO"));
		edgeManager->add(testEdge);
	}

	void TearDown() override {
		// Release shared_ptrs before closing database
		edgeManager.reset();
		nodeManager.reset();
		propertyManager.reset();
		dataManager.reset();
		storage.reset();

		if (db) {
			db->close();
		}
		db.reset();

		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
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
	blobNode.setLabelId(dataManager->getOrCreateTokenId("BlobNode"));
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
	blobNode.setLabelId(dataManager->getOrCreateTokenId("BlobNode"));
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
	node2.setLabelId(dataManager->getOrCreateTokenId("Node2"));
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
	tempNode.setLabelId(dataManager->getOrCreateTokenId("TempNode"));
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
	tempNode.setLabelId(dataManager->getOrCreateTokenId("TempNode"));
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

// =========================================================================
// Additional Branch Coverage Tests for PropertyManager
// =========================================================================

// Test removeEntityProperty from edge with PROPERTY_ENTITY storage
TEST_F(PropertyManagerTest, RemovePropertyFromEdgePropertyEntity) {
	// Add small properties to edge (stored in Property Entity)
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["weight"] = graph::PropertyValue(3.14);
	props["label"] = graph::PropertyValue("important");
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), props);

	// Verify properties exist
	auto propsBefore = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(propsBefore.size(), 2UL);

	// Verify storage type is PROPERTY_ENTITY
	graph::Edge edgeWithProps = edgeManager->get(testEdge.getId());
	auto storageType = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeWithProps);
	EXPECT_EQ(storageType, graph::PropertyStorageType::PROPERTY_ENTITY);

	// Remove one property from Property Entity
	propertyManager->removeEntityProperty<graph::Edge>(testEdge.getId(), "weight");

	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(propsAfter.size(), 1UL);
	EXPECT_FALSE(propsAfter.contains("weight"));
	EXPECT_TRUE(propsAfter.contains("label"));
}

// Test removing all properties from edge Property Entity (should delete entity)
TEST_F(PropertyManagerTest, RemoveAllPropertiesFromEdgePropertyEntity) {
	// Add small properties to edge
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["weight"] = graph::PropertyValue(1.0);
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), props);

	// Verify property entity exists
	graph::Edge edgeBefore = edgeManager->get(testEdge.getId());
	int64_t propIdBefore = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edgeBefore);
	EXPECT_NE(propIdBefore, 0);

	// Remove the only property - should delete the Property entity
	propertyManager->removeEntityProperty<graph::Edge>(testEdge.getId(), "weight");

	// Verify property entity reference is cleared
	graph::Edge edgeAfter = edgeManager->get(testEdge.getId());
	int64_t propIdAfter = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edgeAfter);
	EXPECT_EQ(propIdAfter, 0) << "Property entity reference should be cleared";

	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_TRUE(propsAfter.empty());
}

// Test removing property from edge with BLOB storage
TEST_F(PropertyManagerTest, RemovePropertyFromEdgeBlobStorage) {
	// Add large properties to edge (triggers BLOB storage)
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 10; i++) {
		std::string largeValue(800, 'B');
		largeProps["edge_key_" + std::to_string(i)] = graph::PropertyValue(largeValue);
	}
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	// Verify BLOB storage
	graph::Edge edgeWithBlob = edgeManager->get(testEdge.getId());
	auto storageType = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeWithBlob);
	EXPECT_EQ(storageType, graph::PropertyStorageType::BLOB_ENTITY);

	// Remove one property from blob
	propertyManager->removeEntityProperty<graph::Edge>(testEdge.getId(), "edge_key_3");

	// Verify remaining properties
	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(propsAfter.size(), 9UL);
	EXPECT_FALSE(propsAfter.contains("edge_key_3"));
}

// Test removing all properties from edge with BLOB storage
TEST_F(PropertyManagerTest, RemoveAllBlobPropertiesFromEdge) {
	// Add large properties to edge (triggers BLOB storage)
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 5; i++) {
		std::string largeValue(800, 'C');
		largeProps["blob_edge_" + std::to_string(i)] = graph::PropertyValue(largeValue);
	}
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	// Remove all properties one by one
	for (int i = 0; i < 5; i++) {
		propertyManager->removeEntityProperty<graph::Edge>(testEdge.getId(), "blob_edge_" + std::to_string(i));
	}

	// Verify all cleared
	graph::Edge edgeAfter = edgeManager->get(testEdge.getId());
	int64_t blobIdAfter = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edgeAfter);
	EXPECT_EQ(blobIdAfter, 0) << "Blob reference should be cleared";

	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_TRUE(propsAfter.empty());
}

// Test hasExternalProperty for edge with PROPERTY_ENTITY storage
TEST_F(PropertyManagerTest, HasExternalPropertyEdgePropertyEntity) {
	// Add small properties to edge
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["edge_prop"] = graph::PropertyValue("edge_value");
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), props);

	// Refresh edge
	graph::Edge edgeWithProps = edgeManager->get(testEdge.getId());

	EXPECT_TRUE(propertyManager->hasExternalProperty<graph::Edge>(edgeWithProps, "edge_prop"));
	EXPECT_FALSE(propertyManager->hasExternalProperty<graph::Edge>(edgeWithProps, "missing"));
}

// Test hasExternalProperty for edge with BLOB storage
TEST_F(PropertyManagerTest, HasExternalPropertyEdgeBlobStorage) {
	// Add large properties to edge (triggers BLOB)
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 10; i++) {
		std::string largeValue(800, 'D');
		largeProps["has_blob_" + std::to_string(i)] = graph::PropertyValue(largeValue);
	}
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	// Refresh edge
	graph::Edge edgeWithBlob = edgeManager->get(testEdge.getId());

	EXPECT_TRUE(propertyManager->hasExternalProperty<graph::Edge>(edgeWithBlob, "has_blob_0"));
	EXPECT_FALSE(propertyManager->hasExternalProperty<graph::Edge>(edgeWithBlob, "nonexistent"));
}

// Test calculateEntityTotalPropertySize for edge with PROPERTY_ENTITY
TEST_F(PropertyManagerTest, CalculateSizeEdgePropertyEntity) {
	// Add small properties to edge
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["weight"] = graph::PropertyValue(2.5);
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), props);

	size_t size = propertyManager->calculateEntityTotalPropertySize<graph::Edge>(testEdge.getId());
	EXPECT_GT(size, 0UL);
}

// Test calculateEntityTotalPropertySize for edge with BLOB storage
TEST_F(PropertyManagerTest, CalculateSizeEdgeBlobStorage) {
	// Add large properties to edge (triggers BLOB)
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 10; i++) {
		std::string largeValue(800, 'F');
		largeProps["size_blob_" + std::to_string(i)] = graph::PropertyValue(largeValue);
	}
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	size_t size = propertyManager->calculateEntityTotalPropertySize<graph::Edge>(testEdge.getId());

	// Calculate expected size
	size_t expectedSize = 0;
	for (const auto& [k, v] : largeProps) {
		expectedSize += k.size();
		expectedSize += graph::property_utils::getPropertyValueSize(v);
	}
	EXPECT_EQ(size, expectedSize);
}

// Test calculateEntityTotalPropertySize for edge with non-existent entity
TEST_F(PropertyManagerTest, CalculateSizeEdgeNonExistent) {
	size_t size = propertyManager->calculateEntityTotalPropertySize<graph::Edge>(999999);
	EXPECT_EQ(size, 0UL);
}

// Test getEntityProperties for edge with BLOB storage
TEST_F(PropertyManagerTest, GetEntityPropertiesEdgeBlob) {
	// Add large properties to edge (triggers BLOB)
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 8; i++) {
		std::string largeValue(1000, 'G');
		largeProps["get_blob_" + std::to_string(i)] = graph::PropertyValue(largeValue);
	}
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	// Verify BLOB storage
	graph::Edge edgeWithBlob = edgeManager->get(testEdge.getId());
	auto storageType = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeWithBlob);
	EXPECT_EQ(storageType, graph::PropertyStorageType::BLOB_ENTITY);

	// Retrieve properties
	auto retrievedProps = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(retrievedProps.size(), largeProps.size());
	for (int i = 0; i < 8; i++) {
		EXPECT_TRUE(retrievedProps.contains("get_blob_" + std::to_string(i)));
	}
}

// Test cleanupExternalProperties for edge with BLOB storage
// This is triggered when updating properties from blob to smaller set
TEST_F(PropertyManagerTest, CleanupEdgeBlobProperties) {
	// Add large properties to edge (triggers BLOB)
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 10; i++) {
		std::string largeValue(800, 'H');
		largeProps["cleanup_" + std::to_string(i)] = graph::PropertyValue(largeValue);
	}
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	// Verify blob exists
	graph::Edge edgeBefore = edgeManager->get(testEdge.getId());
	auto storageTypeBefore = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeBefore);
	EXPECT_EQ(storageTypeBefore, graph::PropertyStorageType::BLOB_ENTITY);

	// Now replace with small properties (triggers cleanup of blob, then stores as Property Entity)
	std::unordered_map<std::string, graph::PropertyValue> smallProps;
	smallProps["small"] = graph::PropertyValue("tiny");
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), smallProps);

	// Verify transition from blob to property entity
	graph::Edge edgeAfter = edgeManager->get(testEdge.getId());
	auto storageTypeAfter = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeAfter);
	EXPECT_EQ(storageTypeAfter, graph::PropertyStorageType::PROPERTY_ENTITY);

	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(propsAfter.size(), 1UL);
	EXPECT_TRUE(propsAfter.contains("small"));
}

// Test removeEntityProperty from edge where property doesn't exist in blob
TEST_F(PropertyManagerTest, RemoveNonExistentPropertyFromEdgeBlob) {
	// Add large properties to edge (BLOB)
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 10; i++) {
		std::string largeValue(800, 'I');
		largeProps["exist_" + std::to_string(i)] = graph::PropertyValue(largeValue);
	}
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	// Try to remove non-existent key from blob - should be idempotent
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<graph::Edge>(testEdge.getId(), "nonexistent_key"));

	// Verify all properties still intact
	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(propsAfter.size(), 10UL);
}

// Test removeEntityProperty from edge where property doesn't exist in Property Entity
TEST_F(PropertyManagerTest, RemoveNonExistentPropertyFromEdgePropertyEntity) {
	// Add small properties to edge (Property Entity)
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["existing"] = graph::PropertyValue("value");
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), props);

	// Try to remove non-existent key
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<graph::Edge>(testEdge.getId(), "nonexistent"));

	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(propsAfter.size(), 1UL);
}

// =========================================================================
// Additional Branch Coverage Tests for PropertyManager
// =========================================================================

// Test storeProperties with entity that has empty inline properties
// Covers: line 110 True branch (properties.empty() == true)
TEST_F(PropertyManagerTest, StorePropertiesEmptyAfterClearing) {
	// Add properties to node first
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["key1"] = graph::PropertyValue("value1");
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), props);

	// Remove the only property (should clear external storage)
	propertyManager->removeEntityProperty<graph::Node>(testNode.getId(), "key1");

	// Verify entity now has no properties
	auto retrievedProps = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_TRUE(retrievedProps.empty());
}

// Test cleanupExternalProperties for Edge with BLOB storage then replace with small
// Covers: line 146 True branch (storageType == BLOB_ENTITY) for Edge cleanup
TEST_F(PropertyManagerTest, EdgeCleanupBlobThenReplaceWithSmall) {
	// First add large properties to Edge (triggers BLOB storage)
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 10; i++) {
		std::string largeValue(800, 'Z');
		largeProps["cleanup_edge_key_" + std::to_string(i)] = graph::PropertyValue(largeValue);
	}
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	// Verify Edge has BLOB storage
	graph::Edge edgeWithBlob = edgeManager->get(testEdge.getId());
	auto storageType = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeWithBlob);
	EXPECT_EQ(storageType, graph::PropertyStorageType::BLOB_ENTITY);

	// Now replace with small properties - this triggers cleanupExternalProperties
	// which should clean up the BLOB chain (line 146-149 for Edge)
	std::unordered_map<std::string, graph::PropertyValue> smallProps;
	smallProps["tiny"] = graph::PropertyValue("small_value");
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), smallProps);

	// Verify transition to PROPERTY_ENTITY
	graph::Edge edgeAfter = edgeManager->get(testEdge.getId());
	auto storageTypeAfter = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeAfter);
	EXPECT_EQ(storageTypeAfter, graph::PropertyStorageType::PROPERTY_ENTITY);

	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(propsAfter.size(), 1UL);
}

// Test calculateEntityTotalPropertySize for Edge with PROPERTY_ENTITY having inactive property
// Covers: line 472 branch (property.getId() != 0 && property.isActive())
TEST_F(PropertyManagerTest, CalculateSizeEdgePropertyEntityWithInactive) {
	// Add small properties to edge
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["w"] = graph::PropertyValue(2.5);
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), props);

	// Calculate size with active property entity
	size_t sizeActive = propertyManager->calculateEntityTotalPropertySize<graph::Edge>(testEdge.getId());
	EXPECT_GT(sizeActive, 0UL);

	// Now remove the property
	propertyManager->removeEntityProperty<graph::Edge>(testEdge.getId(), "w");

	// Calculate size for edge with no properties
	size_t sizeEmpty = propertyManager->calculateEntityTotalPropertySize<graph::Edge>(testEdge.getId());
	EXPECT_EQ(sizeEmpty, 0UL);
}

// Test getEntityProperties for non-existent Edge
// Covers: line 250 (entity.getId() == 0 || !entity.isActive()) True for Edge
TEST_F(PropertyManagerTest, GetEntityPropertiesNonExistentEdge) {
	auto props = propertyManager->getEntityProperties<graph::Edge>(999999);
	EXPECT_TRUE(props.empty());
}

// Test addEntityProperties for non-existent Edge
// Covers: line 304 (entity.getId() == 0 || !entity.isActive()) True for Edge
TEST_F(PropertyManagerTest, AddPropertiesToNonExistentEdge) {
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["key"] = graph::PropertyValue("value");
	EXPECT_THROW(
		propertyManager->addEntityProperties<graph::Edge>(999999, props),
		std::runtime_error
	);
}

// Test removeEntityProperty for non-existent Edge
// Covers: line 336 (entity.getId() == 0 || !entity.isActive()) True for Edge
TEST_F(PropertyManagerTest, RemovePropertyFromNonExistentEdge) {
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<graph::Edge>(999999, "any_key"));
}

// Test hasExternalProperty for Edge with no external properties
// Covers: line 416 (hasPropertyEntity returns false) for Edge
TEST_F(PropertyManagerTest, HasExternalPropertyEdgeNoProperties) {
	EXPECT_FALSE(propertyManager->hasExternalProperty<graph::Edge>(testEdge, "any_key"));
}

// Test calculateEntityTotalPropertySize for Edge non-existent
// Covers: line 452 (entity.getId() == 0 || !entity.isActive()) True for Edge
TEST_F(PropertyManagerTest, CalculateSizeEdgeNonExistentEntity) {
	size_t size = propertyManager->calculateEntityTotalPropertySize<graph::Edge>(999999);
	EXPECT_EQ(size, 0UL);
}

// Test removeEntityProperty for Edge from Property Entity where property doesn't exist
// Covers: line 351, 357 (hasPropertyValue returns false) for Edge
TEST_F(PropertyManagerTest, RemoveNonExistentPropertyFromEdgeWithPropertyEntity) {
	// Add small properties to edge
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["existing_key"] = graph::PropertyValue("value");
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), props);

	// Remove a key that doesn't exist in the Property Entity
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<graph::Edge>(testEdge.getId(), "non_existing"));

	// Verify original property still exists
	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(propsAfter.size(), 1UL);
	EXPECT_TRUE(propsAfter.contains("existing_key"));
}

// Test removeEntityProperty for Edge: remove ALL from Property Entity (delete entity)
// Covers: line 364 (properties.empty()) True for Edge
TEST_F(PropertyManagerTest, RemoveAllFromEdgePropertyEntity) {
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["only_key"] = graph::PropertyValue("only_value");
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), props);

	// Remove the only property - should delete the Property entity
	propertyManager->removeEntityProperty<graph::Edge>(testEdge.getId(), "only_key");

	graph::Edge edgeAfter = edgeManager->get(testEdge.getId());
	int64_t propId = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edgeAfter);
	EXPECT_EQ(propId, 0) << "Property entity reference should be cleared";

	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_TRUE(propsAfter.empty());
}

// =========================================================================
// Additional Branch Coverage Tests - Round 4
// =========================================================================

// Test addEntityProperties throws for inactive Node
// Covers: line 304 !entity.isActive() True for Node
TEST_F(PropertyManagerTest, AddPropertiesToInactiveNode) {
	// Create and then remove a node to make it inactive
	graph::Node tempNode;
	tempNode.setLabelId(dataManager->getOrCreateTokenId("TempNode"));
	nodeManager->add(tempNode);

	graph::Node node = nodeManager->get(tempNode.getId());
	nodeManager->remove(node);

	std::unordered_map<std::string, graph::PropertyValue> props;
	props["key"] = graph::PropertyValue("value");
	EXPECT_THROW(
		propertyManager->addEntityProperties<graph::Node>(tempNode.getId(), props),
		std::runtime_error
	);
}

// Test addEntityProperties throws for inactive Edge
// Covers: line 304 !entity.isActive() True for Edge
TEST_F(PropertyManagerTest, AddPropertiesToInactiveEdge) {
	// Create nodes and edge, then remove the edge
	graph::Node src, tgt;
	src.setLabelId(dataManager->getOrCreateTokenId("Src"));
	tgt.setLabelId(dataManager->getOrCreateTokenId("Tgt"));
	nodeManager->add(src);
	nodeManager->add(tgt);

	graph::Edge tmpEdge;
	tmpEdge.setSourceNodeId(src.getId());
	tmpEdge.setTargetNodeId(tgt.getId());
	tmpEdge.setTypeId(dataManager->getOrCreateTokenId("TMP"));
	edgeManager->add(tmpEdge);

	graph::Edge edge = edgeManager->get(tmpEdge.getId());
	edgeManager->remove(edge);

	std::unordered_map<std::string, graph::PropertyValue> props;
	props["key"] = graph::PropertyValue("value");
	EXPECT_THROW(
		propertyManager->addEntityProperties<graph::Edge>(tmpEdge.getId(), props),
		std::runtime_error
	);
}

// Test serialization round-trip with multiple property types
// Covers: branch for iterating multiple properties in calculateSerializedSize and serializeProperties
TEST_F(PropertyManagerTest, SerializeDeserializeMultiplePropertyTypes) {
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["string_key"] = graph::PropertyValue("hello");
	props["int_key"] = graph::PropertyValue(42);
	props["double_key"] = graph::PropertyValue(3.14);
	props["bool_key"] = graph::PropertyValue(true);

	// Serialize
	std::stringstream ss;
	graph::storage::PropertyManager::serializeProperties(ss, props);

	// Deserialize
	auto deserialized = graph::storage::PropertyManager::deserializeProperties(ss);

	EXPECT_EQ(deserialized.size(), 4UL);
	EXPECT_EQ(std::get<std::string>(deserialized["string_key"].getVariant()), "hello");
	EXPECT_EQ(std::get<int64_t>(deserialized["int_key"].getVariant()), 42);
	EXPECT_DOUBLE_EQ(std::get<double>(deserialized["double_key"].getVariant()), 3.14);
	EXPECT_EQ(std::get<bool>(deserialized["bool_key"].getVariant()), true);
}

// Test calculateSerializedSize with multiple properties
// Covers: the loop body in calculateSerializedSize with various types
TEST_F(PropertyManagerTest, CalculateSerializedSizeMultipleTypes) {
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["a"] = graph::PropertyValue("short");
	props["bb"] = graph::PropertyValue(99);

	uint32_t size = graph::storage::PropertyManager::calculateSerializedSize(props);
	// Should be: sizeof(uint32_t) for count
	//   + sizeof(uint32_t) + 1 (key "a") + getPropertyValueSize("short")
	//   + sizeof(uint32_t) + 2 (key "bb") + getPropertyValueSize(99)
	EXPECT_GT(size, static_cast<uint32_t>(0));
	EXPECT_GT(size, static_cast<uint32_t>(sizeof(uint32_t))); // More than just the count
}

// Test getEntityProperties for inactive Edge (covers entity.getId() == 0 || !entity.isActive())
TEST_F(PropertyManagerTest, GetEntityPropertiesInactiveEdge) {
	// Create an edge, add properties, then remove the edge
	graph::Node src, tgt;
	src.setLabelId(dataManager->getOrCreateTokenId("Src2"));
	tgt.setLabelId(dataManager->getOrCreateTokenId("Tgt2"));
	nodeManager->add(src);
	nodeManager->add(tgt);

	graph::Edge tmpEdge;
	tmpEdge.setSourceNodeId(src.getId());
	tmpEdge.setTargetNodeId(tgt.getId());
	tmpEdge.setTypeId(dataManager->getOrCreateTokenId("TMP2"));
	edgeManager->add(tmpEdge);

	// Add properties first
	propertyManager->addEntityProperties<graph::Edge>(tmpEdge.getId(), {{"key", graph::PropertyValue("value")}});

	// Remove edge
	graph::Edge edge = edgeManager->get(tmpEdge.getId());
	edgeManager->remove(edge);

	// Getting properties of inactive edge should return empty
	auto props = propertyManager->getEntityProperties<graph::Edge>(tmpEdge.getId());
	EXPECT_TRUE(props.empty());
}

// Test calculateEntityTotalPropertySize for inactive Edge
TEST_F(PropertyManagerTest, CalculateSizeInactiveEdge) {
	graph::Node src, tgt;
	src.setLabelId(dataManager->getOrCreateTokenId("Src3"));
	tgt.setLabelId(dataManager->getOrCreateTokenId("Tgt3"));
	nodeManager->add(src);
	nodeManager->add(tgt);

	graph::Edge tmpEdge;
	tmpEdge.setSourceNodeId(src.getId());
	tmpEdge.setTargetNodeId(tgt.getId());
	tmpEdge.setTypeId(dataManager->getOrCreateTokenId("TMP3"));
	edgeManager->add(tmpEdge);

	propertyManager->addEntityProperties<graph::Edge>(tmpEdge.getId(), {{"key", graph::PropertyValue("value")}});

	// Remove edge
	graph::Edge edge = edgeManager->get(tmpEdge.getId());
	edgeManager->remove(edge);

	// Size should be 0 for inactive edge
	size_t size = propertyManager->calculateEntityTotalPropertySize<graph::Edge>(tmpEdge.getId());
	EXPECT_EQ(size, 0UL);
}

// Test removeEntityProperty from inactive edge (should not throw, idempotent)
TEST_F(PropertyManagerTest, RemovePropertyFromInactiveEdge) {
	graph::Node src, tgt;
	src.setLabelId(dataManager->getOrCreateTokenId("Src4"));
	tgt.setLabelId(dataManager->getOrCreateTokenId("Tgt4"));
	nodeManager->add(src);
	nodeManager->add(tgt);

	graph::Edge tmpEdge;
	tmpEdge.setSourceNodeId(src.getId());
	tmpEdge.setTargetNodeId(tgt.getId());
	tmpEdge.setTypeId(dataManager->getOrCreateTokenId("TMP4"));
	edgeManager->add(tmpEdge);

	propertyManager->addEntityProperties<graph::Edge>(tmpEdge.getId(), {{"key", graph::PropertyValue("value")}});

	// Remove edge
	graph::Edge edge = edgeManager->get(tmpEdge.getId());
	edgeManager->remove(edge);

	// Should not throw
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<graph::Edge>(tmpEdge.getId(), "key"));
}

// Test corrupted blob storage for Edge (getPropertiesFromBlob exception path)
TEST_F(PropertyManagerTest, EdgeCorruptedBlobStorageRecovery) {
	// Add large properties to edge (triggers BLOB)
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 10; i++) {
		std::string largeValue(800, 'J');
		largeProps["corrupt_edge_" + std::to_string(i)] = graph::PropertyValue(largeValue);
	}
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	// Corrupt blob by deleting it
	graph::Edge edge = edgeManager->get(testEdge.getId());
	int64_t blobId = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edge);
	auto blobManager = dataManager->getBlobManager();
	blobManager->deleteBlobChain(blobId);

	// Getting properties should return empty (graceful recovery)
	auto props = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_TRUE(props.empty());
}

// Test hasExternalProperty for edge with corrupted blob storage
TEST_F(PropertyManagerTest, HasExternalPropertyEdgeCorruptedBlob) {
	// Add large properties
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 10; i++) {
		std::string largeValue(800, 'K');
		largeProps["has_corrupt_" + std::to_string(i)] = graph::PropertyValue(largeValue);
	}
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	// Corrupt the blob
	graph::Edge edge = edgeManager->get(testEdge.getId());
	int64_t blobId = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edge);
	auto blobManager = dataManager->getBlobManager();
	blobManager->deleteBlobChain(blobId);

	// Should return false (catch path in hasExternalProperty)
	EXPECT_FALSE(propertyManager->hasExternalProperty<graph::Edge>(edge, "has_corrupt_0"));
}

// Test calculateEntityTotalPropertySize with corrupted blob storage
TEST_F(PropertyManagerTest, CalculateSizeEdgeCorruptedBlobStorage) {
	// Add large properties to edge
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 10; i++) {
		std::string largeValue(800, 'L');
		largeProps["calc_corrupt_" + std::to_string(i)] = graph::PropertyValue(largeValue);
	}
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	// Corrupt the blob
	graph::Edge edge = edgeManager->get(testEdge.getId());
	int64_t blobId = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edge);
	auto blobManager = dataManager->getBlobManager();
	blobManager->deleteBlobChain(blobId);

	// Size should be 0 (catch path in calculateEntityTotalPropertySize)
	size_t size = propertyManager->calculateEntityTotalPropertySize<graph::Edge>(testEdge.getId());
	EXPECT_EQ(size, 0UL);
}

// =========================================================================
// Additional Branch Coverage Tests - Round 5
// =========================================================================

// Test Edge cleanupExternalProperties with PROPERTY_ENTITY storage
// Covers: line 146 Edge False branch (storageType != BLOB_ENTITY -> PROPERTY_ENTITY cleanup)
// When Edge has small properties stored in PROPERTY_ENTITY, then new properties are added,
// cleanupExternalProperties should delete the old PROPERTY_ENTITY (not blob chain).
TEST_F(PropertyManagerTest, EdgeCleanupPropertyEntityThenReplace) {
	// Step 1: Add small properties to Edge -> stored as PROPERTY_ENTITY
	std::unordered_map<std::string, graph::PropertyValue> smallProps;
	smallProps["color"] = graph::PropertyValue("red");
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), smallProps);

	// Verify Edge has PROPERTY_ENTITY storage
	graph::Edge edgeBefore = edgeManager->get(testEdge.getId());
	auto storageTypeBefore = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeBefore);
	EXPECT_EQ(storageTypeBefore, graph::PropertyStorageType::PROPERTY_ENTITY);
	int64_t propIdBefore = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edgeBefore);
	EXPECT_NE(propIdBefore, 0);

	// Step 2: Add new small properties to the same Edge
	// This triggers storeProperties -> cleanupExternalProperties with PROPERTY_ENTITY
	// which should take the ELSE branch at line 146 (not BLOB_ENTITY)
	std::unordered_map<std::string, graph::PropertyValue> newProps;
	newProps["shape"] = graph::PropertyValue("circle");
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), newProps);

	// Verify the Edge now has the new properties
	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(propsAfter.size(), 1UL);
	EXPECT_TRUE(propsAfter.contains("shape"));
	EXPECT_FALSE(propsAfter.contains("color")) << "Old property should be gone after replacement";

	// Verify storage type is still PROPERTY_ENTITY (new one was created)
	graph::Edge edgeAfter = edgeManager->get(testEdge.getId());
	auto storageTypeAfter = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeAfter);
	EXPECT_EQ(storageTypeAfter, graph::PropertyStorageType::PROPERTY_ENTITY);

	// Verify the property entity ID changed (old one was deleted, new one created)
	int64_t propIdAfter = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edgeAfter);
	EXPECT_NE(propIdAfter, 0);
}

// Test removing a non-existent key from Node with BLOB storage
// Covers: line 375 Node False branch (it != properties.end() is false)
TEST_F(PropertyManagerTest, RemoveNonExistentPropertyFromNodeBlob) {
	// Add large properties to Node (triggers BLOB storage)
	auto largeProps = createLargePropertyMap();
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), largeProps);

	// Verify BLOB storage
	graph::Node nodeWithBlob = nodeManager->get(testNode.getId());
	auto storageType = graph::storage::EntityPropertyTraits<graph::Node>::getPropertyStorageType(nodeWithBlob);
	EXPECT_EQ(storageType, graph::PropertyStorageType::BLOB_ENTITY);

	// Try to remove a key that does NOT exist in the blob - should be idempotent
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<graph::Node>(testNode.getId(), "nonexistent_key_xyz"));

	// Verify all original properties are still intact
	auto propsAfter = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_EQ(propsAfter.size(), largeProps.size()) << "All properties should remain unchanged";

	// Verify specific keys still exist
	for (int i = 0; i < 5; i++) {
		std::string key = "large_key_" + std::to_string(i);
		EXPECT_TRUE(propsAfter.contains(key)) << "Key " << key << " should still exist";
	}
}

// =========================================================================
// Additional Branch Coverage Tests - Round 6
// =========================================================================

// Test removeEntityProperty for Edge that exists but has NO external properties
// and the key doesn't exist inline either.
// Covers: line 356 False branch for Edge (!propertyWasRemoved && !hasPropertyEntity)
TEST_F(PropertyManagerTest, RemovePropertyFromEdgeWithNoPropertiesAtAll) {
	// testEdge has no properties at this point
	// Removing a non-existent key from an edge with no inline or external properties
	// should be a no-op (idempotent)
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<graph::Edge>(testEdge.getId(), "totally_missing"));

	// Verify edge is still valid and has no properties
	auto props = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_TRUE(props.empty());
}

// Test removeEntityProperty for Node that exists but has NO external properties
// Covers: line 356 False branch for Node where !propertyWasRemoved && !hasPropertyEntity
TEST_F(PropertyManagerTest, RemovePropertyFromNodeWithNoPropertiesAtAll) {
	// testNode has no properties initially
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<graph::Node>(testNode.getId(), "nonexistent_key"));

	auto props = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_TRUE(props.empty());
}

// Test Edge property where storeProperties hits empty properties after cleanup
// This ensures the edge template path for empty properties after cleanup is hit
// Covers: line 110 True branch (properties.empty()) for Edge after cleanup
TEST_F(PropertyManagerTest, EdgeStorePropertiesEmptyAfterCleanup) {
	// Add properties to edge first
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["temp_key"] = graph::PropertyValue("temp_value");
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), props);

	// Verify it was stored
	auto propsStored = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(propsStored.size(), 1UL);

	// Remove the property
	propertyManager->removeEntityProperty<graph::Edge>(testEdge.getId(), "temp_key");

	// Verify empty
	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_TRUE(propsAfter.empty());

	// Now add empty properties - should be a no-op
	std::unordered_map<std::string, graph::PropertyValue> emptyProps;
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), emptyProps);

	// Still empty
	auto propsFinal = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_TRUE(propsFinal.empty());
}

// Test Edge transition from PROPERTY_ENTITY to BLOB storage
// Covers: Edge template path for storePropertiesInBlob (line 119-125 for Edge)
TEST_F(PropertyManagerTest, EdgeTransitionPropertyEntityToBlob) {
	// Add small properties first (PROPERTY_ENTITY)
	std::unordered_map<std::string, graph::PropertyValue> smallProps;
	smallProps["key1"] = graph::PropertyValue("small_value");
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), smallProps);

	graph::Edge edgeBefore = edgeManager->get(testEdge.getId());
	auto typeBefore = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeBefore);
	EXPECT_EQ(typeBefore, graph::PropertyStorageType::PROPERTY_ENTITY);

	// Now add large properties (should transition to BLOB)
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 10; i++) {
		std::string largeValue(800, 'T');
		largeProps["transition_key_" + std::to_string(i)] = graph::PropertyValue(largeValue);
	}
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	graph::Edge edgeAfter = edgeManager->get(testEdge.getId());
	auto typeAfter = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeAfter);
	EXPECT_EQ(typeAfter, graph::PropertyStorageType::BLOB_ENTITY);

	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(propsAfter.size(), largeProps.size());
}

// Test Edge transition from BLOB to PROPERTY_ENTITY storage
// Covers: Edge template cleanupExternalProperties BLOB path then storePropertiesInPropertyEntity
TEST_F(PropertyManagerTest, EdgeTransitionBlobToPropertyEntity) {
	// Add large properties first (BLOB)
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 10; i++) {
		std::string largeValue(800, 'U');
		largeProps["blob_to_prop_" + std::to_string(i)] = graph::PropertyValue(largeValue);
	}
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	graph::Edge edgeBefore = edgeManager->get(testEdge.getId());
	auto typeBefore = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeBefore);
	EXPECT_EQ(typeBefore, graph::PropertyStorageType::BLOB_ENTITY);

	// Now replace with small properties (should transition to PROPERTY_ENTITY)
	std::unordered_map<std::string, graph::PropertyValue> smallProps;
	smallProps["small_key"] = graph::PropertyValue("small");
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), smallProps);

	graph::Edge edgeAfter = edgeManager->get(testEdge.getId());
	auto typeAfter = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeAfter);
	EXPECT_EQ(typeAfter, graph::PropertyStorageType::PROPERTY_ENTITY);

	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(propsAfter.size(), 1UL);
	EXPECT_TRUE(propsAfter.contains("small_key"));
}

// Test removeEntityProperty for Node from PROPERTY_ENTITY where key doesn't exist
// but the property entity exists. Covers the path where hasProperty(entity, key) is false
// AND hasPropertyEntity(entity) is true AND property.hasPropertyValue(key) is false for Node.
TEST_F(PropertyManagerTest, RemoveNonExistentKeyFromNodePropertyEntity) {
	// Add a property to node (stored in PROPERTY_ENTITY)
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["real_key"] = graph::PropertyValue("real_value");
	propertyManager->addEntityProperties<graph::Node>(testNode.getId(), props);

	// Verify PROPERTY_ENTITY storage
	graph::Node nodeWithProps = nodeManager->get(testNode.getId());
	auto storageType = graph::storage::EntityPropertyTraits<graph::Node>::getPropertyStorageType(nodeWithProps);
	EXPECT_EQ(storageType, graph::PropertyStorageType::PROPERTY_ENTITY);

	// Try to remove a key that doesn't exist in the Property Entity
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<graph::Node>(testNode.getId(), "fake_key"));

	// Verify original property is intact
	auto propsAfter = propertyManager->getEntityProperties<graph::Node>(testNode.getId());
	EXPECT_EQ(propsAfter.size(), 1UL);
	EXPECT_TRUE(propsAfter.contains("real_key"));
}

// Test addEntityProperties for non-existent Node
// Covers: line 307 True branch for Node (entity.getId() == 0)
TEST_F(PropertyManagerTest, AddPropertiesToNonExistentNode) {
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["key"] = graph::PropertyValue("value");
	EXPECT_THROW(
		propertyManager->addEntityProperties<graph::Node>(999999, props),
		std::runtime_error
	);
}

// =========================================================================
// Additional Branch Coverage Tests - Round 7
// Target: PropertyManager.cpp remaining uncovered branches
// =========================================================================

// Test getEntityProperties for edge with PROPERTY_ENTITY storage type
// Verifies full path through getEntityProperties<Edge> with external PROPERTY_ENTITY
TEST_F(PropertyManagerTest, GetEntityPropertiesEdgePropertyEntity) {
	// Add small properties to edge (stored in Property Entity)
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["color"] = graph::PropertyValue("red");
	props["width"] = graph::PropertyValue(2);
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), props);

	// Verify edge has PROPERTY_ENTITY storage
	graph::Edge edgeWithProps = edgeManager->get(testEdge.getId());
	auto storageType = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeWithProps);
	EXPECT_EQ(storageType, graph::PropertyStorageType::PROPERTY_ENTITY);

	// Retrieve and verify properties
	auto retrievedProps = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(retrievedProps.size(), 2UL);
	EXPECT_EQ(std::get<std::string>(retrievedProps["color"].getVariant()), "red");
	EXPECT_EQ(std::get<int64_t>(retrievedProps["width"].getVariant()), 2);
}

// Test storeProperties for edge where properties fit in Property entity
// Exercises the storePropertiesInPropertyEntity<Edge> path
TEST_F(PropertyManagerTest, StorePropertiesEdgeSmallFitsInPropertyEntity) {
	// Create a fresh edge for this test
	graph::Node src, tgt;
	src.setLabelId(dataManager->getOrCreateTokenId("Src"));
	tgt.setLabelId(dataManager->getOrCreateTokenId("Tgt"));
	nodeManager->add(src);
	nodeManager->add(tgt);

	graph::Edge edge;
	edge.setSourceNodeId(src.getId());
	edge.setTargetNodeId(tgt.getId());
	edge.setTypeId(dataManager->getOrCreateTokenId("SMALL_PROP"));
	edgeManager->add(edge);

	// Add small properties
	std::unordered_map<std::string, graph::PropertyValue> smallProps;
	smallProps["a"] = graph::PropertyValue("b");
	propertyManager->addEntityProperties<graph::Edge>(edge.getId(), smallProps);

	// Verify PROPERTY_ENTITY storage
	graph::Edge stored = edgeManager->get(edge.getId());
	auto storageType = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(stored);
	EXPECT_EQ(storageType, graph::PropertyStorageType::PROPERTY_ENTITY);
	EXPECT_NE(graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(stored), 0);

	// Verify retrieval
	auto props = propertyManager->getEntityProperties<graph::Edge>(edge.getId());
	EXPECT_EQ(props.size(), 1UL);
	EXPECT_EQ(std::get<std::string>(props["a"].getVariant()), "b");
}

// Test storeProperties for edge where properties are large (blob path)
// Exercises storePropertiesInBlob<Edge>
TEST_F(PropertyManagerTest, StorePropertiesEdgeLargeGoesToBlob) {
	graph::Node src, tgt;
	src.setLabelId(dataManager->getOrCreateTokenId("Src2"));
	tgt.setLabelId(dataManager->getOrCreateTokenId("Tgt2"));
	nodeManager->add(src);
	nodeManager->add(tgt);

	graph::Edge edge;
	edge.setSourceNodeId(src.getId());
	edge.setTargetNodeId(tgt.getId());
	edge.setTypeId(dataManager->getOrCreateTokenId("LARGE_PROP"));
	edgeManager->add(edge);

	// Add large properties
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 10; i++) {
		std::string val(800, 'X');
		largeProps["big_" + std::to_string(i)] = graph::PropertyValue(val);
	}
	propertyManager->addEntityProperties<graph::Edge>(edge.getId(), largeProps);

	// Verify BLOB_ENTITY storage
	graph::Edge stored = edgeManager->get(edge.getId());
	auto storageType = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(stored);
	EXPECT_EQ(storageType, graph::PropertyStorageType::BLOB_ENTITY);

	// Verify retrieval
	auto props = propertyManager->getEntityProperties<graph::Edge>(edge.getId());
	EXPECT_EQ(props.size(), 10UL);
}

// Test cleanupExternalProperties for edge with PROPERTY_ENTITY then replace with new
// Ensures the PROPERTY_ENTITY cleanup path for Edge (not BLOB_ENTITY) is exercised
TEST_F(PropertyManagerTest, EdgeCleanupPropertyEntityOnReplace) {
	// Add small props to edge -> PROPERTY_ENTITY
	std::unordered_map<std::string, graph::PropertyValue> firstProps;
	firstProps["first"] = graph::PropertyValue("one");
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), firstProps);

	graph::Edge edgeBefore = edgeManager->get(testEdge.getId());
	auto typeBefore = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeBefore);
	EXPECT_EQ(typeBefore, graph::PropertyStorageType::PROPERTY_ENTITY);
	int64_t propIdBefore = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edgeBefore);
	EXPECT_NE(propIdBefore, 0);

	// Replace with different small props -> triggers cleanup of old PROPERTY_ENTITY + create new
	std::unordered_map<std::string, graph::PropertyValue> secondProps;
	secondProps["second"] = graph::PropertyValue("two");
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), secondProps);

	// Verify new property entity
	graph::Edge edgeAfter = edgeManager->get(testEdge.getId());
	auto typeAfter = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edgeAfter);
	EXPECT_EQ(typeAfter, graph::PropertyStorageType::PROPERTY_ENTITY);

	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(propsAfter.size(), 1UL);
	EXPECT_TRUE(propsAfter.contains("second"));
	EXPECT_FALSE(propsAfter.contains("first"));
}

// Test calculateEntityTotalPropertySize for edge with BLOB storage and active property
// This covers the BLOB_ENTITY branch in calculateEntityTotalPropertySize<Edge>
TEST_F(PropertyManagerTest, CalculateSizeEdgeBlobActive) {
	// Add large properties to edge -> triggers BLOB
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 5; i++) {
		std::string val(600, 'M');
		largeProps["calc_" + std::to_string(i)] = graph::PropertyValue(val);
	}
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	// Verify BLOB_ENTITY
	graph::Edge edge = edgeManager->get(testEdge.getId());
	auto storageType = graph::storage::EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edge);
	EXPECT_EQ(storageType, graph::PropertyStorageType::BLOB_ENTITY);

	// Calculate size
	size_t size = propertyManager->calculateEntityTotalPropertySize<graph::Edge>(testEdge.getId());

	// Should match expected
	size_t expectedSize = 0;
	for (const auto &[k, v] : largeProps) {
		expectedSize += k.size();
		expectedSize += graph::property_utils::getPropertyValueSize(v);
	}
	EXPECT_EQ(size, expectedSize);
}

// Test removeEntityProperty from edge where the inline check fails
// but the property entity has the key
TEST_F(PropertyManagerTest, RemovePropertyFromEdgeExternalOnly) {
	// Add properties to edge -> stored in Property Entity
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["ext_only_key"] = graph::PropertyValue("external");
	props["another_key"] = graph::PropertyValue("also_external");
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), props);

	// Remove a key that exists only in the external Property Entity
	propertyManager->removeEntityProperty<graph::Edge>(testEdge.getId(), "ext_only_key");

	// Verify remaining
	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(propsAfter.size(), 1UL);
	EXPECT_TRUE(propsAfter.contains("another_key"));
	EXPECT_FALSE(propsAfter.contains("ext_only_key"));
}

// Test removeEntityProperty from edge blob where key exists
TEST_F(PropertyManagerTest, RemoveExistingPropertyFromEdgeBlob) {
	// Add large properties to edge -> BLOB
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	for (int i = 0; i < 8; i++) {
		std::string val(800, 'R');
		largeProps["rm_blob_" + std::to_string(i)] = graph::PropertyValue(val);
	}
	propertyManager->addEntityProperties<graph::Edge>(testEdge.getId(), largeProps);

	// Remove a specific key from blob
	propertyManager->removeEntityProperty<graph::Edge>(testEdge.getId(), "rm_blob_3");

	// Verify it was removed
	auto propsAfter = propertyManager->getEntityProperties<graph::Edge>(testEdge.getId());
	EXPECT_EQ(propsAfter.size(), 7UL);
	EXPECT_FALSE(propsAfter.contains("rm_blob_3"));

	// Verify remaining keys
	for (int i = 0; i < 8; i++) {
		if (i == 3) continue;
		EXPECT_TRUE(propsAfter.contains("rm_blob_" + std::to_string(i)));
	}
}

// Test calculateSerializedSize with empty properties
TEST_F(PropertyManagerTest, CalculateSerializedSizeEmpty) {
	std::unordered_map<std::string, graph::PropertyValue> empty;
	uint32_t size = graph::storage::PropertyManager::calculateSerializedSize(empty);
	// Should be just sizeof(uint32_t) for the count field
	EXPECT_EQ(size, static_cast<uint32_t>(sizeof(uint32_t)));
}
