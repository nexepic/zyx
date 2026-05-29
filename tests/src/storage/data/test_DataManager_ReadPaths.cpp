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

TEST_F(DataManagerTest, SnapshotReadsReturnBackupsForAllEntityKinds) {
	auto n1 = createTestNode(dataManager, "SnapshotAllSource");
	auto n2 = createTestNode(dataManager, "SnapshotAllTarget");
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "SNAPSHOT_ALL_EDGE");
	dataManager->addEdge(edge);

	Property property = createTestProperty(n1.getId(), Node::typeId, {{"p", PropertyValue(int64_t(1))}});
	dataManager->addPropertyEntity(property);

	Blob blob = createTestBlob("snapshot_blob_before");
	dataManager->addBlobEntity(blob);

	Index index = createTestIndex(Index::NodeType::LEAF, 100);
	dataManager->addIndexEntity(index);

	State state = createTestState("snapshot.all.state");
	state.setData("state_before");
	dataManager->addStateEntity(state);

	fileStorage->flush();

	CommittedSnapshot snapshot;

	Edge edgeBackup = dataManager->getEdge(edge.getId());
	edgeBackup.setTypeId(dataManager->getOrCreateTokenId("SNAPSHOT_ALL_EDGE_UPDATED"));
	snapshot.edges[edge.getId()] = DirtyEntityInfo<Edge>(EntityChangeType::CHANGE_MODIFIED, edgeBackup);
	EXPECT_EQ(dataManager->getEntityWithSnapshot<Edge>(edge.getId(), &snapshot).getTypeId(), edgeBackup.getTypeId());
	snapshot.edges[edge.getId()] = DirtyEntityInfo<Edge>(EntityChangeType::CHANGE_DELETED);
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Edge>(edge.getId(), &snapshot).isActive());

	Property propertyBackup = dataManager->getProperty(property.getId());
	propertyBackup.setProperties({{"p", PropertyValue(int64_t(2))}});
	snapshot.properties[property.getId()] =
			DirtyEntityInfo<Property>(EntityChangeType::CHANGE_MODIFIED, propertyBackup);
	EXPECT_EQ(std::get<int64_t>(dataManager->getEntityWithSnapshot<Property>(property.getId(), &snapshot)
										.getPropertyValues()
										.at("p")
										.getVariant()),
			  2);
	snapshot.properties[property.getId()] = DirtyEntityInfo<Property>(EntityChangeType::CHANGE_DELETED);
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Property>(property.getId(), &snapshot).isActive());

	Blob blobBackup = dataManager->getBlob(blob.getId());
	blobBackup.setData("snapshot_blob_after");
	snapshot.blobs[blob.getId()] = DirtyEntityInfo<Blob>(EntityChangeType::CHANGE_MODIFIED, blobBackup);
	EXPECT_EQ(dataManager->getEntityWithSnapshot<Blob>(blob.getId(), &snapshot).getDataAsString(),
			  "snapshot_blob_after");
	snapshot.blobs[blob.getId()] = DirtyEntityInfo<Blob>(EntityChangeType::CHANGE_DELETED);
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Blob>(blob.getId(), &snapshot).isActive());

	Index indexBackup = dataManager->getIndex(index.getId());
	indexBackup.setLevel(3);
	snapshot.indexes[index.getId()] = DirtyEntityInfo<Index>(EntityChangeType::CHANGE_MODIFIED, indexBackup);
	EXPECT_EQ(dataManager->getEntityWithSnapshot<Index>(index.getId(), &snapshot).getLevel(), 3);
	snapshot.indexes[index.getId()] = DirtyEntityInfo<Index>(EntityChangeType::CHANGE_DELETED);
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Index>(index.getId(), &snapshot).isActive());

	State stateBackup = dataManager->getState(state.getId());
	stateBackup.setData("state_after");
	snapshot.states[state.getId()] = DirtyEntityInfo<State>(EntityChangeType::CHANGE_MODIFIED, stateBackup);
	EXPECT_EQ(dataManager->getEntityWithSnapshot<State>(state.getId(), &snapshot).getDataAsString(), "state_after");
	snapshot.states[state.getId()] = DirtyEntityInfo<State>(EntityChangeType::CHANGE_DELETED);
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<State>(state.getId(), &snapshot).isActive());
}

TEST_F(DataManagerTest, DirectReadsReturnDirtyBackupsForAllEntityKinds) {
	auto n1 = createTestNode(dataManager, "DirectAllSource");
	auto n2 = createTestNode(dataManager, "DirectAllTarget");
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "DIRECT_ALL_EDGE");
	dataManager->addEdge(edge);

	Property property = createTestProperty(n1.getId(), Node::typeId, {{"p", PropertyValue(int64_t(10))}});
	dataManager->addPropertyEntity(property);

	Blob blob = createTestBlob("direct_blob_before");
	dataManager->addBlobEntity(blob);

	Index index = createTestIndex(Index::NodeType::LEAF, 200);
	dataManager->addIndexEntity(index);

	State state = createTestState("direct.all.state");
	state.setData("direct_state_before");
	dataManager->addStateEntity(state);

	fileStorage->flush();

	Edge edgeBackup = dataManager->getEdge(edge.getId());
	edgeBackup.setTypeId(dataManager->getOrCreateTokenId("DIRECT_ALL_EDGE_UPDATED"));
	dataManager->setEntityDirty(DirtyEntityInfo<Edge>(EntityChangeType::CHANGE_MODIFIED, edgeBackup));
	EXPECT_EQ(dataManager->loadEntityDirect<Edge>(edge.getId()).getTypeId(), edgeBackup.getTypeId());
	dataManager->deleteEdge(edgeBackup);
	EXPECT_FALSE(dataManager->loadEntityDirect<Edge>(edge.getId()).isActive());

	Property propertyBackup = dataManager->getProperty(property.getId());
	propertyBackup.setProperties({{"p", PropertyValue(int64_t(11))}});
	dataManager->setEntityDirty(DirtyEntityInfo<Property>(EntityChangeType::CHANGE_MODIFIED, propertyBackup));
	EXPECT_EQ(std::get<int64_t>(dataManager->loadEntityDirect<Property>(property.getId())
										.getPropertyValues()
										.at("p")
										.getVariant()),
			  11);
	dataManager->deleteProperty(propertyBackup);
	EXPECT_FALSE(dataManager->loadEntityDirect<Property>(property.getId()).isActive());

	Blob blobBackup = dataManager->getBlob(blob.getId());
	blobBackup.setData("direct_blob_after");
	dataManager->setEntityDirty(DirtyEntityInfo<Blob>(EntityChangeType::CHANGE_MODIFIED, blobBackup));
	EXPECT_EQ(dataManager->loadEntityDirect<Blob>(blob.getId()).getDataAsString(), "direct_blob_after");
	dataManager->deleteBlob(blobBackup);
	EXPECT_FALSE(dataManager->loadEntityDirect<Blob>(blob.getId()).isActive());

	Index indexBackup = dataManager->getIndex(index.getId());
	indexBackup.setLevel(4);
	dataManager->setEntityDirty(DirtyEntityInfo<Index>(EntityChangeType::CHANGE_MODIFIED, indexBackup));
	EXPECT_EQ(dataManager->loadEntityDirect<Index>(index.getId()).getLevel(), 4);
	dataManager->deleteIndex(indexBackup);
	EXPECT_FALSE(dataManager->loadEntityDirect<Index>(index.getId()).isActive());

	State stateBackup = dataManager->getState(state.getId());
	stateBackup.setData("direct_state_after");
	dataManager->setEntityDirty(DirtyEntityInfo<State>(EntityChangeType::CHANGE_MODIFIED, stateBackup));
	EXPECT_EQ(dataManager->loadEntityDirect<State>(state.getId()).getDataAsString(), "direct_state_after");
	dataManager->deleteState(stateBackup);
	EXPECT_FALSE(dataManager->loadEntityDirect<State>(state.getId()).isActive());
}

TEST_F(DataManagerTest, MemoryAndSnapshotDiskReadsReturnActiveEntitiesForAllEntityKinds) {
	auto n1 = createTestNode(dataManager, "DiskAllSource");
	auto n2 = createTestNode(dataManager, "DiskAllTarget");
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "DISK_ALL_EDGE");
	dataManager->addEdge(edge);

	Property property = createTestProperty(n1.getId(), Node::typeId, {{"p", PropertyValue(int64_t(21))}});
	dataManager->addPropertyEntity(property);

	Blob blob = createTestBlob("disk_blob");
	dataManager->addBlobEntity(blob);

	Index index = createTestIndex(Index::NodeType::LEAF, 300);
	dataManager->addIndexEntity(index);

	State state = createTestState("disk.all.state");
	state.setData("disk_state");
	dataManager->addStateEntity(state);

	fileStorage->flush();
	dataManager->clearCache();

	CommittedSnapshot snapshot;
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<Node>(n1.getId()).isActive());
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<Edge>(edge.getId()).isActive());
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<Property>(property.getId()).isActive());
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<Blob>(blob.getId()).isActive());
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<Index>(index.getId()).isActive());
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<State>(state.getId()).isActive());

	dataManager->clearCache();
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Node>(n1.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Edge>(edge.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Property>(property.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Blob>(blob.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Index>(index.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<State>(state.getId(), &snapshot).isActive());
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

// ============================================================================
// getNodePropertiesFromMap: node with BLOB_ENTITY storage
// Covers the `storageType == BLOB_ENTITY` branch at DataManager.cpp ~line 323
// ============================================================================

TEST_F(DataManagerTest, GetNodePropertiesFromMap_BlobEntityStorage) {
	auto node = createTestNode(dataManager, "BlobMapNode");
	dataManager->addNode(node);

	std::unordered_map<std::string, PropertyValue> largeProps;
	for (int i = 0; i < 10; ++i) {
		std::string val(800, static_cast<char>('A' + i));
		largeProps["bmap_key_" + std::to_string(i)] = PropertyValue(val);
	}
	dataManager->addNodeProperties(node.getId(), largeProps);

	const Node storedNode = dataManager->getNode(node.getId());
	ASSERT_TRUE(storedNode.isActive());
	ASSERT_EQ(storedNode.getPropertyStorageType(), PropertyStorageType::BLOB_ENTITY);

	// Build an empty (no-op) property map — the blob path ignores the map and
	// reads from the blob chain directly.
	std::unordered_map<int64_t, Property> propMap; // intentionally empty

	// getNodePropertiesFromMap with BLOB_ENTITY reads from blob chain.
	const auto fromMap = dataManager->getNodePropertiesFromMap(storedNode, propMap);
	EXPECT_EQ(fromMap.size(), largeProps.size());
}

// ============================================================================
// getNodePropertiesDirect: node with BLOB_ENTITY storage
// Covers the `storageType == BLOB_ENTITY` fallback at DataManager.cpp ~line 293
// ============================================================================

TEST_F(DataManagerTest, GetNodePropertiesDirect_BlobEntityStorage) {
	auto node = createTestNode(dataManager, "BlobDirectNode");
	dataManager->addNode(node);

	std::unordered_map<std::string, PropertyValue> largeProps;
	for (int i = 0; i < 10; ++i) {
		std::string val(800, static_cast<char>('a' + i));
		largeProps["bd_key_" + std::to_string(i)] = PropertyValue(val);
	}
	dataManager->addNodeProperties(node.getId(), largeProps);

	const Node storedNode = dataManager->getNode(node.getId());
	ASSERT_TRUE(storedNode.isActive());
	ASSERT_EQ(storedNode.getPropertyStorageType(), PropertyStorageType::BLOB_ENTITY);

	// getNodePropertiesDirect uses the blob fallback for BLOB_ENTITY nodes
	const auto direct = dataManager->getNodePropertiesDirect(storedNode);
	EXPECT_EQ(direct.size(), largeProps.size());
}
