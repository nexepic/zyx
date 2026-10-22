/** Snapshot-bound label membership access over existing native scalar indexes. */
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "CypherParserImpl.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/maintenance/OfflineScalarIndexMaterializer.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

using namespace relocatable_storage_backend_test;

namespace {
// These tests exercise migration/build/fallback contracts of pre-membership files.
// Retire the fresh-file defaults through the real DDL/COW path to construct that
// state explicitly rather than weakening fresh initialization or assertions.
void preparePreMembershipFile(const std::filesystem::path &path) {
 RelocatableStorageBackendOptions options; options.data_path = path;
 options.open_mode = OpenMode::OPEN_EXISTING_FILE;
 RelocatableStorageBackend backend(options); backend.open();
 if (!backend.dropLogicalIndexByName("__zyx_node_membership") ||
     !backend.dropLogicalIndexByName("__zyx_edge_membership")) {
  throw std::runtime_error("pre-membership fixture did not contain fresh defaults");
 }
 backend.close();
}
// Install the existing supported LABEL definition and materializer, rather
// than forging new persistent bytes or bypassing the ordinary scalar writer.
void materializeLabelIndex(const std::filesystem::path &path, uint64_t falseMember = 0U,
	IndexDefinitionEntityKind kind = IndexDefinitionEntityKind::IDEK_NODE) {
	auto file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary);
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	SlottedPagePool pool(io, *metadata);
	EntityPageStore entities(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, pool);
	PropertyCollectionRootStore properties(pool, *metadata);
	BTreePageStore pages(pool, *metadata);
	SystemCatalog catalog(pages, *metadata);
	const uint64_t first = io->fileSize() / RSS_SEGMENT_SIZE + 8U;
	NativeGraphWritePlannerOptions options;
	options.first_slot = PhysicalSlotId(first);
	options.last_slot = PhysicalSlotId(first + 64U);
	options.entity_pool_owner_id = 0x123456U;
	options.property_roots = {PhysicalSlotId(first), PhysicalSlotId(first + 64U), 0x123457U};
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, pages, options);
	IndexDefinition definition;
	{
		auto transaction = planner.beginTransaction();
		auto session = catalog.beginWrite(transaction);
		definition.entity_kind = kind;
		definition.name = kind == IndexDefinitionEntityKind::IDEK_NODE ? "all_node_labels" : "all_edge_types";
		definition = session.createIndexDefinition(definition);
		session.finish();
		if (!transaction.commit().committed()) throw std::runtime_error("label definition publication failed");
	}
	OfflineScalarIndexMaterializationOptions build;
	build.build = {PhysicalSlotId(first + 80U), PhysicalSlotId(first + 144U), 0x123458U, 4096U, 128U};
	build.sort = {4096U, 4096U, 4U};
	build.maximum_definitions = 16U;
	build.maximum_property_names = 128U;
	const std::array definitions{definition};
	static_cast<void>(OfflineScalarIndexMaterializer::materialize(
			definitions, pages, entities, labels, properties, catalog, *metadata, build));
	if (falseMember != 0U) {
		auto transaction = planner.beginTransaction();
		const auto token = catalog.findSchemaToken(transaction.baseSnapshot(),
				kind == IndexDefinitionEntityKind::IDEK_NODE ? SystemCatalogSchemaTokenKind::SCSTK_LABEL :
				SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE,
			kind == IndexDefinitionEntityKind::IDEK_NODE ? "Person" : "KNOWS");
		if (!token.has_value()) throw std::runtime_error("missing fixture label");
		const auto key = IndexKeyCodec::encodeEntryKey(PropertyValue(static_cast<int64_t>(*token)), falseMember);
		transaction.stageScalarIndexUpsert(pages, definition.index_id, key, falseMember);
		if (!transaction.commit().committed()) throw std::runtime_error("false fixture membership publication failed");
	}
	io->sync();
}
std::vector<uint64_t> collect(const LogicalGraphReadSnapshot &snapshot,
	const std::vector<std::string> &labels, size_t bound = 32U, bool exhaustive = false) {
	std::vector<uint64_t> ids;
	auto visitor = [&](const EntityRef &ref, bool matches) {
		if (matches) ids.push_back(ref.entity_id);
		return true;
	};
	if (exhaustive) snapshot.visitNodeRefsWithLabelMatches(bound, labels, visitor);
	else snapshot.visitNodeRefsForLabelPredicate(bound, labels, visitor);
	return ids;
}
}

TEST_F(RelocatableStorageBackendTest, LabelCandidatesUsePrimaryValidatedMembershipAndIntersectLabels) {
	const auto fixture = materializeNativeGraphFixture();
	preparePreMembershipFile(data_path_);
	{
		auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
		RelocatableStorageBackend writer(std::move(options)); writer.open();
		static_cast<void>(writer.createLogicalNodes({{{"Person", "Other"}, {}}, {{"Other"}, {}}}));
		writer.close();
	}
	materializeLabelIndex(data_path_);
	RelocatableStorageBackend backend(backendOptions()); backend.open();
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	for (const auto &labels : std::vector<std::vector<std::string>>{
			{"Person"}, {"Other"}, {"Person", "Other"}, {}}) {
		EXPECT_EQ(collect(*snapshot, labels), collect(*snapshot, labels, 32U, true));
	}
	EXPECT_EQ(collect(*snapshot, {"Person", "Person"}), collect(*snapshot, {"Person"}));
	EXPECT_EQ(collect(*snapshot, {"Person", "Other"}).size(), 1U);
	EXPECT_TRUE(collect(*snapshot, {"Missing"}, 1U).empty());
	EXPECT_TRUE(collect(*snapshot, {""}, 1U).empty());
	EXPECT_EQ(collect(*snapshot, {"Person"}).front(), fixture.person_node_id);
	snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, LabelCandidatesRespectStoppingAndCandidateBounds) {
	static_cast<void>(materializeNativeGraphFixture());
	preparePreMembershipFile(data_path_);
	{
		auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
		RelocatableStorageBackend writer(std::move(options)); writer.open();
		static_cast<void>(writer.createLogicalNodes({{{"Person"}, {}}, {{"Person"}, {}}}));
		writer.close();
	}
	materializeLabelIndex(data_path_);
	RelocatableStorageBackend backend(backendOptions()); backend.open();
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	size_t seen = 0U;
	EXPECT_NO_THROW(snapshot->visitNodeRefsForLabelPredicate(1U, {"Person"},
			[&](const EntityRef &, bool) { ++seen; return false; }));
	EXPECT_EQ(seen, 1U);
	EXPECT_THROW(static_cast<void>(collect(*snapshot, {"Person"}, 1U)), std::runtime_error);
	EXPECT_THROW(snapshot->visitNodeRefsForLabelPredicate(4U, {"Person"}, {}), std::invalid_argument);
	snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, LabelCandidatesFollowWritesWithoutChangingAnOlderSnapshot) {
	const auto fixture = materializeNativeGraphFixture();
	preparePreMembershipFile(data_path_);
	materializeLabelIndex(data_path_);
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	auto old = backend.openLogicalGraphReadSnapshot();
	const auto before = collect(*old, {"Person"});
	const auto created = backend.createLogicalNodes({{{"Person", "Other"}, {}}});
	ASSERT_EQ(created.size(), 1U);
	auto current = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(collect(*old, {"Person"}), before);
	EXPECT_EQ(collect(*current, {"Person"}), (std::vector<uint64_t>{fixture.person_node_id, created.front()}));
	EXPECT_EQ(collect(*current, {"Other"}), created);
	current.reset(); old.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, LabelPredicateFallsBackWithoutAnIndex) {
	static_cast<void>(materializeNativeGraphFixture());
	preparePreMembershipFile(data_path_);
	RelocatableStorageBackend backend(backendOptions()); backend.open();
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(collect(*snapshot, {"Person"}), collect(*snapshot, {"Person"}, 32U, true));
	snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, LabelCandidatesRejectMembershipDisagreeingWithPrimaryLabels) {
	const auto fixture = materializeNativeGraphFixture();
	preparePreMembershipFile(data_path_);
	materializeLabelIndex(data_path_, fixture.unlabeled_node_id);
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(options); backend.open();
	// A bounded candidate request must still authenticate the corrupt member.
	static_cast<void>(backend.createLogicalNodes({{{}, {}}}));
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(collect(*snapshot, {"Person"}, 2U)), FormatException);
	snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, LabelCandidatesContinueAcrossBatchesAndDriveRuntimeCounts) {
	static_cast<void>(materializeNativeGraphFixture());
	preparePreMembershipFile(data_path_);
	std::vector<LogicalGraphNodeCreateRequest> requests(513U);
	for (auto &request : requests) request.labels = {"Person"};
	{
		auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
		RelocatableStorageBackend writer(std::move(options)); writer.open();
		ASSERT_EQ(writer.createLogicalNodes(requests).size(), requests.size()); writer.close();
	}
	materializeLabelIndex(data_path_);
	RelocatableStorageBackend backend(backendOptions()); backend.open();
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const auto indexed = collect(*snapshot, {"Person"}, 1024U);
	EXPECT_EQ(indexed.size(), 514U);
	EXPECT_EQ(indexed, collect(*snapshot, {"Person"}, 1024U, true));
	snapshot.reset();
	graph::parser::cypher::CypherParserImpl parser;
	const auto plan = parser.parseToLogical("MATCH (n:Person) RETURN count(n) AS total");
	execution::LogicalPlanReadRuntime runtime(backend);
	execution::LogicalPlanReadRuntimeBounds bounds;
	bounds.maximum_node_records = 514U;
	bounds.maximum_node_page_records = 128U;
	bounds.maximum_metadata_pages_per_node_page = 128U;
	bounds.maximum_edges_per_node = 16U;
	bounds.maximum_edge_records = 1024U;
	bounds.maximum_adjacency_page_records = 16U;
	bounds.maximum_property_entries = 128U;
	bounds.maximum_rows = 1024U;
	bounds.maximum_distinct_rows = 1024U;
	const bool previouslyEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const auto result = runtime.executeToQueryResult(*plan.root, bounds);
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(previouslyEnabled);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(int64_t(514)));
	ASSERT_TRUE(trace.contains("native_graph.read.label_index_candidates"));
	EXPECT_EQ(trace.at("native_graph.read.label_index_candidates").totalValue, 514);
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LabelCandidatesTrackReplacementDeletionAndRollback) {
	const auto fixture = materializeNativeGraphFixture();
	preparePreMembershipFile(data_path_);
	materializeLabelIndex(data_path_);
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	auto old = backend.openLogicalGraphReadSnapshot();
	LogicalGraphMutationBatch replace;
	replace.node_label_replacements.push_back({
			{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
				fixture.person_node_id}, {"Other"}});
	{
		auto transaction = backend.beginLogicalGraphTransaction(false);
		static_cast<void>(transaction->stageMutationBatch(replace));
		transaction->rollback();
	}
	{
		auto unchanged = backend.openLogicalGraphReadSnapshot();
		EXPECT_EQ(collect(*unchanged, {"Person"}), collect(*old, {"Person"}));
		EXPECT_TRUE(collect(*unchanged, {"Other"}).empty());
	}
	static_cast<void>(backend.executeLogicalGraphMutationBatch(replace));
	{
		auto current = backend.openLogicalGraphReadSnapshot();
		EXPECT_TRUE(collect(*current, {"Person"}).empty());
		EXPECT_EQ(collect(*current, {"Other"}), (std::vector<uint64_t>{fixture.person_node_id}));
		EXPECT_EQ(collect(*old, {"Person"}), (std::vector<uint64_t>{fixture.person_node_id}));
	}
	backend.deleteLogicalNodes({{fixture.person_node_id, true, 8U}});
	{
		auto current = backend.openLogicalGraphReadSnapshot();
		EXPECT_TRUE(collect(*current, {"Other"}).empty());
		EXPECT_EQ(collect(*old, {"Person"}), (std::vector<uint64_t>{fixture.person_node_id}));
	}
	old.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, TypeCandidatesValidatePrimaryTypeAndPreserveUntypedFallback) {
	const auto fixture = materializeNativeGraphFixture();
	preparePreMembershipFile(data_path_);
	uint64_t untyped = 0U;
	{
		auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
		RelocatableStorageBackend writer(std::move(options)); writer.open();
		untyped = writer.createLogicalEdges({{fixture.person_node_id, fixture.person_node_id, "", {}}}).front();
		writer.close();
	}
	materializeLabelIndex(data_path_, 0U, IndexDefinitionEntityKind::IDEK_EDGE);
	RelocatableStorageBackend backend(backendOptions()); backend.open();
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const auto knows = snapshot->findRelationshipTypeToken("KNOWS");
	ASSERT_TRUE(knows.has_value());
	std::vector<uint64_t> ids;
	snapshot->visitEdgeRefsForTypePredicate(1U, knows, [&](const EntityRef &ref) {
		EXPECT_EQ(ref.type_token, *knows); ids.push_back(ref.entity_id); return true;
	});
	EXPECT_EQ(ids, (std::vector<uint64_t>{fixture.knows_edge_id}));
	ids.clear();
	snapshot->visitEdgeRefsForTypePredicate(8U, 0U, [&](const EntityRef &ref) {
		if (ref.type_token == 0U) ids.push_back(ref.entity_id); return true;
	});
	EXPECT_EQ(ids, (std::vector<uint64_t>{untyped}));
	snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, TypeCandidatesRejectWrongMembershipAndFallBackWithoutAnIndex) {
	const auto fixture = materializeNativeGraphFixture();
	preparePreMembershipFile(data_path_);
	{
		auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(options); backend.open();
		auto snapshot = backend.openLogicalGraphReadSnapshot();
		size_t seen = 0U;
		snapshot->visitEdgeRefsForTypePredicate(8U, snapshot->findRelationshipTypeToken("KNOWS"),
				[&](const EntityRef &) { ++seen; return true; });
		EXPECT_EQ(seen, 2U); snapshot.reset();
		static_cast<void>(backend.createLogicalEdges({{fixture.person_node_id, fixture.person_node_id, "", {}}}));
		backend.close();
	}
	materializeLabelIndex(data_path_, fixture.mentions_edge_id, IndexDefinitionEntityKind::IDEK_EDGE);
	RelocatableStorageBackend backend(backendOptions()); backend.open();
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(snapshot->visitEdgeRefsForTypePredicate(2U, snapshot->findRelationshipTypeToken("KNOWS"),
			[](const EntityRef &) { return true; }), FormatException);
	snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, TypeCandidatesFollowInsertionsDeletionAndOldSnapshots) {
	const auto fixture = materializeNativeGraphFixture();
	preparePreMembershipFile(data_path_);
	materializeLabelIndex(data_path_, 0U, IndexDefinitionEntityKind::IDEK_EDGE);
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	auto old = backend.openLogicalGraphReadSnapshot();
	const auto token = old->findRelationshipTypeToken("KNOWS");
	ASSERT_TRUE(token.has_value());
	const auto added = backend.createLogicalEdges({{fixture.person_node_id, fixture.person_node_id, "KNOWS", {}}});
	const auto collectEdges = [&](const LogicalGraphReadSnapshot &snapshot) {
		std::vector<uint64_t> ids;
		snapshot.visitEdgeRefsForTypePredicate(8U, token, [&](const EntityRef &ref) {
			if (ref.type_token == *token) ids.push_back(ref.entity_id); return true;
		}); return ids;
	};
	{
		auto current = backend.openLogicalGraphReadSnapshot();
		EXPECT_EQ(collectEdges(*current), (std::vector<uint64_t>{fixture.knows_edge_id, added.front()}));
		EXPECT_EQ(collectEdges(*old), (std::vector<uint64_t>{fixture.knows_edge_id}));
		size_t seen = 0U;
		EXPECT_NO_THROW(current->visitEdgeRefsForTypePredicate(1U, token,
				[&](const EntityRef &) { ++seen; return false; }));
		EXPECT_EQ(seen, 1U);
		EXPECT_THROW(current->visitEdgeRefsForTypePredicate(1U, token,
				[](const EntityRef &) { return true; }), std::runtime_error);
	}
	backend.deleteLogicalEdges({{fixture.knows_edge_id}});
	{
		auto current = backend.openLogicalGraphReadSnapshot();
		EXPECT_EQ(collectEdges(*current), added);
		EXPECT_EQ(collectEdges(*old), (std::vector<uint64_t>{fixture.knows_edge_id}));
	}
	old.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, MembershipBuildPublishesBothRootsAndIsIdempotent) {
	const auto fixture = materializeNativeGraphFixture();
	preparePreMembershipFile(data_path_);
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	auto old = backend.openLogicalGraphReadSnapshot();
	const auto before = collect(*old, {"Person"});
	const auto indexes = backend.ensureLogicalMembershipIndexes();
	ASSERT_EQ(indexes.size(), 2U);
	EXPECT_NE(indexes[0], indexes[1]);
	EXPECT_EQ(backend.ensureLogicalMembershipIndexes(), indexes);
	EXPECT_EQ(collect(*old, {"Person"}), before);
	{
		auto current = backend.openLogicalGraphReadSnapshot();
		EXPECT_EQ(collect(*current, {"Person"}, 1U), before);
		size_t seen = 0U;
		current->visitEdgeRefsForTypePredicate(1U, current->findRelationshipTypeToken("KNOWS"),
				[&](const EntityRef &ref) { EXPECT_EQ(ref.entity_id, fixture.knows_edge_id); ++seen; return true; });
		EXPECT_EQ(seen, 1U);
	}
	old.reset(); backend.close(); backend.open();
	EXPECT_EQ(backend.ensureLogicalMembershipIndexes(), indexes);
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, MembershipBuildCancellationCannotPublishPartialDefinitions) {
	static_cast<void>(materializeNativeGraphFixture());
	preparePreMembershipFile(data_path_);
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	auto before = backend.openSnapshot();
	const auto generation = before.metadataSnapshot().superblock().generation;
	before = {};
	size_t checks = 0U;
	EXPECT_THROW(static_cast<void>(backend.ensureLogicalMembershipIndexes([&] {
		if (++checks == 4U) throw std::runtime_error("cancel member build");
	})), std::runtime_error);
	EXPECT_GE(checks, 4U);
	{
		auto after = backend.openSnapshot();
		EXPECT_EQ(after.metadataSnapshot().superblock().generation, generation);
	}
	EXPECT_NO_THROW(static_cast<void>(backend.ensureLogicalMembershipIndexes()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, TypeCandidatesContinueAcrossBatchesAndDriveRuntimeCounts) {
	const auto fixture = materializeNativeGraphFixture();
	preparePreMembershipFile(data_path_);
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	static_cast<void>(backend.ensureLogicalMembershipIndexes());
	std::vector<LogicalGraphEdgeCreateRequest> requests(513U);
	for (auto &request : requests) {
		request.source_node_id = fixture.person_node_id;
		request.target_node_id = fixture.person_node_id;
		request.type = "KNOWS";
	}
	ASSERT_EQ(backend.createLogicalEdges(requests).size(), 513U);
	// Keep the scan larger than the query budget so this remains a cursor-batching test.
	std::vector<LogicalGraphEdgeCreateRequest> others(16U, {fixture.person_node_id, fixture.person_node_id, "", {}});
	ASSERT_EQ(backend.createLogicalEdges(others).size(), others.size());
	graph::parser::cypher::CypherParserImpl parser;
	const auto plan = parser.parseToLogical("MATCH ()-[e:KNOWS]->() RETURN count(e) AS total");
	execution::LogicalPlanReadRuntime runtime(backend);
	execution::LogicalPlanReadRuntimeBounds bounds{.maximum_node_records = 2U,
		.maximum_node_page_records = 2U, .maximum_metadata_pages_per_node_page = 128U,
		.maximum_edges_per_node = 1024U, .maximum_edge_records = 514U,
		.maximum_adjacency_page_records = 16U, .maximum_property_entries = 128U,
		.maximum_rows = 1024U, .maximum_distinct_rows = 1024U};
	const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const auto result = runtime.executeToQueryResult(*plan.root, bounds);
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(int64_t(514)));
	ASSERT_TRUE(trace.contains("native_graph.read.type_index_candidates"));
	EXPECT_EQ(trace.at("native_graph.read.type_index_candidates").totalValue, 514);
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkMaintainsTypeMembershipIncludingNewTypesAndOldSnapshots) {
	const auto fixture = materializeNativeGraphFixture();
	preparePreMembershipFile(data_path_);
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.online_index_sort_run_bytes = 4096U;
	options.maximum_online_index_sort_runs = 2U;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	static_cast<void>(backend.ensureLogicalMembershipIndexes());
	auto old = backend.openLogicalGraphReadSnapshot();
	const std::vector<uint64_t> sources(513U, fixture.person_node_id);
	const auto targets = sources;
	auto transaction = backend.beginLogicalGraphTransaction(false);
	auto *bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const auto known = bulk->appendPersistedBulkEdges({"KNOWS", sources, targets, {}});
	const auto fresh = bulk->appendPersistedBulkEdges({"NewType", sources, targets, {}});
	const auto untyped = bulk->appendPersistedBulkEdges({"", sources, targets, {}});
	static_cast<void>(transaction->commit());
	transaction.reset();
	const auto collectEdges = [](const LogicalGraphReadSnapshot &snapshot, const std::string &type) {
		std::vector<uint64_t> ids;
		const auto token = type.empty() ? std::optional<uint64_t>(0U) : snapshot.findRelationshipTypeToken(type);
		if (!token.has_value()) return ids;
		snapshot.visitEdgeRefsForTypePredicate(2048U, token, [&](const EntityRef &ref) {
			if (ref.type_token == *token) ids.push_back(ref.entity_id); return true;
		}); return ids;
	};
	{
		auto current = backend.openLogicalGraphReadSnapshot();
		auto expected = known; expected.insert(expected.begin(), fixture.knows_edge_id);
		EXPECT_EQ(collectEdges(*current, "KNOWS"), expected);
		EXPECT_EQ(collectEdges(*current, "NewType"), fresh);
		EXPECT_EQ(collectEdges(*current, ""), untyped);
		EXPECT_EQ(collectEdges(*old, "KNOWS"), (std::vector<uint64_t>{fixture.knows_edge_id}));
		EXPECT_TRUE(collectEdges(*old, "NewType").empty());
	}
	old.reset(); backend.close(); backend.open();
	{
		auto reopened = backend.openLogicalGraphReadSnapshot();
		EXPECT_EQ(collectEdges(*reopened, "NewType"), fresh);
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedMembershipWriteFailureKeepsOldGraphAndAllowsRetry) {
	const auto fixture = materializeNativeGraphFixture();
	preparePreMembershipFile(data_path_);
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto indexes = backend.ensureLogicalMembershipIndexes();
	auto old = backend.openLogicalGraphReadSnapshot();
	const std::array<uint64_t, 1U> endpoints{fixture.person_node_id};
	auto transaction = backend.beginLogicalGraphTransaction(false);
	auto *bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const auto rejected = bulk->appendPersistedBulkEdges({"KNOWS", endpoints, endpoints, {}});
	bool injected = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting failure(
				[&](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (!injected && byteCount == RSS_PAGE_SIZE) {
						bool memberPage = false;
						try {
							const auto page = std::span(static_cast<const std::byte *>(bytes), byteCount);
							const auto owner = PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							memberPage = owner.kind == PageOwnerKind::POK_INDEX && owner.owner_id == indexes[1];
						} catch (const std::exception &) {}
						if (memberPage) {
							injected = true;
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::system_error);
	}
	ASSERT_TRUE(injected);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	{
		auto current = backend.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(current->readEdge(rejected.front())), FormatException);
		std::vector<uint64_t> ids;
		const auto token = current->findRelationshipTypeToken("KNOWS"); ASSERT_TRUE(token);
		current->visitEdgeRefsForTypePredicate(8U, token,
				[&](const EntityRef &ref) { if (ref.type_token == *token) ids.push_back(ref.entity_id); return true; });
		EXPECT_EQ(ids, (std::vector<uint64_t>{fixture.knows_edge_id}));
	}
	auto retry = backend.beginLogicalGraphTransaction(false);
	auto *retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const auto accepted = retryBulk->appendPersistedBulkEdges({"KNOWS", endpoints, endpoints, {}});
	EXPECT_NO_THROW(static_cast<void>(retry->commit())); retry.reset();
	{
		auto current = backend.openLogicalGraphReadSnapshot();
		std::vector<uint64_t> ids;
		const auto token = current->findRelationshipTypeToken("KNOWS"); ASSERT_TRUE(token);
		current->visitEdgeRefsForTypePredicate(8U, token,
				[&](const EntityRef &ref) { if (ref.type_token == *token) ids.push_back(ref.entity_id); return true; });
		EXPECT_EQ(ids, (std::vector<uint64_t>{fixture.knows_edge_id, accepted.front()}));
	}
	old.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, FreshDatabaseHasMembershipRootsAndZeroCardinalityBeforeItsFirstWrite) {
 auto options = backendOptions(); options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
 auto backend = RelocatableStorageBackend::initializeNew(options);
 ASSERT_NE(backend, nullptr); EXPECT_FALSE(backend->isOpen()); backend->open();
 const auto stats = backend->readLogicalGraphCardinalityStatistics();
 ASSERT_TRUE(stats.has_value()); EXPECT_EQ(stats->node_count, 0U); EXPECT_EQ(stats->edge_count, 0U);
 EXPECT_TRUE(backend->listLogicalIndexes(1U).empty());
 const auto before = backend->openLogicalGraphReadSnapshot()->nativeSnapshotGeneration();
 const auto roots = backend->ensureLogicalMembershipIndexes(); ASSERT_EQ(roots.size(), 2U);
 EXPECT_EQ(backend->openLogicalGraphReadSnapshot()->nativeSnapshotGeneration(), before);
 const auto nodes = backend->createLogicalNodes({{{"Person"}, {}}, {{"Other"}, {}}});
 ASSERT_EQ(nodes.size(), 2U);
 const auto edges = backend->createLogicalEdges({{nodes[0], nodes[1], "KNOWS", {}}});
 ASSERT_EQ(edges.size(), 1U);
 { auto snapshot = backend->openLogicalGraphReadSnapshot();
   std::vector<uint64_t> ids;
   snapshot->visitNodeRefsForLabelPredicate(1U, {"Person"}, [&](const EntityRef &ref, bool matches) {
    if (matches) ids.push_back(ref.entity_id); return true;
   }); EXPECT_EQ(ids, (std::vector<uint64_t>{nodes[0]}));
   ids.clear(); snapshot->visitEdgeRefsForTypePredicate(1U, snapshot->findRelationshipTypeToken("KNOWS"),
    [&](const EntityRef &ref) { ids.push_back(ref.entity_id); return true; });
   EXPECT_EQ(ids, edges);
 }
 const auto scalar = backend->createLogicalNodeIndex({"person_score", "Person", {"score"}});
 EXPECT_EQ(backend->listLogicalIndexes(1U), (std::vector<LogicalGraphIndexDefinition>{scalar}));
 static_cast<void>(backend->createLogicalNodeIndex({"person_name", "Person", {"name"}}));
 EXPECT_THROW(static_cast<void>(backend->listLogicalIndexes(1U)), FormatException);
 EXPECT_EQ(backend->listLogicalIndexes(2U).size(), 2U);
 const auto after = backend->readLogicalGraphCardinalityStatistics(); ASSERT_TRUE(after.has_value());
 EXPECT_EQ(after->node_count, 2U); EXPECT_EQ(after->edge_count, 1U);
 backend->close(); backend->open();
 EXPECT_EQ(backend->readLogicalGraphCardinalityStatistics(), after);
 backend->close();
}

TEST_F(RelocatableStorageBackendTest, FreshMembershipPublicationFailureRemovesPrivateFileAndAllowsRetry) {
 auto options = backendOptions(); options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
 bool injected = false;
 {
  StorageIO::ScopedNativeWriteHookForTesting hook([&](file_handle_t fd, const void *bytes, size_t size, pwrite_off_t offset) {
   if (!injected && size == RSS_PAGE_SIZE) {
    const auto payload = std::span<const std::byte>(static_cast<const std::byte *>(bytes), size);
    try {
     const auto owner = PageOwnerHeaderCodec::decode(payload.first(RSS_PAGE_OWNER_HEADER_BYTES));
     if (owner.kind == PageOwnerKind::POK_INDEX && isSystemCatalogIndexDefinitionId(owner.owner_id)) {
      injected = true;
#ifdef _WIN32
      SetLastError(ERROR_DISK_FULL);
#else
      errno = ENOSPC;
#endif
      return static_cast<pwrite_ssize_t>(-1);
     }
    } catch (const FormatException &) {}
   }
   return portable_pwrite(fd, bytes, size, offset);
  });
  EXPECT_THROW(static_cast<void>(RelocatableStorageBackend::initializeNew(options)), std::system_error);
 }
 EXPECT_TRUE(injected); EXPECT_FALSE(std::filesystem::exists(data_path_));
 auto retry = RelocatableStorageBackend::initializeNew(options); retry->open();
 EXPECT_EQ(retry->ensureLogicalMembershipIndexes().size(), 2U); retry->close();
}

TEST_F(RelocatableStorageBackendTest, MembershipBuildBackfillsMissingLiveTotalsFromCompleteSources) {
 const auto fixture = materializeNativeGraphFixture(); preparePreMembershipFile(data_path_);
 {
  auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
  auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
  auto metadata = RelocatableMetadataStore::open(io);
  SlottedPagePool pool(io, *metadata); EntityPageStore entities(io, *metadata);
  EntityLabelSetStore labels(io, *metadata, pool); PropertyCollectionRootStore properties(pool, *metadata);
  BTreePageStore pages(pool, *metadata);
  const uint64_t first = io->fileSize() / RSS_SEGMENT_SIZE + 8U;
  NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(first);
  options.last_slot = PhysicalSlotId(first + 64U); options.entity_pool_owner_id = 0x123456U;
  options.property_roots = {PhysicalSlotId(first), PhysicalSlotId(first + 64U), 0x123457U};
  NativeGraphWritePlanner planner(*metadata, entities, labels, properties, pages, options);
  auto transaction = planner.beginTransaction();
  transaction.stageScalarIndexErase(pages, RSS_SYSTEM_CATALOG_INDEX_ID, SystemCatalogGraphCardinalityCodec::nodeCountKey());
  transaction.stageScalarIndexErase(pages, RSS_SYSTEM_CATALOG_INDEX_ID, SystemCatalogGraphCardinalityCodec::edgeCountKey());
  ASSERT_TRUE(transaction.commit().committed()); io->sync();
 }
 auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
 RelocatableStorageBackend backend(options); backend.open();
 EXPECT_FALSE(backend.readLogicalGraphCardinalityStatistics().has_value());
 static_cast<void>(backend.ensureLogicalMembershipIndexes());
 EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
    std::optional<LogicalGraphCardinalityStatistics>({2U, 2U}));
 { auto snapshot = backend.openLogicalGraphReadSnapshot();
   EXPECT_EQ(collect(*snapshot, {"Person"}), (std::vector<uint64_t>{fixture.person_node_id})); }
 backend.close(); backend.open();
 EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
    std::optional<LogicalGraphCardinalityStatistics>({2U, 2U}));
 backend.close();
}

TEST_F(RelocatableStorageBackendTest, LabelCandidatesChooseTheSmallestKnownMembershipWithoutChangingPredicateSemantics) {
 auto options = backendOptions(); options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
 auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
 std::vector<LogicalGraphNodeCreateRequest> nodes(32U, {{"ACommon"}, {}});
 nodes.front().labels.push_back("ZRare");
 const auto ids = backend->createLogicalNodes(nodes);
 auto old = backend->openLogicalGraphReadSnapshot();
 EXPECT_EQ(collect(*old, {"ACommon", "ZRare"}, 1U), (std::vector<uint64_t>{ids.front()}));
 static_cast<void>(backend->createLogicalNodes({{{"ZRare"}, {}}}));
 { auto current = backend->openLogicalGraphReadSnapshot();
   EXPECT_EQ(collect(*current, {"ACommon", "ZRare"}, 2U), (std::vector<uint64_t>{ids.front()})); }
 EXPECT_EQ(collect(*old, {"ACommon", "ZRare"}, 1U), (std::vector<uint64_t>{ids.front()}));
 old.reset(); backend->close();
}

TEST_F(RelocatableStorageBackendTest, MembershipCostSelectionPreservesSparseDenseAndBudgetedPredicates) {
 auto options = backendOptions(); options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
 auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
 std::vector<LogicalGraphNodeCreateRequest> requests(1024U, {{"Common"}, {}});
 for (size_t i = 0; i < 10U; ++i) requests[i].labels.push_back("Rare");
 const auto nodes = backend->createLogicalNodes(requests);
 std::vector<LogicalGraphEdgeCreateRequest> edges(1024U, {nodes[0], nodes[1], "Other", {}});
 for (size_t i = 0; i < 10U; ++i) edges[i].type = "Selected";
 static_cast<void>(backend->createLogicalEdges(edges));
 auto old = backend->openLogicalGraphReadSnapshot();
 EXPECT_EQ(collect(*old, {"Rare"}, 1024U), collect(*old, {"Rare"}, 1024U, true));
 EXPECT_EQ(collect(*old, {"Common"}, 1024U), nodes);
 const auto token = old->findRelationshipTypeToken("Selected"); ASSERT_TRUE(token);
 size_t visited = 0, matched = 0;
 old->visitEdgeRefsForTypePredicate(1024U, token, [&](const EntityRef &ref) {
  ++visited; matched += ref.type_token == *token ? 1U : 0U; return true;
 });
 EXPECT_EQ(visited, 1024U); EXPECT_EQ(matched, 10U);
 visited = 0; matched = 0;
 old->visitEdgeRefsForTypePredicate(10U, token, [&](const EntityRef &ref) {
  ++visited; matched += ref.type_token == *token ? 1U : 0U; return true;
 });
 EXPECT_EQ(visited, 10U); EXPECT_EQ(matched, 10U);
 static_cast<void>(backend->createLogicalEdges({{nodes[0], nodes[1], "Selected", {}}}));
 visited = 0;
 old->visitEdgeRefsForTypePredicate(10U, token, [&](const EntityRef &) { ++visited; return true; });
 EXPECT_EQ(visited, 10U);
 { auto current = backend->openLogicalGraphReadSnapshot();
   EXPECT_THROW(current->visitEdgeRefsForTypePredicate(10U, token,
      [](const EntityRef &) { return true; }), std::runtime_error); }
 old.reset(); backend->close();
}

TEST_F(RelocatableStorageBackendTest, TypePageCostSelectionAccountsForScatteredMembershipAndKeepsBudgetedAccess) {
 auto options = backendOptions(); options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
 auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
 const auto nodes = backend->createLogicalNodes({{{}, {}}, {{}, {}}});
 constexpr size_t count = 8192U, selected = 81U;
 std::vector<LogicalGraphEdgeCreateRequest> edges;
 for (size_t i = 0; i < count; ++i)
  edges.push_back({nodes[0], nodes[1], (i * 97U) % count < selected ? "Selected" : "Other", {}});
 const bool priorTracing = graph::debug::PerfTrace::areValuesEnabled();
 graph::debug::PerfTrace::setValueCollectionEnabled(true); graph::debug::PerfTrace::reset();
 const auto ids = backend->createLogicalEdges(edges);
 const auto creationTrace = graph::debug::PerfTrace::snapshotAndReset();
 graph::debug::PerfTrace::setValueCollectionEnabled(priorTracing);
 ASSERT_TRUE(creationTrace.contains("native_graph.write.id_reservation_calls"));
 EXPECT_EQ(creationTrace.at("native_graph.write.id_reservation_calls").totalValue, 1);
 RecordProperty("batch_id_acquisitions", std::to_string(creationTrace.at("native_graph.write.id_reservation_calls").totalValue));
 auto snapshot = backend->openLogicalGraphReadSnapshot();
 const auto token = snapshot->findRelationshipTypeToken("Selected"); ASSERT_TRUE(token);
 std::vector<uint64_t> expected;
 for (size_t i = 0; i < count; ++i) if (edges[i].type == "Selected") expected.push_back(ids[i]);
 std::vector<uint64_t> matches;
 size_t examined = 0;
 const auto visitor = [&](const EntityRef &ref) {
  ++examined; if (ref.type_token == *token) matches.push_back(ref.entity_id); return true;
 };
 snapshot->visitEdgeRefsForTypePredicate(count, token, visitor);
 EXPECT_EQ(examined, count); EXPECT_EQ(matches, expected);
 examined = 0; matches.clear();
 snapshot->visitEdgeRefsForTypePredicate(selected, token, visitor);
 EXPECT_EQ(examined, selected); EXPECT_EQ(matches, expected);
 snapshot.reset(); backend->close();
}
