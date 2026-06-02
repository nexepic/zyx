#include "DataManagerTestFixture.hpp"

#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/StorageHeaders.hpp"

#include <algorithm>
#include <vector>

namespace {

Edge addEdgeOfType(const std::shared_ptr<DataManager> &dm, int64_t source, int64_t target, int64_t typeId) {
	Edge edge(0, source, target, typeId);
	dm->addEdge(edge);
	return edge;
}

} // namespace

TEST_F(DataManagerTest, RelationshipSegmentStatsRejectUnsafeCandidateScans) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	Edge edge = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	dataManager->addEdgeProperties(edge.getId(), {{"weight", PropertyValue(int64_t{1})}});

	EXPECT_FALSE(dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 128, followsType).has_value());
	simulateSave();
	dataManager->clearCache();

	EXPECT_FALSE(dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(0, 128, followsType).has_value());
	EXPECT_FALSE(dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(10, 1, followsType).has_value());

	storage::CommittedSnapshot propertySnapshot;
	Property property;
	property.setId(1);
	propertySnapshot.properties.emplace(
			property.getId(), storage::DirtyEntityInfo<Property>(storage::EntityChangeType::CHANGE_MODIFIED, property));
	dataManager->setCurrentSnapshot(&propertySnapshot);
	EXPECT_FALSE(dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 128, followsType).has_value());

	storage::CommittedSnapshot blobSnapshot;
	Blob blob;
	blob.setId(1);
	blobSnapshot.blobs.emplace(blob.getId(), storage::DirtyEntityInfo<Blob>(storage::EntityChangeType::CHANGE_MODIFIED, blob));
	dataManager->setCurrentSnapshot(&blobSnapshot);
	EXPECT_FALSE(dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 128, followsType).has_value());
	dataManager->clearCurrentSnapshot();

	auto candidates = dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 128, followsType);
	ASSERT_TRUE(candidates.has_value());
	EXPECT_EQ(candidates->matchedEdges, 1U);
	EXPECT_EQ(candidates->propertyEntityIds.size(), 1U);
	EXPECT_TRUE(candidates->fallbackEdgeIds.empty());
}

TEST_F(DataManagerTest, RelationshipSegmentStatsOverlayHandlesModifiedTypesAndInactiveEdges) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	const int64_t likesType = dataManager->getOrCreateTokenId("LIKES");
	std::vector<int64_t> followsIds;
	for (int i = 0; i < 130; ++i) {
		Edge edge = addEdgeOfType(dataManager, source.getId(), target.getId(), i % 2 == 0 ? followsType : likesType);
		if (edge.getTypeId() == followsType) {
			followsIds.push_back(edge.getId());
		}
	}
	ASSERT_GE(followsIds.size(), 2U);
	simulateSave();
	dataManager->clearCache();

	auto baseFollows = dataManager->countActiveEdgesByTypeFromSegmentStats(1, 130, followsType);
	ASSERT_TRUE(baseFollows.has_value());
	EXPECT_EQ(*baseFollows, 65);
	auto baseLikes = dataManager->countActiveEdgesByTypeFromSegmentStats(1, 130, likesType);
	ASSERT_TRUE(baseLikes.has_value());
	EXPECT_EQ(*baseLikes, 65);

	Edge changedType = dataManager->getEdge(followsIds.front());
	changedType.setTypeId(likesType);
	dataManager->updateEdge(changedType);
	auto followsAfterTypeChange = dataManager->countActiveEdgesByTypeFromSegmentStats(1, 130, followsType);
	ASSERT_TRUE(followsAfterTypeChange.has_value());
	EXPECT_EQ(*followsAfterTypeChange, 64);
	auto likesAfterTypeChange = dataManager->countActiveEdgesByTypeFromSegmentStats(1, 130, likesType);
	ASSERT_TRUE(likesAfterTypeChange.has_value());
	EXPECT_EQ(*likesAfterTypeChange, 66);
	simulateSave();

	Edge inactive = dataManager->getEdge(followsIds[1]);
	dataManager->deleteEdge(inactive);
	auto followsAfterDelete = dataManager->countActiveEdgesByTypeFromSegmentStats(1, 130, followsType);
	ASSERT_TRUE(followsAfterDelete.has_value());
	EXPECT_EQ(*followsAfterDelete, 63);
	auto allAfterDelete = dataManager->countActiveEdgesByTypeFromSegmentStats(1, 130, 0);
	ASSERT_TRUE(allAfterDelete.has_value());
	EXPECT_EQ(*allAfterDelete, 129);
}

TEST_F(DataManagerTest, RelationshipSegmentStatsHandleEmptyAndOutOfRangeCachedLookups) {
	EXPECT_FALSE(dataManager->cachedRelationshipTypeSegmentStats(0).has_value());
	auto emptyCount = dataManager->countActiveEdgesByTypeFromSegmentStats(1, 128, 0);
	ASSERT_TRUE(emptyCount.has_value());
	EXPECT_EQ(*emptyCount, 0);

	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	for (int i = 0; i < 128; ++i) {
		addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	}
	simulateSave();

	const auto &segments = dataManager->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(segments.empty());
	auto count = dataManager->countActiveEdgesByTypeFromSegmentStats(1, 128, followsType);
	ASSERT_TRUE(count.has_value());
	EXPECT_EQ(*count, 128);
	EXPECT_TRUE(dataManager->cachedRelationshipTypeSegmentStats(segments.front().segmentOffset).has_value());

	auto patched = segments;
	patched.front().segmentOffset += static_cast<uint64_t>(TOTAL_SEGMENT_SIZE) * 10'000ULL;
	dataManager->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, patched);
	dataManager->clearRelationshipSegmentTypeStats();
	EXPECT_FALSE(dataManager->countActiveEdgesByTypeFromSegmentStats(1, 128, followsType).has_value());
}

TEST_F(DataManagerTest, RelationshipSegmentStatsScansMixedPartialAndFullSegments) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	const int64_t likesType = dataManager->getOrCreateTokenId("LIKES");
	std::vector<int64_t> followsIds;
	for (int i = 0; i < 300; ++i) {
		Edge edge = addEdgeOfType(dataManager, source.getId(), target.getId(), i % 2 == 0 ? followsType : likesType);
		if (i == 0) {
			dataManager->deleteEdge(edge);
		}
		if (edge.getTypeId() == followsType) {
			followsIds.push_back(edge.getId());
		}
	}
	simulateSave();
	dataManager->clearRelationshipSegmentTypeStats();

	auto follows = dataManager->countActiveEdgesByTypeFromSegmentStats(2, 260, followsType);
	ASSERT_TRUE(follows.has_value());
	EXPECT_EQ(*follows, 130);
	auto allTypes = dataManager->countActiveEdgesByTypeFromSegmentStats(2, 260, 0);
	ASSERT_TRUE(allTypes.has_value());
	EXPECT_EQ(*allTypes, 259);

	storage::CommittedSnapshot snapshot;
	Edge deletedInSnapshot = dataManager->getEdge(followsIds[1]);
	deletedInSnapshot.markInactive();
	snapshot.edges.emplace(deletedInSnapshot.getId(),
	                       storage::DirtyEntityInfo<Edge>(storage::EntityChangeType::CHANGE_DELETED,
	                                                      deletedInSnapshot));
	Edge addedInSnapshot(301, source.getId(), target.getId(), followsType);
	snapshot.edges.emplace(addedInSnapshot.getId(),
	                       storage::DirtyEntityInfo<Edge>(storage::EntityChangeType::CHANGE_ADDED, addedInSnapshot));
	dataManager->setCurrentSnapshot(&snapshot);
	auto overlay = dataManager->countActiveEdgesByTypeFromSegmentStats(2, 301, followsType);
	ASSERT_TRUE(overlay.has_value());
	EXPECT_EQ(*overlay, 149);
	dataManager->clearCurrentSnapshot();
}

TEST_F(DataManagerTest, RelationshipSegmentStatsUsesFullMiddleSegmentsAndCandidateSkips) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	const int64_t likesType = dataManager->getOrCreateTokenId("LIKES");

	std::vector<int64_t> followsIds;
	for (int i = 0; i < static_cast<int>(EDGES_PER_SEGMENT * 3); ++i) {
		Edge edge = addEdgeOfType(dataManager, source.getId(), target.getId(), i % 3 == 0 ? likesType : followsType);
		if (edge.getTypeId() == followsType) {
			followsIds.push_back(edge.getId());
		}
		if (edge.getId() == static_cast<int64_t>(EDGES_PER_SEGMENT + 1) ||
			edge.getId() == static_cast<int64_t>(EDGES_PER_SEGMENT + 18)) {
			dataManager->addEdgeProperties(edge.getId(), {{"rank", PropertyValue(edge.getId())}});
		}
	}
	simulateSave();
	const auto &segments = dataManager->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_GE(segments.size(), 2U);
	bool inactive = false;
	std::fstream file(testFilePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file.is_open());
	const std::streamoff inactiveOffset =
			static_cast<std::streamoff>(segments[1].segmentOffset + sizeof(SegmentHeader) +
										2U * Edge::getTotalSize() + offsetof(Edge::Metadata, isActive));
	file.seekp(inactiveOffset);
	file.write(reinterpret_cast<const char *>(&inactive), sizeof(inactive));
	file.close();
	dataManager->clearCache();
	dataManager->clearRelationshipSegmentTypeStats();

	// A window spanning partial/full/partial segments exercises the segment-stats fast path
	// without relying on total-file stats.
	auto allTypes = dataManager->countActiveEdgesByTypeFromSegmentStats(2, EDGES_PER_SEGMENT * 2 + 7, 0);
	ASSERT_TRUE(allTypes.has_value());
	EXPECT_EQ(*allTypes, static_cast<int64_t>(EDGES_PER_SEGMENT * 2 + 5));

	auto follows = dataManager->countActiveEdgesByTypeFromSegmentStats(2, EDGES_PER_SEGMENT * 2 + 7, followsType);
	ASSERT_TRUE(follows.has_value());
	EXPECT_GT(*follows, 0);
	EXPECT_LT(*follows, *allTypes);

	auto candidates =
			dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(EDGES_PER_SEGMENT + 1,
																			  EDGES_PER_SEGMENT * 2, followsType);
	ASSERT_TRUE(candidates.has_value());
	EXPECT_GT(candidates->matchedEdges, candidates->propertyEntityIds.size());
	EXPECT_EQ(candidates->propertyEntityIds.size(), 2U);
	EXPECT_TRUE(candidates->fallbackEdgeIds.empty());
}

TEST_F(DataManagerTest, RelationshipSegmentStatsHandlesInactiveHeavyCandidateHeader) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	Edge edge = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	dataManager->addEdgeProperties(edge.getId(), {{"rank", PropertyValue(int64_t{1})}});
	simulateSave();

	const auto &segments = dataManager->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(segments.empty());
	const uint64_t offset = segments.front().segmentOffset;
	SegmentHeader header = dataManager->getSegmentTracker()->getSegmentHeaderCopy(offset);
	header.inactive_count = header.used;
	dataManager->getSegmentTracker()->writeSegmentHeader(offset, header);
	dataManager->clearRelationshipSegmentTypeStats();

	auto candidates = dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 1, followsType);
	ASSERT_TRUE(candidates.has_value());
	EXPECT_EQ(candidates->matchedEdges, 1U);
	EXPECT_EQ(candidates->propertyEntityIds.size(), 1U);
}

TEST_F(DataManagerTest, RelationshipSegmentStatsRejectsCorruptSegmentHeaders) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	simulateSave();

	const auto &segments = dataManager->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(segments.empty());
	const uint64_t offset = segments.front().segmentOffset;
	ASSERT_TRUE(dataManager->countActiveEdgesByTypeFromSegmentStats(1, 1, followsType).has_value());
	ASSERT_TRUE(dataManager->cachedRelationshipTypeSegmentStats(offset).has_value());

	SegmentHeader header = dataManager->getSegmentTracker()->getSegmentHeaderCopy(offset);
	header.data_type = Node::typeId;
	dataManager->getSegmentTracker()->writeSegmentHeader(offset, header);
	dataManager->clearRelationshipSegmentTypeStats();

	EXPECT_FALSE(dataManager->cachedRelationshipTypeSegmentStats(offset).has_value());
	EXPECT_FALSE(dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 1, followsType).has_value());
	EXPECT_FALSE(dataManager->countActiveEdgesByTypeFromSegmentStats(1, 1, followsType).has_value());
}

TEST_F(DataManagerTest, RelationshipSegmentStatsOverlayHandlesSparseSnapshotEntries) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	for (int i = 0; i < 8; ++i) {
		addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	}
	simulateSave();

	storage::CommittedSnapshot snapshot;
	snapshot.edges.emplace(2, storage::DirtyEntityInfo<Edge>(storage::EntityChangeType::CHANGE_MODIFIED));
	Edge outsideRange(8192, source.getId(), target.getId(), followsType);
	snapshot.edges.emplace(outsideRange.getId(), storage::DirtyEntityInfo<Edge>(storage::EntityChangeType::CHANGE_ADDED,
																			   outsideRange));
	Edge notPersisted(4096, source.getId(), target.getId(), followsType);
	snapshot.edges.emplace(notPersisted.getId(),
						   storage::DirtyEntityInfo<Edge>(storage::EntityChangeType::CHANGE_MODIFIED, notPersisted));
	dataManager->setCurrentSnapshot(&snapshot);

	auto count = dataManager->countActiveEdgesByTypeFromSegmentStats(1, 4096, followsType);
	ASSERT_TRUE(count.has_value());
	EXPECT_EQ(*count, 9);
	dataManager->clearCurrentSnapshot();
}

TEST_F(DataManagerTest, RelationshipSegmentStatsOverlayHandlesIndexSlotsOutsideHeader) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	for (int i = 0; i < 4; ++i) {
		addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	}
	simulateSave();

	const auto originalIndex = dataManager->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(originalIndex.empty());
	auto patchedIndex = originalIndex;
	patchedIndex.front().endId = 64;
	dataManager->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, patchedIndex);
	dataManager->clearRelationshipSegmentTypeStats();

	storage::CommittedSnapshot snapshot;
	Edge modifiedBeyondHeader(64, source.getId(), target.getId(), followsType);
	snapshot.edges.emplace(modifiedBeyondHeader.getId(),
						   storage::DirtyEntityInfo<Edge>(storage::EntityChangeType::CHANGE_MODIFIED,
														  modifiedBeyondHeader));
	dataManager->setCurrentSnapshot(&snapshot);

	auto count = dataManager->countActiveEdgesByTypeFromSegmentStats(1, 64, followsType);
	ASSERT_TRUE(count.has_value());
	EXPECT_EQ(*count, 5);
	dataManager->clearCurrentSnapshot();
	dataManager->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, originalIndex);
}

TEST_F(DataManagerTest, RelationshipSegmentStatsSkipsZeroUsedSegments) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	for (int i = 0; i < 4; ++i) {
		addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	}
	simulateSave();

	const auto &segments = dataManager->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(segments.empty());
	const uint64_t offset = segments.front().segmentOffset;
	SegmentHeader header = dataManager->getSegmentTracker()->getSegmentHeaderCopy(offset);
	header.used = 0;
	header.inactive_count = 0;
	dataManager->getSegmentTracker()->writeSegmentHeader(offset, header);
	dataManager->clearRelationshipSegmentTypeStats();

	auto totalWindow = dataManager->countActiveEdgesByTypeFromSegmentStats(1, 4, followsType);
	ASSERT_TRUE(totalWindow.has_value());
	EXPECT_EQ(*totalWindow, 0);
	auto partialWindow = dataManager->countActiveEdgesByTypeFromSegmentStats(2, 3, followsType);
	ASSERT_TRUE(partialWindow.has_value());
	EXPECT_EQ(*partialWindow, 0);
	auto candidates = dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 4, followsType);
	ASSERT_TRUE(candidates.has_value());
	EXPECT_EQ(candidates->matchedEdges, 0U);
}

TEST_F(DataManagerTest, RelationshipSegmentStatsOverlayRejectsDiskHeaderMismatch) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	Edge edge = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	simulateSave();

	const auto &segments = dataManager->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(segments.empty());
	const uint64_t offset = segments.front().segmentOffset;
	SegmentHeader diskHeader = dataManager->getSegmentTracker()->getSegmentHeaderCopy(offset);
	diskHeader.data_type = Node::typeId;
	std::fstream file(testFilePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file.is_open());
	file.seekp(static_cast<std::streamoff>(offset));
	file.write(reinterpret_cast<const char *>(&diskHeader), sizeof(SegmentHeader));
	file.close();

	storage::CommittedSnapshot snapshot;
	Edge modified = edge;
	modified.setTypeId(followsType);
	snapshot.edges.emplace(modified.getId(),
						   storage::DirtyEntityInfo<Edge>(storage::EntityChangeType::CHANGE_MODIFIED, modified));
	dataManager->setCurrentSnapshot(&snapshot);

	EXPECT_FALSE(dataManager->countActiveEdgesByTypeFromSegmentStats(1, 1, followsType).has_value());
	dataManager->clearCurrentSnapshot();
}

TEST_F(DataManagerTest, RelationshipSegmentStatsCoversDefensiveGuardsAndOverlayShapes) {
	std::vector<char> emptyReadBuffer(TOTAL_SEGMENT_SIZE);
	EXPECT_EQ(dataManager->preadSegments(nullptr, 1, 0), -1);
	EXPECT_EQ(dataManager->preadSegments(emptyReadBuffer.data(), 0, 0), 0);

	EXPECT_FALSE(dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(0, 1, 0).has_value());
	EXPECT_FALSE(dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(2, 1, 0).has_value());
	EXPECT_FALSE(dataManager->countActiveEdgesByTypeFromSegmentStats(0, 1, 0).has_value());

	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	const int64_t likesType = dataManager->getOrCreateTokenId("LIKES");
	Edge persisted = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	simulateSave();

	const auto &segments = dataManager->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(segments.empty());
	const uint64_t offset = segments.front().segmentOffset;
	const SegmentHeader header = dataManager->getSegmentTracker()->getSegmentHeaderCopy(offset);
	ASSERT_EQ(header.data_type, Edge::typeId);

	Edge outsideRange(1000, source.getId(), target.getId(), followsType);
	dataManager->addEdge(outsideRange);
	auto rangeCount = dataManager->countActiveEdgesByTypeFromSegmentStats(1, persisted.getId(), followsType);
	ASSERT_TRUE(rangeCount.has_value());
	EXPECT_EQ(*rangeCount, 1);

	Edge changedType = persisted;
	changedType.setTypeId(likesType);
	dataManager->updateEdge(changedType);
	auto followsAfterTypeChange = dataManager->countActiveEdgesByTypeFromSegmentStats(1, persisted.getId(), followsType);
	ASSERT_TRUE(followsAfterTypeChange.has_value());
	EXPECT_EQ(*followsAfterTypeChange, 0);
	auto likesAfterTypeChange = dataManager->countActiveEdgesByTypeFromSegmentStats(1, persisted.getId(), likesType);
	ASSERT_TRUE(likesAfterTypeChange.has_value());
	EXPECT_EQ(*likesAfterTypeChange, 1);

	dataManager->invalidateRelationshipSegmentTypeStats({});
}
