/**
 * @file test_DataManager_RangeAndSnapshot.cpp
 * @brief Coverage tests for DataManager getEntitiesInRange disk pass,
 *        markEntityDeleted existing->delete path, getDirtyEntityInfos
 *        filtering, and getNodePropertiesFromMap/Direct with external props.
 **/

#include "DataManagerSharedTestFixture.hpp"
#include "graph/storage/CommittedSnapshot.hpp"

class DataManagerRangeSnapshotTest : public DataManagerSharedTest {};

TEST_F(DataManagerRangeSnapshotTest, GetEntitiesInRange_DiskPass) {
	// Add nodes and flush to disk
	std::vector<Node> created;
	for (int i = 0; i < 5; ++i) {
		Node n = createTestNode(dataManager, "Range");
		dataManager->addNode(n);
		created.push_back(n);
	}
	simulateSave();
	dataManager->clearCache();

	// Get range that requires disk read (no dirty entities in memory)
	auto result = dataManager->getEntitiesInRange<Node>(
		created.front().getId(), created.back().getId(), 10);
	EXPECT_GE(result.size(), 3u);
}

TEST_F(DataManagerRangeSnapshotTest, GetEntitiesInRange_MemoryPassLimitReached) {
	// Add nodes but don't flush - they'll be in dirty memory
	Node n1 = createTestNode(dataManager, "MemRange1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "MemRange2");
	dataManager->addNode(n2);
	Node n3 = createTestNode(dataManager, "MemRange3");
	dataManager->addNode(n3);

	// Limit of 2 should stop after 2 dirty entities
	auto result = dataManager->getEntitiesInRange<Node>(n1.getId(), n3.getId(), 2);
	EXPECT_EQ(result.size(), 2u);
}

TEST_F(DataManagerRangeSnapshotTest, GetEntitiesInRange_SkipDeletedInMemory) {
	Node n1 = createTestNode(dataManager, "Del1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "Del2");
	dataManager->addNode(n2);
	simulateSave();

	// Delete n1 in memory
	dataManager->deleteNode(n1);

	auto result = dataManager->getEntitiesInRange<Node>(n1.getId(), n2.getId(), 10);
	// n1 should be skipped (CHANGE_DELETED), n2 should appear
	bool foundN1 = false, foundN2 = false;
	for (const auto &r : result) {
		if (r.getId() == n1.getId() && r.isActive()) foundN1 = true;
		if (r.getId() == n2.getId() && r.isActive()) foundN2 = true;
	}
	EXPECT_FALSE(foundN1);
	EXPECT_TRUE(foundN2);
}

TEST_F(DataManagerRangeSnapshotTest, GetDirtyEntityInfos_SingleTypeFilter) {
	Node n = createTestNode(dataManager, "Filter");
	dataManager->addNode(n);

	// Filter for MODIFIED only (should not find the ADDED node)
	auto modified = dataManager->getDirtyEntityInfos<Node>(
		{EntityChangeType::CHANGE_MODIFIED});
	bool found = false;
	for (const auto &info : modified) {
		if (info.backup.has_value() && info.backup->getId() == n.getId())
			found = true;
	}
	EXPECT_FALSE(found);
}

TEST_F(DataManagerRangeSnapshotTest, GetDirtyEntityInfos_TwoTypeFilter) {
	Node n = createTestNode(dataManager, "TwoType");
	dataManager->addNode(n);

	// Filter for ADDED and MODIFIED (2 types, not 3, so takes the filter path)
	auto result = dataManager->getDirtyEntityInfos<Node>(
		{EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED});
	bool found = false;
	for (const auto &info : result) {
		if (info.backup.has_value() && info.backup->getId() == n.getId())
			found = true;
	}
	EXPECT_TRUE(found);
}

TEST_F(DataManagerRangeSnapshotTest, MarkEntityDeleted_ExistingEntity) {
	// Add a node and save to disk
	Node n = createTestNode(dataManager, "MarkDel");
	dataManager->addNode(n);
	simulateSave();
	dataManager->clearCache();

	// Load from disk
	Node loaded = dataManager->getNode(n.getId());
	ASSERT_TRUE(loaded.isActive());

	// Delete - this exercises the markEntityDeleted path where entity exists on disk
	dataManager->deleteNode(loaded);

	auto dirty = dataManager->getDirtyInfo<Node>(n.getId());
	ASSERT_TRUE(dirty.has_value());
	EXPECT_EQ(dirty->changeType, EntityChangeType::CHANGE_DELETED);
}

TEST_F(DataManagerRangeSnapshotTest, MarkEntityDeleted_AddedThenDeleted) {
	// Add a node (in-memory only, not saved)
	Node n = createTestNode(dataManager, "AddDel");
	dataManager->addNode(n);

	// Delete immediately - the ADDED record should just be removed
	dataManager->deleteNode(n);

	auto dirty = dataManager->getDirtyInfo<Node>(n.getId());
	EXPECT_FALSE(dirty.has_value());
}

TEST_F(DataManagerRangeSnapshotTest, GetNodePropertiesDirect_WithExternalProperty) {
	Node n = createTestNode(dataManager, "ExtProp");
	dataManager->addNode(n);
	// Add enough properties to trigger external storage
	std::unordered_map<std::string, PropertyValue> props;
	for (int i = 0; i < 15; ++i) {
		props["key" + std::to_string(i)] = PropertyValue(std::string(50, 'a' + (i % 26)));
	}
	dataManager->addNodeProperties(n.getId(), props);
	simulateSave();
	dataManager->clearCache();

	Node reloaded = dataManager->getNode(n.getId());
	auto directProps = dataManager->getNodePropertiesDirect(reloaded);
	EXPECT_GE(directProps.size(), 15u);
}

TEST_F(DataManagerRangeSnapshotTest, GetNodePropertiesFromMap_WithPropertyEntity) {
	Node n = createTestNode(dataManager, "MapProp");
	dataManager->addNode(n);
	dataManager->addNodeProperties(n.getId(), {{"color", PropertyValue("red")}});
	simulateSave();

	Node reloaded = dataManager->getNode(n.getId());

	// Build a property map with the property entities
	std::unordered_map<int64_t, Property> propMap;
	// Get all properties in range for this purpose
	auto propEntities = dataManager->getEntitiesInRange<Property>(1, 1000, 100);
	for (auto &pe : propEntities) {
		propMap[pe.getId()] = pe;
	}

	auto result = dataManager->getNodePropertiesFromMap(reloaded, propMap);
	EXPECT_FALSE(result.empty());
}

TEST_F(DataManagerRangeSnapshotTest, UpdateEntity_Node) {
	Node n = createTestNode(dataManager, "UpdEntity");
	dataManager->addNode(n);
	simulateSave();

	n.addLabelId(dataManager->getOrCreateTokenId("NewLbl"));
	dataManager->updateEntity<Node>(n);

	Node loaded = dataManager->getEntity<Node>(n.getId());
	EXPECT_TRUE(loaded.isActive());
}

TEST_F(DataManagerRangeSnapshotTest, UpdateEntity_Edge) {
	Node n1 = createTestNode(dataManager, "E1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "E2");
	dataManager->addNode(n2);
	Edge e = createTestEdge(dataManager, n1.getId(), n2.getId(), "RELATED");
	dataManager->addEdge(e);
	simulateSave();

	e.setTypeId(dataManager->getOrCreateTokenId("KNOWS"));
	dataManager->updateEntity<Edge>(e);

	Edge loaded = dataManager->getEntity<Edge>(e.getId());
	EXPECT_TRUE(loaded.isActive());
}

TEST_F(DataManagerRangeSnapshotTest, GetEntitiesInRange_Edges) {
	Node n1 = createTestNode(dataManager, "EN1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "EN2");
	dataManager->addNode(n2);
	Edge e = createTestEdge(dataManager, n1.getId(), n2.getId(), "CONN");
	dataManager->addEdge(e);
	simulateSave();
	dataManager->clearCache();

	auto result = dataManager->getEntitiesInRange<Edge>(e.getId(), e.getId(), 10);
	EXPECT_GE(result.size(), 1u);
}

TEST_F(DataManagerRangeSnapshotTest, BulkLoadEntities_Nodes) {
	std::vector<Node> nodes;
	for (int i = 0; i < 5; ++i) {
		Node n = createTestNode(dataManager, "Bulk");
		dataManager->addNode(n);
		nodes.push_back(n);
	}
	simulateSave();

	auto result = dataManager->bulkLoadEntities<Node>(nodes.front().getId(), nodes.back().getId());
	EXPECT_GE(result.size(), 3u);
}
