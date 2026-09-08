/**
 * @file test_RelocatableLogicalGraphReadSnapshot.cpp
 * @brief Public logical-read snapshot continuation-cache integration tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include "graph/debug/PerfTrace.hpp"

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

TEST_F(RelocatableStorageBackendTest, ResumesAnAdjacentScanFromAnotherSnapshot) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	backend.open();

	std::unique_ptr<LogicalGraphReadSnapshot> origin = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphAdjacencyPage first = origin->scanAdjacentEdges(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
	ASSERT_EQ(first.edges.size(), 1U);
	ASSERT_TRUE(first.continuation.has_value());

	// Continuations contain logical edge IDs, so another snapshot must safely
	// rebuild its own physical cursor rather than relying on the origin cache.
	std::unique_ptr<LogicalGraphReadSnapshot> resumed = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphAdjacencyPage second = resumed->scanAdjacentEdges(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 1U);
	ASSERT_EQ(second.edges.size(), 1U);
	EXPECT_GT(second.edges.front().edge_id, first.edges.front().edge_id);

	resumed.reset();
	origin.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalReadSnapshotValidatesAllVisitorCallbacks) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	backend.open();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();

	const std::function<bool(const LogicalGraphNode &)> noNodeVisitor;
	const std::function<bool(const LogicalGraphEdge &)> noEdgeVisitor;
	const std::function<bool(const LogicalGraphEdgeTopology &)> noTopologyVisitor;
	EXPECT_THROW(snapshot->visitNodes(4U, noNodeVisitor), FormatException);
	EXPECT_THROW(snapshot->visitEdges(4U, noEdgeVisitor), FormatException);
	EXPECT_THROW(snapshot->visitEdgeTopologies(4U, noTopologyVisitor), FormatException);
	EXPECT_THROW(snapshot->visitAdjacentEdges(fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING,
								 4U, noEdgeVisitor), FormatException);
	EXPECT_THROW(snapshot->visitAdjacentEdgeTopologies(fixture.person_node_id,
								 LogicalGraphReadDirection::LGRD_OUTGOING, 4U, noTopologyVisitor), FormatException);

	// The same snapshot remains usable after rejected callbacks, and topology
	// traversal is exposed through the format-independent contract.
	size_t nodeCount = 0U;
	snapshot->visitNodes(4U, [&nodeCount](const LogicalGraphNode &) {
		++nodeCount;
		return true;
	});
	EXPECT_EQ(nodeCount, 2U);
	size_t edgeCount = 0U;
	snapshot->visitEdges(4U, [&edgeCount](const LogicalGraphEdge &edge) {
		EXPECT_NE(edge.edge_id, 0U);
		EXPECT_NE(edge.source_node_id, 0U);
		EXPECT_NE(edge.target_node_id, 0U);
		EXPECT_FALSE(edge.type.empty());
		++edgeCount;
		return true;
	});
	EXPECT_EQ(edgeCount, 2U);
	size_t adjacentEdgeCount = 0U;
	snapshot->visitAdjacentEdges(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, 4U,
			[&adjacentEdgeCount, &fixture](const LogicalGraphEdge &edge) {
				EXPECT_EQ(edge.source_node_id, fixture.person_node_id);
				EXPECT_FALSE(edge.type.empty());
				++adjacentEdgeCount;
				return true;
			});
	EXPECT_EQ(adjacentEdgeCount, 2U);
	size_t incomingEdgeCount = 0U;
	snapshot->visitAdjacentEdges(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_INCOMING, 4U,
			[&incomingEdgeCount, &fixture](const LogicalGraphEdge &edge) {
				EXPECT_EQ(edge.target_node_id, fixture.person_node_id);
				EXPECT_FALSE(edge.type.empty());
				++incomingEdgeCount;
				return true;
			});
	EXPECT_EQ(incomingEdgeCount, 2U);
	size_t topologyCount = 0U;
	snapshot->visitEdgeTopologies(4U, [&topologyCount](const LogicalGraphEdgeTopology &) {
		++topologyCount;
		return true;
	});
	EXPECT_EQ(topologyCount, 2U);

	EXPECT_EQ(snapshot->readNode(fixture.person_node_id).node_id, fixture.person_node_id);
	EXPECT_TRUE(snapshot->containsNode(fixture.person_node_id));
	EXPECT_TRUE(snapshot->supportsNodeExistenceProbes());
	EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id).edge_id, fixture.knows_edge_id);
	const auto exact = snapshot->findNodesByExactProperties(
			{"Person"}, {{"alpha", PropertyValue(int64_t{42})}}, 2U, 2U);
	EXPECT_FALSE(exact.has_value());
	const auto range = snapshot->findNodesByPropertyRange(
			{"Person"}, {"alpha", PropertyValue(int64_t{1}), PropertyValue(int64_t{100}), true, true}, 2U, 2U);
	EXPECT_FALSE(range.has_value());
	const auto composite = snapshot->findNodesByCompositePropertyRange(
			{"Person"}, {{{"alpha", PropertyValue(int64_t{42})}},
					{"beta", PropertyValue("a"), PropertyValue("z"), true, true}},
			2U, 2U);
	EXPECT_FALSE(composite.has_value());
	const LogicalGraphNodePage nodePage = snapshot->scanNodesAfter(0U, 2U, 8U);
	EXPECT_EQ(nodePage.nodes.size(), 2U);
	EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 0U, 8U)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 2U, 0U)), FormatException);
	const LogicalGraphAdjacencyPage adjacencyPage = snapshot->scanAdjacentEdges(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 2U);
	EXPECT_EQ(adjacencyPage.edges.size(), 2U);
	size_t adjacentTopologyCount = 0U;
	snapshot->visitAdjacentEdgeTopologies(fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, 4U,
			[&adjacentTopologyCount](const LogicalGraphEdgeTopology &) {
				++adjacentTopologyCount;
				return true;
			});
	EXPECT_EQ(adjacentTopologyCount, 2U);
	EXPECT_TRUE(snapshot->mutationFence().has_value());

	snapshot.reset();
	std::unique_ptr<LogicalGraphVectorSearchSnapshot> vectorSnapshot = backend.openLogicalGraphVectorSearchSnapshot();
	const LogicalGraphVectorSearchRequest invalidVectorRequest;
	EXPECT_THROW(static_cast<void>(vectorSnapshot->searchLogicalVector(invalidVectorRequest)), std::invalid_argument);
	LogicalGraphVectorSearchRequest describeRequest;
	describeRequest.index_name = "missing";
	describeRequest.query_vector = {1.0F};
	describeRequest.top_k = 1U;
	describeRequest.maximum_mapping_entries = 1U;
	describeRequest.maximum_raw_vector_reads = 1U;
	describeRequest.candidate_search_budget = 1U;
	describeRequest.candidate_refinement_budget = 1U;
	EXPECT_THROW(static_cast<void>(vectorSnapshot->describeLogicalVectorIndex(describeRequest)), std::runtime_error);
	const LogicalGraphVectorIndexDescriptor nonMatchingIndex{1U, "Other", "embedding", 1U, "l2"};
	const auto overlayScore = vectorSnapshot->scoreLogicalVectorExactOverlayCandidate(
			describeRequest, nonMatchingIndex, vectorSnapshot->readNode(fixture.person_node_id));
	EXPECT_FALSE(overlayScore.has_value());
	vectorSnapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalEdgeMaterializationRecordsTraceWhenEnabled) {
	static_cast<void>(materializeNativeGraphFixture());
	RelocatableStorageBackend backend(backendOptions());
	backend.open();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();

	const bool traceWasEnabled = graph::debug::PerfTrace::isEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setEnabled(true);
	size_t visited = 0U;
	snapshot->visitEdges(4U, [&visited](const LogicalGraphEdge &edge) {
		++visited;
		return edge.edge_id != 0U && edge.source_node_id != 0U && edge.target_node_id != 0U;
	});
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(traceWasEnabled);

	EXPECT_EQ(visited, 2U);
	ASSERT_TRUE(trace.contains("native_graph.read.edge_logical_materialization"));
	EXPECT_EQ(trace.at("native_graph.read.edge_logical_materialization").calls, 1U);
	ASSERT_TRUE(trace.contains("native_graph.read.edge_name_materialization"));
	EXPECT_EQ(trace.at("native_graph.read.edge_name_materialization").calls, 2U);
	ASSERT_TRUE(trace.contains("native_graph.read.edge_query_visitor"));
	EXPECT_EQ(trace.at("native_graph.read.edge_query_visitor").calls, 2U);

	snapshot.reset();
	backend.close();
}
