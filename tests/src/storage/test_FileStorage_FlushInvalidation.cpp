#include "storage/FileStorageTestFixture.hpp"

#include <vector>

#include "graph/storage/PersistenceManager.hpp"
#include "src/storage/FileStorageFlushDetail.hpp"

using graph::storage::file_storage_detail::FlushCacheInvalidationMode;
using graph::storage::file_storage_detail::invalidateWrittenSnapshotCache;

TEST_F(FileStorageTest, FlushInvalidationFallsBackToDirtySnapshotWhenNoSegmentsWereReported) {
	graph::storage::FlushSnapshotView snapshot;
	std::vector<uint64_t> touchedSegments;

	const auto mode = invalidateWrittenSnapshotCache(*fileStorage->getDataManager(), snapshot, touchedSegments);

	EXPECT_EQ(mode, FlushCacheInvalidationMode::FCIM_DIRTY_SNAPSHOT);
}

TEST_F(FileStorageTest, FlushInvalidationUsesWriterReportedSegmentsForPreciseCacheEviction) {
	auto dm = fileStorage->getDataManager();
	const int64_t labelId = dm->getOrCreateTokenId("FlushInvalidationUser");
	graph::Node source(0, labelId);
	graph::Node target(0, labelId);
	dm->addNode(source);
	dm->addNode(target);
	const int64_t typeId = dm->getOrCreateTokenId("FLUSH_INVALIDATES");
	graph::Edge edge(0, source.getId(), target.getId(), typeId);
	dm->addEdge(edge);
	fileStorage->flush();

	const auto &segments = dm->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(segments.empty());
	const uint64_t edgeSegment = segments.front().segmentOffset;
	ASSERT_TRUE(dm->collectRelationshipPropertyCandidatesFromSegmentStats(edge.getId(), edge.getId(), typeId)
						.has_value());
	ASSERT_TRUE(dm->cachedRelationshipTypeSegmentStats(edgeSegment).has_value());

	graph::storage::FlushSnapshotView snapshot;
	const std::vector<uint64_t> touchedSegments{edgeSegment};
	const auto mode = invalidateWrittenSnapshotCache(*dm, snapshot, touchedSegments);

	EXPECT_EQ(mode, FlushCacheInvalidationMode::FCIM_TOUCHED_SEGMENTS);
	EXPECT_FALSE(dm->cachedRelationshipTypeSegmentStats(edgeSegment).has_value());
}
