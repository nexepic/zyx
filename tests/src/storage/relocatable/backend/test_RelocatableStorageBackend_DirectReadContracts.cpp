/**
 * @file test_RelocatableStorageBackend_DirectReadContracts.cpp
 * @brief Direct native-backend read and schema convenience contracts.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <chrono>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include "CypherParserImpl.hpp"
#include "graph/debug/PerfTrace.hpp"

using namespace graph::storage;
using namespace relocatable_storage_backend_test;

TEST(RelocatableReadSnapshotTest, DefaultSnapshotIsInvalidUntilBackedByAPinnedBackendGeneration) {
	const RelocatableReadSnapshot snapshot;
	EXPECT_FALSE(snapshot.isValid());
}

TEST(RelocatableStorageBackendContractTest, ReportsFixedCapabilitiesAndClampsBulkAdmissionLimits) {
	RelocatableStorageBackendOptions options;
	options.maximum_bulk_edge_transaction_records = 64U;
	options.bulk_edge_id_lease_block_records = 128U;
	options.maximum_bulk_edge_append_records = 128U;
	options.bulk_edge_sort_run_bytes = RSS_SEGMENT_SIZE;
	options.maximum_bulk_edge_sort_runs = 3U;
	options.maximum_bulk_edge_candidate_pages = 5U;
	options.maximum_bulk_edge_append_bytes = 4096U;
	RelocatableStorageBackend backend(std::move(options));

	EXPECT_FALSE(backend.isOpen());
	EXPECT_TRUE(backend.isReadOnly());
	EXPECT_EQ(backend.formatKind(), StorageFormatKind::SFM_RELOCATABLE);
	EXPECT_EQ(backend.capabilities(), (StorageBackendCapabilities{true, true, true, true, false}));
	EXPECT_FALSE(backend.hasIndeterminateWriteOutcome());
	EXPECT_FALSE(backend.maintenanceEnabled());
	EXPECT_FALSE(backend.nativeMaintenanceAvailable());
	EXPECT_EQ(backend.options().maximum_bulk_edge_transaction_records, 64U);
	EXPECT_EQ(backend.maximumNativeBulkEdgeTransactionRecords(), 64U);
	EXPECT_EQ(backend.nativeBulkEdgeIdLeaseBlockRecords(), 64U);
	EXPECT_EQ(backend.maximumNativeBulkEdgeAppendRecords(), 64U);
	EXPECT_EQ(backend.maximumNativeBulkEdgeAppendBytes(), 4096U);
	const NativeBulkEdgePublicationPreflightOptions preflight = backend.nativeBulkEdgePublicationPreflightOptions();
	EXPECT_EQ(preflight.maximum_records, 64U);
	EXPECT_EQ(preflight.sort_run_bytes, static_cast<size_t>(RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES));
	EXPECT_EQ(preflight.maximum_sort_runs, 3U);
	EXPECT_EQ(preflight.maximum_candidate_fragments, 5U);
}

TEST(RelocatableStorageBackendContractTest, ReportsWritableBeforeOpeningACreateModeBackend) {
	RelocatableStorageBackendOptions options;
	options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	RelocatableStorageBackend backend(std::move(options));

	EXPECT_FALSE(backend.isReadOnly());
}

TEST_F(RelocatableStorageBackendTest, ReadSnapshotRetainsItsPinAcrossMovesAndBecomesInvalidOnRelease) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	const RelocatableStorageBackendOptions reopenOptions = options;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	RelocatableReadSnapshot original = backend.openSnapshot();
	ASSERT_TRUE(original.isValid());
	EXPECT_TRUE(original.superblock().generation.isValid());
	EXPECT_TRUE(original.metadataSnapshot().isValid());

	RelocatableReadSnapshot moved(std::move(original));
	EXPECT_FALSE(original.isValid());
	ASSERT_TRUE(moved.isValid());

	RelocatableReadSnapshot assigned;
	assigned = std::move(moved);
	EXPECT_FALSE(moved.isValid());
	ASSERT_TRUE(assigned.isValid());
	assigned.release();
	EXPECT_FALSE(assigned.isValid());
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalPlanRuntimeExecutesRelocatableNodeAndAdjacencyPagesOnOneSnapshot) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	ASSERT_NO_THROW(backend.open());
	execution::LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	logical::LogicalTraversal root(std::move(scan), "n", "r", "m", "", "out");
	const execution::LogicalPlanReadRuntimeBounds bounds{
			.maximum_node_records = 2U,
			.maximum_node_page_records = 1U,
			.maximum_metadata_pages_per_node_page = 8U,
			.maximum_edges_per_node = 2U,
			.maximum_edge_records = 4U,
			.maximum_adjacency_page_records = 1U,
			.maximum_property_entries = 2U,
			.maximum_rows = 4U,
			.maximum_distinct_rows = 4U,
	};
	std::vector<uint64_t> edgeIds;
	const execution::LogicalPlanReadRuntimeStats stats =
			runtime.execute(root, bounds, [&edgeIds](const execution::LogicalPlanReadRow &row) {
				edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
				return true;
			});
	std::ranges::sort(edgeIds);
	EXPECT_EQ(edgeIds, (std::vector<uint64_t>{fixture.knows_edge_id, fixture.mentions_edge_id}));
	EXPECT_EQ(stats, (execution::LogicalPlanReadRuntimeStats{2U, 2U, 0U, 2U}));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalPlanRuntimeUsesNativeScalarAndCompositeRangeIndexes) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{30})}}},
			 { {"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{25})}}},
			 { {"Person"}, {{"city", graph::PropertyValue("Beijing")}, {"age", graph::PropertyValue(int64_t{35})}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_age", "Person", {"age"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));

	graph::parser::cypher::CypherParserImpl parser;
	execution::LogicalPlanReadRuntime runtime(backend);
	const execution::LogicalPlanReadRuntimeBounds bounds{
			.maximum_node_records = 8U,
			.maximum_node_page_records = 2U,
			.maximum_metadata_pages_per_node_page = 8U,
			.maximum_edges_per_node = 2U,
			.maximum_edge_records = 8U,
			.maximum_adjacency_page_records = 2U,
			.maximum_property_entries = 4U,
			.maximum_rows = 8U,
			.maximum_distinct_rows = 8U,
	};

	const graph::query::QueryPlan scalarPlan = parser.parseToLogical(
			"MATCH (n:Person) WHERE n.age >= 25 AND n.age < 35 RETURN n.age AS age");
	ASSERT_NE(scalarPlan.root, nullptr);
	const graph::query::QueryResult scalar = runtime.executeToQueryResult(*scalarPlan.root, bounds);
	ASSERT_EQ(scalar.rowCount(), 2U);
	std::vector<int64_t> ages;
	for (const auto &row: scalar.getRows()) {
		ages.push_back(std::get<int64_t>(row.at("age").asPrimitive().getVariant()));
	}
	std::ranges::sort(ages);
	EXPECT_EQ(ages, (std::vector<int64_t>{25, 30}));

	const graph::query::QueryPlan compositePlan = parser.parseToLogical(
			"MATCH (n:Person) WHERE n.city = 'Shanghai' AND n.age >= 30 RETURN n.age AS age");
	ASSERT_NE(compositePlan.root, nullptr);
	const graph::query::QueryResult composite = runtime.executeToQueryResult(*compositePlan.root, bounds);
	ASSERT_EQ(composite.rowCount(), 1U);
	EXPECT_EQ(composite.getRows().front().at("age").asPrimitive(), graph::PropertyValue(int64_t{30}));

	// An equality predicate that has no matching single-property index must use
	// the bounded paged scan while preserving the same public result.
	const graph::query::QueryPlan fallbackPlan = parser.parseToLogical(
			"MATCH (n:Person) WHERE n.city = 'Shanghai' RETURN n.city AS city");
	ASSERT_NE(fallbackPlan.root, nullptr);
	const graph::query::QueryResult fallback = runtime.executeToQueryResult(*fallbackPlan.root, bounds);
	EXPECT_EQ(fallback.rowCount(), 2U);

	const graph::query::QueryPlan openRangePlan = parser.parseToLogical(
			"MATCH (n:Person) WHERE n.age < 35 RETURN n.age AS age");
	ASSERT_NE(openRangePlan.root, nullptr);
	const graph::query::QueryResult openRange = runtime.executeToQueryResult(*openRangePlan.root, bounds);
	EXPECT_EQ(openRange.rowCount(), 2U);

	const graph::query::QueryPlan exclusiveCompositePlan = parser.parseToLogical(
			"MATCH (n:Person) WHERE n.city = 'Shanghai' AND n.age > 25 AND n.age < 30 RETURN n");
	ASSERT_NE(exclusiveCompositePlan.root, nullptr);
	const graph::query::QueryResult exclusiveComposite =
			runtime.executeToQueryResult(*exclusiveCompositePlan.root, bounds);
	EXPECT_EQ(exclusiveComposite.rowCount(), 0U);

	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DirectBackendReadPagesRetainThePinnedLogicalSnapshot) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	backend.open();
	RelocatableReadSnapshot snapshot = backend.openSnapshot();

	EXPECT_EQ(backend.readNode(snapshot, fixture.person_node_id).node_id, fixture.person_node_id);
	EXPECT_EQ(backend.readEdge(snapshot, fixture.knows_edge_id).type, "KNOWS");
	EXPECT_TRUE(backend.containsLogicalNode(snapshot, fixture.unlabeled_node_id));
	EXPECT_FALSE(backend.containsLogicalNode(snapshot, fixture.mentions_edge_id + 100U));
	const std::vector<uint64_t> endpointIds{fixture.person_node_id, fixture.unlabeled_node_id,
			fixture.mentions_edge_id + 100U};
	const std::vector<bool> endpointPresence = backend.containsLogicalNodesSorted(snapshot, endpointIds);
	EXPECT_EQ(endpointPresence, (std::vector<bool>{true, true, false}));
	EXPECT_THROW(static_cast<void>(backend.containsLogicalNodesSorted(
			 snapshot, std::vector<uint64_t>{fixture.unlabeled_node_id, fixture.person_node_id})), std::invalid_argument);
	std::unique_ptr<LogicalGraphReadSnapshot> logicalSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(logicalSnapshot, nullptr);
	EXPECT_EQ(logicalSnapshot->containsNodesSorted(endpointIds), endpointPresence);
	EXPECT_THROW(static_cast<void>(backend.enqueueLargeValuePageCompaction(SegmentId(1U), SegmentId(2U), 1U)),
				 std::logic_error);

	const LogicalGraphNodePage first = backend.scanNodesAfter(snapshot, 0U, 1U, 8U);
	ASSERT_EQ(first.nodes.size(), 1U);
	EXPECT_EQ(first.nodes.front().node_id, fixture.person_node_id);
	ASSERT_TRUE(first.may_have_more);
	const LogicalGraphNodePage second = backend.scanNodesAfter(snapshot, fixture.person_node_id, 1U, 8U);
	ASSERT_EQ(second.nodes.size(), 1U);
	EXPECT_EQ(second.nodes.front().node_id, fixture.unlabeled_node_id);
	EXPECT_THROW(static_cast<void>(backend.scanNodesAfter(snapshot, 0U, 0U, 8U)), FormatException);
	logicalSnapshot.reset();

	const LogicalGraphAdjacencyPage firstAdjacency = backend.scanAdjacentEdges(
			snapshot, fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
	ASSERT_EQ(firstAdjacency.edges.size(), 1U);
	ASSERT_TRUE(firstAdjacency.continuation.has_value());
	const LogicalGraphAdjacencyPage secondAdjacency = backend.scanAdjacentEdges(
			snapshot, fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, firstAdjacency.continuation, 1U);
	ASSERT_EQ(secondAdjacency.edges.size(), 1U);
	EXPECT_NE(firstAdjacency.edges.front().edge_id, secondAdjacency.edges.front().edge_id);

	std::vector<uint64_t> visited;
	EXPECT_FALSE(backend.visitNodesUntil(snapshot, 8U, [&visited](const LogicalGraphNode &node) {
		visited.push_back(node.node_id);
		return false;
	}));
	EXPECT_EQ(visited, std::vector<uint64_t>{fixture.person_node_id});
	std::vector<uint64_t> visitedEdges;
	EXPECT_FALSE(backend.visitEdgesUntil(snapshot, 8U, [&visitedEdges](const LogicalGraphEdge &edge) {
		visitedEdges.push_back(edge.edge_id);
		return false;
	}));
	ASSERT_EQ(visitedEdges.size(), 1U);
	EXPECT_TRUE(visitedEdges.front() == fixture.knows_edge_id || visitedEdges.front() == fixture.mentions_edge_id);
	std::vector<uint64_t> visitedAdjacentEdges;
	EXPECT_FALSE(backend.visitAdjacentEdgesUntil(
			snapshot, fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, 8U,
			[&visitedAdjacentEdges](const LogicalGraphEdge &edge) {
				visitedAdjacentEdges.push_back(edge.edge_id);
				return false;
			}));
	ASSERT_EQ(visitedAdjacentEdges.size(), 1U);
	EXPECT_TRUE(visitedAdjacentEdges.front() == fixture.knows_edge_id ||
				visitedAdjacentEdges.front() == fixture.mentions_edge_id);
	snapshot.release();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DirectTraversalContractsRejectMissingVisitorsAndZeroBounds) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	backend.open();
	RelocatableReadSnapshot snapshot = backend.openSnapshot();

	const std::function<void(const LogicalGraphNode &)> noNodeVisitor;
	const std::function<void(const LogicalGraphEdge &)> noEdgeVisitor;
	const std::function<bool(const LogicalGraphNode &)> noNodePredicate;
	const std::function<bool(const LogicalGraphEdge &)> noEdgePredicate;
	EXPECT_THROW(backend.visitNodes(snapshot, 1U, noNodeVisitor), FormatException);
	EXPECT_THROW(backend.visitEdges(snapshot, 1U, noEdgeVisitor), FormatException);
	EXPECT_THROW(static_cast<void>(backend.visitNodesUntil(snapshot, 0U,
			[](const LogicalGraphNode &) { return true; })), FormatException);
	EXPECT_THROW(static_cast<void>(backend.visitNodesUntil(snapshot, 1U, noNodePredicate)), FormatException);
	EXPECT_THROW(static_cast<void>(backend.visitEdgesUntil(snapshot, 0U,
			[](const LogicalGraphEdge &) { return true; })), FormatException);
	EXPECT_THROW(static_cast<void>(backend.visitEdgesUntil(snapshot, 1U, noEdgePredicate)), FormatException);
	EXPECT_THROW(static_cast<void>(backend.scanNodesAfter(snapshot, 0U, 1U, 0U)), FormatException);
	EXPECT_THROW(static_cast<void>(backend.scanAdjacentEdges(
			snapshot, fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 0U)),
			FormatException);
	EXPECT_THROW(backend.visitAdjacentEdges(snapshot, fixture.person_node_id,
			LogicalGraphReadDirection::LGRD_OUTGOING, 1U, noEdgeVisitor), FormatException);
	EXPECT_THROW(static_cast<void>(backend.visitAdjacentEdgesUntil(snapshot, fixture.person_node_id,
			LogicalGraphReadDirection::LGRD_OUTGOING, 0U,
			[](const LogicalGraphEdge &) { return true; })), FormatException);
	EXPECT_THROW(static_cast<void>(backend.visitAdjacentEdgesUntil(snapshot, fixture.person_node_id,
			LogicalGraphReadDirection::LGRD_OUTGOING, 1U, noEdgePredicate)), FormatException);

	size_t nodeVisits = 0U;
	backend.visitNodes(snapshot, 8U, [&nodeVisits](const LogicalGraphNode &) { ++nodeVisits; });
	EXPECT_EQ(nodeVisits, 2U);
	size_t edgeVisits = 0U;
	backend.visitEdges(snapshot, 8U, [&edgeVisits](const LogicalGraphEdge &) { ++edgeVisits; });
	EXPECT_EQ(edgeVisits, 2U);
	size_t adjacencyVisits = 0U;
	backend.visitAdjacentEdges(snapshot, fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, 8U,
			[&adjacencyVisits](const LogicalGraphEdge &) { ++adjacencyVisits; });
	EXPECT_EQ(adjacencyVisits, 2U);

	const std::optional<uint32_t> alphaToken = backend.findPropertyKeyToken(snapshot, "alpha");
	ASSERT_TRUE(alphaToken.has_value());
	EXPECT_EQ(backend.findPropertyKey(snapshot, *alphaToken), std::optional<std::string>("alpha"));
	EXPECT_EQ(backend.findPropertyKeyToken(snapshot, "missing"), std::nullopt);
	EXPECT_EQ(backend.findPropertyKey(snapshot, std::numeric_limits<uint32_t>::max()), std::nullopt);
	const std::optional<uint64_t> personToken = backend.findSchemaTokenId(snapshot, "Person");
	ASSERT_TRUE(personToken.has_value());
	ASSERT_TRUE(backend.findSchemaToken(snapshot, *personToken).has_value());
	EXPECT_EQ(backend.findSchemaToken(snapshot, *personToken)->name, "Person");
	EXPECT_EQ(backend.findSchemaTokenId(snapshot, "missing"), std::nullopt);
	EXPECT_EQ(backend.findSchemaToken(snapshot, std::numeric_limits<uint64_t>::max()), std::nullopt);

	// Every rejected precondition is read-only; the snapshot remains valid after
	// the complete validation matrix and can still resolve its original Node.
	EXPECT_EQ(backend.readNode(snapshot, fixture.person_node_id).node_id, fixture.person_node_id);
	snapshot.release();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DirectSchemaTokenLookupRejectsAmbiguousNamesAcrossDomains) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"Shared"}, {}}, {{}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	ASSERT_EQ(backend.createLogicalEdges({{nodes[0], nodes[1], "Shared", {}}}).size(), 1U);

	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	EXPECT_THROW(static_cast<void>(backend.findSchemaTokenId(snapshot, "Shared")), FormatException);
	snapshot.release();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DirectNativeIndexLookupContractsRejectInvalidBoundsAndPredicateShapes) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	backend.open();
	RelocatableReadSnapshot snapshot = backend.openSnapshot();

	const std::vector<std::pair<std::string, graph::PropertyValue>> validProperties = {
			{"alpha", graph::PropertyValue(int64_t{42})}};
	EXPECT_THROW(static_cast<void>(backend.findNodesByExactProperties(
				snapshot, {"Person"}, validProperties, 0U, 1U)), FormatException);
	EXPECT_THROW(static_cast<void>(backend.findNodesByExactProperties(
				snapshot, {"Person"}, validProperties, 1U, 0U)), FormatException);
	EXPECT_EQ(backend.findNodesByExactProperties(snapshot, {"Person", "Other"}, validProperties, 1U, 1U),
				  std::nullopt);
	EXPECT_EQ(backend.findNodesByExactProperties(snapshot, {""}, validProperties, 1U, 1U), std::nullopt);
	EXPECT_EQ(backend.findNodesByExactProperties(
				snapshot, {"Person"}, {{"", graph::PropertyValue(int64_t{42})}}, 1U, 1U),
				  std::nullopt);
	EXPECT_EQ(backend.findNodesByExactProperties(
				snapshot, {"Person"}, {{"alpha", graph::PropertyValue()},}, 1U, 1U),
				  std::nullopt);
	EXPECT_EQ(backend.findNodesByExactProperties(snapshot, {"Person"},
				  {{"alpha", graph::PropertyValue(int64_t{42})}, {"alpha", graph::PropertyValue(int64_t{43})}}, 1U,
				  1U),
				  std::nullopt);

	const auto range = [](std::string key, graph::PropertyValue minimum, graph::PropertyValue maximum,
						  bool minimumInclusive = true, bool maximumInclusive = true) {
		return LogicalGraphPropertyRange{
				std::move(key), std::move(minimum), std::move(maximum), minimumInclusive, maximumInclusive};
	};
	EXPECT_THROW(static_cast<void>(backend.findNodesByPropertyRange(
				snapshot, {"Person"}, range("alpha", graph::PropertyValue(int64_t{1}), graph::PropertyValue()), 0U,
				1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(backend.findNodesByPropertyRange(
				snapshot, {"Person"}, range("alpha", graph::PropertyValue(int64_t{1}), graph::PropertyValue()), 1U,
				0U)),
			 FormatException);
	EXPECT_EQ(backend.findNodesByPropertyRange(
				  snapshot, {"Person"}, range("", graph::PropertyValue(int64_t{1}), graph::PropertyValue()), 1U, 1U),
				  std::nullopt);
	EXPECT_EQ(backend.findNodesByPropertyRange(snapshot, {"Person"},
				  range("alpha", graph::PropertyValue(), graph::PropertyValue()), 1U, 1U),
				  std::nullopt);
	EXPECT_EQ(backend.findNodesByPropertyRange(snapshot, {"Person"},
				  range("alpha", graph::PropertyValue(int64_t{9}), graph::PropertyValue(int64_t{7})), 1U, 1U),
				  std::optional<std::vector<LogicalGraphNode>>(std::vector<LogicalGraphNode>{}));
	EXPECT_EQ(backend.findNodesByPropertyRange(
				  snapshot, {"Person"},
				  range("alpha", graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(1)}),
						graph::PropertyValue()),
				  1U, 1U),
				  std::nullopt);

	// Invalid lookup requests do not consume the still-pinned read capability.
	EXPECT_EQ(backend.readNode(snapshot, fixture.person_node_id).node_id, fixture.person_node_id);
	snapshot.release();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, CloseRejectsPinnedSnapshotUntilItIsReleased) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	backend.open();
	RelocatableReadSnapshot snapshot = backend.openSnapshot();

	// Closing while a snapshot remains pinned would invalidate its immutable page
	// routes and permit premature reclamation. The rejected close must leave the
	// backend and snapshot usable, rather than partially releasing either one.
	EXPECT_THROW(backend.close(), std::logic_error);
	EXPECT_TRUE(backend.isOpen());
	EXPECT_EQ(backend.readNode(snapshot, fixture.person_node_id).node_id, fixture.person_node_id);

	snapshot.release();
	EXPECT_NO_THROW(backend.close());

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	RelocatableReadSnapshot reopenedSnapshot = reopened.openSnapshot();
	EXPECT_EQ(reopened.readNode(reopenedSnapshot, fixture.person_node_id).node_id, fixture.person_node_id);
	reopenedSnapshot.release();
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, DirectReadRejectsSnapshotOwnedByAnotherDatabase) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend first(backendOptions());
	first.open();
	RelocatableReadSnapshot foreignSnapshot = first.openSnapshot();

	const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path secondPath =
			std::filesystem::temp_directory_path() / ("zyx_foreign_snapshot_" + std::to_string(tick) + ".zyx");
	std::error_code removeError;
	std::filesystem::remove(secondPath, removeError);
	RelocatableStorageBackendOptions secondOptions;
	secondOptions.data_path = secondPath;
	secondOptions.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto initialized = RelocatableStorageBackend::initializeNew(std::move(secondOptions));
	ASSERT_NE(initialized, nullptr);
	initialized->open();
	initialized->close();
	secondOptions = backendOptions();
	secondOptions.data_path = secondPath;
	secondOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend second(std::move(secondOptions));
	second.open();

	// A snapshot is a capability bound to one backend instance. Accepting a
	// foreign snapshot could read a page through unrelated catalog roots.
	EXPECT_THROW(static_cast<void>(second.readNode(foreignSnapshot, fixture.person_node_id)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(second.containsLogicalNode(foreignSnapshot, fixture.person_node_id)),
			 std::invalid_argument);

	foreignSnapshot.release();
	first.close();
	second.close();
	std::filesystem::remove(secondPath, removeError);
}

TEST_F(RelocatableStorageBackendTest, DirectExactLookupAndSchemaDropUseTheCanonicalNativeCatalog) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	static_cast<void>(backend.createLogicalConstraint(
			{"user_email_unique", LogicalGraphConstraintEntityKind::LGCEK_NODE,
			 LogicalGraphConstraintKind::LGCK_UNIQUE, "User", {"email"},
			 LogicalGraphConstraintPropertyType::LGCPT_NONE}));
	const std::vector<uint64_t> users = backend.createLogicalNodes(
			{{{"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))},
						 {"display_name", graph::PropertyValue(std::string("Ada"))}}}});
	ASSERT_EQ(users.size(), 1U);
	static_cast<void>(backend.createLogicalConstraint(
			{"account_handle_key", LogicalGraphConstraintEntityKind::LGCEK_NODE,
			 LogicalGraphConstraintKind::LGCK_NODE_KEY, "Account", {"handle"},
			 LogicalGraphConstraintPropertyType::LGCPT_NONE}));
	const std::vector<uint64_t> accounts =
			backend.createLogicalNodes({{{"Account"}, {{"handle", graph::PropertyValue("primary")}}}});
	ASSERT_EQ(accounts.size(), 1U);

	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	EXPECT_EQ(backend.findNodesByExactProperties(snapshot, {}, {}, 1U, 1U), std::nullopt);
	EXPECT_EQ(backend.findNodesByExactProperties(
			  snapshot, {"User", "Other"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}, 1U, 2U),
			  std::nullopt);
	EXPECT_EQ(backend.findNodesByExactProperties(
			  snapshot, {"User"},
			  {{"email", graph::PropertyValue(std::string("ada@example.test"))},
			   {"email", graph::PropertyValue(std::string("ada@example.test"))}},
			  1U, 2U),
			  std::nullopt);
	const auto unknownLabel = backend.findNodesByExactProperties(
			snapshot, {"Unknown"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}, 1U, 2U);
	ASSERT_TRUE(unknownLabel.has_value());
	EXPECT_TRUE(unknownLabel->empty());
	EXPECT_THROW(static_cast<void>(backend.findNodesByExactProperties(
				snapshot, {"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}, 0U, 2U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(backend.findNodesByExactProperties(
				snapshot, {"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}, 1U, 0U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(backend.findNodesByExactProperties(
				snapshot, {"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}, 1U, 1U)),
			 FormatException);
	const auto matched = backend.findNodesByExactProperties(
			snapshot, {"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}, 1U, 2U);
	ASSERT_TRUE(matched.has_value());
	ASSERT_EQ(matched->size(), 1U);
	EXPECT_EQ(matched->front().node_id, users.front());
	const auto nodeKeyMatch = backend.findNodesByExactProperties(
			snapshot, {"Account"}, {{"handle", graph::PropertyValue("primary")}}, 1U, 1U);
	ASSERT_TRUE(nodeKeyMatch.has_value());
	ASSERT_EQ(nodeKeyMatch->size(), 1U);
	EXPECT_EQ(nodeKeyMatch->front().node_id, accounts.front());
	const auto missing = backend.findNodesByExactProperties(
			snapshot, {"User"}, {{"email", graph::PropertyValue(std::string("missing@example.test"))}}, 1U, 2U);
	ASSERT_TRUE(missing.has_value());
	EXPECT_TRUE(missing->empty());
	snapshot.release();

	// A schema drop for an unknown label must stop at catalog lookup without
	// allocating a writer or mutating any durable state.
	EXPECT_FALSE(backend.dropLogicalNodeIndexBySchema("Unknown", "email"));
	const LogicalGraphIndexDefinition vector =
			backend.createLogicalNodeVectorIndex({"user_embedding_index", "User", "embedding", 2U, "l2"});
	// The schema shortcut identifies Scalar Indexes only. A Vector Index with the
	// same label/property namespace remains durable until its named DDL drop.
	EXPECT_FALSE(backend.dropLogicalNodeIndexBySchema("User", "embedding"));
	static_cast<void>(backend.createLogicalNodeIndex({"user_email_index", "User", {"email"}}));
	EXPECT_TRUE(backend.dropLogicalNodeIndexBySchema("User", "email"));
	EXPECT_FALSE(backend.dropLogicalNodeIndexBySchema("User", "email"));
	EXPECT_THROW(static_cast<void>(backend.dropLogicalNodeIndexBySchema("", "email")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.dropLogicalNodeIndexBySchema("User", "")), std::invalid_argument);
	EXPECT_EQ(backend.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{vector});
	EXPECT_TRUE(backend.dropLogicalIndexByName(vector.name));
	EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());
	backend.close();
}

TEST_F(RelocatableStorageBackendTest,
		NativeIntegrityVerificationChecksNodeAndRelationshipUniqueSupportTrees) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	// UNIQUE permits a missing Property while NODE KEY does not. The integrity
	// scan must preserve that distinction while checking both entity domains.
	static_cast<void>(backend.createLogicalConstraint(
			{"user_email_unique", LogicalGraphConstraintEntityKind::LGCEK_NODE,
			 LogicalGraphConstraintKind::LGCK_UNIQUE, "User", {"email"},
			 LogicalGraphConstraintPropertyType::LGCPT_NONE}));
	static_cast<void>(backend.createLogicalConstraint(
			{"knows_code_unique", LogicalGraphConstraintEntityKind::LGCEK_EDGE,
			 LogicalGraphConstraintKind::LGCK_UNIQUE, "KNOWS", {"code"},
			 LogicalGraphConstraintPropertyType::LGCPT_NONE}));
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"User"}, {{"email", graph::PropertyValue("ada@example.test")}}},
			 {{"User"}, {}},
			 {{"Other"}, {{"email", graph::PropertyValue("ignored@example.test")}}}});
	ASSERT_EQ(nodes.size(), 3U);
	const std::vector<uint64_t> edges = backend.createLogicalEdges(
			{{nodes[0], nodes[2], "KNOWS", {{"code", graph::PropertyValue("k-1")}}},
			 {nodes[2], nodes[0], "", {{"code", graph::PropertyValue("k-1")}}}});
	ASSERT_EQ(edges.size(), 2U);

	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, DirectRangeLookupsUseTheCanonicalNativeIndexes) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
							 {"city", graph::PropertyValue("Shanghai")},
							 {"age", graph::PropertyValue(int64_t{30})}}},
			 {{"Person"}, {{"score", graph::PropertyValue(int64_t{42})},
							 {"city", graph::PropertyValue("Beijing")},
							 {"age", graph::PropertyValue(int64_t{31})}}},
			 {{"Person"}, {{"score", graph::PropertyValue(int64_t{42})},
							 {"city", graph::PropertyValue("Shanghai")},
							 {"age", graph::PropertyValue(int64_t{29})}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));

	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	const auto scalarRange = backend.findNodesByPropertyRange(
			snapshot, {"Person"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{42}), true, true},
			3U, 3U);
	ASSERT_TRUE(scalarRange.has_value());
	ASSERT_EQ(scalarRange->size(), 3U);
	EXPECT_EQ(scalarRange->at(0).node_id, nodes.at(0));
	EXPECT_EQ(scalarRange->at(1).node_id, nodes.at(1));
	EXPECT_EQ(scalarRange->at(2).node_id, nodes.at(2));

	// Exact scalar lookup preserves every matching Node and rejects a caller
	// bound that would silently truncate the native Index result.
	const auto duplicateExact = backend.findNodesByExactProperties(
			snapshot, {"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 2U, 3U);
	ASSERT_TRUE(duplicateExact.has_value());
	ASSERT_EQ(duplicateExact->size(), 2U);
	EXPECT_EQ(duplicateExact->at(0).node_id, nodes.at(1));
	EXPECT_EQ(duplicateExact->at(1).node_id, nodes.at(2));
	EXPECT_THROW(static_cast<void>(backend.findNodesByExactProperties(
				snapshot, {"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 1U, 3U)),
			 FormatException);

	// Equality follows the same native scalar index and returns an explicit
	// empty result when the encoded key has no candidate.
	const auto emptyExact = backend.findNodesByExactProperties(
			snapshot, {"Person"}, {{"score", graph::PropertyValue(int64_t{43})}}, 3U, 3U);
	ASSERT_TRUE(emptyExact.has_value());
	EXPECT_TRUE(emptyExact->empty());

	// A composite index does not impersonate a single-property index. The
	// caller must use its bounded scan fallback when no matching index shape is
	// available for the predicate.
	EXPECT_EQ(backend.findNodesByExactProperties(
			  snapshot, {"Person"}, {{"city", graph::PropertyValue("Shanghai")}}, 3U, 3U),
			  std::nullopt);
	EXPECT_EQ(backend.findNodesByPropertyRange(
			  snapshot, {"Person"},
			  {"city", graph::PropertyValue("Beijing"), graph::PropertyValue("Shanghai"), true, true}, 3U, 3U),
			  std::nullopt);
	EXPECT_EQ(backend.findNodesByCompositePropertyRange(
			  snapshot, {"Person"},
			  {{{"score", graph::PropertyValue(int64_t{42})}},
			   {"city", graph::PropertyValue("Beijing"), graph::PropertyValue("Shanghai"), true, true}},
			  3U, 3U),
			  std::nullopt);

	// A valid scalar index lookup can be empty without falling back to a page
	// scan. The lower bound is beyond the final canonical index entry here.
	const auto emptyScalarRange = backend.findNodesByPropertyRange(
			snapshot, {"Person"}, {"score", graph::PropertyValue(int64_t{43}), graph::PropertyValue(), true, true},
			3U, 3U);
	ASSERT_TRUE(emptyScalarRange.has_value());
	EXPECT_TRUE(emptyScalarRange->empty());

	// The ordered scalar traversal stops when it reaches the first value above
	// its inclusive upper bound, after retaining its preceding matching entry.
	const auto scalarUpperBounded = backend.findNodesByPropertyRange(
			snapshot, {"Person"}, {"score", graph::PropertyValue(), graph::PropertyValue(int64_t{7}), true, true},
			3U, 3U);
	ASSERT_TRUE(scalarUpperBounded.has_value());
	ASSERT_EQ(scalarUpperBounded->size(), 1U);
	EXPECT_EQ(scalarUpperBounded->front().node_id, nodes.at(0));

	const LogicalGraphCompositePropertyRange compositeRange{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{29}), graph::PropertyValue(int64_t{31}), true, false}};
	const auto compositeMatches =
			backend.findNodesByCompositePropertyRange(snapshot, {"Person"}, compositeRange, 3U, 3U);
	ASSERT_TRUE(compositeMatches.has_value());
	ASSERT_EQ(compositeMatches->size(), 2U);
	EXPECT_EQ(compositeMatches->at(0).node_id, nodes.at(0));
	EXPECT_EQ(compositeMatches->at(1).node_id, nodes.at(2));

	// The ordered composite scan must stop at the first entry beyond its upper
	// bound, rather than treating it as a later candidate from the same prefix.
	const auto compositeUpperBounded = backend.findNodesByCompositePropertyRange(
			snapshot, {"Person"},
			{{{"city", graph::PropertyValue("Shanghai")}},
			 {"age", graph::PropertyValue(int64_t{29}), graph::PropertyValue(int64_t{29}), true, true}},
			3U, 3U);
	ASSERT_TRUE(compositeUpperBounded.has_value());
	ASSERT_EQ(compositeUpperBounded->size(), 1U);
	EXPECT_EQ(compositeUpperBounded->front().node_id, nodes.at(2));

	// A non-existent equality prefix starts before the first stored prefix. The
	// composite index must terminate at that different prefix rather than
	// returning an unrelated row.
	const auto emptyCompositePrefix = backend.findNodesByCompositePropertyRange(
			snapshot, {"Person"},
			{{{"city", graph::PropertyValue("Amsterdam")}},
			 {"age", graph::PropertyValue(int64_t{0}), graph::PropertyValue(), true, true}},
			3U, 3U);
	ASSERT_TRUE(emptyCompositePrefix.has_value());
	EXPECT_TRUE(emptyCompositePrefix->empty());
	snapshot.release();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ShapedEdgeBatchSpansEntityPagesAndPreservesLogicalOrder) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	std::vector<LogicalGraphEdgeCreateRequest> requests;
	requests.reserve(600U);
	for (size_t index = 0U; index < 600U; ++index) {
		requests.push_back({nodes.front(), nodes.back(), "LINK", {{"ordinal", PropertyValue(static_cast<int64_t>(index))}}});
	}
	const std::vector<uint64_t> edgeIds = backend.createLogicalEdges(requests);
	ASSERT_EQ(edgeIds.size(), requests.size());

	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const std::optional<uint32_t> ordinalToken = snapshot->findPropertyToken("ordinal");
	ASSERT_TRUE(ordinalToken.has_value());
	const std::vector<LogicalGraphEdge> edges = snapshot->readEdgesShapedSorted(
			edgeIds, EntityReadShape{.include_type = true, .property_tokens = {*ordinalToken}});
	const std::array<std::string_view, 1U> ordinalKey{"ordinal"};
	const std::vector<std::vector<PropertyValue>> selectedColumns =
			snapshot->readEdgePropertyColumnsSorted(edgeIds, ordinalKey);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_EQ(edges.size(), edgeIds.size());
	for (size_t index = 0U; index < edges.size(); ++index) {
		EXPECT_EQ(edges[index].edge_id, edgeIds[index]);
		EXPECT_EQ(edges[index].source_node_id, nodes.front());
		EXPECT_EQ(edges[index].target_node_id, nodes.back());
		EXPECT_EQ(edges[index].type, "LINK");
		ASSERT_EQ(edges[index].properties.size(), 1U);
		EXPECT_EQ(edges[index].properties.front().key, "ordinal");
		EXPECT_EQ(edges[index].properties.front().value, PropertyValue(static_cast<int64_t>(index)));
	}
	ASSERT_EQ(selectedColumns.size(), 1U);
	ASSERT_EQ(selectedColumns.front().size(), edgeIds.size());
	for (size_t index = 0U; index < selectedColumns.front().size(); ++index) {
		EXPECT_EQ(selectedColumns.front()[index], PropertyValue(static_cast<int64_t>(index)));
	}
	ASSERT_TRUE(trace.contains("native_graph.read.edge_sorted_batch_calls"));
	EXPECT_EQ(trace.at("native_graph.read.edge_sorted_batch_calls").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.read.edge_sorted_batch_entries"));
	EXPECT_EQ(trace.at("native_graph.read.edge_sorted_batch_entries").totalValue,
			static_cast<int64_t>(edgeIds.size()));
	ASSERT_TRUE(trace.contains("native_graph.read.edge_sorted_batch_directory_range_calls"));
	EXPECT_EQ(trace.at("native_graph.read.edge_sorted_batch_directory_range_calls").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.read.edge_sorted_batch_unique_io_ids"));
	EXPECT_EQ(trace.at("native_graph.read.edge_sorted_batch_unique_io_ids").totalValue,
			static_cast<int64_t>(edgeIds.size()));
	ASSERT_TRUE(trace.contains("native_graph.read.edge_sorted_batch_entity_page_pins"));
	EXPECT_GT(trace.at("native_graph.read.edge_sorted_batch_entity_page_pins").totalValue, 1);
	EXPECT_LE(trace.at("native_graph.read.edge_sorted_batch_entity_page_pins").totalValue,
			static_cast<int64_t>(edgeIds.size()));
	ASSERT_TRUE(trace.contains("native_graph.read.edge_selected_property_output_values"));
	EXPECT_EQ(trace.at("native_graph.read.edge_selected_property_output_values").totalValue,
			static_cast<int64_t>(edgeIds.size()));
	ASSERT_TRUE(trace.contains("native_graph.read.edge_selected_property_output_payload_bytes"));
	EXPECT_GT(trace.at("native_graph.read.edge_selected_property_output_payload_bytes").totalValue, 0);
	ASSERT_TRUE(trace.contains("native_graph.read.edge_selected_column_output_values"));
	EXPECT_EQ(trace.at("native_graph.read.edge_selected_column_output_values").totalValue,
			static_cast<int64_t>(edgeIds.size()));
	ASSERT_TRUE(trace.contains("native_graph.read.edge_selected_column_output_payload_bytes"));
	EXPECT_GT(trace.at("native_graph.read.edge_selected_column_output_payload_bytes").totalValue, 0);
	const std::array<uint64_t, 2U> unorderedIds{edgeIds[1U], edgeIds[0U]};
	EXPECT_THROW(static_cast<void>(snapshot->readEdgesShapedSorted(
			unorderedIds, EntityReadShape{.include_type = true})), std::exception);
	const std::array<uint64_t, 2U> duplicateIds{edgeIds[0U], edgeIds[0U]};
	EXPECT_THROW(static_cast<void>(snapshot->readEdgesShapedSorted(
			duplicateIds, EntityReadShape{.include_type = true})), std::exception);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ShapedEdgeBatchReadsLargeValuePropertyColumn) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::string body(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 97U, 'x');
	const std::vector<uint64_t> edgeIds = backend.createLogicalEdges(
			{{nodes.front(), nodes.back(), "PAYLOAD", {{"body", PropertyValue(body)}}}});
	ASSERT_EQ(edgeIds.size(), 1U);

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const std::optional<uint32_t> bodyToken = snapshot->findPropertyToken("body");
	ASSERT_TRUE(bodyToken.has_value());
	const std::vector<LogicalGraphEdge> edges = snapshot->readEdgesShapedSorted(
			edgeIds, EntityReadShape{.include_type = true, .property_tokens = {*bodyToken}});
	ASSERT_EQ(edges.size(), 1U);
	ASSERT_EQ(edges.front().edge_id, edgeIds.front());
	ASSERT_EQ(edges.front().source_node_id, nodes.front());
	ASSERT_EQ(edges.front().target_node_id, nodes.back());
	ASSERT_EQ(edges.front().type, "PAYLOAD");
	ASSERT_EQ(edges.front().properties.size(), 1U);
	EXPECT_EQ(edges.front().properties.front().key, "body");
	EXPECT_EQ(edges.front().properties.front().value, PropertyValue(body));

	snapshot.reset();
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> reopenedSnapshot = reopened.openLogicalGraphReadSnapshot();
	ASSERT_NE(reopenedSnapshot, nullptr);
	const std::optional<uint32_t> reopenedBodyToken = reopenedSnapshot->findPropertyToken("body");
	ASSERT_TRUE(reopenedBodyToken.has_value());
	const std::vector<LogicalGraphEdge> reopenedEdges = reopenedSnapshot->readEdgesShapedSorted(
			edgeIds, EntityReadShape{.property_tokens = {*reopenedBodyToken}});
	ASSERT_EQ(reopenedEdges.size(), 1U);
	ASSERT_EQ(reopenedEdges.front().properties.size(), 1U);
	EXPECT_EQ(reopenedEdges.front().properties.front().value, PropertyValue(body));
	reopenedSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, ShapedEdgeBatchPreservesOldSnapshotAcrossPropertyReplacement) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	const RelocatableStorageBackendOptions reopenOptions = options;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> edgeIds = backend.createLogicalEdges(
			{{nodes.front(), nodes.back(), "VERSIONED", {{"body", PropertyValue("old")}}}});
	ASSERT_EQ(edgeIds.size(), 1U);
	std::unique_ptr<LogicalGraphReadSnapshot> oldSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(oldSnapshot, nullptr);
	const std::optional<uint32_t> oldToken = oldSnapshot->findPropertyToken("body");
	ASSERT_TRUE(oldToken.has_value());
	const auto readBody = [&](const LogicalGraphReadSnapshot &snapshot) {
		const std::vector<LogicalGraphEdge> result = snapshot.readEdgesShapedSorted(
				edgeIds, EntityReadShape{.property_tokens = {*oldToken}});
			if (result.size() != 1U || result.front().properties.size() != 1U) {
				throw std::runtime_error("versioned Edge shaped read returned an unexpected shape");
			}
			return result.front().properties.front().value;
		};
	EXPECT_EQ(readBody(*oldSnapshot), PropertyValue("old"));

	backend.replaceLogicalEdgeProperties({{edgeIds.front(), {{"body", PropertyValue("new")}}}});
	EXPECT_EQ(readBody(*oldSnapshot), PropertyValue("old"));
	std::unique_ptr<LogicalGraphReadSnapshot> currentSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(currentSnapshot, nullptr);
	const std::optional<uint32_t> currentToken = currentSnapshot->findPropertyToken("body");
	ASSERT_TRUE(currentToken.has_value());
	const std::vector<LogicalGraphEdge> current = currentSnapshot->readEdgesShapedSorted(
			edgeIds, EntityReadShape{.property_tokens = {*currentToken}});
	ASSERT_EQ(current.size(), 1U);
	ASSERT_EQ(current.front().properties.size(), 1U);
	EXPECT_EQ(current.front().properties.front().value, PropertyValue("new"));
	currentSnapshot.reset();
	oldSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(reopenOptions);
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> reopenedSnapshot = reopened.openLogicalGraphReadSnapshot();
	ASSERT_NE(reopenedSnapshot, nullptr);
	const std::optional<uint32_t> reopenedToken = reopenedSnapshot->findPropertyToken("body");
	ASSERT_TRUE(reopenedToken.has_value());
	const std::vector<LogicalGraphEdge> reopenedEdges = reopenedSnapshot->readEdgesShapedSorted(
			edgeIds, EntityReadShape{.property_tokens = {*reopenedToken}});
	ASSERT_EQ(reopenedEdges.size(), 1U);
	ASSERT_EQ(reopenedEdges.front().properties.front().value, PropertyValue("new"));
	reopenedSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, DirectNativeIndexesPreserveExactAndOpenRangeSemantics) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
							 {"city", graph::PropertyValue("Shanghai")},
							 {"age", graph::PropertyValue(int64_t{30})}}},
			 { {"Person"}, {{"score", graph::PropertyValue(int64_t{42})},
							  {"city", graph::PropertyValue("Beijing")},
							  {"age", graph::PropertyValue(int64_t{31})}}},
			 { {"Person"}, {{"score", graph::PropertyValue(int64_t{42})},
							  {"city", graph::PropertyValue("Shanghai")},
							  {"age", graph::PropertyValue(int64_t{29})}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city", "Person", {"city"}}));
	// A same-width index with the same range suffix is not usable unless its
	// equality prefix has the requested property order.
	static_cast<void>(backend.createLogicalNodeIndex({"person_score_age", "Person", {"score", "age"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));

	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	const auto exactComposite = backend.findNodesByExactProperties(
			snapshot, {"Person"}, {{"age", graph::PropertyValue(int64_t{29})}, {"city", graph::PropertyValue("Shanghai")}},
			2U, 3U);
	ASSERT_TRUE(exactComposite.has_value());
	ASSERT_EQ(exactComposite->size(), 1U);
	EXPECT_EQ(exactComposite->front().node_id, nodes.at(2));

	const auto aboveMinimum = backend.findNodesByPropertyRange(
			snapshot, {"Person"},
			{"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), false, true}, 3U, 3U);
	ASSERT_TRUE(aboveMinimum.has_value());
	ASSERT_EQ(aboveMinimum->size(), 2U);
	EXPECT_EQ(aboveMinimum->at(0).node_id, nodes.at(1));
	EXPECT_EQ(aboveMinimum->at(1).node_id, nodes.at(2));

	const auto belowMaximum = backend.findNodesByPropertyRange(
			snapshot, {"Person"},
			{"score", graph::PropertyValue(), graph::PropertyValue(int64_t{42}), true, false}, 3U, 3U);
	ASSERT_TRUE(belowMaximum.has_value());
	ASSERT_EQ(belowMaximum->size(), 1U);
	EXPECT_EQ(belowMaximum->front().node_id, nodes.at(0));

	const auto cityRange = backend.findNodesByPropertyRange(
			snapshot, {"Person"},
			{"city", graph::PropertyValue("Beijing"), graph::PropertyValue("Shanghai"), true, true}, 3U, 3U);
	ASSERT_TRUE(cityRange.has_value());
	ASSERT_EQ(cityRange->size(), 3U);
	EXPECT_EQ(cityRange->at(0).node_id, nodes.at(0));
	EXPECT_EQ(cityRange->at(1).node_id, nodes.at(1));
	EXPECT_EQ(cityRange->at(2).node_id, nodes.at(2));

	EXPECT_THROW(static_cast<void>(backend.findNodesByPropertyRange(
				snapshot, {"Person"},
				{"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}, 1U, 3U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(backend.findNodesByCompositePropertyRange(
				snapshot, {"Person"},
				{{{"city", graph::PropertyValue("Shanghai")}},
				 {"age", graph::PropertyValue(int64_t{29}), graph::PropertyValue(), true, true}},
				1U, 3U)),
			 FormatException);

	snapshot.release();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DirectNativeIndexesSupportBooleanAndFiniteDoubleKeys) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Metric"}, {{"enabled", graph::PropertyValue(true)}, {"weight", graph::PropertyValue(1.5)}}},
			 {{"Metric"}, {{"enabled", graph::PropertyValue(false)}, {"weight", graph::PropertyValue(2.5)}}},
			 {{"Metric"}, {{"enabled", graph::PropertyValue(true)}, {"weight", graph::PropertyValue(3.5)}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend.createLogicalNodeIndex({"metric_enabled", "Metric", {"enabled"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"metric_weight", "Metric", {"weight"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"metric_enabled_weight", "Metric", {"enabled", "weight"}}));

	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	const auto enabled = backend.findNodesByExactProperties(
			snapshot, {"Metric"}, {{"enabled", graph::PropertyValue(true)}}, 3U, 2U);
	ASSERT_TRUE(enabled.has_value());
	ASSERT_EQ(enabled->size(), 2U);
	EXPECT_EQ(enabled->at(0).node_id, nodes.at(0));
	EXPECT_EQ(enabled->at(1).node_id, nodes.at(2));

	const auto disabled = backend.findNodesByExactProperties(
			snapshot, {"Metric"}, {{"enabled", graph::PropertyValue(false)}}, 3U, 2U);
	ASSERT_TRUE(disabled.has_value());
	ASSERT_EQ(disabled->size(), 1U);
	EXPECT_EQ(disabled->front().node_id, nodes.at(1));
	const auto exactDouble = backend.findNodesByExactProperties(
			snapshot, {"Metric"}, {{"weight", graph::PropertyValue(2.5)}}, 3U, 2U);
	ASSERT_TRUE(exactDouble.has_value());
	ASSERT_EQ(exactDouble->size(), 1U);
	EXPECT_EQ(exactDouble->front().node_id, nodes.at(1));
	const auto booleanRange = backend.findNodesByPropertyRange(
			snapshot, {"Metric"}, {"enabled", graph::PropertyValue(false), graph::PropertyValue(true), true, true}, 3U, 2U);
	ASSERT_TRUE(booleanRange.has_value());
	ASSERT_EQ(booleanRange->size(), 3U);
	EXPECT_EQ(booleanRange->at(0).node_id, nodes.at(0));
	EXPECT_EQ(booleanRange->at(1).node_id, nodes.at(1));
	EXPECT_EQ(booleanRange->at(2).node_id, nodes.at(2));
	const auto compositeBooleanPrefix = backend.findNodesByCompositePropertyRange(
			snapshot, {"Metric"},
		{{{"enabled", graph::PropertyValue(true)}},
		 {"weight", graph::PropertyValue(1.5), graph::PropertyValue(3.5), false, true}},
		3U, 2U);
	ASSERT_TRUE(compositeBooleanPrefix.has_value());
	ASSERT_EQ(compositeBooleanPrefix->size(), 1U);
	EXPECT_EQ(compositeBooleanPrefix->front().node_id, nodes.at(2));

	const auto openDoubleRange = backend.findNodesByPropertyRange(
			snapshot, {"Metric"}, {"weight", graph::PropertyValue(1.5), graph::PropertyValue(), false, true}, 3U, 2U);
	ASSERT_TRUE(openDoubleRange.has_value());
	ASSERT_EQ(openDoubleRange->size(), 2U);
	EXPECT_EQ(openDoubleRange->at(0).node_id, nodes.at(1));
	EXPECT_EQ(openDoubleRange->at(1).node_id, nodes.at(2));

	const auto closedDoubleRange = backend.findNodesByPropertyRange(
			snapshot, {"Metric"}, {"weight", graph::PropertyValue(1.5), graph::PropertyValue(2.5), true, true}, 3U, 2U);
	ASSERT_TRUE(closedDoubleRange.has_value());
	ASSERT_EQ(closedDoubleRange->size(), 2U);
	EXPECT_EQ(closedDoubleRange->at(0).node_id, nodes.at(0));
	EXPECT_EQ(closedDoubleRange->at(1).node_id, nodes.at(1));
	snapshot.release();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DirectNativeIndexLookupsHonorDegenerateAndBoundedRequests) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	static_cast<void>(backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}, {"city", graph::PropertyValue("Shanghai")},
							 {"age", graph::PropertyValue(int64_t{30})}}},
			 { {"Person"}, {{"score", graph::PropertyValue(int64_t{42})}, {"city", graph::PropertyValue("Shanghai")},
							  {"age", graph::PropertyValue(int64_t{29})}}}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));

	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	EXPECT_EQ(backend.findNodesByExactProperties(
			  snapshot, {""}, {{"score", graph::PropertyValue(int64_t{7})}}, 2U, 3U),
			  std::nullopt);
	EXPECT_EQ(backend.findNodesByExactProperties(
			  snapshot, {"Person"}, {{"score", graph::PropertyValue()}}, 2U, 3U),
			  std::nullopt);
	EXPECT_EQ(backend.findNodesByExactProperties(
			  snapshot, {"Person"},
			  {{"score", graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(int64_t{7})})}},
			  2U, 3U),
			  std::nullopt);
	// Ordered native indexes intentionally omit non-finite scalar values. Such a
	// predicate remains a valid logical expression but must choose the bounded
	// scan fallback rather than manufacture an index key.
	EXPECT_EQ(backend.findNodesByExactProperties(
			  snapshot, {"Person"},
			  {{"score", graph::PropertyValue(std::numeric_limits<double>::quiet_NaN())}}, 2U, 3U),
			  std::nullopt);
	EXPECT_THROW(static_cast<void>(backend.findNodesByExactProperties(
				snapshot, {"Person"}, {{"score", graph::PropertyValue(int64_t{7})}},
				std::numeric_limits<size_t>::max(), 3U)),
			 FormatException);
	EXPECT_EQ(backend.findNodesByPropertyRange(
			  snapshot, {}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{42}), true, true},
			  2U, 3U),
			  std::nullopt);
	EXPECT_EQ(backend.findNodesByPropertyRange(
			  snapshot, {"Person"}, {"score", graph::PropertyValue(), graph::PropertyValue(), true, true}, 2U, 3U),
			  std::nullopt);
	EXPECT_EQ(backend.findNodesByPropertyRange(
			  snapshot, {"Person"},
			  {"score", graph::PropertyValue(std::numeric_limits<double>::quiet_NaN()), graph::PropertyValue(), true,
			   true},
			  2U, 3U),
			  std::nullopt);
	const auto unknownRangeLabel = backend.findNodesByPropertyRange(
			snapshot, {"Unknown"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true},
			2U, 3U);
	ASSERT_TRUE(unknownRangeLabel.has_value());
	EXPECT_TRUE(unknownRangeLabel->empty());
	const auto unknownRangeProperty = backend.findNodesByPropertyRange(
			snapshot, {"Person"}, {"missing", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true},
			2U, 3U);
	ASSERT_TRUE(unknownRangeProperty.has_value());
	EXPECT_TRUE(unknownRangeProperty->empty());
	EXPECT_THROW(static_cast<void>(backend.findNodesByPropertyRange(
				snapshot, {"Person"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true},
				0U, 3U)),
			 FormatException);

	const LogicalGraphCompositePropertyRange validComposite{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{29}), graph::PropertyValue(), true, true}};
	EXPECT_EQ(backend.findNodesByCompositePropertyRange(
			  snapshot, {"Person"}, {{}, validComposite.range}, 2U, 3U),
			  std::nullopt);
	EXPECT_EQ(backend.findNodesByCompositePropertyRange(
			  snapshot, {"Person"},
			  {{{"age", graph::PropertyValue(int64_t{29})}}, validComposite.range}, 2U, 3U),
			  std::nullopt);
	EXPECT_EQ(backend.findNodesByCompositePropertyRange(
			  snapshot, {"Person"},
			  {{{"city", graph::PropertyValue(std::numeric_limits<double>::quiet_NaN())}}, validComposite.range}, 2U,
			  3U),
			  std::nullopt);
	const auto unknownCompositeLabel = backend.findNodesByCompositePropertyRange(
			snapshot, {"Unknown"}, validComposite, 2U, 3U);
	ASSERT_TRUE(unknownCompositeLabel.has_value());
	EXPECT_TRUE(unknownCompositeLabel->empty());
	const auto unknownCompositeProperty = backend.findNodesByCompositePropertyRange(
			snapshot, {"Person"},
			{{{"missing", graph::PropertyValue("Shanghai")}}, validComposite.range}, 2U, 3U);
	ASSERT_TRUE(unknownCompositeProperty.has_value());
	EXPECT_TRUE(unknownCompositeProperty->empty());

	const auto emptyRange = backend.findNodesByPropertyRange(
			snapshot, {"Person"},
			{"score", graph::PropertyValue(int64_t{42}), graph::PropertyValue(int64_t{42}), true, false}, 2U, 3U);
	ASSERT_TRUE(emptyRange.has_value());
	EXPECT_TRUE(emptyRange->empty());
	EXPECT_THROW(static_cast<void>(backend.findNodesByPropertyRange(
				snapshot, {"Person"},
				{"score", graph::PropertyValue(), graph::PropertyValue(int64_t{42}), true, true}, 1U, 3U)),
			 FormatException);

	const auto upperBounded = backend.findNodesByCompositePropertyRange(
			snapshot, {"Person"},
			{{{"city", graph::PropertyValue("Shanghai")}},
			 {"age", graph::PropertyValue(int64_t{30}), graph::PropertyValue(), true, true}},
			2U, 3U);
	ASSERT_TRUE(upperBounded.has_value());
	ASSERT_EQ(upperBounded->size(), 1U);
	const auto age = std::ranges::find_if(upperBounded->front().properties,
			[](const auto &entry) { return entry.key == "age"; });
	ASSERT_NE(age, upperBounded->front().properties.end());
	EXPECT_EQ(age->value, graph::PropertyValue(int64_t{30}));

	snapshot.release();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DirectNativeIndexReadFailuresPreserveThePinnedSnapshot) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}, {"city", graph::PropertyValue("Shanghai")},
							 {"age", graph::PropertyValue(int64_t{30})}}},
			 { {"Person"}, {{"score", graph::PropertyValue(int64_t{42})}, {"city", graph::PropertyValue("Shanghai")},
							  {"age", graph::PropertyValue(int64_t{29})}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city", "Person", {"city"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));

	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	const LogicalGraphCompositePropertyRange validComposite{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{29}), graph::PropertyValue(int64_t{31}), true, true}};

	// Read bounds are validation-only: each rejected request must leave the
	// immutable snapshot usable for the next index lookup.
	EXPECT_THROW(static_cast<void>(backend.findNodesByExactProperties(
				snapshot, {"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}, 2U, 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(backend.findNodesByPropertyRange(
				snapshot, {"Person"},
				{"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{42}), true, true}, 2U,
				1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(
				backend.findNodesByCompositePropertyRange(snapshot, {"Person"}, validComposite, 2U, 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(
				backend.findNodesByCompositePropertyRange(snapshot, {"Person"}, validComposite, 0U, 3U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(
				backend.findNodesByCompositePropertyRange(snapshot, {"Person"}, validComposite, 2U, 0U)),
			 FormatException);

	EXPECT_EQ(backend.readNode(snapshot, nodes.front()).node_id, nodes.front());
	EXPECT_EQ(backend.findNodesByCompositePropertyRange(
				  snapshot, {"Person"},
				  {{{"city", graph::PropertyValue("Shanghai")}},
				   {"age", graph::PropertyValue(int64_t{30}), graph::PropertyValue(int64_t{30}), true, false}},
				  2U, 3U),
			  std::vector<LogicalGraphNode>{});
	EXPECT_EQ(backend.findNodesByCompositePropertyRange(
				  snapshot, {"Person"},
				  {{{"city", graph::PropertyValue("Shanghai")}},
				   {"age", graph::PropertyValue(int64_t{31}), graph::PropertyValue(int64_t{29}), true, true}},
				  2U, 3U),
			  std::vector<LogicalGraphNode>{});
	EXPECT_EQ(backend.findNodesByCompositePropertyRange(
				  snapshot, {"Person"},
				  {{{"city", graph::PropertyValue("Shanghai")}},
				   {"age", graph::PropertyValue(std::numeric_limits<double>::quiet_NaN()), graph::PropertyValue(), true,
					true}},
				  2U, 3U),
			  std::nullopt);
	EXPECT_THROW(static_cast<void>(backend.findNodesByExactProperties(
				snapshot, {"Person"}, {{"city", graph::PropertyValue("Shanghai")}}, 1U, 3U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(
				backend.findNodesByCompositePropertyRange(snapshot, {"Person"}, validComposite, 1U, 3U)),
			 FormatException);

	const auto recovered = backend.findNodesByPropertyRange(
			snapshot, {"Person"},
			{"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{7}), true, true}, 2U, 3U);
	ASSERT_TRUE(recovered.has_value());
	ASSERT_EQ(recovered->size(), 1U);
	EXPECT_EQ(recovered->front().node_id, nodes.front());
	snapshot.release();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalTransactionRejectsForgedEntityReferenceKindsBeforePublication) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Source"}, {}}, {{"Target"}, {}}};
	batch.edge_creates = {{
			{LogicalGraphMutationEntityKind::LGMK_NODE,
			 static_cast<LogicalGraphMutationReferenceKind>(99U),
			 0U},
			{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
			"REJECTED",
			{},
	}};
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(batch))), std::invalid_argument);
	transaction->rollback();
	transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch forgedEntity;
	forgedEntity.node_creates = {{{"Target"}, {}}};
	forgedEntity.edge_creates = {{
			{static_cast<LogicalGraphMutationEntityKind>(99U),
			 LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE,
			 0U},
			{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
			"REJECTED",
			{},
	}};
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(forgedEntity))), std::invalid_argument);
	transaction->rollback();

	// Final-state folding is part of staging, not a deferred commit-only check.
	// Otherwise an invalid enum could remain in the transaction after the caller
	// believes the rejected batch had no effect.
	transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch forgedProperty;
	forgedProperty.node_creates = {{{"Target"}, {}}};
	forgedProperty.property_replacements = {{
			{LogicalGraphMutationEntityKind::LGMK_NODE,
			 static_cast<LogicalGraphMutationReferenceKind>(99U),
			 0U},
			{{"rejected", graph::PropertyValue(int64_t{1})}},
	}};
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(forgedProperty))), std::invalid_argument);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
	transaction->rollback();

	transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch forgedLabel;
	forgedLabel.node_creates = {{{"Target"}, {}}};
	forgedLabel.node_label_replacements = {{
			{LogicalGraphMutationEntityKind::LGMK_NODE,
			 static_cast<LogicalGraphMutationReferenceKind>(99U),
			 0U},
			{"Rejected"},
	}};
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(forgedLabel))), std::invalid_argument);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
	transaction->rollback();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalTransactionRejectsInvalidCreatedEntityFinalStateBeforePublication) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const LogicalGraphMutationEntityReference createdNode{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	{
		std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphMutationBatch batch;
		batch.node_creates = {{{"DuplicateProperties"}, {}}};
		batch.property_replacements = {{createdNode, {{"first", graph::PropertyValue(int64_t{1})}}},
								   {createdNode, {{"second", graph::PropertyValue(int64_t{2})}}}};
		EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(batch))), std::invalid_argument);
		EXPECT_NO_THROW(transaction->rollback());
	}

	{
		std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphMutationBatch batch;
		batch.node_creates = {{{"InvalidLabelTarget"}, {}}};
		batch.node_label_replacements = {{
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
				{"InvalidLabelTarget"},
		}};
		EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(batch))), std::invalid_argument);
		EXPECT_NO_THROW(transaction->rollback());
	}

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"DeleteSource"}, {}}, {{"DeleteTarget"}, {}}};
	create.edge_creates = {{
			createdNode,
			{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
			"LIVE_EDGE",
			{},
	}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(create));
	ASSERT_EQ(staged.created_node_ids.size(), 2U);
	ASSERT_EQ(staged.created_edge_ids.size(), 1U);
	LogicalGraphMutationBatch erase;
	erase.node_deletes = {{staged.created_node_ids.front(), false, 0U}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(erase)).created_node_ids.empty());
	EXPECT_THROW(static_cast<void>(transaction->commit()), std::invalid_argument);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(staged.created_node_ids.front())), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readNode(staged.created_node_ids.back())), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(staged.created_edge_ids.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, MixedSelectedColumnsPreserveInlineLargeValueAndReopenedResults) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	const auto reopenOptions = options;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const PropertyValue bytes(PropertyValue::Bytes{std::byte{0}, std::byte{255}, std::byte{17}});
	const PropertyValue list(std::vector<PropertyValue>{PropertyValue(7), PropertyValue(), PropertyValue("nested")});
	const PropertyValue map(PropertyValue::MapType{{"items", list}, {"binary", bytes}});
	std::vector<LogicalGraphWriteProperty> small = {
		{"integer", PropertyValue(7)}, {"real", PropertyValue(7.5)}, {"flag", PropertyValue(true)},
		{"text", PropertyValue("short")}, {"nil", PropertyValue()}, {"binary", bytes}, {"list", list}, {"map", map}};
	auto large = small;
	large.push_back({"padding", PropertyValue(std::string(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 257U, 'p'))});
	const auto ids = backend.createLogicalNodes({{{"Inline"}, small}, {{"Large"}, large}});
	ASSERT_EQ(ids.size(), 2U);
	const std::array<std::string_view, 9> keys{"map", "integer", "text", "absent", "list", "binary", "nil", "real", "flag"};
	const std::array<PropertyValue, 9> expected{map, PropertyValue(7), PropertyValue("short"), PropertyValue(),
		list, bytes, PropertyValue(), PropertyValue(7.5), PropertyValue(true)};
	std::vector<std::vector<PropertyValue>> retained;
	const auto check = [&](RelocatableStorageBackend &store) {
		auto snapshot = store.openLogicalGraphReadSnapshot();
		ASSERT_NE(snapshot, nullptr);
		auto columns = snapshot->readNodePropertyColumnsSorted(ids, keys);
		ASSERT_EQ(columns.size(), keys.size());
		for (size_t column = 0U; column < keys.size(); ++column) {
			ASSERT_EQ(columns[column].size(), ids.size());
			EXPECT_EQ(columns[column][0], expected[column]);
			EXPECT_EQ(columns[column][1], expected[column]);
		}
		const auto repeated = snapshot->readNodePropertyColumnsSorted(ids, keys);
		EXPECT_EQ(columns, repeated);
		columns[0][0] = PropertyValue("caller changed its result");
		EXPECT_EQ(repeated[0][0], map);
		EXPECT_EQ(snapshot->readNodePropertyColumnsSorted(ids, keys), repeated);
		retained = repeated;
	};
	check(backend);
	backend.close();
	EXPECT_EQ(retained[0][0], map);
	RelocatableStorageBackend reopened(reopenOptions); reopened.open();
	check(reopened);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, GrowingPropertyRootCanRelocateFromAFullPinnedPool) {
 materializeNativeDatabase();
 auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;
 const auto reopenOptions=options;
 RelocatableStorageBackend backend(std::move(options));backend.open();
 const PropertyValue original(std::string(1800U,'o'));
 std::vector<LogicalGraphNodeCreateRequest> nodes;
 for(size_t i=0;i<2U*RSS_PAGE_POOL_DATA_FRAME_COUNT+2U;++i)
  nodes.push_back({{"RootGrowth"},{{"payload",original}}});
 const auto ids=backend.createLogicalNodes(nodes);
 ASSERT_EQ(ids.size(),nodes.size());
 auto before=backend.openLogicalGraphReadSnapshot();
 const std::array<uint64_t,2U> selected{ids[0],ids[1]};
 const std::array<std::string_view,1U> keys{"payload"};
 const PropertyValue replacement(std::string(3500U,'r'));
 ASSERT_NO_THROW(backend.replaceLogicalNodeProperties({{ids[0],{{"payload",replacement}}}}));
 auto after=backend.openLogicalGraphReadSnapshot();
 const auto oldValues=before->readNodePropertyColumnsSorted(selected,keys);
 const auto newValues=after->readNodePropertyColumnsSorted(selected,keys);
 ASSERT_EQ(oldValues.size(),1U);ASSERT_EQ(newValues.size(),1U);
 EXPECT_EQ(oldValues[0],(std::vector<PropertyValue>{original,original}));
 EXPECT_EQ(newValues[0],(std::vector<PropertyValue>{replacement,original}));
 before.reset();after.reset();backend.close();
 RelocatableStorageBackend reopened(reopenOptions);reopened.open();
 auto persisted=reopened.openLogicalGraphReadSnapshot();
 EXPECT_EQ(persisted->readNodePropertyColumnsSorted(selected,keys)[0],(std::vector<PropertyValue>{replacement,original}));
 persisted.reset();EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));reopened.close();
}
