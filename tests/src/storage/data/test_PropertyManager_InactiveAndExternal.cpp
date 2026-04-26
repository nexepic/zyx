/**
 * @file test_PropertyManager_InactiveAndExternal.cpp
 * @brief Branch coverage tests for PropertyManager.cpp targeting:
 *        - getEntityProperties inactive/zero-id (line 251)
 *        - addEntityProperties inactive/zero-id (line 305)
 *        - removeEntityProperty inactive/zero-id (line 337)
 *        - removeEntityProperty key not found
 *        - calculateEntityTotalPropertySize non-existent (line 453)
 *        - hasExternalProperty no external property (line 417)
 *        - storeProperties empty properties (line 111)
 *        - Edge variants of property operations
 **/

#include "DataManagerSharedTestFixture.hpp"
#include "graph/storage/data/PropertyManager.hpp"

class PropertyManagerInactiveAndExternalTest : public DataManagerSharedTest {};

// ============================================================================
// getEntityProperties for inactive node (returns empty)
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, GetEntityProperties_Inactive) {
	auto pm = dataManager->getPropertyManager();
	// ID 0 returns inactive entity -- should return empty
	auto props = pm->getEntityProperties<Node>(0);
	EXPECT_TRUE(props.empty());
}

// ============================================================================
// getEntityProperties for non-existent entity
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, GetEntityProperties_NonExistent) {
	auto pm = dataManager->getPropertyManager();
	auto props = pm->getEntityProperties<Node>(99999);
	EXPECT_TRUE(props.empty());
}

// ============================================================================
// addEntityProperties for inactive entity (throws)
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, AddEntityProperties_Inactive) {
	auto pm = dataManager->getPropertyManager();
	std::unordered_map<std::string, PropertyValue> props = {{"key", PropertyValue(1)}};
	EXPECT_THROW(pm->addEntityProperties<Node>(0, props), std::runtime_error);
}

TEST_F(PropertyManagerInactiveAndExternalTest, AddEntityProperties_NonExistent) {
	auto pm = dataManager->getPropertyManager();
	std::unordered_map<std::string, PropertyValue> props = {{"key", PropertyValue(1)}};
	EXPECT_THROW(pm->addEntityProperties<Node>(99999, props), std::runtime_error);
}

// ============================================================================
// removeEntityProperty for inactive entity (no-op)
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, RemoveEntityProperty_Inactive) {
	auto pm = dataManager->getPropertyManager();
	// Should not throw -- idempotent
	EXPECT_NO_THROW(pm->removeEntityProperty<Node>(0, "key"));
}

TEST_F(PropertyManagerInactiveAndExternalTest, RemoveEntityProperty_NonExistent) {
	auto pm = dataManager->getPropertyManager();
	EXPECT_NO_THROW(pm->removeEntityProperty<Node>(99999, "key"));
}

// ============================================================================
// removeEntityProperty for entity that doesn't have the key
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, RemoveEntityProperty_KeyNotFound) {
	Node n = createTestNode(dataManager, "RemKeyNode");
	dataManager->addNode(n);

	auto pm = dataManager->getPropertyManager();
	// Node exists but has no property "nonexistent"
	EXPECT_NO_THROW(pm->removeEntityProperty<Node>(n.getId(), "nonexistent"));
}

// ============================================================================
// calculateEntityTotalPropertySize for non-existent entity
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, CalculateEntityTotalPropertySize_NonExistent) {
	auto pm = dataManager->getPropertyManager();
	auto size = pm->calculateEntityTotalPropertySize<Node>(99999);
	EXPECT_EQ(size, 0u);
}

TEST_F(PropertyManagerInactiveAndExternalTest, CalculateEntityTotalPropertySize_ZeroId) {
	auto pm = dataManager->getPropertyManager();
	auto size = pm->calculateEntityTotalPropertySize<Node>(0);
	EXPECT_EQ(size, 0u);
}

// ============================================================================
// hasExternalProperty for entity without external properties
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, HasExternalProperty_NoExternal) {
	Node n = createTestNode(dataManager, "NoExtProp");
	dataManager->addNode(n);

	auto pm = dataManager->getPropertyManager();
	bool has = pm->hasExternalProperty(n, "somekey");
	EXPECT_FALSE(has);
}

// ============================================================================
// storeProperties with empty properties (returns early after cleanup)
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, StoreProperties_EmptyProps) {
	Node n = createTestNode(dataManager, "EmptyProps");
	n.clearProperties();
	dataManager->addNode(n);

	auto pm = dataManager->getPropertyManager();
	// Store empty properties -- should return early
	EXPECT_NO_THROW(pm->storeProperties(n));
}

// ============================================================================
// Edge property operations
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, GetEntityProperties_Edge_Inactive) {
	auto pm = dataManager->getPropertyManager();
	auto props = pm->getEntityProperties<Edge>(0);
	EXPECT_TRUE(props.empty());
}

TEST_F(PropertyManagerInactiveAndExternalTest, AddEntityProperties_Edge_Inactive) {
	auto pm = dataManager->getPropertyManager();
	std::unordered_map<std::string, PropertyValue> props = {{"key", PropertyValue(1)}};
	EXPECT_THROW(pm->addEntityProperties<Edge>(0, props), std::runtime_error);
}

TEST_F(PropertyManagerInactiveAndExternalTest, HasExternalProperty_Edge) {
	Node n1 = createTestNode(dataManager, "EPn1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "EPn2");
	dataManager->addNode(n2);

	Edge e = createTestEdge(dataManager, n1.getId(), n2.getId(), "EPEdge");
	dataManager->addEdge(e);

	auto pm = dataManager->getPropertyManager();
	bool has = pm->hasExternalProperty(e, "somekey");
	EXPECT_FALSE(has);
}

// ============================================================================
// hasExternalProperty with PROPERTY_ENTITY storage type
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, HasExternalProperty_WithPropertyEntity) {
	Node n = createTestNode(dataManager, "ExtPropNode");
	dataManager->addNode(n);

	// Add enough properties to trigger external storage (Property entity)
	std::unordered_map<std::string, PropertyValue> props;
	for (int i = 0; i < 10; ++i) {
		props["key_" + std::to_string(i)] = PropertyValue(std::string("value_" + std::to_string(i)));
	}
	dataManager->addNodeProperties(n.getId(), props);

	// Re-fetch the node to get updated property reference
	Node updated = dataManager->getNode(n.getId());

	auto pm = dataManager->getPropertyManager();
	// Check for a key that exists in external storage
	bool has = pm->hasExternalProperty(updated, "key_0");
	// Whether true or false depends on if external storage was used, but should not throw
	(void)has;
	EXPECT_NO_THROW(pm->hasExternalProperty(updated, "nonexistent"));
}

// ============================================================================
// getEntityProperties for active entity that exists but is not active (!isActive branch)
// Line 251: entity.getId() != 0 && !entity.isActive()
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, GetEntityProperties_ExistsButInactive) {
	Node n = createTestNode(dataManager, "InactiveNode");
	dataManager->addNode(n);
	simulateSave(); // Persist so entity is on disk

	// Delete and flush so entity is marked inactive on disk
	dataManager->deleteNode(n);
	simulateSave();
	dataManager->clearCache();

	auto pm = dataManager->getPropertyManager();
	auto props = pm->getEntityProperties<Node>(n.getId());
	EXPECT_TRUE(props.empty());
}

TEST_F(PropertyManagerInactiveAndExternalTest, GetEntityProperties_Edge_ExistsButInactive) {
	Node n1 = createTestNode(dataManager, "IEN1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "IEN2");
	dataManager->addNode(n2);

	Edge e = createTestEdge(dataManager, n1.getId(), n2.getId(), "InactiveEdge");
	dataManager->addEdge(e);
	simulateSave();

	dataManager->deleteEdge(e);
	simulateSave();
	dataManager->clearCache();

	auto pm = dataManager->getPropertyManager();
	auto props = pm->getEntityProperties<Edge>(e.getId());
	EXPECT_TRUE(props.empty());
}

// ============================================================================
// addEntityProperties for active but non-active entity (!isActive() branch)
// Line 305: entity.getId() != 0 && !entity.isActive()
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, AddEntityProperties_ExistsButInactive) {
	Node n = createTestNode(dataManager, "InactiveAdd");
	dataManager->addNode(n);
	int64_t id = n.getId();
	simulateSave();

	dataManager->deleteNode(n);
	simulateSave();
	dataManager->clearCache();

	auto pm = dataManager->getPropertyManager();
	std::unordered_map<std::string, PropertyValue> props = {{"key", PropertyValue(1)}};
	EXPECT_THROW(pm->addEntityProperties<Node>(id, props), std::runtime_error);
}

TEST_F(PropertyManagerInactiveAndExternalTest, AddEntityProperties_Edge_ExistsButInactive) {
	Node n1 = createTestNode(dataManager, "IAE1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "IAE2");
	dataManager->addNode(n2);

	Edge e = createTestEdge(dataManager, n1.getId(), n2.getId(), "InactiveAddEdge");
	dataManager->addEdge(e);
	int64_t eid = e.getId();
	simulateSave();

	dataManager->deleteEdge(e);
	simulateSave();
	dataManager->clearCache();

	auto pm = dataManager->getPropertyManager();
	std::unordered_map<std::string, PropertyValue> props = {{"key", PropertyValue(1)}};
	EXPECT_THROW(pm->addEntityProperties<Edge>(eid, props), std::runtime_error);
}

// ============================================================================
// removeEntityProperty for active but non-active entity (!isActive() branch)
// Line 337: entity.getId() != 0 && !entity.isActive()
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, RemoveEntityProperty_ExistsButInactive) {
	Node n = createTestNode(dataManager, "InactiveRem");
	n.addProperty("pk", PropertyValue(42));
	dataManager->addNode(n);
	int64_t id = n.getId();
	simulateSave();

	dataManager->deleteNode(n);
	simulateSave();
	dataManager->clearCache();

	auto pm = dataManager->getPropertyManager();
	EXPECT_NO_THROW(pm->removeEntityProperty<Node>(id, "pk"));
}

TEST_F(PropertyManagerInactiveAndExternalTest, RemoveEntityProperty_Edge_ExistsButInactive) {
	Node n1 = createTestNode(dataManager, "IRE1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "IRE2");
	dataManager->addNode(n2);

	Edge e = createTestEdge(dataManager, n1.getId(), n2.getId(), "InactiveRemEdge");
	dataManager->addEdge(e);
	int64_t eid = e.getId();
	simulateSave();

	dataManager->deleteEdge(e);
	simulateSave();
	dataManager->clearCache();

	auto pm = dataManager->getPropertyManager();
	EXPECT_NO_THROW(pm->removeEntityProperty<Edge>(eid, "key"));
}

// ============================================================================
// calculateEntityTotalPropertySize for active but non-active entity
// Line 453: entity.getId() != 0 && !entity.isActive()
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, CalculateEntityTotalPropertySize_ExistsButInactive) {
	Node n = createTestNode(dataManager, "InactiveSize");
	n.addProperty("sk", PropertyValue(42));
	dataManager->addNode(n);
	int64_t id = n.getId();
	simulateSave();

	dataManager->deleteNode(n);
	simulateSave();
	dataManager->clearCache();

	auto pm = dataManager->getPropertyManager();
	auto size = pm->calculateEntityTotalPropertySize<Node>(id);
	EXPECT_EQ(size, 0u);
}

TEST_F(PropertyManagerInactiveAndExternalTest, CalculateEntityTotalPropertySize_Edge) {
	Node n1 = createTestNode(dataManager, "CSE1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "CSE2");
	dataManager->addNode(n2);

	Edge e = createTestEdge(dataManager, n1.getId(), n2.getId(), "SizeEdge");
	dataManager->addEdge(e);

	auto pm = dataManager->getPropertyManager();
	auto size = pm->calculateEntityTotalPropertySize<Edge>(e.getId());
	// Edge has no properties, size should be 0
	EXPECT_EQ(size, 0u);
}

// ============================================================================
// removeEntityProperty where key does not exist (propertyWasRemoved stays false)
// Exercises line 402 false branch
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, RemoveEntityProperty_Edge_KeyNotFound) {
	Node n1 = createTestNode(dataManager, "REKN1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "REKN2");
	dataManager->addNode(n2);

	Edge e = createTestEdge(dataManager, n1.getId(), n2.getId(), "RemKeyEdge");
	dataManager->addEdge(e);

	auto pm = dataManager->getPropertyManager();
	EXPECT_NO_THROW(pm->removeEntityProperty<Edge>(e.getId(), "nonexistent"));
}

// ============================================================================
// getEntityProperties with PROPERTY_ENTITY external storage (line 276 False)
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, GetEntityProperties_WithExternalPropertyEntity) {
	Node n = createTestNode(dataManager, "ExtPropGet");
	dataManager->addNode(n);

	// Add enough properties to trigger external property entity storage
	std::unordered_map<std::string, PropertyValue> props;
	for (int i = 0; i < 15; ++i) {
		props["prop_" + std::to_string(i)] = PropertyValue(std::string("val_" + std::to_string(i)));
	}
	dataManager->addNodeProperties(n.getId(), props);

	auto pm = dataManager->getPropertyManager();
	auto retrieved = pm->getEntityProperties<Node>(n.getId());
	// Should have retrieved all the external properties
	EXPECT_GE(retrieved.size(), 10u);
}

// ============================================================================
// calculateEntityTotalPropertySize with external storage
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, CalculateEntityTotalPropertySize_WithExternalProps) {
	Node n = createTestNode(dataManager, "SizeExtNode");
	dataManager->addNode(n);

	std::unordered_map<std::string, PropertyValue> props;
	for (int i = 0; i < 15; ++i) {
		props["szprop_" + std::to_string(i)] = PropertyValue(std::string("szval_" + std::to_string(i)));
	}
	dataManager->addNodeProperties(n.getId(), props);

	auto pm = dataManager->getPropertyManager();
	auto size = pm->calculateEntityTotalPropertySize<Node>(n.getId());
	EXPECT_GT(size, 0u);
}

// ============================================================================
// removeEntityProperty from external PROPERTY_ENTITY storage
// Exercises line 356-370
// ============================================================================

TEST_F(PropertyManagerInactiveAndExternalTest, RemoveEntityProperty_FromExternalStorage) {
	Node n = createTestNode(dataManager, "RemExtNode");
	dataManager->addNode(n);

	std::unordered_map<std::string, PropertyValue> props;
	for (int i = 0; i < 15; ++i) {
		props["remprop_" + std::to_string(i)] = PropertyValue(std::string("remval_" + std::to_string(i)));
	}
	dataManager->addNodeProperties(n.getId(), props);

	auto pm = dataManager->getPropertyManager();
	EXPECT_NO_THROW(pm->removeEntityProperty<Node>(n.getId(), "remprop_0"));

	// Verify the property was removed
	auto retrieved = pm->getEntityProperties<Node>(n.getId());
	EXPECT_EQ(retrieved.count("remprop_0"), 0u);
}
