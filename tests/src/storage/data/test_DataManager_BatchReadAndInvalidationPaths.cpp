/**
 * @file test_DataManager_BatchReadAndInvalidationPaths.cpp
 * @brief DataManager batch update, read-through, adjacency, and invalidation behavior tests.
 *
 * These tests exercise public storage behavior that is easy to regress when the
 * DataManager read/write paths are optimized.
 **/

#include "DataManagerTestFixture.hpp"
#include <array>

#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/storage/PersistenceManager.hpp"
#include "graph/storage/StorageHeaders.hpp"

TEST_F(DataManagerTest, BatchUpdatesHandleEmptyInputsAndNotifyObservers) {
	std::vector<Node> emptyNodes;
	std::vector<Edge> emptyEdges;
	EXPECT_NO_THROW(dataManager->updateNodes(emptyNodes));
	EXPECT_NO_THROW(dataManager->updateEdges(emptyEdges));
	EXPECT_NO_THROW(dataManager->updateNodesWithBeforeImages(emptyNodes, emptyNodes));
	EXPECT_NO_THROW(dataManager->updateEdgesWithBeforeImages(emptyEdges, emptyEdges));

	auto first = createTestNode(dataManager, "BatchUpdateA");
	auto second = createTestNode(dataManager, "BatchUpdateB");
	dataManager->addNode(first);
	dataManager->addNode(second);
	auto edge = createTestEdge(dataManager, first.getId(), second.getId(), "BATCH_REL");
	dataManager->addEdge(edge);
	observer->reset();

	auto updatedFirst = first;
	updatedFirst.addLabelId(dataManager->getOrCreateTokenId("UpdatedA"));
	auto updatedSecond = second;
	updatedSecond.addLabelId(dataManager->getOrCreateTokenId("UpdatedB"));
	std::vector<Node> updatedNodes{updatedFirst, updatedSecond};
	dataManager->updateNodes(updatedNodes);

	auto updatedEdge = edge;
	updatedEdge.setTypeId(dataManager->getOrCreateTokenId("BATCH_REL_UPDATED"));
	std::vector<Edge> updatedEdges{updatedEdge};
	dataManager->updateEdges(updatedEdges);

	EXPECT_EQ(observer->updatedNodes.size(), 2u);
	EXPECT_EQ(observer->updatedEdges.size(), 1u);
	EXPECT_TRUE(dataManager->getNode(first.getId()).hasLabelId(updatedFirst.getLabelId()));
	EXPECT_EQ(dataManager->getEdge(edge.getId()).getTypeId(), updatedEdge.getTypeId());
}

TEST_F(DataManagerTest, NodeBatchReadsHandleEmptyDirtyDeletedAndMissingRows) {
	EXPECT_TRUE(dataManager->getNodeBatch({}).empty());

	auto kept = createTestNode(dataManager, "BatchReadKept");
	auto removed = createTestNode(dataManager, "BatchReadRemoved");
	dataManager->addNode(kept);
	dataManager->addNode(removed);

	auto updated = kept;
	const auto updatedLabel = dataManager->getOrCreateTokenId("BatchReadUpdated");
	updated.addLabelId(updatedLabel);
	dataManager->updateNode(updated);
	dataManager->deleteNode(removed);

	const auto rows = dataManager->getNodeBatch({kept.getId(), removed.getId(), 999999});
	ASSERT_EQ(rows.size(), 1u);
	EXPECT_EQ(rows.front().getId(), kept.getId());
	EXPECT_TRUE(rows.front().hasLabelId(updatedLabel));
}

TEST_F(DataManagerTest, NodeBatchReadsSkipPersistedInactiveRows) {
	auto active = createTestNode(dataManager, "BatchReadActiveOnDisk");
	auto removed = createTestNode(dataManager, "BatchReadInactiveOnDisk");
	dataManager->addNode(active);
	dataManager->addNode(removed);
	simulateSave();

	dataManager->deleteNode(removed);
	simulateSave();
	dataManager->clearCache();

	const auto rows = dataManager->getNodeBatch({active.getId(), removed.getId()});
	ASSERT_EQ(rows.size(), 1u);
	EXPECT_EQ(rows.front().getId(), active.getId());
}

TEST_F(DataManagerTest, TransactionWalFlushWithoutActiveTransactionIsNoOp) {
	EXPECT_NO_THROW(dataManager->flushTransactionWALRecords());
}

TEST_F(DataManagerTest, PreadSegmentsHandlesGuardsBypassAndReadThroughCache) {
	std::array<uint8_t, TOTAL_SEGMENT_SIZE> buffer{};
	EXPECT_EQ(dataManager->preadSegments(nullptr, 1, 0), -1);
	EXPECT_EQ(dataManager->preadSegments(buffer.data(), 0, 0), 0);

	auto node = createTestNode(dataManager, "PreadSegmentNode");
	dataManager->addNode(node);
	simulateSave();
	const uint64_t nodeSegment = dataManager->getFileHeaderRef().node_segment_head;
	ASSERT_NE(nodeSegment, 0u);

	EXPECT_GE(dataManager->preadSegments(buffer.data(), 1, nodeSegment, SegmentReadCachePolicy::SRCP_BYPASS), 0);
	EXPECT_EQ(dataManager->preadSegments(buffer.data(), 1, nodeSegment, SegmentReadCachePolicy::SRCP_READ_THROUGH),
			  static_cast<ssize_t>(TOTAL_SEGMENT_SIZE));
	EXPECT_EQ(dataManager->preadSegments(buffer.data(), 1, nodeSegment, SegmentReadCachePolicy::SRCP_READ_THROUGH),
			  static_cast<ssize_t>(TOTAL_SEGMENT_SIZE));
}

TEST_F(DataManagerTest, PreadSegmentsReadThroughWorksWithZeroPageCache) {
	const auto zeroCachePath = testFilePath.string() + ".zero_cache";
	{
		graph::Database zeroCacheDb(
				zeroCachePath,
				storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE,
				0);
		zeroCacheDb.open();
		auto zeroCacheStorage = zeroCacheDb.getStorage();
		auto zeroCacheDataManager = zeroCacheStorage->getDataManager();
		ASSERT_EQ(zeroCacheDataManager->getPagePool().capacity(), 0U);

		auto node = createTestNode(zeroCacheDataManager, "ZeroCachePreadNode");
		zeroCacheDataManager->addNode(node);
		zeroCacheStorage->flush();
		const uint64_t nodeSegment = zeroCacheDataManager->getFileHeaderRef().node_segment_head;
		ASSERT_NE(nodeSegment, 0U);

		std::array<uint8_t, TOTAL_SEGMENT_SIZE> buffer{};
		EXPECT_EQ(zeroCacheDataManager->preadSegments(
						  buffer.data(), 1, nodeSegment, SegmentReadCachePolicy::SRCP_READ_THROUGH),
				  static_cast<ssize_t>(TOTAL_SEGMENT_SIZE));
		EXPECT_EQ(zeroCacheDataManager->preadSegments(
						  buffer.data(), 1, nodeSegment, SegmentReadCachePolicy::SRCP_READ_THROUGH),
				  static_cast<ssize_t>(TOTAL_SEGMENT_SIZE));
		zeroCacheDb.close();
	}
	std::error_code ec;
	std::filesystem::remove(zeroCachePath, ec);
}

TEST_F(DataManagerTest, EdgeTraversalDirectionsAndVisitorsUsePublicAdjacencyPaths) {
	auto source = createTestNode(dataManager, "TraversalSource");
	auto target = createTestNode(dataManager, "TraversalTarget");
	dataManager->addNode(source);
	dataManager->addNode(target);
	auto edge = createTestEdge(dataManager, source.getId(), target.getId(), "TRAVERSES");
	dataManager->addEdge(edge);

	const auto incoming = dataManager->findEdgesByNode(target.getId(), "in");
	ASSERT_FALSE(incoming.empty());
	EXPECT_EQ(incoming.front().getTargetNodeId(), target.getId());

	size_t incomingCallbacks = 0;
	const size_t incomingVisited = dataManager->visitEdgesByNode(
			target.getId(),
			[&](const Edge &visited) {
				EXPECT_EQ(visited.getTargetNodeId(), target.getId());
				++incomingCallbacks;
				return true;
			},
			"in");
	EXPECT_EQ(incomingVisited, incomingCallbacks);
	EXPECT_GT(incomingVisited, 0u);

	size_t connectedCallbacks = 0;
	EXPECT_GT(dataManager->visitEdgesByNode(
					  source.getId(),
					  [&](const Edge &) {
						  ++connectedCallbacks;
						  return true;
					  },
					  "both"),
			  0u);
	EXPECT_GT(connectedCallbacks, 0u);
	EXPECT_EQ(dataManager->visitEdgesByNode(source.getId(), {}, "both"), 0u);
}

TEST_F(DataManagerTest, InvalidateSegmentsAcceptsSnapshotsViewsAndExplicitOffsets) {
	auto node = createTestNode(dataManager, "InvalidationNode");
	dataManager->addNode(node);
	simulateSave();
	const uint64_t nodeSegment = dataManager->getFileHeaderRef().node_segment_head;
	ASSERT_NE(nodeSegment, 0u);

	auto updated = node;
	updated.addLabelId(dataManager->getOrCreateTokenId("Invalidated"));
	dataManager->updateNode(updated);

	const auto snapshot = dataManager->prepareFlushSnapshot();
	EXPECT_NO_THROW(dataManager->invalidateDirtySegments(snapshot));

	FlushSnapshotView emptyView;
	EXPECT_TRUE(emptyView.isEmpty());
	EXPECT_NO_THROW(dataManager->invalidateDirtySegments(emptyView));

	const auto view = dataManager->prepareFlushSnapshotView();
	EXPECT_NO_THROW(dataManager->invalidateDirtySegments(view));

	const std::array<uint64_t, 2> explicitSegments{0, nodeSegment};
	EXPECT_NO_THROW(dataManager->invalidateSegments(explicitSegments));
}

TEST_F(DataManagerTest, EdgeForAdjacencyHandlesInvalidDirtySnapshotAndDiskRows) {
	EXPECT_FALSE(dataManager->getEdgeForAdjacency(0).isActive());

	auto source = createTestNode(dataManager, "AdjacencySource");
	auto target = createTestNode(dataManager, "AdjacencyTarget");
	dataManager->addNode(source);
	dataManager->addNode(target);
	auto edge = createTestEdge(dataManager, source.getId(), target.getId(), "ADJ");
	dataManager->addEdge(edge);

	auto dirtyEdge = edge;
	dirtyEdge.setTypeId(dataManager->getOrCreateTokenId("ADJ_DIRTY"));
	dataManager->updateEdge(dirtyEdge);
	EXPECT_EQ(dataManager->getEdgeForAdjacency(edge.getId()).getTypeId(), dirtyEdge.getTypeId());

	CommittedSnapshot snapshot;
	auto snapshotEdge = edge;
	snapshotEdge.setTypeId(dataManager->getOrCreateTokenId("ADJ_SNAPSHOT"));
	snapshot.edges[edge.getId()] = DirtyEntityInfo<Edge>(EntityChangeType::CHANGE_MODIFIED, snapshotEdge);
	dataManager->setCurrentSnapshot(&snapshot);
	EXPECT_EQ(dataManager->getEdgeForAdjacency(edge.getId()).getTypeId(), snapshotEdge.getTypeId());
	dataManager->clearCurrentSnapshot();

	simulateSave();
	dataManager->clearCache();
	const auto diskEdge = dataManager->getEdgeForAdjacency(edge.getId());
	EXPECT_TRUE(diskEdge.isActive());
	EXPECT_EQ(diskEdge.getId(), edge.getId());
	EXPECT_FALSE(dataManager->getEdgeForAdjacency(123456789).isActive());
}

TEST_F(DataManagerTest, SnapshotNodeReadsIgnoreEntriesWithoutBeforeImages) {
	auto node = createTestNode(dataManager, "SnapshotFallbackNode");
	dataManager->addNode(node);
	simulateSave();
	dataManager->clearCache();

	CommittedSnapshot snapshot;
	snapshot.nodes[node.getId()] = DirtyEntityInfo<Node>(EntityChangeType::CHANGE_MODIFIED);
	dataManager->setCurrentSnapshot(&snapshot);

	const auto loaded = dataManager->getNode(node.getId());
	EXPECT_TRUE(loaded.isActive());
	EXPECT_EQ(loaded.getId(), node.getId());
	dataManager->clearCurrentSnapshot();

	CommittedSnapshot deletedSnapshot;
	deletedSnapshot.nodes[node.getId()] = DirtyEntityInfo<Node>(EntityChangeType::CHANGE_DELETED);
	dataManager->setCurrentSnapshot(&deletedSnapshot);
	EXPECT_FALSE(dataManager->getNode(node.getId()).isActive());
	dataManager->clearCurrentSnapshot();
}
