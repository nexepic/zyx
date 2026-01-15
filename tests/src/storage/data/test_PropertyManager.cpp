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
	EXPECT_EQ(size0, 0);

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
	EXPECT_EQ(sizeInvalid, 0);
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
	EXPECT_EQ(size, 0);

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
