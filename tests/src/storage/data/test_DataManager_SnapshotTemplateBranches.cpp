/**
 * @file test_DataManager_SnapshotTemplateBranches.cpp
 * @brief Template-branch coverage tests for DataManager snapshot/disk read paths.
 */

#include "DataManagerTestFixture.hpp"

#include "graph/storage/CommittedSnapshot.hpp"

TEST_F(DataManagerTest, SnapshotFallbackToDiskCoversAllEntityTypeTemplates) {
	auto n1 = createTestNode(dataManager, "SnapNode");
	auto n2 = createTestNode(dataManager, "SnapNode");
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	auto e = createTestEdge(dataManager, n1.getId(), n2.getId(), "SnapEdge");
	dataManager->addEdge(e);

	Property p = createTestProperty(n1.getId(), Node::typeId, {{"k", PropertyValue(int64_t(1))}});
	dataManager->addPropertyEntity(p);

	Blob b = createTestBlob("snapshot_blob");
	dataManager->addBlobEntity(b);

	Index idx = createTestIndex(Index::NodeType::LEAF, 1);
	dataManager->addIndexEntity(idx);

	State s = createTestState("snapshot.state");
	dataManager->addStateEntity(s);

	simulateSave();
	dataManager->clearCache();

	CommittedSnapshot snapshot;

	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Node>(n1.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Edge>(e.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Property>(p.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Blob>(b.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Index>(idx.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<State>(s.getId(), &snapshot).isActive());
}

TEST_F(DataManagerTest, SnapshotMapDeletedAndBackupPathsCoverAllEntityTypes) {
	auto n1 = createTestNode(dataManager, "SnapMapNode");
	auto n2 = createTestNode(dataManager, "SnapMapNode");
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	auto e = createTestEdge(dataManager, n1.getId(), n2.getId(), "SnapMapEdge");
	dataManager->addEdge(e);

	Property p = createTestProperty(n1.getId(), Node::typeId, {{"k", PropertyValue(int64_t(2))}});
	dataManager->addPropertyEntity(p);

	Blob b = createTestBlob("snapshot_map_blob");
	dataManager->addBlobEntity(b);

	Index idx = createTestIndex(Index::NodeType::LEAF, 2);
	dataManager->addIndexEntity(idx);

	State s = createTestState("snapshot.map.state");
	dataManager->addStateEntity(s);

	simulateSave();

	CommittedSnapshot snapshot;
	snapshot.nodes[n1.getId()] = DirtyEntityInfo<Node>(EntityChangeType::CHANGE_DELETED);
	snapshot.edges[e.getId()] = DirtyEntityInfo<Edge>(EntityChangeType::CHANGE_DELETED);
	snapshot.properties[p.getId()] = DirtyEntityInfo<Property>(EntityChangeType::CHANGE_DELETED);
	snapshot.blobs[b.getId()] = DirtyEntityInfo<Blob>(EntityChangeType::CHANGE_DELETED);
	snapshot.indexes[idx.getId()] = DirtyEntityInfo<Index>(EntityChangeType::CHANGE_DELETED);
	snapshot.states[s.getId()] = DirtyEntityInfo<State>(EntityChangeType::CHANGE_DELETED);

	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Node>(n1.getId(), &snapshot).isActive());
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Edge>(e.getId(), &snapshot).isActive());
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Property>(p.getId(), &snapshot).isActive());
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Blob>(b.getId(), &snapshot).isActive());
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Index>(idx.getId(), &snapshot).isActive());
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<State>(s.getId(), &snapshot).isActive());

	snapshot.nodes[n1.getId()] = DirtyEntityInfo<Node>(EntityChangeType::CHANGE_MODIFIED, dataManager->getNode(n1.getId()));
	snapshot.edges[e.getId()] = DirtyEntityInfo<Edge>(EntityChangeType::CHANGE_MODIFIED, dataManager->getEdge(e.getId()));
	snapshot.properties[p.getId()] = DirtyEntityInfo<Property>(EntityChangeType::CHANGE_MODIFIED, dataManager->getProperty(p.getId()));
	snapshot.blobs[b.getId()] = DirtyEntityInfo<Blob>(EntityChangeType::CHANGE_MODIFIED, dataManager->getBlob(b.getId()));
	snapshot.indexes[idx.getId()] = DirtyEntityInfo<Index>(EntityChangeType::CHANGE_MODIFIED, dataManager->getIndex(idx.getId()));
	snapshot.states[s.getId()] = DirtyEntityInfo<State>(EntityChangeType::CHANGE_MODIFIED, dataManager->getState(s.getId()));

	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Node>(n1.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Edge>(e.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Property>(p.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Blob>(b.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Index>(idx.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<State>(s.getId(), &snapshot).isActive());
}

TEST_F(DataManagerTest, MemoryOrDiskReadActivePathsCoverAllEntityTemplates) {
	auto n1 = createTestNode(dataManager, "MemDiskNode");
	auto n2 = createTestNode(dataManager, "MemDiskNode");
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	auto e = createTestEdge(dataManager, n1.getId(), n2.getId(), "MemDiskEdge");
	dataManager->addEdge(e);

	Property p = createTestProperty(n1.getId(), Node::typeId, {{"k", PropertyValue(int64_t(3))}});
	dataManager->addPropertyEntity(p);

	Blob b = createTestBlob("mem_disk_blob");
	dataManager->addBlobEntity(b);

	Index idx = createTestIndex(Index::NodeType::LEAF, 3);
	dataManager->addIndexEntity(idx);

	State s = createTestState("mem.disk.state");
	dataManager->addStateEntity(s);

	simulateSave();
	dataManager->clearCache();

	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<Node>(n1.getId()).isActive());
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<Edge>(e.getId()).isActive());
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<Property>(p.getId()).isActive());
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<Blob>(b.getId()).isActive());
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<Index>(idx.getId()).isActive());
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<State>(s.getId()).isActive());
}

TEST_F(DataManagerTest, SnapshotReadsUsePagePoolHitPathAcrossAllEntityTypes) {
	auto n1 = createTestNode(dataManager, "SnapPoolNode");
	auto n2 = createTestNode(dataManager, "SnapPoolNode");
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	auto e = createTestEdge(dataManager, n1.getId(), n2.getId(), "SnapPoolEdge");
	dataManager->addEdge(e);

	Property p = createTestProperty(n1.getId(), Node::typeId, {{"k", PropertyValue(int64_t(11))}});
	dataManager->addPropertyEntity(p);

	Blob b = createTestBlob("snap_pool_blob");
	dataManager->addBlobEntity(b);

	Index idx = createTestIndex(Index::NodeType::LEAF, 7);
	dataManager->addIndexEntity(idx);

	State s = createTestState("snap.pool.state");
	dataManager->addStateEntity(s);

	simulateSave();
	dataManager->clearCache();

	// Prime page pool for each entity type through the lock-free path.
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<Node>(n1.getId()).isActive());
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<Edge>(e.getId()).isActive());
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<Property>(p.getId()).isActive());
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<Blob>(b.getId()).isActive());
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<Index>(idx.getId()).isActive());
	EXPECT_TRUE(dataManager->getEntityFromMemoryOrDisk<State>(s.getId()).isActive());

	CommittedSnapshot snapshot;
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Node>(n1.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Edge>(e.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Property>(p.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Blob>(b.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<Index>(idx.getId(), &snapshot).isActive());
	EXPECT_TRUE(dataManager->getEntityWithSnapshot<State>(s.getId(), &snapshot).isActive());
}

TEST_F(DataManagerTest, SnapshotReadsFallbackForMissingIdsAcrossAllEntityTypes) {
	CommittedSnapshot snapshot;
	constexpr int64_t kMissing = 9'999'991;

	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Node>(kMissing, &snapshot).isActive());
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Edge>(kMissing, &snapshot).isActive());
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Property>(kMissing, &snapshot).isActive());
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Blob>(kMissing, &snapshot).isActive());
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<Index>(kMissing, &snapshot).isActive());
	EXPECT_FALSE(dataManager->getEntityWithSnapshot<State>(kMissing, &snapshot).isActive());
}
