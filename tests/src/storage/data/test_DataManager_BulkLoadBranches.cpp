/**
 * @file test_DataManager_BulkLoadBranches.cpp
 * @brief Branch-focused tests for DataManager::bulkLoadEntities.
 */

#include "DataManagerTestFixture.hpp"

#include <unordered_set>

TEST_F(DataManagerTest, BulkLoadEntitiesCoversRangeSkipAndInactiveFiltering) {
	auto n1 = createTestNode(dataManager, "BulkBranchNode");
	auto n2 = createTestNode(dataManager, "BulkBranchNode");
	auto n3 = createTestNode(dataManager, "BulkBranchNode");
	dataManager->addNode(n1);
	dataManager->addNode(n2);
	dataManager->addNode(n3);

	auto e1 = createTestEdge(dataManager, n1.getId(), n3.getId(), "BulkBranchEdge");
	auto e2 = createTestEdge(dataManager, n3.getId(), n1.getId(), "BulkBranchEdge");
	auto e3 = createTestEdge(dataManager, n1.getId(), n3.getId(), "BulkBranchEdge");
	dataManager->addEdge(e1);
	dataManager->addEdge(e2);
	dataManager->addEdge(e3);

	Property p1 = createTestProperty(n1.getId(), Node::typeId, {{"k1", PropertyValue(int64_t(41))}});
	Property p2 = createTestProperty(n2.getId(), Node::typeId, {{"k2", PropertyValue(int64_t(42))}});
	Property p3 = createTestProperty(n3.getId(), Node::typeId, {{"k3", PropertyValue(int64_t(43))}});
	dataManager->addPropertyEntity(p1);
	dataManager->addPropertyEntity(p2);
	dataManager->addPropertyEntity(p3);

	simulateSave();

	// Mark one record per type as inactive on disk to hit active-filter branches.
	Node deleted = dataManager->getNode(n2.getId());
	dataManager->deleteNode(deleted);
	Edge deletedEdge = dataManager->getEdge(e2.getId());
	dataManager->deleteEdge(deletedEdge);
	Property deletedProp = dataManager->getProperty(p2.getId());
	dataManager->deleteProperty(deletedProp);
	simulateSave();
	dataManager->clearCache();

	const auto loadedNodes = dataManager->bulkLoadEntities<Node>(n1.getId(), n3.getId());
	std::unordered_set<int64_t> ids;
	for (const auto &node : loadedNodes) {
		ids.insert(node.getId());
	}

	EXPECT_TRUE(ids.contains(n1.getId()));
	EXPECT_TRUE(ids.contains(n3.getId()));
	EXPECT_FALSE(ids.contains(n2.getId()));

	// Filter window before any segment start ID: triggers seg.startId > filterEnd.
	const auto preNode = dataManager->bulkLoadEntities<Node>(0, 0);
	const auto preEdge = dataManager->bulkLoadEntities<Edge>(0, 0);
	const auto preProp = dataManager->bulkLoadEntities<Property>(0, 0);
	EXPECT_TRUE(preNode.empty());
	EXPECT_TRUE(preEdge.empty());
	EXPECT_TRUE(preProp.empty());

	// Narrow windows force per-entity range checks (<start, in-range, >end).
	const auto deletedEdgeLoad = dataManager->bulkLoadEntities<Edge>(e2.getId(), e2.getId());
	const auto deletedPropLoad = dataManager->bulkLoadEntities<Property>(p2.getId(), p2.getId());
	EXPECT_TRUE(deletedEdgeLoad.empty());
	EXPECT_TRUE(deletedPropLoad.empty());

	const auto edgeRange = dataManager->bulkLoadEntities<Edge>(e1.getId(), e3.getId());
	const auto propRange = dataManager->bulkLoadEntities<Property>(p1.getId(), p3.getId());
	std::unordered_set<int64_t> edgeIds;
	for (const auto &edge : edgeRange) {
		edgeIds.insert(edge.getId());
	}
	std::unordered_set<int64_t> propIds;
	for (const auto &prop : propRange) {
		propIds.insert(prop.getId());
	}

	EXPECT_TRUE(edgeIds.contains(e1.getId()));
	EXPECT_TRUE(edgeIds.contains(e3.getId()));
	EXPECT_FALSE(edgeIds.contains(e2.getId()));

	EXPECT_TRUE(propIds.contains(p1.getId()));
	EXPECT_TRUE(propIds.contains(p3.getId()));
	EXPECT_FALSE(propIds.contains(p2.getId()));

	// Range completely outside existing IDs -> segment-range skip branch.
	const auto noNode = dataManager->bulkLoadEntities<Node>(n3.getId() + 1000, n3.getId() + 2000);
	const auto noEdge = dataManager->bulkLoadEntities<Edge>(e3.getId() + 1000, e3.getId() + 2000);
	const auto noProp = dataManager->bulkLoadEntities<Property>(p3.getId() + 1000, p3.getId() + 2000);

	EXPECT_TRUE(noNode.empty());
	EXPECT_TRUE(noEdge.empty());
	EXPECT_TRUE(noProp.empty());
}

TEST_F(DataManagerTest, LoadNodeFromDiskReturnsInactiveWhenEntityReadIsShort) {
	auto n1 = createTestNode(dataManager, "BulkShortReadNode");
	dataManager->addNode(n1);
	simulateSave();
	dataManager->clearCache();

	ASSERT_TRUE(std::filesystem::exists(testFilePath));
	std::filesystem::resize_file(testFilePath, 1);

	const Node loaded = dataManager->loadNodeFromDisk(n1.getId());
	EXPECT_FALSE(loaded.isActive());
}
