/**
 * @file test_RelocatableLogicalGraphReadSnapshot.cpp
 * @brief Public logical-read snapshot continuation-cache integration tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include "CypherParserImpl.hpp"

#include "graph/debug/PerfTrace.hpp"
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
			.maximum_materialized_bytes = 512U};
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
