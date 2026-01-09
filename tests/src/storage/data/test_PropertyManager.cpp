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
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/storage/FileStorage.hpp"
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
		testNode.setLabel("TestNode");
		nodeManager->add(testNode);

		graph::Node targetNode;
		targetNode.setLabel("TargetNode");
		nodeManager->add(targetNode);

		testEdge.setSourceNodeId(testNode.getId());
		testEdge.setTargetNodeId(targetNode.getId());
		testEdge.setLabel("CONNECTS_TO");
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
