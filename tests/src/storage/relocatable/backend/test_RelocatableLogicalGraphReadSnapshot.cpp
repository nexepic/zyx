/**
 * @file test_RelocatableLogicalGraphReadSnapshot.cpp
 * @brief Public logical-read snapshot continuation-cache integration tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include "CypherParserImpl.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/execution/LogicalPlanProcedureRuntime.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"

using namespace relocatable_storage_backend_test;


namespace {

	void addThirdSelfEdge(const std::filesystem::path &path, uint64_t nodeId) {
		zyx::Database database(path.string());
		database.open();
		ASSERT_GT(database.createEdge(static_cast<int64_t>(nodeId), static_cast<int64_t>(nodeId), "FOLLOWS", {}),
				  0);
		database.close();
	}

	PageId publishedGlobalAdjacencyRootPage(const std::filesystem::path &path) {
		auto data = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary);
		if (!data->is_open()) {
			throw std::runtime_error("could not open native graph fixture for adjacency corruption");
		}
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const PageId pageId = metadata->findIndexRoot(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID).root_page_id;
		snapshot.release();
		metadata.reset();
		io.reset();
		data->close();
		return pageId;
	}

	PageId publishedEdgeEntityPage(const std::filesystem::path &path, const uint64_t edgeId) {
		auto data = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary);
		if (!data->is_open()) {
			throw std::runtime_error("could not open native graph fixture for Entity page lookup");
		}
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const PageId pageId = metadata->findEdgePageLocation(snapshot, edgeId).page_id;
		snapshot.release();
		metadata.reset();
		io.reset();
		data->close();
		return pageId;
	}

	void corruptPublishedPagePayload(const std::filesystem::path &path, const PageId pageId) {
		auto data = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary);
		if (!data->is_open()) {
			throw std::runtime_error("could not open native graph fixture for page corruption");
		}
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const PageLocation location = metadata->findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata->findCatalog(snapshot, location.segment_id);
		const uint64_t pageOffset = PhysicalSegmentAllocator::checkedSlotOffset(
				snapshot.superblock().data_region_offset, pool.physical_slot) +
				static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		const uint64_t payloadOffset = pageOffset + RSS_PAGE_OWNER_HEADER_BYTES;
		std::byte value{};
		if (io->readAt(payloadOffset, &value, sizeof(value)) != sizeof(value)) {
			throw std::runtime_error("could not read native graph page payload for corruption");
		}
		value ^= std::byte{0x01U};
		io->writeAt(payloadOffset, &value, sizeof(value));
		io->sync();
		snapshot.release();
		metadata.reset();
		io.reset();
		data->close();
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
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	EXPECT_EQ(snapshot->readNodeProperty(fixture.person_node_id, "alpha"), PropertyValue(int64_t{42}));
	EXPECT_EQ(snapshot->readNodeProperty(fixture.person_node_id, "missing"), PropertyValue());
	EXPECT_EQ(snapshot->readEdgeProperty(fixture.knows_edge_id, "alpha"), PropertyValue(int64_t{7}));
	EXPECT_EQ(snapshot->readEdgeProperty(fixture.knows_edge_id, "missing"), PropertyValue());
	// Complete scans above intentionally populate this snapshot's bounded
	// Property cache. Verify that scalar reads reuse it without decoding again,
	// then use a fresh snapshot to exercise the selected-decode path itself.
	const graph::debug::PerfTrace::Snapshot cachedTrace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	EXPECT_FALSE(cachedTrace.contains("native_graph.read.property_selected_decode"));
	EXPECT_FALSE(cachedTrace.contains("native_graph.read.property_full_decode"));
	std::unique_ptr<LogicalGraphReadSnapshot> freshPropertySnapshot = backend.openLogicalGraphReadSnapshot();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	EXPECT_EQ(freshPropertySnapshot->readNodeProperty(fixture.person_node_id, "alpha"), PropertyValue(int64_t{42}));
	EXPECT_EQ(freshPropertySnapshot->readEdgeProperty(fixture.knows_edge_id, "alpha"), PropertyValue(int64_t{7}));
	const graph::debug::PerfTrace::Snapshot selectiveTrace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_TRUE(selectiveTrace.contains("native_graph.read.property_selected_decode"));
	EXPECT_EQ(selectiveTrace.at("native_graph.read.property_selected_decode").totalValue, 2);
	EXPECT_FALSE(selectiveTrace.contains("native_graph.read.property_full_decode"));
	const EntityReadShape identityOnly;
	EXPECT_EQ(snapshot->readNodeRef(fixture.person_node_id, identityOnly),
			  (EntityRef{fixture.person_node_id, false, 0U, 0U, 0U}));
	const EntityReadShape topologyOnly{.include_type = true};
	const EntityRef edgeRef = snapshot->readEdgeRef(fixture.knows_edge_id, topologyOnly);
	EXPECT_EQ(edgeRef.entity_id, fixture.knows_edge_id);
	EXPECT_TRUE(edgeRef.is_edge);
	EXPECT_EQ(edgeRef.source_node_id, fixture.person_node_id);
	EXPECT_TRUE(snapshot->supportsNativeRelationshipTypeTokens());
	const std::optional<uint64_t> knowsToken = snapshot->findRelationshipTypeToken("KNOWS");
	ASSERT_TRUE(knowsToken.has_value());
	EXPECT_EQ(edgeRef.type_token, *knowsToken);
	EXPECT_FALSE(snapshot->findRelationshipTypeToken("MISSING").has_value());
	const std::optional<uint32_t> alphaToken = snapshot->findPropertyToken("alpha");
	ASSERT_TRUE(alphaToken.has_value());
	const LogicalGraphNode shapedNode = snapshot->readNodeShaped(
			fixture.person_node_id, EntityReadShape{.property_tokens = {*alphaToken}});
	EXPECT_EQ(shapedNode.node_id, fixture.person_node_id);
	EXPECT_TRUE(shapedNode.labels.empty());
	ASSERT_EQ(shapedNode.properties.size(), 1U);
	EXPECT_EQ(shapedNode.properties.front().key, "alpha");
	EXPECT_EQ(shapedNode.properties.front().value, PropertyValue(int64_t{42}));
	const LogicalGraphEdge shapedEdge = snapshot->readEdgeShaped(
			fixture.knows_edge_id, EntityReadShape{.include_type = true});
	EXPECT_EQ(shapedEdge.edge_id, fixture.knows_edge_id);
	EXPECT_EQ(shapedEdge.type, "KNOWS");
	EXPECT_TRUE(shapedEdge.properties.empty());
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
	const LogicalGraphAdjacencyTopologyPage topologyPage = snapshot->scanAdjacentEdgeTopologies(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
	ASSERT_EQ(topologyPage.edges.size(), 1U);
	ASSERT_TRUE(topologyPage.continuation.has_value());
	EXPECT_EQ(topologyPage.edges.front().source_node_id, fixture.person_node_id);
	const LogicalGraphAdjacencyTopologyPage resumedTopologyPage = snapshot->scanAdjacentEdgeTopologies(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, topologyPage.continuation, 1U);
	ASSERT_EQ(resumedTopologyPage.edges.size(), 1U);
	EXPECT_GT(resumedTopologyPage.edges.front().edge_id, topologyPage.edges.front().edge_id);
	EXPECT_FALSE(resumedTopologyPage.continuation.has_value());
	EXPECT_TRUE(snapshot->supportsNativeAdjacentEdgeRefScans());
	const LogicalGraphAdjacencyReferencePage referencePage = snapshot->scanAdjacentEdgeRefs(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
	ASSERT_EQ(referencePage.edges.size(), 1U);
	EXPECT_TRUE(referencePage.edges.front().is_edge);
	EXPECT_EQ(referencePage.edges.front().source_node_id, fixture.person_node_id);
	EXPECT_EQ(referencePage.edges.front().type_token, *knowsToken);
	ASSERT_TRUE(referencePage.continuation.has_value());
	const LogicalGraphAdjacencyReferencePage resumedReferencePage = snapshot->scanAdjacentEdgeRefs(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, referencePage.continuation, 1U);
	ASSERT_EQ(resumedReferencePage.edges.size(), 1U);
	EXPECT_GT(resumedReferencePage.edges.front().entity_id, referencePage.edges.front().entity_id);
	size_t adjacentTopologyCount = 0U;
	snapshot->visitAdjacentEdgeTopologies(fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, 4U,
			[&adjacentTopologyCount](const LogicalGraphEdgeTopology &) {
				++adjacentTopologyCount;
				return true;
			});
	EXPECT_EQ(adjacentTopologyCount, 2U);
	EXPECT_TRUE(snapshot->mutationFence().has_value());

	freshPropertySnapshot.reset();
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
	EXPECT_EQ(vectorSnapshot->exactOverlayCandidateReadShape(nonMatchingIndex),
			(EntityReadShape{.include_labels = true, .all_properties = true}));
	const auto vectorPropertyToken = vectorSnapshot->findPropertyToken("alpha");
	ASSERT_TRUE(vectorPropertyToken.has_value());
	EXPECT_EQ(vectorSnapshot->exactOverlayCandidateReadShape({1U, "Person", "alpha", 1U, "l2"}),
			(EntityReadShape{.include_labels = true, .property_tokens = {*vectorPropertyToken}}));
	const auto overlayScore = vectorSnapshot->scoreLogicalVectorExactOverlayCandidate(
			describeRequest, nonMatchingIndex, vectorSnapshot->readNode(fixture.person_node_id));
	EXPECT_FALSE(overlayScore.has_value());
	vectorSnapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, PropertyPresenceProbeAvoidsValueMaterialization) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::string largeValue(8192U, 'x');
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Payload"}, {{"payload", PropertyValue(largeValue)}, {"small", PropertyValue(int64_t{1})}}},
			 {{"Payload"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	ASSERT_NE(nodes[0], 0U);
	ASSERT_NE(nodes[1], 0U);
	const std::vector<uint64_t> edges = backend.createLogicalEdges(
			{{nodes.front(), nodes.front(), "HAS", {{"payload", PropertyValue(largeValue)}}}});
	ASSERT_EQ(edges.size(), 1U);

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	EXPECT_TRUE(snapshot->nodePropertyExists(nodes.front(), "payload").value_or(false));
	EXPECT_TRUE(snapshot->nodePropertyExists(nodes.front(), "small").value_or(false));
	EXPECT_FALSE(snapshot->nodePropertyExists(nodes.front(), "missing").value_or(true));
	EXPECT_TRUE(snapshot->edgePropertyExists(edges.front(), "payload").value_or(false));
	EXPECT_FALSE(snapshot->edgePropertyExists(edges.front(), "missing").value_or(true));
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_TRUE(trace.contains("native_graph.read.property_presence_probe"));
	// Unknown tokens are rejected at the schema-token boundary and do not touch
	// the Property Directory; only the three resolved tokens reach the storage probe.
	EXPECT_EQ(trace.at("native_graph.read.property_presence_probe").totalValue, 3);
	EXPECT_FALSE(trace.contains("native_graph.read.property_selected_decode"));
	EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
	// A known-but-absent token is cached independently from decoded values. A
	// subsequent scalar read must reuse that authenticated negative result rather
	// than reopening the Property page.
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	EXPECT_FALSE(snapshot->nodePropertyExists(nodes[1], "payload").value_or(true));
	EXPECT_EQ(snapshot->readNodeProperty(nodes[1], "payload"), PropertyValue());
	EXPECT_FALSE(snapshot->edgePropertyExists(edges.front(), "small").value_or(true));
	EXPECT_EQ(snapshot->readEdgeProperty(edges.front(), "small"), PropertyValue());
	const graph::debug::PerfTrace::Snapshot negativeTrace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	EXPECT_GE(negativeTrace.at("native_graph.read.property_presence_probe").totalValue, 1);
	EXPECT_FALSE(negativeTrace.contains("native_graph.read.property_selected_decode"));
	EXPECT_FALSE(negativeTrace.contains("native_graph.read.property_full_decode"));
	// Batch selected reads must reuse the same authenticated negative results as
	// scalar reads. A presence-only probe must not trigger another Directory
	// descent or Property payload decode when the value is requested in a column.
	const std::array<uint64_t, 1U> absentNodeIds{nodes[1]};
	const std::array<uint64_t, 1U> absentEdgeIds{edges.front()};
	const std::array<std::string_view, 2U> absentNodeKeys{"payload", "small"};
	const std::array<std::string_view, 1U> absentEdgeKeys{"small"};
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	EXPECT_EQ(snapshot->readNodePropertiesSorted(absentNodeIds, "payload"),
			  (std::vector<PropertyValue>{PropertyValue()}));
	const auto absentNodeColumns = snapshot->readNodePropertyColumnsSorted(absentNodeIds, absentNodeKeys);
	ASSERT_EQ(absentNodeColumns.size(), 2U);
	EXPECT_EQ(absentNodeColumns[0], (std::vector<PropertyValue>{PropertyValue()}));
	EXPECT_EQ(absentNodeColumns[1], (std::vector<PropertyValue>{PropertyValue()}));
	const auto absentEdgeColumns = snapshot->readEdgePropertyColumnsSorted(absentEdgeIds, absentEdgeKeys);
	ASSERT_EQ(absentEdgeColumns.size(), 1U);
	EXPECT_EQ(absentEdgeColumns.front(), (std::vector<PropertyValue>{PropertyValue()}));
	const graph::debug::PerfTrace::Snapshot batchNegativeTrace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	EXPECT_FALSE(batchNegativeTrace.contains("native_graph.read.property_selected_decode"));
	EXPECT_FALSE(batchNegativeTrace.contains("native_graph.read.property_full_decode"));
	EXPECT_EQ(snapshot->readNodeProperty(nodes.front(), "payload"), PropertyValue(largeValue));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, CountPropertyUsesPresenceProbeUnderSmallMaterializationBudget) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::string largeValue(8192U, 'y');
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"Payload"},
			{{"payload", PropertyValue(largeValue)}}}, {{"Payload"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	graph::parser::cypher::CypherParserImpl parser;
	const graph::query::QueryPlan plan = parser.parseToLogical(
			"MATCH (n:Payload) RETURN count(n.payload) AS payload_count");
	ASSERT_NE(plan.root, nullptr);
	execution::LogicalPlanReadRuntime runtime(backend);
	execution::LogicalPlanReadRuntimeBounds bounds{
			.maximum_node_records = 4U,
			.maximum_node_page_records = 2U,
			.maximum_metadata_pages_per_node_page = 8U,
			.maximum_edges_per_node = 1U,
			.maximum_edge_records = 1U,
			.maximum_adjacency_page_records = 1U,
			.maximum_property_entries = 2U,
			.maximum_rows = 4U,
			.maximum_distinct_rows = 4U,
			// Include result/carrier/column storage while staying below the unrequested 8 KiB payload.
			.maximum_materialized_bytes = 1024U};
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const graph::query::QueryResult result = runtime.executeToQueryResult(*plan.root, bounds);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("payload_count").asPrimitive(), PropertyValue(int64_t{1}));
	ASSERT_TRUE(trace.contains("native_graph.read.property_presence_probe"));
	EXPECT_GE(trace.at("native_graph.read.property_presence_probe").totalValue, 1);
	EXPECT_FALSE(trace.contains("native_graph.read.property_selected_decode"));
	EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, CountNonNullProbePreservesNullAndKeyExistenceForInlineAndLargeValues) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::string large(8192U, 'q');
	const auto nodes = backend.createLogicalNodes({
		{{"CountNull"}, {{"payload", PropertyValue()}, {"padding", PropertyValue(large)}}},
		{{"CountNull"}, {{"payload", PropertyValue(0)}}},
		{{"CountNull"}, {{"payload", PropertyValue(large)}}},
		{{"CountNull"}, {}},
		{{"CountNull"}, {{"payload", PropertyValue()}}}});
	ASSERT_EQ(nodes.size(), 5U);
	const auto edges = backend.createLogicalEdges({
		{nodes[0], nodes[0], "COUNT_NULL", {{"payload", PropertyValue()}, {"padding", PropertyValue(large)}}},
		{nodes[0], nodes[0], "COUNT_NULL", {{"payload", PropertyValue(0)}}},
		{nodes[0], nodes[0], "COUNT_NULL", {{"payload", PropertyValue(large)}}},
		{nodes[0], nodes[0], "COUNT_NULL", {}},
		{nodes[0], nodes[0], "COUNT_NULL", {{"payload", PropertyValue()}}}});
	ASSERT_EQ(edges.size(), 5U);
	{
		auto snapshot = backend.openLogicalGraphReadSnapshot();
		ASSERT_NE(snapshot, nullptr);
		for (size_t index = 0U; index < 5U; ++index) {
			const bool nonNull = index == 1U || index == 2U;
			EXPECT_EQ(snapshot->nodePropertyIsNonNull(nodes[index], "payload"), std::optional<bool>{nonNull});
			EXPECT_EQ(snapshot->edgePropertyIsNonNull(edges[index], "payload"), std::optional<bool>{nonNull});
		}
		EXPECT_TRUE(snapshot->nodePropertyExists(nodes[4], "payload").value_or(false));
		EXPECT_TRUE(snapshot->nodePropertyExists(nodes[0], "payload").value_or(false));
		EXPECT_TRUE(snapshot->edgePropertyExists(edges[4], "payload").value_or(false));
		EXPECT_TRUE(snapshot->edgePropertyExists(edges[0], "payload").value_or(false));
		EXPECT_EQ(snapshot->readNodeProperty(nodes[0], "payload"), PropertyValue());
		EXPECT_EQ(snapshot->readEdgeProperty(edges[0], "payload"), PropertyValue());
		EXPECT_FALSE(snapshot->nodePropertyIsNonNull(nodes[0], "payload").value_or(true));
		EXPECT_FALSE(snapshot->edgePropertyIsNonNull(edges[0], "payload").value_or(true));
		EXPECT_TRUE(snapshot->nodePropertyExists(nodes[0], "payload").value_or(false));
		EXPECT_TRUE(snapshot->edgePropertyExists(edges[0], "payload").value_or(false));
	}
	execution::LogicalPlanReadRuntime runtime(backend);
	execution::LogicalPlanReadRuntimeBounds bounds{
		.maximum_node_records = 16U, .maximum_node_page_records = 2U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U,
		.maximum_edge_records = 16U, .maximum_adjacency_page_records = 2U,
		.maximum_property_entries = 4U, .maximum_rows = 16U,
		.maximum_distinct_rows = 16U, .maximum_materialized_bytes = 1024U};
	graph::parser::cypher::CypherParserImpl parser;
	for (const std::string query: {
		"MATCH (n:CountNull) RETURN count(n.payload) AS count",
		"MATCH ()-[r:COUNT_NULL]->() RETURN count(r.payload) AS count"}) {
		SCOPED_TRACE(query);
		const auto plan = parser.parseToLogical(query);
		ASSERT_NE(plan.root, nullptr);
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setValueCollectionEnabled(true);
		const auto result = runtime.executeToQueryResult(*plan.root, bounds);
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("count").asPrimitive(), PropertyValue(2));
		EXPECT_FALSE(trace.contains("native_graph.read.property_selected_decode"));
		EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
	}
	{
		auto transaction = backend.beginLogicalGraphTransaction(true);
		LogicalGraphTransactionOverlay overlay(*transaction);
		for (const bool vectorSnapshot: {false, true}) {
			std::unique_ptr<LogicalGraphReadSnapshot> snapshot = vectorSnapshot
				? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
				: overlay.openLogicalGraphReadSnapshot();
			EXPECT_TRUE(snapshot->supportsPropertyNonNullProbes());
			EXPECT_TRUE(snapshot->supportsNativeNodeRefScans());
			EXPECT_TRUE(snapshot->supportsNativeEdgeRefScans());
			size_t nodeVisits = 0U, edgeVisits = 0U;
			snapshot->visitNodeRefs(1U, [&](const EntityRef &) { ++nodeVisits; return false; });
			snapshot->visitEdgeRefs(1U, [&](const EntityRef &) { ++edgeVisits; return false; });
			EXPECT_EQ(nodeVisits, 1U);
			EXPECT_EQ(edgeVisits, 1U);
			EXPECT_EQ(snapshot->nodePropertyIsNonNull(nodes[0], "payload"), std::optional<bool>{false});
			EXPECT_EQ(snapshot->nodePropertyIsNonNull(nodes[2], "payload"), std::optional<bool>{true});
			EXPECT_EQ(snapshot->edgePropertyIsNonNull(edges[0], "payload"), std::optional<bool>{false});
			EXPECT_EQ(snapshot->edgePropertyIsNonNull(edges[2], "payload"), std::optional<bool>{true});
		}
		transaction->rollback();
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, GroupedMixedCountDoesNotPrefetchLargeCountOnlyProperties) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::string large(8192U, 'c');
	const auto nodes = backend.createLogicalNodes({
		{{"GroupedCount"}, {{"bucket", PropertyValue(0)}, {"rank", PropertyValue(1)}, {"payload", PropertyValue(large)}}},
		{{"GroupedCount"}, {{"bucket", PropertyValue(0)}, {"rank", PropertyValue(2)}, {"payload", PropertyValue()}}},
		{{"GroupedCount"}, {{"bucket", PropertyValue(1)}, {"rank", PropertyValue(3)}}}});
	ASSERT_EQ(nodes.size(), 3U);
	ASSERT_EQ(backend.createLogicalEdges({
		{nodes[0], nodes[0], "GROUPED_COUNT", {{"bucket", PropertyValue(0)}, {"rank", PropertyValue(1)}, {"payload", PropertyValue(large)}}},
		{nodes[0], nodes[0], "GROUPED_COUNT", {{"bucket", PropertyValue(0)}, {"rank", PropertyValue(2)}, {"payload", PropertyValue()}}},
		{nodes[0], nodes[0], "GROUPED_COUNT", {{"bucket", PropertyValue(1)}, {"rank", PropertyValue(3)}}}}).size(), 3U);
	execution::LogicalPlanReadRuntimeBounds bounds{
		.maximum_node_records = 16U, .maximum_node_page_records = 2U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U,
		.maximum_edge_records = 16U, .maximum_adjacency_page_records = 2U,
		.maximum_property_entries = 4U, .maximum_rows = 16U,
		.maximum_distinct_rows = 16U, .maximum_materialized_bytes = 4096U};
	graph::parser::cypher::CypherParserImpl parser;
	for (const bool edges: {false, true}) {
		SCOPED_TRACE(edges);
		const std::string variable = edges ? "r" : "n";
		const auto plan = parser.parseToLogical(std::string(edges ? "MATCH ()-[r:GROUPED_COUNT]->()" : "MATCH (n:GroupedCount)") +
			" RETURN " + variable + ".bucket AS bucket, count(" + variable + ".payload) AS count, sum(" + variable + ".rank) AS sum");
		ASSERT_NE(plan.root, nullptr);
		for (const bool transactional: {false, true}) {
			SCOPED_TRACE(transactional);
			auto transaction = backend.beginLogicalGraphTransaction(true);
			LogicalGraphTransactionOverlay overlay(*transaction);
			execution::LogicalPlanReadRuntime runtime(transactional ? static_cast<LogicalGraphReadBackend &>(overlay)
				: static_cast<LogicalGraphReadBackend &>(backend));
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset();
			graph::debug::PerfTrace::setValueCollectionEnabled(true);
			const auto result = runtime.executeToQueryResult(*plan.root, bounds);
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			ASSERT_EQ(result.rowCount(), 2U);
			std::map<int64_t, std::pair<PropertyValue, PropertyValue>> groups;
			for (const auto &row: result.getRows()) {
				const PropertyValue key = row.at("bucket").asPrimitive();
				groups.emplace(std::get<int64_t>(key.getVariant()), std::make_pair(row.at("count").asPrimitive(), row.at("sum").asPrimitive()));
			}
			EXPECT_EQ(groups.at(0), (std::pair<PropertyValue, PropertyValue>{PropertyValue(1), PropertyValue(3)}));
			EXPECT_EQ(groups.at(1), (std::pair<PropertyValue, PropertyValue>{PropertyValue(0), PropertyValue(3)}));
			EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
			EXPECT_TRUE(trace.contains("native_graph.read.property_presence_probe"));
			transaction->rollback();
		}
	}
	{
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		ASSERT_EQ(overlay.createLogicalEdges({{nodes[0], nodes[0], "STAGED_COUNT_TYPE", {}}}).size(), 1U);
		auto snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_FALSE(snapshot->supportsNativeRelationshipTypeTokens());
		EXPECT_TRUE(snapshot->supportsNativeEdgeRefScans());
		const auto plan = parser.parseToLogical("MATCH ()-[r:STAGED_COUNT_TYPE]->() RETURN count(r) AS count");
		ASSERT_NE(plan.root, nullptr);
		execution::LogicalPlanReadRuntime runtime(overlay);
		const auto result = runtime.executeToQueryResult(*plan.root, bounds);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("count").asPrimitive(), PropertyValue(1));
		transaction->rollback();
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StagedNodeCountUsesMergedReferencesWithoutFullPropertyDecode) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto nodes = backend.createLogicalNodes({
		{{"StagedCount"}, {{"rank", PropertyValue(1)}, {"payload", PropertyValue(std::string(8192U, 'a'))}}},
		{{"StagedCount"}, {{"rank", PropertyValue(99)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	{
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		overlay.replaceLogicalNodeProperties({{nodes[0], {{"rank", PropertyValue(2)},
			{"payload", PropertyValue(std::string(8192U, 'b'))}}}});
		overlay.deleteLogicalNodes({{nodes[1], false, 0U}});
		ASSERT_EQ(overlay.createLogicalNodes({{{"StagedCount", "OnlyStaged"},
			{{"rank", PropertyValue(3)}, {"payload", PropertyValue(std::string(8192U, 'c'))}}}}).size(), 1U);
		execution::LogicalPlanReadRuntimeBounds bounds{
			.maximum_node_records = 16U, .maximum_node_page_records = 2U,
			.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U,
			.maximum_edge_records = 16U, .maximum_adjacency_page_records = 2U,
			.maximum_property_entries = 4U, .maximum_rows = 16U,
			.maximum_distinct_rows = 16U, .maximum_materialized_bytes = 4096U};
		graph::parser::cypher::CypherParserImpl parser;
		execution::LogicalPlanReadRuntime runtime(overlay);
		for (const bool onlyStaged: {false, true}) {
			SCOPED_TRACE(onlyStaged);
			const auto plan = parser.parseToLogical(std::string("MATCH (n:") + (onlyStaged ? "OnlyStaged" : "StagedCount") +
				") RETURN count(n.payload) AS count, sum(n.rank) AS sum");
			ASSERT_NE(plan.root, nullptr);
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset();
			graph::debug::PerfTrace::setValueCollectionEnabled(true);
			const auto result = runtime.executeToQueryResult(*plan.root, bounds);
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			ASSERT_EQ(result.rowCount(), 1U);
			EXPECT_EQ(result.getRows().front().at("count").asPrimitive(), PropertyValue(onlyStaged ? 1 : 2));
			EXPECT_EQ(result.getRows().front().at("sum").asPrimitive(), PropertyValue(onlyStaged ? 3 : 5));
			EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
		}
		transaction->rollback();
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StagedEdgeCountUsesMergedReferencesWithoutFullPropertyDecode) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto nodes = backend.createLogicalNodes({{{"EdgeEndpoint"}, {}}});
	ASSERT_EQ(nodes.size(), 1U);
	const auto edges = backend.createLogicalEdges({
		{nodes[0], nodes[0], "STAGED_EDGE_COUNT", {{"rank", PropertyValue(1)}, {"payload", PropertyValue(std::string(8192U, 'a'))}}},
		{nodes[0], nodes[0], "STAGED_EDGE_COUNT", {{"rank", PropertyValue(99)}}}});
	ASSERT_EQ(edges.size(), 2U);
	{
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		overlay.replaceLogicalEdgeProperties({{edges[0], {{"rank", PropertyValue(2)},
			{"payload", PropertyValue(std::string(8192U, 'b'))}}}});
		overlay.deleteLogicalEdges({{edges[1]}});
		ASSERT_EQ(overlay.createLogicalEdges({{nodes[0], nodes[0], "STAGED_EDGE_COUNT",
			{{"rank", PropertyValue(3)}, {"payload", PropertyValue(std::string(8192U, 'c'))}}}}).size(), 1U);
		auto snapshot = overlay.openLogicalGraphReadSnapshot();
		ASSERT_TRUE(snapshot->supportsNativeEdgeRefScans());
		ASSERT_TRUE(snapshot->supportsNativeRelationshipTypeTokens());
		execution::LogicalPlanReadRuntimeBounds bounds{
			.maximum_node_records = 16U, .maximum_node_page_records = 2U,
			.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U,
			.maximum_edge_records = 16U, .maximum_adjacency_page_records = 2U,
			.maximum_property_entries = 4U, .maximum_rows = 16U,
			.maximum_distinct_rows = 16U, .maximum_materialized_bytes = 4096U};
		graph::parser::cypher::CypherParserImpl parser;
		execution::LogicalPlanReadRuntime runtime(overlay);
		for (const bool typed: {false, true}) {
			SCOPED_TRACE(typed);
			const auto plan = parser.parseToLogical(std::string("MATCH ()-[r") + (typed ? ":STAGED_EDGE_COUNT" : "") +
				"]->() RETURN count(r.payload) AS count, sum(r.rank) AS sum");
			ASSERT_NE(plan.root, nullptr);
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset();
			graph::debug::PerfTrace::setValueCollectionEnabled(true);
			const auto result = runtime.executeToQueryResult(*plan.root, bounds);
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			ASSERT_EQ(result.rowCount(), 1U);
			EXPECT_EQ(result.getRows().front().at("count").asPrimitive(), PropertyValue(2));
			EXPECT_EQ(result.getRows().front().at("sum").asPrimitive(), PropertyValue(5));
			EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
		}
		transaction->rollback();
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ScopedPatternReadsAvoidUnrequestedLargeEntityProperties) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::string payload(8192U, 'p');
	const auto nodes = backend.createLogicalNodes({
		{{"PatternSource"}, {{"payload", PropertyValue(payload)}}},
		{{"PatternTarget"}, {{"rank", PropertyValue(1)}, {"payload", PropertyValue(payload)}}},
		{{"PatternTarget"}, {{"rank", PropertyValue(2)}, {"payload", PropertyValue(payload)}}}});
	ASSERT_EQ(nodes.size(), 3U);
	ASSERT_EQ(backend.createLogicalEdges({
		{nodes[0], nodes[1], "PATTERN_LINK", {{"payload", PropertyValue(payload)}}},
		{nodes[0], nodes[2], "PATTERN_LINK", {{"payload", PropertyValue(payload)}}}}).size(), 2U);
	execution::LogicalPlanReadRuntimeBounds bounds{
		.maximum_node_records = 16U, .maximum_node_page_records = 2U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U,
		.maximum_edge_records = 16U, .maximum_adjacency_page_records = 2U,
		.maximum_property_entries = 4U, .maximum_rows = 16U,
		.maximum_distinct_rows = 16U, .maximum_materialized_bytes = 4096U};
	graph::parser::cypher::CypherParserImpl parser;
	execution::LogicalPlanReadRuntime runtime(backend);
	for (const std::string expression: {
		"[(n)-[:PATTERN_LINK]->(m) | m.rank]",
		"[(n)-[:PATTERN_LINK]->(m) | labels(m)]",
		"[(n)-[:PATTERN_LINK]->(m) | m.never_registered]",
		"[(n)-[r:PATTERN_LINK]->(m) | r.never_registered]",
		"EXISTS((n)-[:PATTERN_LINK]->(m) WHERE m.rank > 1)",
		"EXISTS((n)-[:PATTERN_LINK]->(m) WHERE size(labels(m)) > 0)"}) {
		SCOPED_TRACE(expression);
		const auto plan = parser.parseToLogical("MATCH (n:PatternSource) RETURN " + expression + " AS value");
		ASSERT_NE(plan.root, nullptr);
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setValueCollectionEnabled(true);
		const auto result = runtime.executeToQueryResult(*plan.root, bounds);
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		ASSERT_EQ(result.rowCount(), 1U);
		const auto actual = result.getRows().front().at("value").asPrimitive();
		if (expression.starts_with("EXISTS")) EXPECT_EQ(actual, PropertyValue(true));
		else if (expression.find("never_registered") != std::string::npos) {
			EXPECT_EQ(actual, PropertyValue(std::vector<PropertyValue>{PropertyValue(), PropertyValue()}));
		} else if (expression.find("labels") != std::string::npos) {
			const PropertyValue labels(std::vector<PropertyValue>{PropertyValue("PatternTarget")});
			EXPECT_EQ(actual, PropertyValue(std::vector<PropertyValue>{labels, labels}));
		} else EXPECT_EQ(actual, PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)}));
		EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
	}
	ASSERT_EQ(backend.createLogicalEdges({
		{nodes[1], nodes[2], "PATTERN_LINK", {{"payload", PropertyValue(payload)}}}}).size(), 1U);
	for (const std::string expression: {
		"[(n)-[:PATTERN_LINK]->(m) | [(m)-[:PATTERN_LINK]->(k) | m.rank + k.rank]]",
		"[(n)-[:PATTERN_LINK]->(m) | EXISTS((m)-[:PATTERN_LINK]->(k) WHERE k.rank > m.rank)]"}) {
		SCOPED_TRACE(expression);
		const auto plan = parser.parseToLogical("MATCH (n:PatternSource) RETURN " + expression + " AS value");
		ASSERT_NE(plan.root, nullptr);
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setValueCollectionEnabled(true);
		const auto result = runtime.executeToQueryResult(*plan.root, bounds);
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		ASSERT_EQ(result.rowCount(), 1U);
		const auto actual = result.getRows().front().at("value").asPrimitive();
		if (expression.find("EXISTS") != std::string::npos) {
			EXPECT_EQ(actual, PropertyValue(std::vector<PropertyValue>{PropertyValue(true), PropertyValue(false)}));
		} else {
			EXPECT_EQ(actual, PropertyValue(std::vector<PropertyValue>{
				PropertyValue(std::vector<PropertyValue>{PropertyValue(3)}), PropertyValue(std::vector<PropertyValue>{})}));
		}
		EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, VariableLengthPredicatesSkipUnrequestedPropertiesAndPreserveFullOutput) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::string payload(8192U, 'v');
	const auto nodes = backend.createLogicalNodes({
		{{"VarSource"}, {{"rank", PropertyValue(0)}, {"payload", PropertyValue(payload)}}},
		{{"VarTarget"}, {{"rank", PropertyValue(1)}, {"payload", PropertyValue(payload)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	ASSERT_EQ(backend.createLogicalEdges({
		{nodes[0], nodes[1], "VAR_LINK", {{"payload", PropertyValue(payload)}}}}).size(), 1U);
	execution::LogicalPlanReadRuntimeBounds bounds{
		.maximum_node_records = 16U, .maximum_node_page_records = 2U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U,
		.maximum_edge_records = 16U, .maximum_adjacency_page_records = 2U,
		.maximum_property_entries = 4U, .maximum_rows = 16U,
		.maximum_distinct_rows = 16U, .maximum_materialized_bytes = 4096U};
	graph::parser::cypher::CypherParserImpl parser;
	execution::LogicalPlanReadRuntime runtime(backend);
	for (const auto &[query, expected]: std::vector<std::pair<std::string, uint64_t>>{
		{"MATCH (n:VarSource)-[:VAR_LINK*1..1]->(m:VarTarget) RETURN id(m) AS value", nodes[1]},
		{"MATCH (n:VarSource)-[:VAR_LINK*1..1]->(m:VarTarget {rank: 1}) RETURN id(m) AS value", nodes[1]},
		{"MATCH (n:VarSource)-[:VAR_LINK*0..0]->(m:VarSource {rank: 0}) RETURN id(m) AS value", nodes[0]},
		{"MATCH (n:VarSource)-[:VAR_LINK*0..0]->(m:VarSource {rank: 0}) RETURN count(m) AS value", 1U}}) {
		SCOPED_TRACE(query);
		const auto plan = parser.parseToLogical(query);
		ASSERT_NE(plan.root, nullptr);
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setValueCollectionEnabled(true);
		const auto result = runtime.executeToQueryResult(*plan.root, bounds);
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(static_cast<int64_t>(expected)));
		EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
	}
	const auto fullPlan = parser.parseToLogical(
			"MATCH (n:VarSource)-[:VAR_LINK*1..1]->(m:VarTarget {rank: 1}) RETURN m");
	ASSERT_NE(fullPlan.root, nullptr);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*fullPlan.root, bounds)), std::runtime_error);
	bounds.maximum_materialized_bytes = 65536U;
	const auto full = runtime.executeToQueryResult(*fullPlan.root, bounds);
	ASSERT_EQ(full.rowCount(), 1U);
	const auto &properties = full.getRows().front().at("m").asLogicalNode().properties;
	const auto found = properties.find("payload");
	ASSERT_NE(found, properties.end());
	EXPECT_EQ(found->second, PropertyValue(payload));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ShortestPathScalarConsumersAvoidCompletePropertyDecode) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::string payload(8192U, 's');
	const auto nodes = backend.createLogicalNodes({
		{{"SizeStart"}, {{"payload", PropertyValue(payload)}}},
		{{"SizeEnd"}, {{"payload", PropertyValue(payload)}}},
		{{"SizeMiddle"}, {{"payload", PropertyValue(payload)}}},
		{{"SizeMiddle"}, {{"payload", PropertyValue(payload)}}}});
	ASSERT_EQ(nodes.size(), 4U);
	ASSERT_EQ(backend.createLogicalEdges({
		{nodes[0], nodes[2], "SIZE_LINK", {{"payload", PropertyValue(payload)}}},
		{nodes[2], nodes[1], "SIZE_LINK", {{"payload", PropertyValue(payload)}}},
		{nodes[0], nodes[3], "SIZE_LINK", {{"payload", PropertyValue(payload)}}},
		{nodes[3], nodes[1], "SIZE_LINK", {{"payload", PropertyValue(payload)}}}}).size(), 4U);
	execution::LogicalPlanReadRuntimeBounds bounds{
		.maximum_node_records = 16U, .maximum_node_page_records = 2U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U,
		.maximum_edge_records = 16U, .maximum_adjacency_page_records = 2U,
		.maximum_property_entries = 4U, .maximum_rows = 16U,
		.maximum_distinct_rows = 16U, .maximum_materialized_bytes = 4096U};
	graph::parser::cypher::CypherParserImpl parser;
	execution::LogicalPlanReadRuntime runtime(backend);
	for (const std::string scalar: {"size", "length"}) {
		for (const std::string function: {"shortestPath", "allShortestPaths"}) {
			const auto plan = parser.parseToLogical("MATCH (a:SizeStart), (b:SizeEnd) RETURN " + scalar + "(" +
					function + "((a)-[:SIZE_LINK*0..3]->(b))) AS value");
			ASSERT_NE(plan.root, nullptr);
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset();
			graph::debug::PerfTrace::setValueCollectionEnabled(true);
			const auto result = runtime.executeToQueryResult(*plan.root, bounds);
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			ASSERT_EQ(result.rowCount(), 1U);
			EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(function == "shortestPath" && scalar == "size" ? 5 : 2));
			EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
			EXPECT_FALSE(trace.contains("native_graph.read.property_selected_decode"));
		}
	}
	for (const std::string function: {"shortestPath", "allShortestPaths"}) {
		const auto aliasPlan = parser.parseToLogical("MATCH (a:SizeStart), (b:SizeEnd) WITH " + function +
				"((a)-[:SIZE_LINK*0..3]->(b)) AS p, id(a) AS startId "
				"RETURN size(p) AS size, length(p) AS length, startId");
		ASSERT_NE(aliasPlan.root, nullptr);
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setValueCollectionEnabled(true);
		const auto result = runtime.executeToQueryResult(*aliasPlan.root, bounds);
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("size").asPrimitive(), PropertyValue(function == "shortestPath" ? 5 : 2));
		EXPECT_EQ(result.getRows().front().at("length").asPrimitive(), PropertyValue(2));
		EXPECT_EQ(result.getRows().front().at("startId").asPrimitive(), PropertyValue(static_cast<int64_t>(nodes[0])));
		EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
	}
	const auto fullPlan = parser.parseToLogical(
			"MATCH (a:SizeStart), (b:SizeEnd) RETURN shortestPath((a)-[:SIZE_LINK*0..3]->(b)) AS path");
	ASSERT_NE(fullPlan.root, nullptr);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*fullPlan.root, bounds)), std::runtime_error);
	const auto distinctPathPlan = parser.parseToLogical(
			"MATCH (a:SizeStart), (b:SizeEnd) WITH DISTINCT shortestPath((a)-[:SIZE_LINK*0..3]->(b)) AS p RETURN size(p)");
	ASSERT_NE(distinctPathPlan.root, nullptr);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*distinctPathPlan.root, bounds)), std::runtime_error);
	bounds.maximum_materialized_bytes = 131072U;
	const auto full = runtime.executeToQueryResult(*fullPlan.root, bounds);
	ASSERT_EQ(full.rowCount(), 1U);
	const auto &path = full.getRows().front().at("path").asPrimitive().getList();
	ASSERT_EQ(path.size(), 5U);
	for (const auto &entity: path) EXPECT_EQ(entity.getMap().at("payload"), PropertyValue(payload));
	backend.replaceLogicalNodeProperties({{nodes[2], {
		{"payload", PropertyValue(payload)}, {"_type", PropertyValue()}}}});
	bounds.maximum_materialized_bytes = 4096U;
	const auto markerPlan = parser.parseToLogical(
			"MATCH (a:SizeStart), (b:SizeEnd) RETURN length(shortestPath((a)-[:SIZE_LINK*0..3]->(b))) AS value");
	ASSERT_NE(markerPlan.root, nullptr);
	const bool markerTraceEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const auto markerResult = runtime.executeToQueryResult(*markerPlan.root, bounds);
	const auto markerTrace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(markerTraceEnabled);
	ASSERT_EQ(markerResult.rowCount(), 1U);
	// An explicit NULL overrides the generated marker; the existing scalar
	// contract then treats the alternating path representation as a generic list.
	EXPECT_EQ(markerResult.getRows().front().at("value").asPrimitive(), PropertyValue(5));
	EXPECT_FALSE(markerTrace.contains("native_graph.read.property_full_decode"));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, PropertyPresenceIsSnapshotBoundAcrossReplacementAndReopen) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(options);
	backend.open();
	const std::string largeValue(8192U, 'z');
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Payload"}, {{"payload", PropertyValue(largeValue)}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const std::vector<uint64_t> edges = backend.createLogicalEdges(
			{{nodes.front(), nodes.front(), "HAS", {{"payload", PropertyValue(largeValue)}}}});
	ASSERT_EQ(edges.size(), 1U);

	std::unique_ptr<LogicalGraphReadSnapshot> oldSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(oldSnapshot, nullptr);
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	EXPECT_TRUE(oldSnapshot->nodePropertyExists(nodes.front(), "payload").value_or(false));
	EXPECT_TRUE(oldSnapshot->edgePropertyExists(edges.front(), "payload").value_or(false));
	const graph::debug::PerfTrace::Snapshot oldTrace = graph::debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(oldTrace.contains("native_graph.read.property_presence_probe"));
	EXPECT_FALSE(oldTrace.contains("native_graph.read.property_selected_decode"));
	EXPECT_FALSE(oldTrace.contains("native_graph.read.property_full_decode"));

	// Retain a small replacement field so the new snapshot must authenticate the
	// replacement root before reporting that the former LargeValue field is absent.
	backend.replaceLogicalNodeProperties({{nodes.front(), {{"replacement", PropertyValue(int64_t{1})}}}});
	backend.replaceLogicalEdgeProperties({{edges.front(), {{"replacement", PropertyValue(int64_t{1})}}}});
	std::unique_ptr<LogicalGraphReadSnapshot> currentSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(currentSnapshot, nullptr);
	graph::debug::PerfTrace::reset();
	EXPECT_TRUE(oldSnapshot->nodePropertyExists(nodes.front(), "payload").value_or(false));
	EXPECT_TRUE(oldSnapshot->edgePropertyExists(edges.front(), "payload").value_or(false));
	EXPECT_FALSE(currentSnapshot->nodePropertyExists(nodes.front(), "payload").value_or(true));
	EXPECT_FALSE(currentSnapshot->edgePropertyExists(edges.front(), "payload").value_or(true));
	const graph::debug::PerfTrace::Snapshot currentTrace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	EXPECT_TRUE(currentTrace.contains("native_graph.read.property_presence_probe"));
	EXPECT_FALSE(currentTrace.contains("native_graph.read.property_selected_decode"));
	EXPECT_FALSE(currentTrace.contains("native_graph.read.property_full_decode"));

	currentSnapshot.reset();
	oldSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(std::move(options));
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> reopenedSnapshot = reopened.openLogicalGraphReadSnapshot();
	ASSERT_NE(reopenedSnapshot, nullptr);
	EXPECT_FALSE(reopenedSnapshot->nodePropertyExists(nodes.front(), "payload").value_or(true));
	EXPECT_FALSE(reopenedSnapshot->edgePropertyExists(edges.front(), "payload").value_or(true));
	reopenedSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
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

	// Topology-only consumers must not decode relationship Properties merely to
	// enumerate endpoints and type. Use a fresh trace so this assertion cannot be
	// satisfied by the complete Edge visit above.
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setEnabled(true);
	size_t topologyVisited = 0U;
	snapshot->visitEdgeTopologies(4U, [&topologyVisited](const LogicalGraphEdgeTopology &edge) {
		++topologyVisited;
		return edge.edge_id != 0U && edge.source_node_id != 0U && edge.target_node_id != 0U;
	});
	const graph::debug::PerfTrace::Snapshot topologyTrace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(traceWasEnabled);
	EXPECT_EQ(topologyVisited, 2U);
	ASSERT_TRUE(topologyTrace.contains("native_graph.read.edge_topology_scan_calls"));
	EXPECT_EQ(topologyTrace.at("native_graph.read.edge_topology_scan_calls").totalValue, 1);
	ASSERT_TRUE(topologyTrace.contains("native_graph.read.edge_topology_scan_edges"));
	EXPECT_EQ(topologyTrace.at("native_graph.read.edge_topology_scan_edges").totalValue, 2);
	EXPECT_FALSE(topologyTrace.contains("native_graph.read.property_full_decode"));
	EXPECT_FALSE(topologyTrace.contains("native_graph.read.property_selected_decode"));

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, AdjacencyTopologyPageRecordsTraceWithoutPropertyDecode) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	backend.open();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();

	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const LogicalGraphAdjacencyTopologyPage first = snapshot->scanAdjacentEdgeTopologies(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
	ASSERT_EQ(first.edges.size(), 1U);
	ASSERT_TRUE(first.continuation.has_value());
	const LogicalGraphAdjacencyTopologyPage second = snapshot->scanAdjacentEdgeTopologies(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 1U);
	ASSERT_EQ(second.edges.size(), 1U);
	EXPECT_FALSE(second.continuation.has_value());
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(trace.contains("native_graph.read.adjacency_topology_scan_calls"));
	EXPECT_EQ(trace.at("native_graph.read.adjacency_topology_scan_calls").totalValue, 2);
	ASSERT_TRUE(trace.contains("native_graph.read.adjacency_topology_scan_edges"));
	EXPECT_EQ(trace.at("native_graph.read.adjacency_topology_scan_edges").totalValue, 2);
	EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
	EXPECT_FALSE(trace.contains("native_graph.read.property_selected_decode"));

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, AdjacencyTopologyPagesRemainSnapshotBoundAcrossPublicationAndReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(options);
	backend.open();
	std::unique_ptr<LogicalGraphReadSnapshot> oldSnapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphAdjacencyTopologyPage beforePublication = oldSnapshot->scanAdjacentEdgeTopologies(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U);
	ASSERT_EQ(beforePublication.edges.size(), 2U);

	const std::vector<uint64_t> created = backend.createLogicalEdges(
			{{fixture.person_node_id, fixture.person_node_id, "FOLLOWS", {{"payload", PropertyValue("new")}}}});
	ASSERT_EQ(created.size(), 1U);
	std::unique_ptr<LogicalGraphReadSnapshot> currentSnapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphAdjacencyTopologyPage afterPublication = currentSnapshot->scanAdjacentEdgeTopologies(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U);
	ASSERT_EQ(afterPublication.edges.size(), 3U);
	EXPECT_EQ(afterPublication.edges.back().edge_id, created.front());
	EXPECT_EQ(afterPublication.edges.back().type, "FOLLOWS");

	// A topology cursor is bound to its capture generation, independently of a
	// later adjacency publication and without opening the new Edge's Properties.
	const LogicalGraphAdjacencyTopologyPage oldView = oldSnapshot->scanAdjacentEdgeTopologies(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U);
	EXPECT_EQ(oldView.edges, beforePublication.edges);
	currentSnapshot.reset();
	oldSnapshot.reset();
	backend.close();

	RelocatableStorageBackend reopened(std::move(options));
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> reopenedSnapshot = reopened.openLogicalGraphReadSnapshot();
	const LogicalGraphAdjacencyTopologyPage afterReopen = reopenedSnapshot->scanAdjacentEdgeTopologies(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U);
	ASSERT_EQ(afterReopen.edges.size(), 3U);
	EXPECT_EQ(afterReopen.edges.back().edge_id, created.front());
	reopenedSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, AdjacencyTopologyVisitorHonorsEarlyTermination) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	backend.open();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();

	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	std::vector<uint64_t> visited;
	snapshot->visitAdjacentEdgeTopologies(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, 4U,
			[&visited](const LogicalGraphEdgeTopology &edge) {
				visited.push_back(edge.edge_id);
				return false;
			});
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_EQ(visited.size(), 1U);
	EXPECT_NE(visited.front(), 0U);
	ASSERT_TRUE(trace.contains("native_graph.read.adjacency_topology_scan_calls"));
	EXPECT_EQ(trace.at("native_graph.read.adjacency_topology_scan_calls").totalValue, 1);
	EXPECT_FALSE(trace.contains("native_graph.read.entity_node_record_cache_miss"));
	EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
	EXPECT_FALSE(trace.contains("native_graph.read.property_selected_decode"));

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, AdjacencyTopologyReadRejectsCorruptedGlobalAdjacencyPage) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;

	// A topology scan begins at the Global Adjacency B+Tree. Its page checksum
	// must be checked by the public reader rather than only by offline integrity
	// maintenance, so a corrupted traversal cannot expose partial topology.
	const PageId adjacencyRoot = publishedGlobalAdjacencyRootPage(data_path_);
	corruptPublishedPagePayload(data_path_, adjacencyRoot);
	RelocatableStorageBackend corruptedAdjacency(options);
	corruptedAdjacency.open();
	std::unique_ptr<LogicalGraphReadSnapshot> adjacencySnapshot = corruptedAdjacency.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(adjacencySnapshot->scanAdjacentEdgeTopologies(
				 fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U)),
			 FormatException);
	adjacencySnapshot.reset();
	corruptedAdjacency.close();
}

TEST_F(RelocatableStorageBackendTest, AdjacencyTopologyReadRejectsCorruptedEdgeEntityPage) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;

	// The valid adjacency reference reaches this Entity page through the pinned
	// Entity Map. Its page codec must likewise fail before any topology record
	// reaches a logical consumer.
	corruptPublishedPagePayload(data_path_, publishedEdgeEntityPage(data_path_, fixture.knows_edge_id));
	RelocatableStorageBackend corruptedEntity(std::move(options));
	corruptedEntity.open();
	std::unique_ptr<LogicalGraphReadSnapshot> entitySnapshot = corruptedEntity.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(entitySnapshot->scanAdjacentEdgeTopologies(
				 fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U)),
			 FormatException);
	entitySnapshot.reset();
	corruptedEntity.close();
}

TEST_F(RelocatableStorageBackendTest, NativeSnapshotAdmissionAndPrefetchPreserveRetryOrderAndCancellation) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	for (char value: {'a', 'b', 'c'}) {
		requests.push_back({{"Admission"}, {{"payload", PropertyValue(std::string(500U, value))}}});
	}
	const auto nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), 3U);
	std::vector<LogicalGraphEdgeCreateRequest> edgeRequests;
	for (size_t index = 0U; index < nodes.size(); ++index) {
		edgeRequests.push_back({nodes[index], nodes[index], "ADMISSION",
			{{"payload", PropertyValue(std::string(500U, static_cast<char>('a' + index)))}}});
	}
	const auto edges = backend.createLogicalEdges(edgeRequests);
	ASSERT_EQ(edges.size(), 3U);
	{
		auto snapshot = backend.openLogicalGraphReadSnapshot();
		ASSERT_TRUE(snapshot->supportsPropertyReadAdmission());
		const std::array<std::string_view, 2U> keys{"payload", "payload"};
		for (const bool isEdge: {false, true}) {
			SCOPED_TRACE(isEdge);
			const std::array<uint64_t, 1U> ids{isEdge ? edges[0] : nodes[0]};
			const auto read = [&](const std::function<void(size_t)> &admission) {
				return isEdge ? snapshot->readEdgePropertyColumnsSortedWithAdmission(ids, keys, admission)
						: snapshot->readNodePropertyColumnsSortedWithAdmission(ids, keys, admission);
			};
			const std::function<void(size_t)> reject = [](size_t) { throw std::runtime_error("source rejected"); };
			EXPECT_THROW(static_cast<void>(read(reject)), std::runtime_error);
			for (size_t pass = 0U; pass < 2U; ++pass) {
				size_t bytes = 0U;
				const auto columns = read([&](size_t count) { bytes += count; });
				EXPECT_EQ(bytes, 1028U);
				ASSERT_EQ(columns.size(), 2U);
				EXPECT_EQ(columns[0][0], PropertyValue(std::string(500U, 'a')));
				EXPECT_EQ(columns[1][0], columns[0][0]);
			}
			EXPECT_THROW(static_cast<void>(read({})), std::invalid_argument);
		}
	}
	execution::LogicalPlanReadRuntime runtime(backend);
	execution::LogicalPlanReadRuntimeBounds bounds{
		.maximum_node_records = 16U, .maximum_node_page_records = 4U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 4U,
		.maximum_edge_records = 16U, .maximum_adjacency_page_records = 4U,
		.maximum_property_entries = 2U, .maximum_rows = 16U,
		.maximum_distinct_rows = 16U, .maximum_materialized_bytes = 4096U};
	graph::parser::cypher::CypherParserImpl parser;
	for (const std::string query: {
		"MATCH (n:Admission) UNWIND [1, 2] AS repeat RETURN n.payload AS payload",
		"MATCH ()-[r:ADMISSION]->() UNWIND [1, 2] AS repeat RETURN r.payload AS payload"}) {
		const auto plan = parser.parseToLogical(query);
		for (const bool stop: {false, true, false}) {
			std::vector<PropertyValue> actual;
			const bool traceEnabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset();
			graph::debug::PerfTrace::setValueCollectionEnabled(true);
			EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, bounds, [&](const execution::LogicalPlanReadRow &row) {
				actual.push_back(std::get<PropertyValue>(row.values.at("payload")));
				return !stop;
			})));
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(traceEnabled);
			ASSERT_TRUE(trace.contains("logical_plan.property_source.admitted_bytes"));
			EXPECT_GE(trace.at("logical_plan.property_source.admitted_bytes").totalValue, 514);
			std::vector<PropertyValue> expected;
			for (char value: {'a', 'a', 'b', 'b', 'c', 'c'}) expected.emplace_back(std::string(500U, value));
			if (stop) expected.resize(1U);
			EXPECT_EQ(actual, expected);
		}
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, OverlayColumnAdmissionMergesBaseReplacementsAndCreatesWithoutPublication) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto nodes = backend.createLogicalNodes({
		{{"OverlayAdmission"}, {{"payload", PropertyValue(std::string(500U, 'a'))}}},
		{{"OverlayAdmission"}, {{"payload", PropertyValue("old")}}}});
	ASSERT_EQ(nodes.size(), 2U);
	const auto edges = backend.createLogicalEdges({
		{nodes[0], nodes[0], "OVERLAY_ADMISSION", {{"payload", PropertyValue(std::string(500U, 'a'))}}},
		{nodes[1], nodes[1], "OVERLAY_ADMISSION", {{"payload", PropertyValue("old")}}}});
	ASSERT_EQ(edges.size(), 2U);
	auto transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphTransactionOverlay overlay(*transaction);
	auto vectorSnapshot = overlay.openLogicalGraphVectorSearchSnapshot();
	ASSERT_TRUE(vectorSnapshot->supportsPropertyReadAdmission());
	const std::array<uint64_t, 1U> cleanIds{nodes[0]};
	const std::array<std::string_view, 1U> cleanKeys{"payload"};
	size_t cleanBytes = 0U;
	const auto cleanColumns = vectorSnapshot->readNodePropertyColumnsSortedWithAdmission(
		cleanIds, cleanKeys, [&](size_t bytes) { cleanBytes += bytes; });
	EXPECT_EQ(cleanBytes, 514U);
	EXPECT_EQ(cleanColumns[0][0], PropertyValue(std::string(500U, 'a')));
	overlay.replaceLogicalNodeProperties({{nodes[1], {{"payload", PropertyValue(std::string(500U, 'b'))}, {"nullable", PropertyValue()}}}});
	overlay.replaceLogicalEdgeProperties({{edges[1], {{"payload", PropertyValue(std::string(500U, 'b'))}, {"nullable", PropertyValue()}}}});
	const uint64_t created = overlay.createLogicalNodes({
		{{"OverlayAdmission"}, {{"payload", PropertyValue(std::string(500U, 'c'))}}}}).front();
	const uint64_t createdEdge = overlay.createLogicalEdges({
		{created, created, "OVERLAY_ADMISSION", {{"payload", PropertyValue(std::string(500U, 'c'))}}}}).front();
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	ASSERT_TRUE(snapshot->supportsPropertyReadAdmission());
	const std::array<std::string_view, 4U> keys{"payload", "nullable", "payload", "missing"};
	for (LogicalGraphReadSnapshot *reader: {snapshot.get(), static_cast<LogicalGraphReadSnapshot *>(vectorSnapshot.get())}) {
		ASSERT_TRUE(reader->supportsPropertyReadAdmission());
		for (const bool isEdge: {false, true}) {
			SCOPED_TRACE(isEdge);
			const std::array<uint64_t, 3U> ids = isEdge ? std::array{edges[0], edges[1], createdEdge}
					: std::array{nodes[0], nodes[1], created};
			const auto read = [&](const std::function<void(size_t)> &admission) {
				return isEdge ? reader->readEdgePropertyColumnsSortedWithAdmission(ids, keys, admission)
						: reader->readNodePropertyColumnsSortedWithAdmission(ids, keys, admission);
			};
			size_t used = 0U;
			const std::function<void(size_t)> bounded = [&](size_t bytes) {
				if (bytes > 2000U - used) throw std::runtime_error("overlay column admission exhausted");
				used += bytes;
			};
			EXPECT_THROW(static_cast<void>(read(bounded)), std::runtime_error);
			EXPECT_LE(used, 2000U);
			std::vector<size_t> admitted;
			// Zero-byte calls are cancellation checkpoints, not additional Property charges.
			const auto columns = read([&](size_t bytes) { if (bytes != 0U) admitted.push_back(bytes); });
			ASSERT_EQ(columns.size(), 4U);
			for (size_t row = 0U; row < 3U; ++row) {
				EXPECT_EQ(columns[0][row], PropertyValue(std::string(500U, static_cast<char>('a' + row))));
				EXPECT_EQ(columns[2][row], columns[0][row]);
				EXPECT_EQ(columns[1][row], PropertyValue());
				EXPECT_EQ(columns[3][row], PropertyValue());
			}
			std::ranges::sort(admitted);
			EXPECT_EQ(admitted, (std::vector<size_t>{10U, 514U, 514U, 514U, 514U, 514U, 514U}));
			EXPECT_THROW(static_cast<void>(read({})), std::invalid_argument);
		}
	}

	execution::LogicalPlanReadRuntime runtime(overlay);
	execution::LogicalPlanReadRuntimeBounds bounds{
		.maximum_node_records = 16U, .maximum_node_page_records = 4U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 4U,
		.maximum_edge_records = 16U, .maximum_adjacency_page_records = 4U,
		.maximum_property_entries = 2U, .maximum_rows = 16U,
		.maximum_distinct_rows = 16U, .maximum_materialized_bytes = 4096U};
	graph::parser::cypher::CypherParserImpl parser;
	for (const std::string query: {
		"MATCH (n:OverlayAdmission) UNWIND [1, 2] AS repeat RETURN n.payload AS payload",
		"MATCH ()-[r:OVERLAY_ADMISSION]->() UNWIND [1, 2] AS repeat RETURN r.payload AS payload"}) {
		const auto plan = parser.parseToLogical(query);
		for (const bool stop: {false, true, false}) {
			std::vector<PropertyValue> actual;
			EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, bounds, [&](const execution::LogicalPlanReadRow &row) {
				actual.push_back(std::get<PropertyValue>(row.values.at("payload")));
				return !stop;
			})));
			std::vector<PropertyValue> expected;
			for (char value: {'a', 'a', 'b', 'b', 'c', 'c'}) expected.emplace_back(std::string(500U, value));
			if (stop) expected.resize(1U);
			EXPECT_EQ(actual, expected);
		}
	}
	transaction->rollback();
	EXPECT_THROW(static_cast<void>(snapshot->supportsPropertyReadAdmission()), std::logic_error);
	EXPECT_THROW(static_cast<void>(snapshot->readNodePropertyColumnsSortedWithAdmission({}, {}, [](size_t) {})), std::logic_error);
	EXPECT_THROW(static_cast<void>(snapshot->readEdgePropertyColumnsSortedWithAdmission({}, {}, [](size_t) {})), std::logic_error);
	EXPECT_THROW(static_cast<void>(vectorSnapshot->supportsPropertyReadAdmission()), std::logic_error);
	vectorSnapshot.reset();
	snapshot.reset();
	auto persisted = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(persisted->readNodeProperty(nodes[1], "payload"), PropertyValue("old"));
	EXPECT_EQ(persisted->readEdgeProperty(edges[1], "payload"), PropertyValue("old"));
	persisted.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, NativeAndOverlayFilteredCountUseSourceAdmissionAndSplitColumns) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto nodes = backend.createLogicalNodes({{{"CountAdmission"}, {}}});
	ASSERT_EQ(nodes.size(), 1U);
	const std::string payload(1000U, 'a');
	std::vector<LogicalGraphEdgeCreateRequest> requests;
	for (char value: {'a', 'b', 'a'}) {
		requests.push_back({nodes[0], nodes[0], "COUNT_ADMISSION", {{"payload", PropertyValue(std::string(1000U, value))}}});
	}
	ASSERT_EQ(backend.createLogicalEdges(requests).size(), 3U);
	graph::parser::cypher::CypherParserImpl parser;
	const auto plan = parser.parseToLogical("MATCH ()-[r:COUNT_ADMISSION]->() WHERE r.payload = '" + payload + "' RETURN count(r) AS count");
	execution::LogicalPlanReadRuntimeBounds bounds{
		.maximum_node_records = 8U, .maximum_node_page_records = 4U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U,
		.maximum_edge_records = 8U, .maximum_adjacency_page_records = 4U,
		.maximum_property_entries = 2U, .maximum_rows = 8U,
		.maximum_distinct_rows = 8U, .maximum_materialized_bytes = 2000U};
	for (bool transactional: {false, true}) {
		auto transaction = backend.beginLogicalGraphTransaction(true);
		LogicalGraphTransactionOverlay overlay(*transaction);
		execution::LogicalPlanReadRuntime runtime(transactional ? static_cast<LogicalGraphReadBackend &>(overlay)
				: static_cast<LogicalGraphReadBackend &>(backend));
		for (bool stop: {false, true, false}) {
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset();
			graph::debug::PerfTrace::setValueCollectionEnabled(true);
			std::vector<PropertyValue> results;
			EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, bounds, [&](const execution::LogicalPlanReadRow &row) {
				results.push_back(std::get<PropertyValue>(row.values.at("count")));
				return !stop;
			})));
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			EXPECT_EQ(results, (std::vector<PropertyValue>{PropertyValue(int64_t{2})}));
			ASSERT_TRUE(trace.contains("logical_plan.direct_relationship_count_source_bytes"));
			EXPECT_GE(trace.at("logical_plan.direct_relationship_count_source_bytes").totalValue, 3042);
			ASSERT_TRUE(trace.contains("logical_plan.direct_relationship_count_split_batches"));
			EXPECT_GT(trace.at("logical_plan.direct_relationship_count_split_batches").totalValue, 0);
			EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
		}
		transaction->rollback();
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, NativeAndOverlayNodePredicatesAdmitBeforeDecodeAndSplitColumns) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	for (char value: {'a', 'b', 'a'}) requests.push_back({{"NodeAdmission"}, {{"payload", PropertyValue(std::string(1000U, value))}}});
	const auto nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), 3U);
	graph::parser::cypher::CypherParserImpl parser;
	const auto plan = parser.parseToLogical("MATCH (n:NodeAdmission {payload: '" + std::string(1000U, 'a') + "'}) RETURN id(n) AS id");
	execution::LogicalPlanReadRuntimeBounds bounds{
		.maximum_node_records = 8U, .maximum_node_page_records = 4U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U,
		.maximum_edge_records = 8U, .maximum_adjacency_page_records = 4U,
		.maximum_property_entries = 2U, .maximum_rows = 8U,
		.maximum_distinct_rows = 8U, .maximum_materialized_bytes = 2000U};
	for (bool transactional: {false, true}) {
		auto transaction = backend.beginLogicalGraphTransaction(true);
		LogicalGraphTransactionOverlay overlay(*transaction);
		execution::LogicalPlanReadRuntime runtime(transactional ? static_cast<LogicalGraphReadBackend &>(overlay)
				: static_cast<LogicalGraphReadBackend &>(backend));
		for (bool stop: {false, true, false}) {
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset();
			graph::debug::PerfTrace::setValueCollectionEnabled(true);
			std::vector<PropertyValue> results;
			EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, bounds, [&](const execution::LogicalPlanReadRow &row) {
				results.push_back(std::get<PropertyValue>(row.values.at("id"))); return !stop;
			})));
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			std::vector<PropertyValue> expected{PropertyValue(static_cast<int64_t>(nodes[0])), PropertyValue(static_cast<int64_t>(nodes[2]))};
			if (stop) expected.resize(1U);
			EXPECT_EQ(results, expected);
			ASSERT_TRUE(trace.contains("logical_plan.node_predicate_source_bytes"));
			EXPECT_GT(trace.at("logical_plan.node_predicate_source_bytes").totalValue, 0);
			ASSERT_TRUE(trace.contains("logical_plan.node_predicate_split_batches"));
			EXPECT_GT(trace.at("logical_plan.node_predicate_split_batches").totalValue, 0);
			EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
		}
		transaction->rollback();
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ExactCandidateProducerRejectsKnownPayloadBeforeValidationAndRuntimeLeavesRowHeadroom) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const PropertyValue payload(std::string(1000U, 'a'));
	const auto nodes = backend.createLogicalNodes({{{"ExactAdmission"}, {{"payload", payload}}},
		{{"ExactAdmission"}, {{"payload", payload}}}, {{"ExactAdmission"}, {{"payload", payload}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend.createLogicalNodeIndex({"exact_admission", "ExactAdmission", {"payload"}}));
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByExactProperties({"ExactAdmission"}, {{"payload", payload}},
		8U, 2U, 256U, [&](const IndexCandidateBatch &) { ++delivered; return true; })), FormatException);
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
	EXPECT_EQ(delivered, 0U);
	EXPECT_FALSE(trace.contains("native_graph.read.property_selected_decode"));
	EXPECT_FALSE(trace.contains("native_graph.read.index_candidate_verified_rows"));
	std::vector<uint64_t> ids;
	EXPECT_NO_THROW(static_cast<void>(snapshot->visitNodeCandidatesByExactProperties({"ExactAdmission"}, {{"payload", payload}},
		8U, 2U, 2200U, [&](const IndexCandidateBatch &batch) {
			EXPECT_TRUE(batch.predicates_verified);
			for (const auto &ref: batch.candidates) ids.push_back(ref.entity_id);
			return true;
		})));
	EXPECT_EQ(ids, nodes);
	graph::parser::cypher::CypherParserImpl parser;
	const auto plan = parser.parseToLogical("MATCH (n:ExactAdmission {payload: '" + std::string(1000U, 'a') + "'}) RETURN n.payload AS payload");
	execution::LogicalPlanReadRuntime runtime(backend);
	execution::LogicalPlanReadRuntimeBounds bounds{
		.maximum_node_records = 8U, .maximum_node_page_records = 4U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U,
		.maximum_edge_records = 8U, .maximum_adjacency_page_records = 4U,
		.maximum_property_entries = 2U, .maximum_rows = 8U,
		.maximum_distinct_rows = 8U, .maximum_materialized_bytes = 7000U};
	for (bool stop: {false, true, false}) {
		std::vector<PropertyValue> values;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, bounds, [&](const execution::LogicalPlanReadRow &row) {
			values.push_back(std::get<PropertyValue>(row.values.at("payload"))); return !stop;
		})));
		EXPECT_EQ(values, std::vector<PropertyValue>(stop ? 1U : 3U, payload));
	}
	snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, ScalarRangeSourceRejectsBeforeDecodeAndRetriesColdAndCachedColumns) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const PropertyValue payload(std::string(1000U, 'a'));
	const auto nodes = backend.createLogicalNodes({{{"RangeAdmission"}, {{"payload", payload}}},
		{{"RangeAdmission"}, {{"payload", payload}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend.createLogicalNodeIndex({"range_admission", "RangeAdmission", {"payload"}}));
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphPropertyRange range{"payload", payload, PropertyValue(), true, true};
	for (size_t attempt = 0U; attempt < 2U; ++attempt) {
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByPropertyRange({"RangeAdmission"}, range,
			8U, 2U, 256U, [&](const IndexCandidateBatch &) { ++delivered; return true; })), FormatException);
		const auto rejected = graph::debug::PerfTrace::snapshotAndReset();
		EXPECT_EQ(delivered, 0U);
		// The selected-decode metric counts entry, before payload admission.
		EXPECT_FALSE(rejected.contains("native_graph.read.range_candidate_source_bytes"));
		EXPECT_FALSE(rejected.contains("native_graph.read.index_candidate_liveness_probes"));
		// Borrowing avoids the cache copy: candidates fit within 4 KiB on initial and repeated reads.
		std::vector<uint64_t> compactIds;
		EXPECT_NO_THROW(static_cast<void>(snapshot->visitNodeCandidatesByPropertyRange({"RangeAdmission"}, range,
			8U, 2U, 4096U, [&](const IndexCandidateBatch &batch) {
				for (const auto &ref: batch.candidates) compactIds.push_back(ref.entity_id);
				return true;
			})));
		EXPECT_EQ(compactIds, nodes);
		std::vector<uint64_t> ids;
		EXPECT_NO_THROW(static_cast<void>(snapshot->visitNodeCandidatesByPropertyRange({"RangeAdmission"}, range,
			8U, 2U, 64U * 1024U, [&](const IndexCandidateBatch &batch) {
				for (const auto &ref: batch.candidates) ids.push_back(ref.entity_id); return true;
			})));
		const auto accepted = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		EXPECT_EQ(ids, nodes);
		ASSERT_TRUE(accepted.contains("native_graph.read.range_candidate_source_bytes"));
		EXPECT_GE(accepted.at("native_graph.read.range_candidate_source_bytes").totalValue, 2028); // Splits may retry decoding.
	}
	graph::parser::cypher::CypherParserImpl parser;
	const auto plan = parser.parseToLogical("MATCH (n:RangeAdmission) WHERE n.payload >= '" + std::string(1000U, 'a') + "' RETURN n.payload AS payload");
	execution::LogicalPlanReadRuntime runtime(backend);
	execution::LogicalPlanReadRuntimeBounds bounds{
		.maximum_node_records = 8U, .maximum_node_page_records = 4U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U,
		.maximum_edge_records = 8U, .maximum_adjacency_page_records = 4U,
		.maximum_property_entries = 2U, .maximum_rows = 8U,
		.maximum_distinct_rows = 8U, .maximum_materialized_bytes = 32U * 1024U, .maximum_candidate_bytes = 12U * 1024U};
	for (bool stop: {false, true, false}) {
		std::vector<PropertyValue> values;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, bounds, [&](const execution::LogicalPlanReadRow &row) {
			values.push_back(std::get<PropertyValue>(row.values.at("payload"))); return !stop;
		})));
		EXPECT_EQ(values, std::vector<PropertyValue>(stop ? 1U : 2U, payload));
	}
	snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, CompositeRangeSourcePreservesFieldAndRowOrderAcrossAdmissionRetryAndCancellation) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const PropertyValue tenant(std::string(1000U, 'a'));
	std::vector<LogicalGraphNodeCreateRequest> requests;
	for (int64_t rank: {3, 1, 2}) requests.push_back({{"CompositeAdmission"}, {{"rank", PropertyValue(rank)}, {"tenant", tenant}}});
	const auto nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend.createLogicalNodeIndex({"composite_admission", "CompositeAdmission", {"tenant", "rank"}}));
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphCompositePropertyRange request{{{"tenant", tenant}}, {"rank", PropertyValue(1), PropertyValue(3), true, true}};
	for (size_t retry = 0U; retry < 2U; ++retry) {
		const auto complete = snapshot->findNodeCandidatesByCompositePropertyRange({"CompositeAdmission"}, request, 8U, 2U);
		ASSERT_TRUE(complete.has_value());
		ASSERT_EQ(complete->candidates.size(), nodes.size());
		ASSERT_EQ(complete->verified_property_columns.size(), 2U);
		for (size_t index = 0U; index < nodes.size(); ++index) {
			EXPECT_EQ(complete->candidates[index].entity_id, nodes[index]);
			EXPECT_EQ(complete->verified_property_columns[0].values[index], tenant);
			EXPECT_EQ(complete->verified_property_columns[1].values[index], PropertyValue(index == 0U ? 3 : index == 1U ? 1 : 2));
		}
	}
	for (size_t attempt = 0U; attempt < 2U; ++attempt) {
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByCompositePropertyRange({"CompositeAdmission"}, request,
			8U, 2U, 256U, [&](const IndexCandidateBatch &) { ++delivered; return true; })), FormatException);
		const auto rejected = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		EXPECT_EQ(delivered, 0U);
		if (rejected.contains("native_graph.read.composite_range_candidate_source_bytes")) {
			EXPECT_LE(rejected.at("native_graph.read.composite_range_candidate_source_bytes").totalValue, 256);
		}
		// Borrowed count pages avoid a full-page copy even after values are cached.
		// Include pending keys and candidate/token arrays in each allowance.
		for (size_t allowance: {size_t{4096U}, size_t{12U * 1024U}, size_t{64U * 1024U}}) {
			for (bool stop: {false, true, false}) {
				std::vector<uint64_t> ids;
				EXPECT_NO_THROW(static_cast<void>(snapshot->visitNodeCandidatesByCompositePropertyRange({"CompositeAdmission"}, request,
					8U, 2U, allowance, [&](const IndexCandidateBatch &batch) {
						if (batch.verified_property_columns.size() != 2U) { ADD_FAILURE() << "invalid column count"; return false; }
						EXPECT_EQ(batch.verified_property_columns[0].key, "tenant");
						EXPECT_EQ(batch.verified_property_columns[1].key, "rank");
						for (size_t index = 0U; index < batch.candidates.size(); ++index) {
							const auto id = batch.candidates[index].entity_id;
							ids.push_back(id);
							EXPECT_EQ(batch.verified_property_columns[0].values[index], tenant);
							const int64_t rank = id == nodes[0] ? 3 : id == nodes[1] ? 1 : 2;
							EXPECT_EQ(batch.verified_property_columns[1].values[index], PropertyValue(rank));
						}
						return !stop;
					})));
				std::vector<uint64_t> expected = allowance <= 12U * 1024U ? std::vector<uint64_t>{nodes[1], nodes[2], nodes[0]} : nodes;
				if (stop && allowance <= 12U * 1024U) expected.resize(1U);
				EXPECT_EQ(ids, expected);
			}
		}
	}
	namespace logical = graph::query::logical;
	namespace expressions = graph::query::expressions;
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"CompositeAdmission"},
		std::vector<std::pair<std::string, PropertyValue>>{{"tenant", tenant}});
	scan->setRangePredicates({{"rank", PropertyValue(1), PropertyValue(3), true, true}});
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "tenant"), "tenant");
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank");
	logical::LogicalProject plan(std::move(scan), std::move(items));
	execution::LogicalPlanReadRuntime runtime(backend);
	execution::LogicalPlanReadRuntimeBounds bounds{
		.maximum_node_records = 8U, .maximum_node_page_records = 4U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U,
		.maximum_edge_records = 8U, .maximum_adjacency_page_records = 4U,
		.maximum_property_entries = 2U, .maximum_rows = 8U,
		.maximum_distinct_rows = 8U, .maximum_materialized_bytes = 32U * 1024U, .maximum_candidate_bytes = 12U * 1024U};
	for (bool stop: {false, true, false}) {
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
		std::vector<PropertyValue> ranks;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(plan, bounds, [&](const execution::LogicalPlanReadRow &row) {
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("tenant")), tenant);
			ranks.push_back(std::get<PropertyValue>(row.values.at("rank"))); return !stop;
		})));
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		ASSERT_TRUE(trace.contains("native_graph.read.composite_range_candidate_source_bytes"));
		EXPECT_GT(trace.at("native_graph.read.composite_range_candidate_source_bytes").totalValue, 0);
		std::vector<PropertyValue> expected{PropertyValue(1), PropertyValue(2), PropertyValue(3)};
		if (stop) expected.resize(1U);
		EXPECT_EQ(ranks, expected);
	}
	snapshot.reset(); backend.close();
}
TEST_F(RelocatableStorageBackendTest, ShapedPropertySourceAdmissionRejectsAndRetriesNativeNodesAndEdges) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	backend.open();
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_TRUE(snapshot->supportsShapedPropertyReadAdmission());
	const EntityReadShape complete{.all_properties = true};
	const std::array<uint64_t, 1U> nodes{fixture.person_node_id};
	const std::array<uint64_t, 1U> edges{fixture.knows_edge_id};
	const std::function<void(size_t)> reject = [](size_t) { throw std::length_error("native shaped source rejected"); };
	EXPECT_THROW(static_cast<void>(snapshot->readNodesShapedSortedWithAdmission(nodes, complete, reject)), std::length_error);
	EXPECT_THROW(static_cast<void>(snapshot->readEdgesShapedSortedWithAdmission(edges, complete, reject)), std::length_error);
	const auto expectedNodes = snapshot->readNodesShapedSorted(nodes, complete);
	const auto expectedEdges = snapshot->readEdgesShapedSorted(edges, complete);
	for (int retry = 0; retry < 2; ++retry) {
		size_t calls = 0U;
		const std::function<void(size_t)> accept = [&](size_t bytes) { ++calls; EXPECT_GT(bytes, sizeof(uint32_t)); };
		const auto actualNodes = snapshot->readNodesShapedSortedWithAdmission(nodes, complete, accept);
		ASSERT_EQ(actualNodes.size(), 1U);
		EXPECT_EQ(actualNodes[0].properties, expectedNodes[0].properties);
		EXPECT_EQ(calls, 2U);
		const auto actualEdges = snapshot->readEdgesShapedSortedWithAdmission(edges, complete, accept);
		ASSERT_EQ(actualEdges.size(), 1U);
		EXPECT_EQ(actualEdges[0].properties, expectedEdges[0].properties);
		EXPECT_EQ(calls, 3U);
	}
	EXPECT_THROW(static_cast<void>(snapshot->readNodesShapedSortedWithAdmission(nodes, complete, {})), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(snapshot->readEdgesShapedSortedWithAdmission(edges, complete, {})), std::invalid_argument);
}
TEST_F(RelocatableStorageBackendTest, TopologyBufferAdmissionForwardsThroughNativeSnapshotAndUnchangedOverlay) {
	const auto fixture = materializeNativeGraphFixture();
	auto config = backendOptions();
	config.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(config);
	backend.open();
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_TRUE(snapshot->supportsAdjacencyTopologyBufferAdmission());
	EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgeTopologiesWithBufferAdmission(
		fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 2U,
		[](size_t) { throw std::runtime_error("topology allocation rejected"); })), std::runtime_error);
	size_t retainedBytes = 0U;
	size_t peakBytes = 0U;
	const auto page = snapshot->scanAdjacentEdgeTopologiesWithBufferAdmission(
		fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 2U,
		[&](size_t bytes) { retainedBytes = bytes; peakBytes = std::max(peakBytes, bytes); });
	ASSERT_EQ(page.edges.size(), 2U);
	size_t expectedBytes = page.edges.capacity() * sizeof(LogicalGraphEdgeTopology);
	for (const auto &edge: page.edges) expectedBytes += edge.type.capacity();
	EXPECT_EQ(retainedBytes, expectedBytes);
	EXPECT_GT(peakBytes, retainedBytes);
	auto transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphTransactionOverlay overlay(*transaction);
	auto unchanged = overlay.openLogicalGraphReadSnapshot();
	ASSERT_TRUE(unchanged->supportsAdjacencyTopologyBufferAdmission());
	const auto repeated = unchanged->scanAdjacentEdgeTopologiesWithBufferAdmission(
		fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 2U,
		[&](size_t bytes) { retainedBytes = bytes; });
	EXPECT_EQ(repeated.edges, page.edges);
	EXPECT_EQ(retainedBytes, expectedBytes);
	ASSERT_EQ(overlay.createLogicalNodes({{{"Staged"}, {}}}).size(), 1U);
	auto changed = overlay.openLogicalGraphReadSnapshot();
	ASSERT_TRUE(changed->supportsAdjacencyTopologyBufferAdmission());
	const auto merged = changed->scanAdjacentEdgeTopologiesWithBufferAdmission(
		fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 2U,
		[&](size_t bytes) { retainedBytes = bytes; });
	EXPECT_EQ(merged.edges, page.edges);
	size_t mergedBytes = merged.edges.capacity() * sizeof(LogicalGraphEdgeTopology);
	for (const auto &edge: merged.edges) mergedBytes += edge.type.capacity();
	EXPECT_EQ(retainedBytes, mergedBytes);
	transaction->rollback();
}
TEST_F(RelocatableStorageBackendTest, ModifiedOverlayTopologyAdmitsMergeLookaheadAndRetriesContinuation) {
	const auto fixture = materializeNativeGraphFixture();
	auto config = backendOptions();
	config.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(config);
	backend.open();
	for (const bool vectorSnapshot: {false, true}) {
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		const std::string type(128U, 't');
		const auto created = overlay.createLogicalEdges({
			{fixture.person_node_id, fixture.person_node_id, type, {}},
			{fixture.person_node_id, fixture.person_node_id, type, {}},
			{fixture.person_node_id, fixture.person_node_id, type, {}}});
		ASSERT_EQ(created.size(), 3U);
		auto snapshot = vectorSnapshot
			? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
			: overlay.openLogicalGraphReadSnapshot();
		ASSERT_TRUE(snapshot->supportsAdjacencyTopologyBufferAdmission());
		const auto bytesOf = [](const auto &page) {
			size_t bytes = page.edges.capacity() * sizeof(LogicalGraphEdgeTopology);
			for (const auto &edge: page.edges) bytes += edge.type.capacity();
			return bytes;
		};
		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgeTopologiesWithBufferAdmission(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 8U,
			[](size_t bytes) { if (bytes > 256U) throw std::runtime_error("merge budget"); })), std::runtime_error);
		size_t retained = 0U;
		size_t peak = 0U;
		const auto admit = [&](size_t bytes) { retained = bytes; peak = std::max(peak, bytes); };
		const auto first = snapshot->scanAdjacentEdgeTopologiesWithBufferAdmission(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 2U, admit);
		ASSERT_EQ(first.edges.size(), 2U);
		ASSERT_TRUE(first.continuation.has_value());
		EXPECT_EQ(retained, bytesOf(first));
		EXPECT_GT(peak, retained);
		const auto continuation = first.continuation;
		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgeTopologiesWithBufferAdmission(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, continuation, 2U,
			[](size_t) { throw std::runtime_error("pending row budget"); })), std::runtime_error);
		const auto tail = snapshot->scanAdjacentEdgeTopologiesWithBufferAdmission(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, continuation, 2U, admit);
		ASSERT_EQ(tail.edges.size(), 2U);
		EXPECT_EQ(tail.edges[0].edge_id, created[0]);
		EXPECT_EQ(tail.edges[1].edge_id, created[1]);
		EXPECT_EQ(tail.edges[0].type, type);
		EXPECT_EQ(tail.edges[1].type, type);
		EXPECT_EQ(retained, bytesOf(tail));
		ASSERT_TRUE(tail.continuation.has_value());
		const auto last = snapshot->scanAdjacentEdgeTopologiesWithBufferAdmission(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, tail.continuation, 2U, admit);
		ASSERT_EQ(last.edges.size(), 1U);
		EXPECT_EQ(last.edges[0].edge_id, created[2]);
		EXPECT_FALSE(last.continuation.has_value());
		EXPECT_EQ(retained, bytesOf(last));
		const auto replay = snapshot->scanAdjacentEdgeTopologiesWithBufferAdmission(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, continuation, 3U, admit);
		ASSERT_EQ(replay.edges.size(), 3U);
		EXPECT_FALSE(replay.continuation.has_value());
		EXPECT_EQ(replay.edges[0], tail.edges[0]);
		EXPECT_EQ(replay.edges[1], tail.edges[1]);
		EXPECT_EQ(replay.edges[2], last.edges[0]);
		EXPECT_EQ(retained, bytesOf(replay));
		const auto nodes = overlay.createLogicalNodes({{{"Staged"}, {}}});
		ASSERT_EQ(nodes.size(), 1U);
		const auto isolatedEdges = overlay.createLogicalEdges({{nodes[0], nodes[0], type, {}}});
		ASSERT_EQ(isolatedEdges.size(), 1U);
		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgeTopologiesWithBufferAdmission(
			nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U,
			[&](size_t bytes) {
				if (bytes > sizeof(LogicalGraphEdgeTopology) + type.size() - 1U)
					throw std::runtime_error("created type copy budget");
			})), std::runtime_error);
		const auto isolated = snapshot->scanAdjacentEdgeTopologiesWithBufferAdmission(
			nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U, admit);
		ASSERT_EQ(isolated.edges.size(), 1U);
		EXPECT_EQ(isolated.edges[0].edge_id, isolatedEdges[0]);
		EXPECT_EQ(isolated.edges[0].type, type);
		EXPECT_EQ(retained, bytesOf(isolated));
		EXPECT_FALSE(isolated.continuation.has_value());
		transaction->rollback();
	}
}
TEST_F(RelocatableStorageBackendTest, NativeShortestPathRetriesSourceBufferAdmissionAcrossTopologyPages) {
	using namespace graph::query;
	const auto fixture = materializeNativeGraphFixture();
	auto config = backendOptions();
	config.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(config);
	backend.open();
	std::vector<LogicalGraphEdgeCreateRequest> edges;
	for (size_t index = 0U; index < 199U; ++index) {
		edges.push_back({fixture.person_node_id, fixture.person_node_id, "KNOWS", {}});
	}
	edges.push_back({fixture.person_node_id, fixture.unlabeled_node_id, "KNOWS", {}});
	ASSERT_EQ(backend.createLogicalEdges(edges).size(), edges.size());
	const auto step = std::make_shared<expressions::VariableReferenceExpression>("step");
	const auto length = std::make_shared<expressions::VariableReferenceExpression>("path_length");
	logical::LogicalProject project(std::make_unique<logical::LogicalCallProcedure>("algo.shortestPath",
		std::vector<PropertyValue>{PropertyValue(static_cast<int64_t>(fixture.person_node_id)),
			PropertyValue(static_cast<int64_t>(fixture.unlabeled_node_id)), PropertyValue(1)}),
		{{step, "step"}, {length, "length"}});
	LogicalReadExecutionOptions options;
	options.bounds.maximum_materialized_bytes = 8192U;
	execution::LogicalPlanProcedureRuntime runtime(backend, nullptr, nullptr);
	const bool oldValues = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	const auto result = runtime.execute(project, options.bounds, options.maximum_vector_mapping_entries,
		options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
		options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
		options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
		options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(oldValues);
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows()[1].at("step").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(result.getRows()[1].at("length").asPrimitive(), PropertyValue(2));
	ASSERT_TRUE(trace.contains("native_graph.read.adjacency_topology_scan_calls"));
	EXPECT_GT(trace.at("native_graph.read.adjacency_topology_scan_calls").totalValue, 2);
	EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
}
TEST_F(RelocatableStorageBackendTest, NamedPropertyColumnsValidateIdsEvenWhenAllKeysAreUnknown) {
	const auto fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	backend.open();
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	for (const bool edge: {false, true}) {
		size_t admissions = 0U;
		const std::function<void(size_t)> admission = [&](size_t) { ++admissions; };
		const auto read = [&](std::span<const uint64_t> ids, std::span<const std::string_view> keys) {
			return edge ? snapshot->readEdgePropertyColumnsSortedWithAdmission(ids, keys, admission)
				: snapshot->readNodePropertyColumnsSortedWithAdmission(ids, keys, admission);
		};
		const std::array<std::string_view, 1U> unknown{"not_a_persisted_property"};
		for (const auto &ids: {std::vector<uint64_t>{0U}, std::vector<uint64_t>{2U, 1U},
			std::vector<uint64_t>{1U, 1U}})
			EXPECT_THROW(static_cast<void>(read(ids, unknown)), FormatException);
		const std::array<std::string_view, 1U> emptyKey{""};
		EXPECT_THROW(static_cast<void>(read({}, emptyKey)), FormatException);
		EXPECT_EQ(admissions, 0U);
		const std::array<uint64_t, 1U> ids{edge ? fixture.knows_edge_id : fixture.person_node_id};
		EXPECT_TRUE(read(ids, {}).empty());
		const auto missing = read(ids, unknown);
		ASSERT_EQ(missing.size(), 1U);
		EXPECT_EQ(missing.front(), (std::vector<PropertyValue>{PropertyValue()}));
		EXPECT_EQ(admissions, 0U);
		const std::array<std::string_view, 5U> mixedKeys{"beta", "unknown", "alpha", "beta", "alpha"};
		auto mixed = read(ids, mixedKeys);
		ASSERT_EQ(mixed.size(), mixedKeys.size());
		const PropertyValue beta = edge ? PropertyValue() : PropertyValue("value");
		EXPECT_EQ(mixed[0][0], beta);
		EXPECT_EQ(mixed[1][0], PropertyValue());
		EXPECT_EQ(mixed[2][0], PropertyValue(int64_t(edge ? 7 : 42)));
		EXPECT_EQ(mixed[3][0], beta);
		EXPECT_EQ(mixed[4][0], mixed[2][0]);
		mixed[0][0] = PropertyValue("changed");
		EXPECT_EQ(mixed[3][0], beta);
	}
}
TEST_F(RelocatableStorageBackendTest, NativeColumnsGatherOnlyColdOwnersAndRestoreInterleavedPositions) {
	materializeNativeDatabase();
	auto config = backendOptions();
	config.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(config);
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	for (int64_t value = 0; value < 5; ++value)
		requests.push_back({{}, {{"alpha", PropertyValue(value)}, {"beta", PropertyValue()}}});
	const auto nodes = backend.createLogicalNodes(requests);
	std::vector<LogicalGraphEdgeCreateRequest> edgeRequests;
	for (size_t index = 0U; index < nodes.size(); ++index)
		edgeRequests.push_back({nodes[index], nodes[index], "STAGING",
			{{"alpha", PropertyValue(static_cast<int64_t>(index))}, {"beta", PropertyValue()}}});
	const auto edges = backend.createLogicalEdges(edgeRequests);
	const std::array<std::string_view, 2U> keys{"alpha", "beta"};
	for (const bool edge: {false, true}) {
		auto snapshot = backend.openLogicalGraphReadSnapshot();
		const auto &ids = edge ? edges : nodes;
		const std::array<uint64_t, 3U> warm{ids[0], ids[2], ids[4]};
		const auto read = [&](std::span<const uint64_t> owners) {
			return edge ? snapshot->readEdgePropertyColumnsSorted(owners, keys)
				: snapshot->readNodePropertyColumnsSorted(owners, keys);
		};
		static_cast<void>(read(warm));
		for (const int64_t expectedCapacity: {2, 0}) {
			const bool wasEnabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::setValueCollectionEnabled(true);
			graph::debug::PerfTrace::reset();
			const auto columns = read(ids);
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(wasEnabled);
			ASSERT_EQ(columns.size(), 2U);
			ASSERT_EQ(columns[0].size(), ids.size());
			ASSERT_EQ(columns[1].size(), ids.size());
			for (size_t index = 0U; index < ids.size(); ++index) {
				EXPECT_EQ(columns[0][index], PropertyValue(static_cast<int64_t>(index)));
				EXPECT_EQ(columns[1][index], PropertyValue());
			}
			EXPECT_EQ(trace.at("native_graph.read.property_column_gather_capacity").totalValue, expectedCapacity);
		}
	}
}

TEST_F(RelocatableStorageBackendTest, ScalarRangeBoundsLargeValueBuffersWithSmallSelectedProperty) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto ids = backend.createLogicalNodes({{{"LargeRangeBuffers"},
		{{"rank", PropertyValue(int64_t{7})}, {"payload", PropertyValue(std::string(16000U, 'p'))}}}});
	ASSERT_EQ(ids.size(), 1U);
	static_cast<void>(backend.createLogicalNodeIndex({"large_range_buffers", "LargeRangeBuffers", {"rank"}}));
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphPropertyRange range{"rank", PropertyValue(int64_t{7}), PropertyValue(), true, true};
	for (int retry = 0; retry < 2; ++retry) {
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByPropertyRange({"LargeRangeBuffers"}, range,
			8U, 4U, 4096U, [&](const IndexCandidateBatch &) { ++delivered; return true; })), FormatException);
		EXPECT_EQ(delivered, 0U);
		std::vector<uint64_t> found;
		EXPECT_NO_THROW(static_cast<void>(snapshot->visitNodeCandidatesByPropertyRange({"LargeRangeBuffers"}, range,
			8U, 4U, 65536U, [&](const IndexCandidateBatch &batch) {
				for (const auto &candidate : batch.candidates) found.push_back(candidate.entity_id);
				return true;
			})));
		EXPECT_EQ(found, ids);
	}
	snapshot.reset(); backend.close();
}
