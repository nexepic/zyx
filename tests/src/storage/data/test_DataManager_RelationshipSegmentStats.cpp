#include "DataManagerTestFixture.hpp"

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/data/RelationshipSegmentStatsScanner.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

	constexpr const char *kRelationshipTypeTotalStatsStateKey = "rel.type.total.stats";

	Edge addEdgeOfType(const std::shared_ptr<DataManager> &dm, int64_t source, int64_t target, int64_t typeId) {
		Edge edge(0, source, target, typeId);
		dm->addEdge(edge);
		return edge;
	}

	std::shared_ptr<DataManager> makeNoPreadDataManager() {
		static FileHeader header{};
		IDAllocators allocators{};
		auto noPreadIO = std::make_shared<StorageIO>(nullptr, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto segmentTracker = std::make_shared<SegmentTracker>(noPreadIO, header);
		return std::make_shared<DataManager>(nullptr, 16, header, allocators, segmentTracker, noPreadIO);
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

	storage::CommittedSnapshot edgeSnapshot;
	edgeSnapshot.edges.emplace(
			edge.getId(), storage::DirtyEntityInfo<Edge>(storage::EntityChangeType::CHANGE_MODIFIED, edge));
	dataManager->setCurrentSnapshot(&edgeSnapshot);
	EXPECT_FALSE(dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 128, followsType).has_value());

	storage::CommittedSnapshot blobSnapshot;
	Blob blob;
	blob.setId(1);
	blobSnapshot.blobs.emplace(blob.getId(),
							   storage::DirtyEntityInfo<Blob>(storage::EntityChangeType::CHANGE_MODIFIED, blob));
	dataManager->setCurrentSnapshot(&blobSnapshot);
	EXPECT_FALSE(dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 128, followsType).has_value());
	dataManager->clearCurrentSnapshot();

	EXPECT_FALSE(
			dataManager->collectCachedRelationshipPropertyCandidatesFromSegmentStats(1, 128, followsType).has_value());
	auto candidates = dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 128, followsType);
	ASSERT_TRUE(candidates.has_value());
	EXPECT_EQ(candidates->matchedEdges, 1U);
	EXPECT_EQ(candidates->propertyEntityIds.size(), 1U);
	EXPECT_EQ(candidates->propertyEdgeIds.size(), candidates->propertyEntityIds.size());
	EXPECT_TRUE(candidates->fallbackEdgeIds.empty());
	auto cachedCandidates =
			dataManager->collectCachedRelationshipPropertyCandidatesFromSegmentStats(1, 128, followsType);
	ASSERT_TRUE(cachedCandidates.has_value());
	EXPECT_EQ(cachedCandidates->matchedEdges, candidates->matchedEdges);
	EXPECT_EQ(cachedCandidates->propertyEntityIds, candidates->propertyEntityIds);
	EXPECT_EQ(cachedCandidates->propertyEdgeIds, candidates->propertyEdgeIds);
	EXPECT_EQ(cachedCandidates->fallbackEdgeIds, candidates->fallbackEdgeIds);
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
	ASSERT_TRUE(dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 128, followsType).has_value());
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
	snapshot.edges.emplace(
			deletedInSnapshot.getId(),
			storage::DirtyEntityInfo<Edge>(storage::EntityChangeType::CHANGE_DELETED, deletedInSnapshot));
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
			static_cast<std::streamoff>(segments[1].segmentOffset + sizeof(SegmentHeader) + 2U * Edge::getTotalSize() +
										offsetof(Edge::Metadata, isActive));
	file.seekp(inactiveOffset);
	file.write(reinterpret_cast<const char *>(&inactive), sizeof(inactive));
	file.close();
	dataManager->clearCache();
	dataManager->clearRelationshipSegmentTypeStats();

	// A window spanning partial/full/partial segments exercises the segment-stats hot path
	// without relying on total-file stats.
	auto allTypes = dataManager->countActiveEdgesByTypeFromSegmentStats(2, EDGES_PER_SEGMENT * 2 + 7, 0);
	ASSERT_TRUE(allTypes.has_value());
	EXPECT_EQ(*allTypes, static_cast<int64_t>(EDGES_PER_SEGMENT * 2 + 5));

	auto follows = dataManager->countActiveEdgesByTypeFromSegmentStats(2, EDGES_PER_SEGMENT * 2 + 7, followsType);
	ASSERT_TRUE(follows.has_value());
	EXPECT_GT(*follows, 0);
	EXPECT_LT(*follows, *allTypes);

	auto candidates = dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(
			EDGES_PER_SEGMENT + 1, EDGES_PER_SEGMENT * 2, followsType);
	ASSERT_TRUE(candidates.has_value());
	EXPECT_GT(candidates->matchedEdges, candidates->propertyEntityIds.size());
	EXPECT_EQ(candidates->propertyEntityIds.size(), 2U);
	EXPECT_EQ(candidates->propertyEdgeIds.size(), candidates->propertyEntityIds.size());
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
	EXPECT_EQ(candidates->propertyEdgeIds.size(), 1U);
}

TEST_F(DataManagerTest, RelationshipSegmentStatsIgnoresNonPropertyCandidateStorage) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	Edge edge = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	edge.setPropertyEntityId(999, PropertyStorageType::NONE);
	dataManager->updateEdge(edge);
	simulateSave();

	auto candidates = dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 1, followsType);
	ASSERT_TRUE(candidates.has_value());
	EXPECT_EQ(candidates->matchedEdges, 1U);
	EXPECT_TRUE(candidates->propertyEntityIds.empty());
	EXPECT_TRUE(candidates->propertyEdgeIds.empty());
	EXPECT_TRUE(candidates->fallbackEdgeIds.empty());
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
	ASSERT_TRUE(dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 1, followsType).has_value());
	ASSERT_TRUE(dataManager->cachedRelationshipTypeSegmentStats(offset).has_value());

	SegmentHeader header = dataManager->getSegmentTracker()->getSegmentHeaderCopy(offset);
	header.data_type = Node::typeId;
	dataManager->getSegmentTracker()->writeSegmentHeader(offset, header);
	dataManager->clearRelationshipSegmentTypeStats();

	EXPECT_FALSE(dataManager->cachedRelationshipTypeSegmentStats(offset).has_value());
	EXPECT_FALSE(dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 1, followsType).has_value());
	EXPECT_FALSE(dataManager->countActiveEdgesByTypeFromSegmentStats(1, 1, followsType).has_value());
}

TEST_F(DataManagerTest, RelationshipSegmentStatsScannerHandlesDirectGuardPaths) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	const int64_t likesType = dataManager->getOrCreateTokenId("LIKES");
	Edge firstFollows = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	Edge firstLikes = addEdgeOfType(dataManager, source.getId(), target.getId(), likesType);
	Edge secondFollows = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	Edge secondLikes = addEdgeOfType(dataManager, source.getId(), target.getId(), likesType);
	simulateSave();
	dataManager->clearCache();

	const auto originalIndex = dataManager->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(originalIndex.empty());
	const uint64_t offset = originalIndex.front().segmentOffset;
	const SegmentHeader header = dataManager->getSegmentTracker()->getSegmentHeaderCopy(offset);
	ASSERT_EQ(header.data_type, Edge::typeId);
	ASSERT_GE(header.used, 4U);

	RelationshipSegmentStatsScanner scanner(*dataManager);
	auto noPreadDataManager = makeNoPreadDataManager();
	RelationshipSegmentStatsScanner noPreadScanner(*noPreadDataManager);
	EXPECT_FALSE(noPreadScanner.build(offset, header, false).has_value());
	EXPECT_FALSE(noPreadScanner.countActiveInWindow(
							 offset,
							 header,
							 firstFollows.getId(),
							 firstFollows.getId(),
							 followsType)
							 .has_value());
	EXPECT_FALSE(noPreadScanner.persistedEdgeMatchesType(firstFollows.getId(), followsType).has_value());
	EXPECT_FALSE(noPreadDataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 128, followsType)
						 .has_value());
	EXPECT_FALSE(noPreadDataManager->countActivePersistedEdgeIdsByType(std::vector<int64_t>{1}, followsType)
						 .has_value());
	EXPECT_FALSE(noPreadDataManager->countActiveEdgesByTypeFromSegmentStats(1, 128, followsType).has_value());

	SegmentHeader wrongType = header;
	wrongType.data_type = Node::typeId;
	EXPECT_FALSE(scanner.build(offset, wrongType, false).has_value());
	EXPECT_FALSE(scanner.countActiveInWindow(offset, wrongType, firstFollows.getId(), secondLikes.getId(), 0).has_value());

	SegmentHeader zeroUsed = header;
	zeroUsed.used = 0;
	EXPECT_FALSE(scanner.build(offset, zeroUsed, false).has_value());
	EXPECT_FALSE(scanner.countActiveInWindow(offset, zeroUsed, firstFollows.getId(), firstFollows.getId(), 0).has_value());
	EXPECT_FALSE(scanner.countActiveInWindow(offset, header, secondLikes.getId(), firstFollows.getId(), 0).has_value());
	EXPECT_FALSE(scanner.countActiveInWindow(
						 offset,
						 header,
						 header.start_id - 1,
						 header.start_id,
						 0)
						 .has_value());

	const auto followsInWindow =
			scanner.countActiveInWindow(offset, header, firstFollows.getId(), secondLikes.getId(), followsType);
	ASSERT_TRUE(followsInWindow.has_value());
	EXPECT_EQ(*followsInWindow, 2);

	const uint64_t missingOffset = offset + static_cast<uint64_t>(TOTAL_SEGMENT_SIZE) * 10'000ULL;
	EXPECT_FALSE(scanner.build(missingOffset, header, false).has_value());
	EXPECT_FALSE(scanner.countActiveInWindow(
						 missingOffset,
						 header,
						 firstFollows.getId(),
						 firstFollows.getId(),
						 followsType)
						 .has_value());

	EXPECT_FALSE(scanner.persistedEdgeMatchesType(0, followsType).has_value());
	auto firstIsLikes = scanner.persistedEdgeMatchesType(firstFollows.getId(), likesType);
	ASSERT_TRUE(firstIsLikes.has_value());
	EXPECT_FALSE(*firstIsLikes);
	auto missingEdge = scanner.persistedEdgeMatchesType(secondLikes.getId() + 1'000'000, followsType);
	ASSERT_TRUE(missingEdge.has_value());
	EXPECT_FALSE(*missingEdge);

	auto patchedIndex = originalIndex;
	patchedIndex.front().endId = header.start_id + static_cast<int64_t>(header.used) + 10;
	dataManager->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, patchedIndex);
	auto outsideHeader = scanner.persistedEdgeMatchesType(header.start_id + static_cast<int64_t>(header.used), 0);
	ASSERT_TRUE(outsideHeader.has_value());
	EXPECT_FALSE(*outsideHeader);

	patchedIndex.front().segmentOffset = missingOffset;
	patchedIndex.front().startId = firstLikes.getId();
	patchedIndex.front().endId = firstLikes.getId();
	dataManager->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, patchedIndex);
	EXPECT_FALSE(scanner.persistedEdgeMatchesType(firstLikes.getId(), likesType).has_value());

	dataManager->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, originalIndex);

	const auto firstIdOffset = static_cast<std::streamoff>(
			offset + sizeof(SegmentHeader) + offsetof(Edge::Metadata, id));
	const int64_t corruptedId = firstFollows.getId() + 10'000;
	{
		std::fstream file(testFilePath, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(file.is_open());
		file.seekp(firstIdOffset);
		file.write(reinterpret_cast<const char *>(&corruptedId), sizeof(corruptedId));
	}
	auto corruptedWindow = scanner.countActiveInWindow(offset, header, firstFollows.getId(), firstFollows.getId(), 0);
	ASSERT_TRUE(corruptedWindow.has_value());
	EXPECT_EQ(*corruptedWindow, 0);
	auto corruptedMatch = scanner.persistedEdgeMatchesType(firstFollows.getId(), 0);
	ASSERT_TRUE(corruptedMatch.has_value());
	EXPECT_FALSE(*corruptedMatch);

	SegmentHeader shiftedHeader = header;
	shiftedHeader.start_id = firstFollows.getId() + 1;
	{
		std::fstream file(testFilePath, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(file.is_open());
		file.seekp(static_cast<std::streamoff>(offset));
		file.write(reinterpret_cast<const char *>(&shiftedHeader), sizeof(shiftedHeader));
	}
	patchedIndex = originalIndex;
	patchedIndex.front().startId = firstFollows.getId();
	patchedIndex.front().endId = firstFollows.getId();
	dataManager->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, patchedIndex);
	auto beforeHeader = scanner.persistedEdgeMatchesType(firstFollows.getId(), 0);
	ASSERT_TRUE(beforeHeader.has_value());
	EXPECT_FALSE(*beforeHeader);

	const uint64_t shortReadOffset = static_cast<uint64_t>(std::filesystem::file_size(testFilePath)) +
									 static_cast<uint64_t>(TOTAL_SEGMENT_SIZE);
	SegmentHeader headerOnly = header;
	headerOnly.start_id = firstLikes.getId();
	headerOnly.used = 1;
	{
		std::fstream file(testFilePath, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(file.is_open());
		file.seekp(static_cast<std::streamoff>(shortReadOffset));
		file.write(reinterpret_cast<const char *>(&headerOnly), sizeof(headerOnly));
	}
	patchedIndex.front().segmentOffset = shortReadOffset;
	patchedIndex.front().startId = firstLikes.getId();
	patchedIndex.front().endId = firstLikes.getId();
	dataManager->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, patchedIndex);
	EXPECT_FALSE(scanner.persistedEdgeMatchesType(firstLikes.getId(), 0).has_value());
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
	snapshot.edges.emplace(outsideRange.getId(),
						   storage::DirtyEntityInfo<Edge>(storage::EntityChangeType::CHANGE_ADDED, outsideRange));
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
	snapshot.edges.emplace(
			modifiedBeyondHeader.getId(),
			storage::DirtyEntityInfo<Edge>(storage::EntityChangeType::CHANGE_MODIFIED, modifiedBeyondHeader));
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
	auto followsAfterTypeChange =
			dataManager->countActiveEdgesByTypeFromSegmentStats(1, persisted.getId(), followsType);
	ASSERT_TRUE(followsAfterTypeChange.has_value());
	EXPECT_EQ(*followsAfterTypeChange, 0);
	auto likesAfterTypeChange = dataManager->countActiveEdgesByTypeFromSegmentStats(1, persisted.getId(), likesType);
	ASSERT_TRUE(likesAfterTypeChange.has_value());
	EXPECT_EQ(*likesAfterTypeChange, 1);

	dataManager->invalidateRelationshipSegmentTypeStats({});
}

TEST_F(DataManagerTest, RelationshipTypeCountsDoNotPersistLegacyTotalStatsState) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	const int64_t likesType = dataManager->getOrCreateTokenId("LIKES");
	for (int i = 0; i < 130; ++i) {
		addEdgeOfType(dataManager, source.getId(), target.getId(), i % 2 == 0 ? followsType : likesType);
	}
	simulateSave();

	EXPECT_TRUE(fileStorage->getSystemStateManager()->getAll(kRelationshipTypeTotalStatsStateKey).empty());

	observer.reset();
	dataManager.reset();
	fileStorage.reset();
	database->close();
	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	dataManager = fileStorage->getDataManager();

	EXPECT_TRUE(fileStorage->getSystemStateManager()->getAll(kRelationshipTypeTotalStatsStateKey).empty());
	auto followsCount = dataManager->countActiveEdgesByTypeFromSegmentStats(1, 130, followsType);
	ASSERT_TRUE(followsCount.has_value());
	EXPECT_EQ(*followsCount, 65);
	auto likesCount = dataManager->countActiveEdgesByTypeFromSegmentStats(1, 130, likesType);
	ASSERT_TRUE(likesCount.has_value());
	EXPECT_EQ(*likesCount, 65);
}

TEST_F(DataManagerTest, RelationshipTypeCountsStayCorrectAfterTypeChangeAndDeleteWithoutTotalStatsState) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	const int64_t likesType = dataManager->getOrCreateTokenId("LIKES");
	Edge first = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	Edge second = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	Edge third = addEdgeOfType(dataManager, source.getId(), target.getId(), likesType);
	simulateSave();

	first.setTypeId(likesType);
	dataManager->updateEdge(first);
	dataManager->deleteEdge(second);
	simulateSave();

	EXPECT_TRUE(fileStorage->getSystemStateManager()->getAll(kRelationshipTypeTotalStatsStateKey).empty());
	auto followsCount = dataManager->countActiveEdgesByTypeFromSegmentStats(1, third.getId(), followsType);
	ASSERT_TRUE(followsCount.has_value());
	EXPECT_EQ(*followsCount, 0);
	auto likesCount = dataManager->countActiveEdgesByTypeFromSegmentStats(1, third.getId(), likesType);
	ASSERT_TRUE(likesCount.has_value());
	EXPECT_EQ(*likesCount, 2);
}

TEST_F(DataManagerTest, CountActivePersistedEdgeIdsByTypeFiltersSortedIdsAndUnsafeContexts) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	const int64_t likesType = dataManager->getOrCreateTokenId("LIKES");
	const int64_t missingType = dataManager->getOrCreateTokenId("MISSING_TYPE");

	Edge firstFollows = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	Edge firstLikes = addEdgeOfType(dataManager, source.getId(), target.getId(), likesType);
	Edge secondFollows = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	simulateSave();
	dataManager->clearCache();

	ASSERT_TRUE(dataManager->countActiveEdgesByTypeFromSegmentStats(1, secondFollows.getId(), followsType).has_value());
	EXPECT_EQ(dataManager->countActivePersistedEdgeIdsByType({}, followsType), std::optional<int64_t>(0));

	const std::vector<int64_t> unsortedIds{
			0,
			secondFollows.getId(),
			firstLikes.getId(),
			firstFollows.getId(),
			secondFollows.getId(),
			secondFollows.getId() + 1000};
	auto allTypes = dataManager->countActivePersistedEdgeIdsByType(unsortedIds, 0);
	ASSERT_TRUE(allTypes.has_value());
	EXPECT_EQ(*allTypes, 3);

	auto follows = dataManager->countActivePersistedEdgeIdsByType(unsortedIds, followsType);
	ASSERT_TRUE(follows.has_value());
	EXPECT_EQ(*follows, 2);

	auto missing = dataManager->countActivePersistedEdgeIdsByType(unsortedIds, missingType);
	ASSERT_TRUE(missing.has_value());
	EXPECT_EQ(*missing, 0);

	const std::vector<int64_t> idsOutsidePersistedSegments{secondFollows.getId() + 10'000};
	EXPECT_EQ(dataManager->countActivePersistedEdgeIdsByType(idsOutsidePersistedSegments, followsType),
			  std::optional<int64_t>(0));

	storage::CommittedSnapshot snapshot;
	Edge snapshotEdge(firstFollows.getId(), source.getId(), target.getId(), likesType);
	snapshot.edges.emplace(
			snapshotEdge.getId(),
			storage::DirtyEntityInfo<Edge>(storage::EntityChangeType::CHANGE_MODIFIED, snapshotEdge));
	dataManager->setCurrentSnapshot(&snapshot);
	EXPECT_FALSE(dataManager->countActivePersistedEdgeIdsByType(unsortedIds, followsType).has_value());
	dataManager->clearCurrentSnapshot();

	addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	EXPECT_FALSE(dataManager->countActivePersistedEdgeIdsByType(unsortedIds, followsType).has_value());
}

TEST_F(DataManagerTest, RelationshipSegmentStatsPrunesRangesAndIgnoresUnrelatedDirtyOverlays) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	const int64_t likesType = dataManager->getOrCreateTokenId("LIKES");
	Edge follows = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	Edge likes = addEdgeOfType(dataManager, source.getId(), target.getId(), likesType);
	simulateSave();
	dataManager->clearRelationshipSegmentTypeStats();

	auto outsideRange = dataManager->countActiveEdgesByTypeFromSegmentStats(
			likes.getId() + 10'000, likes.getId() + 10'127, followsType);
	ASSERT_TRUE(outsideRange.has_value());
	EXPECT_EQ(*outsideRange, 0);

	Edge pendingLike = addEdgeOfType(dataManager, source.getId(), target.getId(), likesType);
	auto followsWithUnrelatedAdd =
			dataManager->countActiveEdgesByTypeFromSegmentStats(follows.getId(), pendingLike.getId(), followsType);
	ASSERT_TRUE(followsWithUnrelatedAdd.has_value());
	EXPECT_EQ(*followsWithUnrelatedAdd, 1);
}

TEST_F(DataManagerTest, CountActivePersistedEdgeIdsByTypeParallelMatchesSequentialAcrossSegments) {
	Node source = createTestNode(dataManager, "StatsParallelUser");
	Node target = createTestNode(dataManager, "StatsParallelUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("PARALLEL_FOLLOWS");
	const int64_t likesType = dataManager->getOrCreateTokenId("PARALLEL_LIKES");
	const int64_t missingType = dataManager->getOrCreateTokenId("PARALLEL_MISSING");

	const size_t edgeCount = static_cast<size_t>(EDGES_PER_SEGMENT) * 3 + 17;
	std::vector<int64_t> selectedIds;
	selectedIds.reserve(edgeCount + 16);
	int64_t expectedFollows = 0;
	for (size_t i = 0; i < edgeCount; ++i) {
		const bool follows = i % 4 != 0;
		Edge edge = addEdgeOfType(dataManager, source.getId(), target.getId(), follows ? followsType : likesType);
		selectedIds.push_back(edge.getId());
		if (i % 257 == 0) {
			selectedIds.push_back(edge.getId());
		}
		if (follows) {
			++expectedFollows;
		}
	}
	selectedIds.push_back(0);
	selectedIds.push_back(-1);
	std::reverse(selectedIds.begin(), selectedIds.end());

	simulateSave();
	dataManager->clearCache();
	dataManager->clearRelationshipSegmentTypeStats();

	graph::concurrent::ThreadPool pool(4);
	auto sequentialAll = dataManager->countActivePersistedEdgeIdsByType(selectedIds, 0);
	auto parallelAll = dataManager->countActivePersistedEdgeIdsByType(selectedIds, 0, &pool);
	ASSERT_TRUE(sequentialAll.has_value());
	ASSERT_TRUE(parallelAll.has_value());
	EXPECT_EQ(*sequentialAll, static_cast<int64_t>(edgeCount));
	EXPECT_EQ(*parallelAll, *sequentialAll);

	auto sequentialFollows = dataManager->countActivePersistedEdgeIdsByType(selectedIds, followsType);
	auto parallelFollows = dataManager->countActivePersistedEdgeIdsByType(selectedIds, followsType, &pool);
	ASSERT_TRUE(sequentialFollows.has_value());
	ASSERT_TRUE(parallelFollows.has_value());
	EXPECT_EQ(*sequentialFollows, expectedFollows);
	EXPECT_EQ(*parallelFollows, *sequentialFollows);

	auto parallelMissing = dataManager->countActivePersistedEdgeIdsByType(selectedIds, missingType, &pool);
	ASSERT_TRUE(parallelMissing.has_value());
	EXPECT_EQ(*parallelMissing, 0);
}

TEST_F(DataManagerTest, RelationshipSegmentStatsCollectsCandidatesWithoutEdgeReferenceMaterialization) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	const int64_t likesType = dataManager->getOrCreateTokenId("LIKES");
	const int64_t missingType = dataManager->getOrCreateTokenId("MISSING_TYPE");
	Edge follows = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	Edge likes = addEdgeOfType(dataManager, source.getId(), target.getId(), likesType);
	dataManager->addEdgeProperties(follows.getId(), {{"rank", PropertyValue(int64_t{1})}});
	dataManager->addEdgeProperties(likes.getId(), {{"rank", PropertyValue(int64_t{2})}});
	simulateSave();
	dataManager->clearRelationshipSegmentTypeStats();

	EXPECT_FALSE(dataManager->hasCachedRelationshipSegmentTypeStats());
	EXPECT_FALSE(dataManager->hasCachedRelationshipSegmentTypeStats(true));
	EXPECT_FALSE(dataManager
						 ->collectRelationshipPropertyCandidatesFromSegmentStats(
								 follows.getId(), follows.getId(), followsType, false)
						 .has_value());

	auto allTypes = dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(
			follows.getId(), likes.getId(), 0, false);
	ASSERT_TRUE(allTypes.has_value());
	EXPECT_EQ(allTypes->matchedEdges, 2U);
	EXPECT_EQ(allTypes->propertyEntityIds.size(), 2U);
	EXPECT_TRUE(allTypes->propertyEdgeIds.empty());
	EXPECT_TRUE(allTypes->fallbackEdgeIds.empty());
	EXPECT_TRUE(dataManager->hasCachedRelationshipSegmentTypeStats());
	EXPECT_TRUE(dataManager->hasCachedRelationshipSegmentTypeStats(true));

	auto cachedFollows = dataManager->collectCachedRelationshipPropertyCandidatesFromSegmentStats(
			follows.getId(), likes.getId(), followsType, false);
	ASSERT_TRUE(cachedFollows.has_value());
	EXPECT_EQ(cachedFollows->matchedEdges, 1U);
	EXPECT_EQ(cachedFollows->propertyEntityIds.size(), 1U);
	EXPECT_TRUE(cachedFollows->propertyEdgeIds.empty());

	auto cachedMissing = dataManager->collectCachedRelationshipPropertyCandidatesFromSegmentStats(
			follows.getId(), likes.getId(), missingType, false);
	ASSERT_TRUE(cachedMissing.has_value());
	EXPECT_EQ(cachedMissing->matchedEdges, 0U);
	EXPECT_TRUE(cachedMissing->propertyEntityIds.empty());
}

TEST_F(DataManagerTest, CountActivePersistedEdgeIdsByTypeReusesCachedSegmentSummaries) {
	simulateSave();
	const std::vector<int64_t> missingBeforeEdges{1};
	EXPECT_EQ(dataManager->countActivePersistedEdgeIdsByType(missingBeforeEdges, 0), std::optional<int64_t>(0));

	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	const int64_t likesType = dataManager->getOrCreateTokenId("LIKES");
	const int64_t missingType = dataManager->getOrCreateTokenId("MISSING_TYPE");
	for (int i = 0; i < static_cast<int>(EDGES_PER_SEGMENT * 2); ++i) {
		const bool firstSegment = i < static_cast<int>(EDGES_PER_SEGMENT);
		const int64_t typeId = firstSegment ? (i % 2 == 0 ? followsType : likesType) : likesType;
		addEdgeOfType(dataManager, source.getId(), target.getId(), typeId);
	}
	simulateSave();
	dataManager->clearRelationshipSegmentTypeStats();

	const std::vector<int64_t> nonPositiveIds{0, -5};
	EXPECT_EQ(dataManager->countActivePersistedEdgeIdsByType(nonPositiveIds, followsType), std::optional<int64_t>(0));

	auto cached =
			dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, EDGES_PER_SEGMENT * 2, 0, false);
	ASSERT_TRUE(cached.has_value());
	ASSERT_TRUE(dataManager->hasCachedRelationshipSegmentTypeStats());

	const std::vector<int64_t> mixedFirstSegment{1, 2, 3};
	auto follows = dataManager->countActivePersistedEdgeIdsByType(mixedFirstSegment, followsType);
	ASSERT_TRUE(follows.has_value());
	EXPECT_EQ(*follows, 2);

	auto allTypes = dataManager->countActivePersistedEdgeIdsByType(mixedFirstSegment, 0);
	ASSERT_TRUE(allTypes.has_value());
	EXPECT_EQ(*allTypes, 3);

	auto missing = dataManager->countActivePersistedEdgeIdsByType(mixedFirstSegment, missingType);
	ASSERT_TRUE(missing.has_value());
	EXPECT_EQ(*missing, 0);

	storage::CommittedSnapshot emptySnapshot;
	dataManager->setCurrentSnapshot(&emptySnapshot);
	auto withEmptySnapshot = dataManager->countActivePersistedEdgeIdsByType(mixedFirstSegment, followsType);
	ASSERT_TRUE(withEmptySnapshot.has_value());
	EXPECT_EQ(*withEmptySnapshot, 2);
	dataManager->clearCurrentSnapshot();

	const std::vector<int64_t> secondSegmentOnly{
			static_cast<int64_t>(EDGES_PER_SEGMENT + 1),
			static_cast<int64_t>(EDGES_PER_SEGMENT + 2),
	};
	auto likes = dataManager->countActivePersistedEdgeIdsByType(secondSegmentOnly, likesType);
	ASSERT_TRUE(likes.has_value());
	EXPECT_EQ(*likes, 2);
}

TEST_F(DataManagerTest, CountActivePersistedEdgeIdsByTypeIgnoresIdsOutsidePersistedHeaderWindow) {
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

	const std::vector<int64_t> idsOutsideHeader{64};
	auto count = dataManager->countActivePersistedEdgeIdsByType(idsOutsideHeader, followsType);
	ASSERT_TRUE(count.has_value());
	EXPECT_EQ(*count, 0);
	dataManager->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, originalIndex);
}

TEST_F(DataManagerTest, CountActivePersistedEdgeIdsByTypeHandlesIndexGapsAndReadFailures) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	Edge first = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	Edge second = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	simulateSave();

	const auto originalIndex = dataManager->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(originalIndex.empty());

	auto gappedIndex = originalIndex;
	gappedIndex.front().startId = second.getId();
	dataManager->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, gappedIndex);
	EXPECT_EQ(dataManager->countActivePersistedEdgeIdsByType(std::vector<int64_t>{first.getId()}, followsType),
			  std::optional<int64_t>(0));

	auto missingSegmentIndex = originalIndex;
	missingSegmentIndex.front().segmentOffset += static_cast<uint64_t>(TOTAL_SEGMENT_SIZE) * 10'000ULL;
	dataManager->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, missingSegmentIndex);
	EXPECT_FALSE(dataManager->countActivePersistedEdgeIdsByType(std::vector<int64_t>{second.getId()}, followsType)
						 .has_value());

	dataManager->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, originalIndex);
}

TEST_F(DataManagerTest, RelationshipSegmentStatsCacheValidatesHeaderShape) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	for (int i = 0; i < 8; ++i) {
		addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	}
	simulateSave();

	const auto &segments = dataManager->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(segments.empty());
	const uint64_t offset = segments.front().segmentOffset;
	const SegmentHeader header = dataManager->getSegmentTracker()->getSegmentHeaderCopy(offset);
	ASSERT_TRUE(dataManager->countActiveEdgesByTypeFromSegmentStats(1, 8, followsType).has_value());
	ASSERT_TRUE(dataManager->collectRelationshipPropertyCandidatesFromSegmentStats(1, 8, followsType).has_value());
	ASSERT_TRUE(dataManager->cachedRelationshipTypeSegmentStats(offset).has_value());
	EXPECT_FALSE(dataManager->cachedRelationshipTypeSegmentStats(offset + TOTAL_SEGMENT_SIZE * 10'000ULL).has_value());

	auto zeroUsed = header;
	zeroUsed.used = 0;
	dataManager->getSegmentTracker()->writeSegmentHeader(offset, zeroUsed);
	EXPECT_FALSE(dataManager->cachedRelationshipTypeSegmentStats(offset).has_value());
	dataManager->getSegmentTracker()->writeSegmentHeader(offset, header);

	auto shiftedStart = header;
	++shiftedStart.start_id;
	dataManager->getSegmentTracker()->writeSegmentHeader(offset, shiftedStart);
	EXPECT_FALSE(dataManager->cachedRelationshipTypeSegmentStats(offset).has_value());
	dataManager->getSegmentTracker()->writeSegmentHeader(offset, header);

	auto shortened = header;
	--shortened.used;
	dataManager->getSegmentTracker()->writeSegmentHeader(offset, shortened);
	EXPECT_FALSE(dataManager->cachedRelationshipTypeSegmentStats(offset).has_value());
	dataManager->getSegmentTracker()->writeSegmentHeader(offset, header);

	auto inactiveChanged = header;
	++inactiveChanged.inactive_count;
	dataManager->getSegmentTracker()->writeSegmentHeader(offset, inactiveChanged);
	EXPECT_FALSE(dataManager->cachedRelationshipTypeSegmentStats(offset).has_value());
	dataManager->getSegmentTracker()->writeSegmentHeader(offset, header);
}

TEST_F(DataManagerTest, RelationshipSegmentStatsScannerBuildsCountsAndHandlesLookupGuards) {
	Node source = createTestNode(dataManager, "StatsUser");
	Node target = createTestNode(dataManager, "StatsUser");
	dataManager->addNode(source);
	dataManager->addNode(target);
	const int64_t followsType = dataManager->getOrCreateTokenId("FOLLOWS");
	const int64_t likesType = dataManager->getOrCreateTokenId("LIKES");
	Edge first = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	Edge second = addEdgeOfType(dataManager, source.getId(), target.getId(), likesType);
	Edge third = addEdgeOfType(dataManager, source.getId(), target.getId(), followsType);
	Edge zeroType = addEdgeOfType(dataManager, source.getId(), target.getId(), 0);
	dataManager->addEdgeProperties(first.getId(), {{"rank", PropertyValue(int64_t{1})}});
	simulateSave();

	const auto &segments = dataManager->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(segments.empty());
	const uint64_t offset = segments.front().segmentOffset;
	const SegmentHeader header = dataManager->getSegmentTracker()->getSegmentHeaderCopy(offset);
	RelationshipSegmentStatsScanner scanner(*dataManager);

	auto stats = scanner.build(offset, header, true);
	ASSERT_TRUE(stats.has_value());
	EXPECT_EQ(stats->segmentOffset, offset);
	EXPECT_EQ(stats->activeCount, 4);
	EXPECT_EQ(stats->activeCountByType[followsType], 2);
	EXPECT_EQ(stats->activeCountByType[likesType], 1);
	EXPECT_EQ(stats->activeCountByType[0], 1);
	EXPECT_FALSE(stats->activeIdRangeByType.contains(0));
	EXPECT_EQ(stats->activePropertyEdgeIds, std::vector<int64_t>{first.getId()});
	ASSERT_TRUE(stats->activeIdRangeByType.contains(followsType));
	EXPECT_EQ(stats->activeIdRangeByType[followsType].first, first.getId());
	EXPECT_EQ(stats->activeIdRangeByType[followsType].second, third.getId());

	auto allInWindow = scanner.countActiveInWindow(offset, header, first.getId(), second.getId(), 0);
	ASSERT_TRUE(allInWindow.has_value());
	EXPECT_EQ(*allInWindow, 2);
	auto followsInWindow = scanner.countActiveInWindow(offset, header, first.getId(), third.getId(), followsType);
	ASSERT_TRUE(followsInWindow.has_value());
	EXPECT_EQ(*followsInWindow, 2);
	EXPECT_FALSE(scanner.countActiveInWindow(offset, header, third.getId(), second.getId(), followsType).has_value());
	EXPECT_FALSE(scanner.countActiveInWindow(offset, header, header.start_id + header.used, header.start_id + header.used,
											 followsType)
						 .has_value());
	EXPECT_FALSE(scanner.countActiveInWindow(offset, header, header.start_id - 1, header.start_id, followsType)
						 .has_value());

	EXPECT_EQ(scanner.persistedEdgeMatchesType(first.getId(), followsType), std::optional<bool>(true));
	EXPECT_EQ(scanner.persistedEdgeMatchesType(first.getId(), likesType), std::optional<bool>(false));
	EXPECT_EQ(scanner.persistedEdgeMatchesType(third.getId() + 10'000, followsType), std::optional<bool>(false));
	EXPECT_FALSE(scanner.persistedEdgeMatchesType(0, followsType).has_value());

	const auto originalIndex = dataManager->getSegmentIndexManager()->getEdgeSegmentIndex();
	auto shiftedIndex = originalIndex;
	shiftedIndex.front().startId = first.getId() + 1;
	dataManager->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, shiftedIndex);
	EXPECT_EQ(scanner.persistedEdgeMatchesType(first.getId(), followsType), std::optional<bool>(false));
	dataManager->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, originalIndex);

	dataManager->deleteEdge(second);
	simulateSave();
	EXPECT_EQ(scanner.persistedEdgeMatchesType(second.getId(), likesType), std::optional<bool>(false));
	EXPECT_EQ(scanner.persistedEdgeMatchesType(zeroType.getId(), 0), std::optional<bool>(true));
}
