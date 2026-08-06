/**
 * @file test_RelocatableLogicalGraphReadSnapshot.cpp
 * @brief Public logical-read snapshot continuation-cache integration tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

using namespace relocatable_storage_backend_test;

namespace {

	void addThirdSelfEdge(const std::filesystem::path &path, uint64_t nodeId) {
		zyx::Database database(path.string());
		database.open();
		ASSERT_GT(database.createEdge(static_cast<int64_t>(nodeId), static_cast<int64_t>(nodeId), "FOLLOWS", {}),
				  0);
		database.close();
	}

} // namespace

TEST_F(RelocatableStorageBackendTest,
       ResumesAdjacentScansAfterCursorCacheEvictionAndWhenSnapshotCursorCachingIsDisabled) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	addThirdSelfEdge(data_path_, fixture.person_node_id);

	RelocatableStorageBackendOptions limitedOptions = backendOptions();
	limitedOptions.maximum_snapshot_adjacency_cursors = 1U;
	RelocatableStorageBackend limited(std::move(limitedOptions));
	limited.open();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = limited.openLogicalGraphReadSnapshot();

	const LogicalGraphAdjacencyPage outgoing = snapshot->scanAdjacentEdges(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
	ASSERT_EQ(outgoing.edges.size(), 1U);
	ASSERT_TRUE(outgoing.continuation.has_value());

	const LogicalGraphAdjacencyPage cachedOutgoing = snapshot->scanAdjacentEdges(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, outgoing.continuation, 1U);
	ASSERT_EQ(cachedOutgoing.edges.size(), 1U);
	ASSERT_TRUE(cachedOutgoing.continuation.has_value());
	EXPECT_GT(cachedOutgoing.edges.front().edge_id, outgoing.edges.front().edge_id);

	// The incoming cursor evicts the sole outgoing cursor. A later resume must
	// validate the logical continuation and produce the next page without a cache entry.
	const LogicalGraphAdjacencyPage incoming = snapshot->scanAdjacentEdges(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 1U);
	ASSERT_EQ(incoming.edges.size(), 1U);
	ASSERT_TRUE(incoming.continuation.has_value());
	const LogicalGraphAdjacencyPage resumedOutgoing = snapshot->scanAdjacentEdges(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, cachedOutgoing.continuation, 1U);
	ASSERT_EQ(resumedOutgoing.edges.size(), 1U);
	EXPECT_GT(resumedOutgoing.edges.front().edge_id, cachedOutgoing.edges.front().edge_id);
	snapshot.reset();
	limited.close();

	RelocatableStorageBackendOptions uncachedOptions = backendOptions();
	uncachedOptions.maximum_snapshot_adjacency_cursors = 0U;
	RelocatableStorageBackend uncached(std::move(uncachedOptions));
	uncached.open();
	snapshot = uncached.openLogicalGraphReadSnapshot();
	const LogicalGraphAdjacencyPage first = snapshot->scanAdjacentEdges(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
	ASSERT_EQ(first.edges.size(), 1U);
	ASSERT_TRUE(first.continuation.has_value());
	const LogicalGraphAdjacencyPage second = snapshot->scanAdjacentEdges(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 1U);
	ASSERT_EQ(second.edges.size(), 1U);
	EXPECT_GT(second.edges.front().edge_id, first.edges.front().edge_id);
	snapshot.reset();
	uncached.close();
}
