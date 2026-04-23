/**
 * @file test_DataManager_ReadPaths.cpp
 * @brief Focused tests for DataManager snapshot/direct-read/bulk-read paths.
 **/

#include "DataManagerTestFixture.hpp"
#include "graph/storage/CommittedSnapshot.hpp"
#include <utility>

TEST_F(DataManagerTest, GetNodePropertiesDirectHandlesInvalidNodeAndPropertyEntity) {
	Node invalid;
	invalid.markInactive();
	EXPECT_TRUE(dataManager->getNodePropertiesDirect(invalid).empty());

	auto node = createTestNode(dataManager, "DirectPropsNode");
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), {
		{"name", PropertyValue(std::string("Alice"))},
		{"age", PropertyValue(int64_t(42))}
	});

	const Node storedNode = dataManager->getNode(node.getId());
	ASSERT_TRUE(storedNode.hasPropertyEntity());
	EXPECT_EQ(storedNode.getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);

	const auto props = dataManager->getNodePropertiesDirect(storedNode);
	ASSERT_EQ(props.size(), 2UL);
	EXPECT_EQ(std::get<std::string>(props.at("name").getVariant()), "Alice");
	EXPECT_EQ(std::get<int64_t>(props.at("age").getVariant()), 42);
}

TEST_F(DataManagerTest, GetNodePropertiesFromMapUsesProvidedPropertyMap) {
	auto node = createTestNode(dataManager, "MapPropsNode");
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), {
		{"k1", PropertyValue(std::string("v1"))},
		{"k2", PropertyValue(int64_t(7))}
	});

	const Node storedNode = dataManager->getNode(node.getId());
	ASSERT_TRUE(storedNode.hasPropertyEntity());
	const int64_t propertyId = storedNode.getPropertyEntityId();

	std::unordered_map<int64_t, Property> propertyMap;
	propertyMap[propertyId] = dataManager->getProperty(propertyId);

	const auto fromMap = dataManager->getNodePropertiesFromMap(storedNode, propertyMap);
	ASSERT_EQ(fromMap.size(), 2UL);
	EXPECT_EQ(std::get<std::string>(fromMap.at("k1").getVariant()), "v1");
	EXPECT_EQ(std::get<int64_t>(fromMap.at("k2").getVariant()), 7);

	const auto missingMap = dataManager->getNodePropertiesFromMap(storedNode, {});
	EXPECT_TRUE(missingMap.empty());
}

TEST_F(DataManagerTest, BulkLoadPropertyEntitiesLoadsPersistedProperties) {
	auto n1 = createTestNode(dataManager, "PropBulkNode");
	auto n2 = createTestNode(dataManager, "PropBulkNode");
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	dataManager->addNodeProperties(n1.getId(), {{"a", PropertyValue(int64_t(1))}});
	dataManager->addNodeProperties(n2.getId(), {{"b", PropertyValue(std::string("x"))}});

	const Node s1 = dataManager->getNode(n1.getId());
	const Node s2 = dataManager->getNode(n2.getId());
	ASSERT_TRUE(s1.hasPropertyEntity());
	ASSERT_TRUE(s2.hasPropertyEntity());

	fileStorage->flush();
	dataManager->clearCache();

	const std::vector<int64_t> ids = {s1.getPropertyEntityId(), s2.getPropertyEntityId()};
	const auto loaded = dataManager->bulkLoadPropertyEntities(ids, nullptr);
	ASSERT_EQ(loaded.size(), 2UL);
	EXPECT_TRUE(loaded.contains(ids[0]));
	EXPECT_TRUE(loaded.contains(ids[1]));
	EXPECT_EQ(std::get<int64_t>(loaded.at(ids[0]).getPropertyValues().at("a").getVariant()), 1);
	EXPECT_EQ(std::get<std::string>(loaded.at(ids[1]).getPropertyValues().at("b").getVariant()), "x");
}

TEST_F(DataManagerTest, BulkLoadEntitiesReadsNodesEdgesAndProperties) {
	auto n1 = createTestNode(dataManager, "BulkNode");
	auto n2 = createTestNode(dataManager, "BulkNode");
	auto n3 = createTestNode(dataManager, "BulkNode");
	dataManager->addNode(n1);
	dataManager->addNode(n2);
	dataManager->addNode(n3);

	auto e1 = createTestEdge(dataManager, n1.getId(), n2.getId(), "BULK_EDGE");
	dataManager->addEdge(e1);

	Property p1 = createTestProperty(n1.getId(), Node::typeId, {{"score", PropertyValue(int64_t(99))}});
	dataManager->addPropertyEntity(p1);

	fileStorage->flush();
	dataManager->clearCache();

	const auto nodes = dataManager->bulkLoadEntities<Node>(n1.getId(), n3.getId());
	const auto edges = dataManager->bulkLoadEntities<Edge>(e1.getId(), e1.getId());
	const auto props = dataManager->bulkLoadEntities<Property>(p1.getId(), p1.getId());

	EXPECT_GE(nodes.size(), 3UL);
	ASSERT_EQ(edges.size(), 1UL);
	ASSERT_EQ(props.size(), 1UL);
	EXPECT_EQ(edges.front().getId(), e1.getId());
	EXPECT_EQ(props.front().getId(), p1.getId());
}

TEST_F(DataManagerTest, SnapshotReadsPreferSnapshotStateAndDeletionMarker) {
	auto node = createTestNode(dataManager, "SnapshotNode");
	dataManager->addNode(node);
	fileStorage->flush();

	const int64_t nodeId = node.getId();
	const int64_t newLabelId = dataManager->getOrCreateTokenId("SnapshotNodeUpdated");

	CommittedSnapshot snapshot;
	Node shadow = dataManager->getNode(nodeId);
	shadow.setLabelId(newLabelId);
	snapshot.nodes[nodeId] = DirtyEntityInfo<Node>(EntityChangeType::CHANGE_MODIFIED, shadow);

	dataManager->setCurrentSnapshot(&snapshot);
	const auto snapRead = dataManager->getEntityFromMemoryOrDisk<Node>(nodeId);
	EXPECT_EQ(snapRead.getLabelId(), newLabelId);

	snapshot.nodes[nodeId] = DirtyEntityInfo<Node>(EntityChangeType::CHANGE_DELETED);
	const auto deletedRead = dataManager->getEntityFromMemoryOrDisk<Node>(nodeId);
	EXPECT_FALSE(deletedRead.isActive());
	dataManager->clearCurrentSnapshot();
}

TEST_F(DataManagerTest, LoadEntityDirectReturnsDirtyOverrideAndDiskFallback) {
	auto node = createTestNode(dataManager, "DirectNode");
	dataManager->addNode(node);
	fileStorage->flush();

	const int64_t nodeId = node.getId();
	const int64_t updatedLabelId = dataManager->getOrCreateTokenId("DirectNodeUpdated");

	Node updated = dataManager->getNode(nodeId);
	updated.setLabelId(updatedLabelId);
	dataManager->updateNode(updated);

	const auto directDirty = dataManager->loadEntityDirect<Node>(nodeId);
	EXPECT_EQ(directDirty.getLabelId(), updatedLabelId);

	fileStorage->flush();
	dataManager->clearCache();
	const auto directDisk = dataManager->loadEntityDirect<Node>(nodeId);
	EXPECT_TRUE(directDisk.isActive());
	EXPECT_EQ(directDisk.getLabelId(), updatedLabelId);

	Node toDelete = dataManager->getNode(nodeId);
	dataManager->deleteNode(toDelete);
	const auto directDeleted = dataManager->loadEntityDirect<Node>(nodeId);
	EXPECT_FALSE(directDeleted.isActive());
}

// ============================================================================
// getNodePropertiesFromMap: inactive non-zero node → early-return {}
// Covers the !node.isActive() branch at line ~307 (node.getId()!=0 but inactive)
// ============================================================================

TEST_F(DataManagerTest, GetNodePropertiesFromMap_InactiveNodeWithNonZeroId) {
	// Create and add a node, flush to disk, then delete it so it gets a non-zero ID
	// but becomes inactive.
	auto node = createTestNode(dataManager, "InactiveMapNode");
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), {{"x", PropertyValue(int64_t(42))}});
	simulateSave();

	// Build a property map from the persisted property entity
	const Node storedNode = dataManager->getNode(node.getId());
	ASSERT_NE(storedNode.getId(), 0);
	ASSERT_TRUE(storedNode.isActive());

	// Now create a copy with the same non-zero ID but mark it inactive
	Node inactiveNode = storedNode;
	inactiveNode.markInactive();
	EXPECT_NE(inactiveNode.getId(), 0);
	EXPECT_FALSE(inactiveNode.isActive());

	// getNodePropertiesFromMap should return {} for an inactive node (even with non-zero ID)
	std::unordered_map<int64_t, Property> propMap;
	if (storedNode.hasPropertyEntity()) {
		propMap[storedNode.getPropertyEntityId()] = dataManager->getProperty(storedNode.getPropertyEntityId());
	}
	const auto result = dataManager->getNodePropertiesFromMap(inactiveNode, propMap);
	EXPECT_TRUE(result.empty());
}

// ============================================================================
// getNodePropertiesDirect: inactive node with non-zero ID → early-return {}
// Covers the !node.isActive() branch at line ~275 (getId()!=0 but inactive)
// ============================================================================

TEST_F(DataManagerTest, GetNodePropertiesDirect_InactiveNodeWithNonZeroId) {
	auto node = createTestNode(dataManager, "DirectInactiveNode");
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), {{"y", PropertyValue(int64_t(7))}});
	simulateSave();

	const Node storedNode = dataManager->getNode(node.getId());
	ASSERT_NE(storedNode.getId(), 0);
	ASSERT_TRUE(storedNode.isActive());

	// Make an inactive copy with non-zero ID
	Node inactiveNode = storedNode;
	inactiveNode.markInactive();
	EXPECT_NE(inactiveNode.getId(), 0);
	EXPECT_FALSE(inactiveNode.isActive());

	// getNodePropertiesDirect should return {} for an inactive node with non-zero ID
	const auto result = dataManager->getNodePropertiesDirect(inactiveNode);
	EXPECT_TRUE(result.empty());
}

// ============================================================================
// addNodes / addEdges: empty vector → early return (line ~194)
// ============================================================================

TEST_F(DataManagerTest, AddNodes_EmptyVector_NoOp) {
	std::vector<Node> emptyNodes;
	EXPECT_NO_THROW(dataManager->addNodes(emptyNodes));
	EXPECT_EQ(observer->addedNodes.size(), 0u);
}

TEST_F(DataManagerTest, AddEdges_EmptyVector_NoOp) {
	std::vector<Edge> emptyEdges;
	EXPECT_NO_THROW(dataManager->addEdges(emptyEdges));
	EXPECT_EQ(observer->addedEdges.size(), 0u);
}

TEST_F(DataManagerTest, HeaderSnapshotAndTransactionAccessors) {
	const auto header = dataManager->getFileHeader();
	EXPECT_EQ(dataManager->getFileVersion(), header.version);
	EXPECT_EQ(dataManager->getFileHeaderRef().version, header.version);

	auto &observerManager = dataManager->getObserverManager();
	const auto &observerManagerConst = std::as_const(*dataManager).getObserverManager();
	EXPECT_EQ(observerManager.getObservers().size(), observerManagerConst.getObservers().size());

	(void)dataManager->hasPreadSupport();

	CommittedSnapshot snapshot;
	dataManager->setCurrentSnapshot(&snapshot);
	EXPECT_EQ(dataManager->getCurrentSnapshot(), &snapshot);
	dataManager->clearCurrentSnapshot();
	EXPECT_EQ(dataManager->getCurrentSnapshot(), nullptr);

	EXPECT_FALSE(dataManager->hasActiveTransaction());
	dataManager->setActiveTransaction(123);
	EXPECT_TRUE(dataManager->hasActiveTransaction());
	EXPECT_EQ(dataManager->getActiveTxnId(), 123UL);
	EXPECT_TRUE(dataManager->getTransactionOps().empty());
	(void)dataManager->getTransactionContext();
	dataManager->clearActiveTransaction();
	EXPECT_FALSE(dataManager->hasActiveTransaction());
}

// ============================================================================
// resolveTokenId("") → early return 0 (line ~157)
// ============================================================================

TEST_F(DataManagerTest, ResolveTokenId_EmptyName_ReturnsZero) {
	EXPECT_EQ(dataManager->resolveTokenId(""), 0);
}

// ============================================================================
// getNodePropertiesDirect: node with no external property entity → skip blob/prop branch
// Covers the hasPropertyEntity() == false branch at line ~282
// ============================================================================

TEST_F(DataManagerTest, GetNodePropertiesDirect_NodeWithNoExternalProperties) {
	// Create a node with no properties — no external property entity is created
	auto node = createTestNode(dataManager, "NoPropNode");
	dataManager->addNode(node);

	const Node storedNode = dataManager->getNode(node.getId());
	ASSERT_TRUE(storedNode.isActive());
	EXPECT_FALSE(storedNode.hasPropertyEntity());

	// getNodePropertiesDirect should return {} since there are no properties at all
	const auto result = dataManager->getNodePropertiesDirect(storedNode);
	EXPECT_TRUE(result.empty());
}

// ============================================================================
// getNodePropertiesFromMap: node with no external property entity → skip map lookup
// Covers the hasPropertyEntity() == false branch at line ~312
// ============================================================================

TEST_F(DataManagerTest, GetNodePropertiesFromMap_NodeWithNoExternalProperties) {
	// Create a node with no properties
	auto node = createTestNode(dataManager, "NoPropMapNode");
	dataManager->addNode(node);

	const Node storedNode = dataManager->getNode(node.getId());
	ASSERT_TRUE(storedNode.isActive());
	EXPECT_FALSE(storedNode.hasPropertyEntity());

	// Even with a non-empty map, should return {} because node has no external property entity
	std::unordered_map<int64_t, Property> propMap;
	propMap[999] = Property{}; // irrelevant entry
	const auto result = dataManager->getNodePropertiesFromMap(storedNode, propMap);
	EXPECT_TRUE(result.empty());
}

// ============================================================================
// getNodePropertiesFromMap: property map entry has id==0 → skip that entry
// Covers the it->second.getId() != 0 → False branch at line ~318
// ============================================================================

TEST_F(DataManagerTest, GetNodePropertiesFromMap_PropertyMapEntryWithZeroId) {
	// Create a node with external properties so hasPropertyEntity() == true
	auto node = createTestNode(dataManager, "ZeroIdPropNode");
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), {{"z", PropertyValue(int64_t(0))}});

	const Node storedNode = dataManager->getNode(node.getId());
	ASSERT_TRUE(storedNode.hasPropertyEntity());
	const int64_t propEntityId = storedNode.getPropertyEntityId();

	// Put a Property with id==0 in the map for the correct key
	std::unordered_map<int64_t, Property> propMap;
	propMap[propEntityId] = Property{}; // default-constructed: id==0

	// Should return empty because the map entry's id is 0
	const auto result = dataManager->getNodePropertiesFromMap(storedNode, propMap);
	EXPECT_TRUE(result.empty());
}

// ============================================================================
// getNodePropertiesDirect: property entity has been deleted → loadEntityDirect returns id==0
// Covers the property.getId() != 0 → False branch at line ~288
// ============================================================================

TEST_F(DataManagerTest, GetNodePropertiesDirect_DeletedPropertyEntityReturnsEmpty) {
	// Create a node with external properties
	auto node = createTestNode(dataManager, "DeletedPropNode");
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), {{"q", PropertyValue(int64_t(1))}});
	simulateSave();

	const Node storedNode = dataManager->getNode(node.getId());
	ASSERT_TRUE(storedNode.hasPropertyEntity());
	ASSERT_EQ(storedNode.getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);

	// Directly delete the property entity so it ends up in dirty layer as CHANGE_DELETED
	Property prop = dataManager->getProperty(storedNode.getPropertyEntityId());
	ASSERT_NE(prop.getId(), 0);
	dataManager->deleteProperty(prop);

	// getNodePropertiesDirect calls loadEntityDirect<Property>, which finds CHANGE_DELETED
	// and returns a property with id==0, hitting the False branch of property.getId() != 0
	const auto result = dataManager->getNodePropertiesDirect(storedNode);
	EXPECT_TRUE(result.empty());
}
