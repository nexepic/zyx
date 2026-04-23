/**
 * @file test_PropertyManager_ExternalStorage.cpp
 * @brief Coverage tests for PropertyManager blob storage paths,
 *        cleanup external properties, and property size calculation.
 **/

#include "DataManagerSharedTestFixture.hpp"
#include "graph/storage/data/PropertyManager.hpp"

class PropertyManagerExternalTest : public DataManagerSharedTest {};

TEST_F(PropertyManagerExternalTest, StoreProperties_LargeData_UsesBlob) {
	Node n = createTestNode(dataManager, "BlobNode");
	dataManager->addNode(n);

	// Create a very large property set that exceeds Property entity payload
	std::unordered_map<std::string, PropertyValue> props;
	for (int i = 0; i < 30; ++i) {
		props["long_key_" + std::to_string(i)] = PropertyValue(std::string(100, 'a' + (i % 26)));
	}
	dataManager->addNodeProperties(n.getId(), props);
	simulateSave();
	dataManager->clearCache();

	// Reload and verify properties are accessible
	auto loadedProps = dataManager->getNodeProperties(n.getId());
	EXPECT_GE(loadedProps.size(), 25u);
}

TEST_F(PropertyManagerExternalTest, RemoveProperty_FromPropertyEntity) {
	Node n = createTestNode(dataManager, "RemProp");
	dataManager->addNode(n);
	dataManager->addNodeProperties(n.getId(), {
		{"color", PropertyValue("blue")},
		{"size", PropertyValue(42)}
	});
	simulateSave();

	// Remove one property from Property entity
	dataManager->removeNodeProperty(n.getId(), "color");

	auto props = dataManager->getNodeProperties(n.getId());
	EXPECT_EQ(props.count("color"), 0u);
	EXPECT_EQ(props.count("size"), 1u);
}

TEST_F(PropertyManagerExternalTest, RemoveProperty_LastPropertyDeletesEntity) {
	Node n = createTestNode(dataManager, "LastProp");
	dataManager->addNode(n);
	dataManager->addNodeProperties(n.getId(), {{"only", PropertyValue("one")}});
	simulateSave();

	// Remove the only property - should delete the Property entity
	dataManager->removeNodeProperty(n.getId(), "only");

	auto props = dataManager->getNodeProperties(n.getId());
	EXPECT_TRUE(props.empty());
}

TEST_F(PropertyManagerExternalTest, RemoveProperty_NonExistent_Idempotent) {
	Node n = createTestNode(dataManager, "Idempotent");
	dataManager->addNode(n);
	simulateSave();

	// Removing a non-existent property should not throw
	EXPECT_NO_THROW(dataManager->removeNodeProperty(n.getId(), "doesNotExist"));
}

TEST_F(PropertyManagerExternalTest, RemoveProperty_FromBlobEntity) {
	Node n = createTestNode(dataManager, "BlobRem");
	dataManager->addNode(n);

	// Create large enough properties for blob storage
	std::unordered_map<std::string, PropertyValue> props;
	for (int i = 0; i < 30; ++i) {
		props["blob_key_" + std::to_string(i)] = PropertyValue(std::string(100, 'x'));
	}
	dataManager->addNodeProperties(n.getId(), props);
	simulateSave();

	// Remove one property from blob
	dataManager->removeNodeProperty(n.getId(), "blob_key_0");

	auto loadedProps = dataManager->getNodeProperties(n.getId());
	EXPECT_EQ(loadedProps.count("blob_key_0"), 0u);
	EXPECT_GE(loadedProps.size(), 28u);
}

TEST_F(PropertyManagerExternalTest, RemoveProperty_AllFromBlob) {
	Node n = createTestNode(dataManager, "BlobAllRem");
	dataManager->addNode(n);

	std::unordered_map<std::string, PropertyValue> props;
	for (int i = 0; i < 30; ++i) {
		props["k" + std::to_string(i)] = PropertyValue(std::string(100, 'y'));
	}
	dataManager->addNodeProperties(n.getId(), props);
	simulateSave();

	// Remove all properties one by one
	for (int i = 0; i < 30; ++i) {
		dataManager->removeNodeProperty(n.getId(), "k" + std::to_string(i));
	}

	auto loadedProps = dataManager->getNodeProperties(n.getId());
	EXPECT_TRUE(loadedProps.empty());
}

TEST_F(PropertyManagerExternalTest, HasExternalProperty_PropertyEntity) {
	Node n = createTestNode(dataManager, "HasExtProp");
	dataManager->addNode(n);
	dataManager->addNodeProperties(n.getId(), {{"external", PropertyValue("val")}});
	simulateSave();

	auto pm = dataManager->getPropertyManager();
	Node loaded = dataManager->getNode(n.getId());

	bool hasIt = pm->hasExternalProperty<Node>(loaded, "external");
	EXPECT_TRUE(hasIt);

	bool hasMissing = pm->hasExternalProperty<Node>(loaded, "nonexistent");
	EXPECT_FALSE(hasMissing);
}

TEST_F(PropertyManagerExternalTest, CalculateEntityTotalPropertySize) {
	Node n = createTestNode(dataManager, "SizeCalc");
	dataManager->addNode(n);
	dataManager->addNodeProperties(n.getId(), {
		{"name", PropertyValue("test")},
		{"age", PropertyValue(25)}
	});
	simulateSave();

	auto pm = dataManager->getPropertyManager();
	size_t size = pm->calculateEntityTotalPropertySize<Node>(n.getId());
	EXPECT_GT(size, 0u);
}

TEST_F(PropertyManagerExternalTest, CalculateEntityTotalPropertySize_InactiveEntity) {
	auto pm = dataManager->getPropertyManager();
	// Non-existent entity
	size_t size = pm->calculateEntityTotalPropertySize<Node>(999999);
	EXPECT_EQ(size, 0u);
}

TEST_F(PropertyManagerExternalTest, StoreProperties_ReplaceExisting) {
	Node n = createTestNode(dataManager, "Replace");
	dataManager->addNode(n);
	dataManager->addNodeProperties(n.getId(), {{"a", PropertyValue("first")}});
	simulateSave();

	// Replace with new properties
	dataManager->addNodeProperties(n.getId(), {{"b", PropertyValue("second")}});
	simulateSave();

	auto props = dataManager->getNodeProperties(n.getId());
	EXPECT_EQ(props.count("b"), 1u);
}

TEST_F(PropertyManagerExternalTest, EdgeProperties_ExternalStorage) {
	Node n1 = createTestNode(dataManager, "EP1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "EP2");
	dataManager->addNode(n2);
	Edge e = createTestEdge(dataManager, n1.getId(), n2.getId(), "HAS");
	dataManager->addEdge(e);
	dataManager->addEdgeProperties(e.getId(), {{"weight", PropertyValue(3.14)}});
	simulateSave();

	auto props = dataManager->getEdgeProperties(e.getId());
	EXPECT_EQ(props.count("weight"), 1u);
}

TEST_F(PropertyManagerExternalTest, RemoveEdgeProperty) {
	Node n1 = createTestNode(dataManager, "RE1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "RE2");
	dataManager->addNode(n2);
	Edge e = createTestEdge(dataManager, n1.getId(), n2.getId(), "WITH");
	dataManager->addEdge(e);
	dataManager->addEdgeProperties(e.getId(), {
		{"x", PropertyValue(1)},
		{"y", PropertyValue(2)}
	});
	simulateSave();

	dataManager->removeEdgeProperty(e.getId(), "x");
	auto props = dataManager->getEdgeProperties(e.getId());
	EXPECT_EQ(props.count("x"), 0u);
	EXPECT_EQ(props.count("y"), 1u);
}
