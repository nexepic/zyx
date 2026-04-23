/**
 * @file test_DataManager_CoveragePaths.cpp
 * @brief Branch coverage tests for DataManager — covers edge cases in
 *        getEntityFromMemoryOrDisk, findEdgesByNode, getDirtyEntityInfos,
 *        and other underexercised paths.
 **/

#include "DataManagerSharedTestFixture.hpp"
#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/storage/data/PropertyManager.hpp"

class DataManagerCoverageTest : public DataManagerSharedTest {};

TEST_F(DataManagerCoverageTest, FindEdgesByNode_OutDirection) {
	Node n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);
	Edge e = createTestEdge(dataManager, n1.getId(), n2.getId(), "KNOWS");
	dataManager->addEdge(e);
	simulateSave();

	auto out = dataManager->findEdgesByNode(n1.getId(), "out");
	EXPECT_GE(out.size(), 1u);
}

TEST_F(DataManagerCoverageTest, FindEdgesByNode_InDirection) {
	Node n1 = createTestNode(dataManager, "X");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "Y");
	dataManager->addNode(n2);
	Edge e = createTestEdge(dataManager, n1.getId(), n2.getId(), "LIKES");
	dataManager->addEdge(e);
	simulateSave();

	auto in = dataManager->findEdgesByNode(n2.getId(), "in");
	EXPECT_GE(in.size(), 1u);
}

TEST_F(DataManagerCoverageTest, FindEdgesByNode_BothDirection) {
	Node n1 = createTestNode(dataManager, "P");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "Q");
	dataManager->addNode(n2);
	Edge e = createTestEdge(dataManager, n1.getId(), n2.getId(), "CONNECTS");
	dataManager->addEdge(e);
	simulateSave();

	auto both = dataManager->findEdgesByNode(n1.getId(), "both");
	EXPECT_GE(both.size(), 1u);
}

TEST_F(DataManagerCoverageTest, GetDirtyEntityInfos_FilterByType) {
	Node n1 = createTestNode(dataManager, "Filter");
	dataManager->addNode(n1);

	auto addedOnly = dataManager->getDirtyEntityInfos<Node>({EntityChangeType::CHANGE_ADDED});
	bool foundAdded = false;
	for (const auto &info : addedOnly) {
		if (info.backup.has_value() && info.backup->getId() == n1.getId())
			foundAdded = true;
	}
	EXPECT_TRUE(foundAdded);

	auto deletedOnly = dataManager->getDirtyEntityInfos<Node>({EntityChangeType::CHANGE_DELETED});
	bool foundInDeleted = false;
	for (const auto &info : deletedOnly) {
		if (info.backup.has_value() && info.backup->getId() == n1.getId())
			foundInDeleted = true;
	}
	EXPECT_FALSE(foundInDeleted);
}

TEST_F(DataManagerCoverageTest, GetDirtyEntityInfos_AllTypes) {
	Node n = createTestNode(dataManager, "All");
	dataManager->addNode(n);

	auto all = dataManager->getDirtyEntityInfos<Node>(
		{EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED, EntityChangeType::CHANGE_DELETED});
	EXPECT_GE(all.size(), 1u);
}

TEST_F(DataManagerCoverageTest, SetEntityDirty_ZeroId) {
	Node n;
	n.setId(0);
	DirtyEntityInfo<Node> info(EntityChangeType::CHANGE_ADDED, n);
	dataManager->setEntityDirty(info);
	auto dirty = dataManager->getDirtyInfo<Node>(0);
	EXPECT_FALSE(dirty.has_value());
}

TEST_F(DataManagerCoverageTest, GetEntityFromMemoryOrDisk_DeletedEntity) {
	Node n = createTestNode(dataManager, "Del");
	dataManager->addNode(n);
	simulateSave();

	dataManager->deleteNode(n);

	Node got = dataManager->getNode(n.getId());
	EXPECT_FALSE(got.isActive());
}

TEST_F(DataManagerCoverageTest, MarkEntityDeleted_WasAdded) {
	Node n = createTestNode(dataManager, "AddThenDel");
	dataManager->addNode(n);

	dataManager->deleteNode(n);

	auto dirty = dataManager->getDirtyInfo<Node>(n.getId());
	EXPECT_FALSE(dirty.has_value());
}

TEST_F(DataManagerCoverageTest, MarkEntityDeleted_WasExisting) {
	Node n = createTestNode(dataManager, "Existing");
	dataManager->addNode(n);
	simulateSave();
	dataManager->clearCache();

	Node reloaded = dataManager->getNode(n.getId());
	ASSERT_TRUE(reloaded.isActive());

	dataManager->deleteNode(reloaded);

	auto dirty = dataManager->getDirtyInfo<Node>(n.getId());
	ASSERT_TRUE(dirty.has_value());
	EXPECT_EQ(dirty->changeType, EntityChangeType::CHANGE_DELETED);
}

TEST_F(DataManagerCoverageTest, GetNodePropertiesDirect_NoProperties) {
	Node n = createTestNode(dataManager, "NoProp");
	dataManager->addNode(n);
	simulateSave();

	auto props = dataManager->getNodePropertiesDirect(n);
	EXPECT_TRUE(props.empty());
}

TEST_F(DataManagerCoverageTest, GetNodePropertiesDirect_InactiveNode) {
	Node n;
	n.setId(0);
	auto props = dataManager->getNodePropertiesDirect(n);
	EXPECT_TRUE(props.empty());
}

TEST_F(DataManagerCoverageTest, GetNodePropertiesFromMap_InactiveNode) {
	Node n;
	n.setId(0);
	std::unordered_map<int64_t, Property> propMap;
	auto props = dataManager->getNodePropertiesFromMap(n, propMap);
	EXPECT_TRUE(props.empty());
}

TEST_F(DataManagerCoverageTest, ResolveTokenName_ZeroId) {
	EXPECT_EQ(dataManager->resolveTokenName(0), "");
}

TEST_F(DataManagerCoverageTest, ResolveTokenId_EmptyName) {
	EXPECT_EQ(dataManager->resolveTokenId(""), 0);
}

TEST_F(DataManagerCoverageTest, GetOrCreateTokenId_EmptyName) {
	EXPECT_EQ(dataManager->getOrCreateTokenId(""), 0);
}

TEST_F(DataManagerCoverageTest, CheckAndTriggerAutoFlush_DuringTransaction) {
	dataManager->setActiveTransaction(1);
	EXPECT_NO_THROW(dataManager->checkAndTriggerAutoFlush());
	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerCoverageTest, UpdateEntityNode) {
	Node n = createTestNode(dataManager, "Upd");
	dataManager->addNode(n);
	simulateSave();

	n.addLabelId(dataManager->getOrCreateTokenId("NewLabel"));
	dataManager->updateEntity<Node>(n);

	Node reloaded = dataManager->getEntity<Node>(n.getId());
	EXPECT_TRUE(reloaded.isActive());
}

TEST_F(DataManagerCoverageTest, UpdateEntityEdge) {
	Node n1 = createTestNode(dataManager, "U1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "U2");
	dataManager->addNode(n2);
	Edge e = createTestEdge(dataManager, n1.getId(), n2.getId(), "REL");
	dataManager->addEdge(e);
	simulateSave();

	e.setTypeId(dataManager->getOrCreateTokenId("NEWREL"));
	dataManager->updateEntity<Edge>(e);

	Edge reloaded = dataManager->getEntity<Edge>(e.getId());
	EXPECT_TRUE(reloaded.isActive());
}

TEST_F(DataManagerCoverageTest, GetEntitiesInRange_StartGtEnd) {
	auto result = dataManager->getEntitiesInRange<Node>(100, 1, 10);
	EXPECT_TRUE(result.empty());
}

TEST_F(DataManagerCoverageTest, GetEntitiesInRange_ZeroLimit) {
	auto result = dataManager->getEntitiesInRange<Node>(1, 100, 0);
	EXPECT_TRUE(result.empty());
}

TEST_F(DataManagerCoverageTest, GetEntitiesInRange_LimitReachedFromMemory) {
	// Create many dirty entities so the memory pass fills the limit
	Node n1 = createTestNode(dataManager, "Limit1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "Limit2");
	dataManager->addNode(n2);

	// Request with limit 1 - should return after first dirty entity
	auto result = dataManager->getEntitiesInRange<Node>(n1.getId(), n2.getId(), 1);
	EXPECT_EQ(result.size(), 1u);
}
