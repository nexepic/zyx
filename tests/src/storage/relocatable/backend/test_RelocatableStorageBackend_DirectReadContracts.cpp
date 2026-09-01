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
#include <vector>

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include "CypherParserImpl.hpp"

using namespace graph::storage;
using namespace relocatable_storage_backend_test;

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
	const auto missing = backend.findNodesByExactProperties(
			snapshot, {"User"}, {{"email", graph::PropertyValue(std::string("missing@example.test"))}}, 1U, 2U);
	ASSERT_TRUE(missing.has_value());
	EXPECT_TRUE(missing->empty());
	snapshot.release();

	static_cast<void>(backend.createLogicalNodeIndex({"user_email_index", "User", {"email"}}));
	EXPECT_TRUE(backend.dropLogicalNodeIndexBySchema("User", "email"));
	EXPECT_FALSE(backend.dropLogicalNodeIndexBySchema("User", "email"));
	EXPECT_THROW(static_cast<void>(backend.dropLogicalNodeIndexBySchema("", "email")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.dropLogicalNodeIndexBySchema("User", "")), std::invalid_argument);
	EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());
	backend.close();
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

	const LogicalGraphCompositePropertyRange compositeRange{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{29}), graph::PropertyValue(int64_t{31}), true, false}};
	const auto compositeMatches =
			backend.findNodesByCompositePropertyRange(snapshot, {"Person"}, compositeRange, 3U, 3U);
	ASSERT_TRUE(compositeMatches.has_value());
	ASSERT_EQ(compositeMatches->size(), 2U);
	EXPECT_EQ(compositeMatches->at(0).node_id, nodes.at(0));
	EXPECT_EQ(compositeMatches->at(1).node_id, nodes.at(2));
	snapshot.release();
	backend.close();
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
