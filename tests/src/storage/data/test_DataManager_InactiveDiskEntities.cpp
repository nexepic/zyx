/**
 * @file test_DataManager_InactiveDiskEntities.cpp
 * @brief Covers DataManager inactive-entity disk/snapshot branches across entity types.
 */

#include "DataManagerTestFixture.hpp"

#include "graph/storage/CommittedSnapshot.hpp"

TEST_F(DataManagerTest, SnapshotAndMemoryReadsReturnInactiveForDeletedDiskEntitiesAllTypes) {
	auto n1 = createTestNode(dataManager, "InactiveNode");
	auto n2 = createTestNode(dataManager, "InactiveNode");
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	auto e = createTestEdge(dataManager, n1.getId(), n2.getId(), "InactiveEdge");
	dataManager->addEdge(e);

	Property p = createTestProperty(n1.getId(), Node::typeId, {{"k", PropertyValue(int64_t(9))}});
	dataManager->addPropertyEntity(p);

	Blob b = createTestBlob("inactive_blob");
	dataManager->addBlobEntity(b);

	Index idx = createTestIndex(Index::NodeType::LEAF, 5);
	dataManager->addIndexEntity(idx);

	State s = createTestState("inactive.state");
	dataManager->addStateEntity(s);

	simulateSave();

	Node dn = dataManager->getNode(n1.getId());
	dataManager->deleteNode(dn);

	Edge de = dataManager->getEdge(e.getId());
	dataManager->deleteEdge(de);

	Property dp = dataManager->getProperty(p.getId());
	dataManager->deleteProperty(dp);

	Blob db = dataManager->getBlob(b.getId());
	dataManager->deleteBlob(db);

	Index di = dataManager->getIndex(idx.getId());
	dataManager->deleteIndex(di);

	State ds = dataManager->getState(s.getId());
	dataManager->deleteState(ds);

	simulateSave();
	dataManager->clearCache();

	CommittedSnapshot snapshot;

	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Node>(n1.getId(), &snapshot).isActive());
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Edge>(e.getId(), &snapshot).isActive());
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Property>(p.getId(), &snapshot).isActive());
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Blob>(b.getId(), &snapshot).isActive());
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Index>(idx.getId(), &snapshot).isActive());
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<State>(s.getId(), &snapshot).isActive());

	EXPECT_FALSE(dataManager->getEntityFromMemoryOrDisk<Node>(n1.getId()).isActive());
	EXPECT_FALSE(dataManager->getEntityFromMemoryOrDisk<Edge>(e.getId()).isActive());
	EXPECT_FALSE(dataManager->getEntityFromMemoryOrDisk<Property>(p.getId()).isActive());
	EXPECT_FALSE(dataManager->getEntityFromMemoryOrDisk<Blob>(b.getId()).isActive());
	EXPECT_FALSE(dataManager->getEntityFromMemoryOrDisk<Index>(idx.getId()).isActive());
	EXPECT_FALSE(dataManager->getEntityFromMemoryOrDisk<State>(s.getId()).isActive());
}
