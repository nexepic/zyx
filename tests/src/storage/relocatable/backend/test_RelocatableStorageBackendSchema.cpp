/**
 * @file test_RelocatableStorageBackendSchema.cpp
 * @brief End-to-end online scalar-index and Vector DDL tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendSchemaTestFixture.hpp"

#include <atomic>
#ifndef _WIN32
#include <cerrno>
#endif

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

TEST_F(RelocatableStorageBackendSchemaTest, BuildsHistoricalScalarEntriesPublishesDefinitionsAndDropsThem) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 1U);

	const LogicalGraphIndexDefinition created = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	EXPECT_NE(created.index_id, 0U);
	EXPECT_EQ(created.kind, LogicalGraphIndexKind::LGIK_PROPERTY);
	EXPECT_EQ(backend.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{created});
	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	EXPECT_EQ(backend.readIndexDefinitions(snapshot, 8U).size(), 1U);
	snapshot.release();
	const std::vector<uint64_t> laterNodes =
			backend.createLogicalNodes({{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}}});
	ASSERT_EQ(laterNodes.size(), 1U);
	backend.close();

	const std::vector<BTreePageEntry> entries = readIndexEntries(created.index_id);
	ASSERT_EQ(entries.size(), 2U);
	EXPECT_EQ(entries[0],
			  (BTreePageEntry{IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{7}), laterNodes.front()),
							  laterNodes.front()}));
	EXPECT_EQ(entries[1],
			  (BTreePageEntry{IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{42}), nodes.front()),
							  nodes.front()}));

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_EQ(reopened.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{created});
	EXPECT_TRUE(reopened.dropLogicalIndexByName(created.name));
	EXPECT_FALSE(reopened.dropLogicalIndexByName(created.name));
	EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, DerivesStableNamesForUnnamedScalarAndVectorIndexes) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend backend(options());
	backend.open();
	static_cast<void>(backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}, {"embedding", vectorValue(1.0, -2.0)}}}}));

	const LogicalGraphIndexDefinition scalar = backend.createLogicalNodeIndex({"", "Person", {"score"}});
	EXPECT_EQ(scalar.name, "index_node_6_Person_5_score");
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndex({"", "Person", {}})), std::invalid_argument);
	const LogicalGraphIndexDefinition vector =
			backend.createLogicalNodeVectorIndex({"", "Person", "embedding", 2U, "INNER_PRODUCT"});
	EXPECT_EQ(vector.name, "index_vector_node_6_Person_9_embedding_2_ip");
	EXPECT_EQ(vector.vector_metric, "ip");
	// Unnamed indexes derive a durable name from the complete schema. A valid
	// catalog token can still make that derived name exceed its persisted bound.
	const std::vector<std::string> longProperties{
			std::string(1024U, 'a'), std::string(1024U, 'b'), std::string(1024U, 'c'), std::string(1024U, 'd'),
			std::string(1024U, 'e')};
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndex({"", "LongSchema", longProperties})),
			 std::invalid_argument);
	EXPECT_EQ(backend.listLogicalIndexes(8U), (std::vector<LogicalGraphIndexDefinition>{scalar, vector}));
	backend.close();
}
TEST_F(RelocatableStorageBackendSchemaTest, RejectsInvalidVectorMetricsBeforePublishingSchema) {
	RelocatableStorageBackend backend(options());
	backend.open();

	const auto rejectsScalarSchema = [&backend](LogicalGraphNodeIndexCreateRequest request) {
		EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndex(std::move(request))), std::invalid_argument);
		EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());
	};
	rejectsScalarSchema({"invalid_scalar", "", {"score"}});
	rejectsScalarSchema({"invalid_scalar", "Person", {}});
	rejectsScalarSchema({"invalid_scalar", "Person", {"score", "score"}});
	rejectsScalarSchema({"invalid_scalar", "Person",
					 std::vector<std::string>{std::string(RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES + 1U, 'p')}});

	const auto rejectsVectorSchema = [&backend](LogicalGraphNodeVectorIndexCreateRequest request) {
		EXPECT_THROW(static_cast<void>(backend.createLogicalNodeVectorIndex(std::move(request))), std::invalid_argument);
		EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());
	};
	rejectsVectorSchema({"invalid_vector", "", "embedding", 2U, "l2"});
	rejectsVectorSchema({"invalid_vector", "Person", "", 2U, "l2"});
	rejectsVectorSchema({"invalid_vector", "Person", "embedding", 0U, "l2"});
	rejectsVectorSchema(
			{"invalid_vector", "Person", "embedding", RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS + 1U, "l2"});

	const auto rejects = [&backend](std::string metric) {
		EXPECT_THROW(static_cast<void>(backend.createLogicalNodeVectorIndex(
					 {"invalid_metric", "Person", "embedding", 2U, std::move(metric)})),
				 std::invalid_argument);
		EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());
	};

	// The public DDL boundary accepts only its canonical distance metric names;
	// every rejected spelling must leave no catalog definition behind.
	rejects("");
	rejects("unsupported");
	rejects(std::string(RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES + 1U, 'm'));

	const LogicalGraphIndexDefinition normalized =
			backend.createLogicalNodeVectorIndex({"cosine_metric", "Person", "embedding", 2U, "CoSiNe"});
	EXPECT_EQ(normalized.vector_metric, "cosine");
	const LogicalGraphIndexDefinition innerProduct =
			backend.createLogicalNodeVectorIndex({"inner_product_metric", "Person", "inner", 2U, "innerproduct"});
	EXPECT_EQ(innerProduct.vector_metric, "ip");
	EXPECT_EQ(backend.listLogicalIndexes(8U),
			  (std::vector<LogicalGraphIndexDefinition>{normalized, innerProduct}));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsStaleSemanticManifestBeforeVectorTraining) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, -2.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	VectorManifest stale = readVectorManifest(index.index_id);
	stale.node_mapping_root_generation = Generation(stale.node_mapping_root_generation.value() + 1U);
	overwriteVectorManifest(stale);

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	EXPECT_THROW(static_cast<void>(corrupted.trainLogicalVector({"person_embedding", 16U, 16U})), FormatException);
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, MaintainsUniqueConstraintSupportTreeWithoutPublishingPublicIndexMetadata) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const LogicalGraphConstraintDefinition constraint =
			backend.createLogicalConstraint({"user_email_unique",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_UNIQUE,
											 "User",
											 {"email"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE});
	ASSERT_TRUE(isSystemCatalogConstraintDefinitionId(constraint.constraint_id));
	EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());

	const std::vector<uint64_t> first =
			backend.createLogicalNodes({{{"User"}, {{"email", graph::PropertyValue(std::string("a@example.test"))}}}});
	ASSERT_EQ(first.size(), 1U);
	backend.deleteLogicalNodes({{first.front(), false, 0U}});
	const std::vector<uint64_t> replacement =
			backend.createLogicalNodes({{{"User"}, {{"email", graph::PropertyValue(std::string("a@example.test"))}}}});
	ASSERT_EQ(replacement.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	const std::vector<BTreePageEntry> entries = readIndexEntries(constraint.constraint_id);
	ASSERT_EQ(entries.size(), 1U);
	const std::vector<graph::PropertyValue> values = {graph::PropertyValue(std::string("a@example.test"))};
	const std::vector<std::byte> canonical = ConstraintSupportTree::canonicalKey(values);
	EXPECT_EQ(entries.front(),
			  (BTreePageEntry{ConstraintSupportTree::entryKey(canonical, replacement.front()), replacement.front()}));
}

TEST_F(RelocatableStorageBackendSchemaTest, PersistsEveryLogicalConstraintKindAndPropertyType) {
	RelocatableStorageBackend backend(options());
	backend.open();

	const std::vector<LogicalGraphConstraintDefinition> created = {
			backend.createLogicalConstraint({"person_unique",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_UNIQUE,
											 "Person",
											 {"unique_value"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE}),
			backend.createLogicalConstraint({"knows_required",
											 LogicalGraphConstraintEntityKind::LGCEK_EDGE,
											 LogicalGraphConstraintKind::LGCK_NOT_NULL,
											 "KNOWS",
											 {"required_value"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE}),
			backend.createLogicalConstraint({"person_boolean",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE,
											 "Person",
											 {"boolean_value"},
											 LogicalGraphConstraintPropertyType::LGCPT_BOOLEAN}),
			backend.createLogicalConstraint({"person_integer",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE,
											 "Person",
											 {"integer_value"},
											 LogicalGraphConstraintPropertyType::LGCPT_INTEGER}),
			backend.createLogicalConstraint({"person_float",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE,
											 "Person",
											 {"float_value"},
											 LogicalGraphConstraintPropertyType::LGCPT_FLOAT}),
			backend.createLogicalConstraint({"person_string",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE,
											 "Person",
											 {"string_value"},
											 LogicalGraphConstraintPropertyType::LGCPT_STRING}),
			backend.createLogicalConstraint({"person_list",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE,
											 "Person",
											 {"list_value"},
											 LogicalGraphConstraintPropertyType::LGCPT_LIST}),
			backend.createLogicalConstraint({"person_map",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE,
											 "Person",
											 {"map_value"},
											 LogicalGraphConstraintPropertyType::LGCPT_MAP}),
			backend.createLogicalConstraint({"person_key",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_NODE_KEY,
											 "Person",
											 {"key_one", "key_two"},
																		 LogicalGraphConstraintPropertyType::LGCPT_NONE}),
	};
	EXPECT_THROW(static_cast<void>(backend.listLogicalConstraints(created.size() - 1U)), FormatException);
	EXPECT_EQ(backend.listLogicalConstraints(16U), created);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_THROW(static_cast<void>(reopened.listLogicalConstraints(created.size() - 1U)), FormatException);
	EXPECT_EQ(reopened.listLogicalConstraints(16U), created);
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, PublishesAndReopensDurableConfigurationWithoutMixingRuntimeStatistics) {
	RelocatableStorageBackend backend(options());
	backend.open();
	EXPECT_EQ(backend.findConfiguration("query.timeout_ms"), std::nullopt);
	backend.setConfiguration("query.timeout_ms", graph::PropertyValue(int64_t{30}));
	backend.setConfiguration("query.slow_log.enabled", graph::PropertyValue(true));
	backend.setConfiguration("query.memory_ratio", graph::PropertyValue(0.75));
	backend.setConfiguration("query.profile_name", graph::PropertyValue("native"));
	backend.setConfiguration("query.timeout_ms", graph::PropertyValue(int64_t{45}));
	// Enumeration is deliberately bounded: callers must not receive a partial
	// durable configuration set when their advertised capacity is too small.
	EXPECT_THROW(static_cast<void>(backend.readConfigurations(0U)), FormatException);
	EXPECT_THROW(static_cast<void>(backend.readConfigurations(1U)), FormatException);
	EXPECT_THROW(static_cast<void>(backend.readConfigurations(3U)), FormatException);
	EXPECT_THROW(static_cast<void>(backend.readConfigurations(std::numeric_limits<size_t>::max())), FormatException);
	EXPECT_EQ(backend.readConfigurations(4U),
			  (std::vector<LogicalGraphConfiguration>{{"query.memory_ratio", graph::PropertyValue(0.75)},
														  {"query.profile_name", graph::PropertyValue("native")},
														  {"query.slow_log.enabled", graph::PropertyValue(true)},
														  {"query.timeout_ms", graph::PropertyValue(int64_t{45})}}));
	EXPECT_THROW(backend.setConfiguration("query.invalid", graph::PropertyValue(std::vector<graph::PropertyValue>{
																   graph::PropertyValue(true)})),
				 FormatException);
	const std::vector<LogicalGraphRuntimeStatistic> statistics = backend.runtimeStatistics();
	EXPECT_TRUE(std::ranges::any_of(statistics, [](const LogicalGraphRuntimeStatistic &statistic) {
		return statistic.category == "io" && statistic.metric == "read_calls";
	}));
	EXPECT_TRUE(std::ranges::any_of(statistics, [](const LogicalGraphRuntimeStatistic &statistic) {
		return statistic.category == "buffer_pool" && statistic.metric == "hits";
	}));
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_EQ(reopened.findConfiguration("query.timeout_ms"), graph::PropertyValue(int64_t{45}));
	EXPECT_EQ(reopened.findConfiguration("query.slow_log.enabled"), graph::PropertyValue(true));
	EXPECT_EQ(reopened.findConfiguration("query.memory_ratio"), graph::PropertyValue(0.75));
	EXPECT_EQ(reopened.findConfiguration("query.profile_name"), graph::PropertyValue("native"));
	reopened.resetIoStats();
	EXPECT_EQ(reopened.findConfiguration("query.timeout_ms"), graph::PropertyValue(int64_t{45}));
	reopened.close();

	RelocatableStorageBackendOptions readOnlyOptions = options();
	readOnlyOptions.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
	RelocatableStorageBackend readOnly(readOnlyOptions);
	readOnly.open();
	EXPECT_EQ(readOnly.findConfiguration("query.timeout_ms"), graph::PropertyValue(int64_t{45}));
	EXPECT_THROW(readOnly.setConfiguration("query.timeout_ms", graph::PropertyValue(int64_t{60})), std::runtime_error);
	readOnly.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, PublicGraphWritesReportPublicationFenceCountsAcrossIdLeaseReuse) {
	RelocatableStorageBackend backend(options());
	backend.open();

	backend.resetIoStats();
	const std::vector<uint64_t> first =
			backend.createLogicalNodes({{{"Person"}, {{"name", graph::PropertyValue("Ada")}}}});
	ASSERT_EQ(first.size(), 1U);
	const StorageIOStats firstStatistics = backend.ioStats().data;
	// A fresh writer first reserves a durable logical-ID lease, then publishes
	// the graph mutation. Both publications have one candidate-data and one
	// Superblock fence.
	EXPECT_EQ(firstStatistics.data_sync_calls, 2U);
	EXPECT_EQ(firstStatistics.superblock_sync_calls, 2U);

	backend.resetIoStats();
	const std::vector<uint64_t> second =
			backend.createLogicalNodes({{{"Person"}, {{"name", graph::PropertyValue("Grace")}}}});
	ASSERT_EQ(second.size(), 1U);
	const StorageIOStats cachedLeaseStatistics = backend.ioStats().data;
	EXPECT_EQ(cachedLeaseStatistics.data_sync_calls, 1U);
	EXPECT_EQ(cachedLeaseStatistics.superblock_sync_calls, 1U);

	const std::vector<LogicalGraphRuntimeStatistic> runtime = backend.runtimeStatistics();
	EXPECT_TRUE(std::ranges::any_of(runtime, [](const LogicalGraphRuntimeStatistic &statistic) {
		return statistic.category == "io" && statistic.metric == "data_sync_calls" &&
			   statistic.value == graph::PropertyValue(int64_t{1});
	}));
	EXPECT_TRUE(std::ranges::any_of(runtime, [](const LogicalGraphRuntimeStatistic &statistic) {
		return statistic.category == "io" && statistic.metric == "superblock_sync_calls" &&
			   statistic.value == graph::PropertyValue(int64_t{1});
	}));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, NormalReopenDoesNotScanTheLargeValueDataRegion) {
	constexpr size_t PAYLOAD_BYTES = 2U * 1024U * 1024U;
    auto rawOptions = options(); rawOptions.adaptive_property_row_chunks = false;
    // Keep a real uncompressed data region: a highly compressible carrier
    // cannot detect an accidental scan of the intended two MiB of frames.
	RelocatableStorageBackend writer(rawOptions);
	writer.open();
	const std::vector<uint64_t> nodes = writer.createLogicalNodes(
			{{{"Document"}, {{"body", graph::PropertyValue(std::string(PAYLOAD_BYTES, 'x'))}}}});
	ASSERT_EQ(nodes.size(), 1U);
	writer.close();

	const uint64_t fileBytes = std::filesystem::file_size(data_path_);
	ASSERT_GT(fileBytes, PAYLOAD_BYTES);
	RelocatableStorageBackend reopened(rawOptions);
	reopened.open();
	const StorageIOStats openStatistics = reopened.ioStats().data;
    RecordProperty("raw_payload_bytes", std::to_string(PAYLOAD_BYTES));
    RecordProperty("reopen_read_bytes", std::to_string(openStatistics.read_bytes));
	// A normal open follows the A/B Superblock roots; it must not read the raw
	// LargeValue payload merely to reconstruct the storage graph.
	// Root-anchored Catalog/allocator validation has a fixed metadata cost, but
	// it must not scale with the 2 MiB LargeValue payload. Keeping the bound
	// below half that payload rejects a regression that reads its raw chunks.
	EXPECT_LT(openStatistics.read_bytes, PAYLOAD_BYTES / 2U);
	EXPECT_GT(openStatistics.read_calls, 0U);
	RelocatableReadSnapshot snapshot = reopened.openSnapshot();
	EXPECT_EQ(reopened.readNode(snapshot, nodes.front()).node_id, nodes.front());
	snapshot.release();
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ExecutesNativeDbmsConfigurationAndStatisticsProcedures) {
	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	graph::query::QueryEngine engine(backend, backend);

	const graph::query::QueryResult set = engine.execute("CALL dbms.setConfig('query.timeout_ms', 45)");
	ASSERT_EQ(set.getColumns(), (std::vector<std::string>{"key", "value", "status"}));
	ASSERT_EQ(set.rowCount(), 1U);
	EXPECT_EQ(set.getRows().front().at("key").asPrimitive(), graph::PropertyValue("query.timeout_ms"));
	EXPECT_EQ(set.getRows().front().at("value").asPrimitive(), graph::PropertyValue(int64_t{45}));
	EXPECT_EQ(set.getRows().front().at("status").asPrimitive(), graph::PropertyValue("OK"));

	const graph::query::QueryResult projected =
			engine.execute("CALL dbms.getConfig('query.timeout_ms') YIELD key, value RETURN value");
	ASSERT_EQ(projected.getColumns(), (std::vector<std::string>{"value"}));
	ASSERT_EQ(projected.rowCount(), 1U);
	EXPECT_EQ(projected.getRows().front().at("value").asPrimitive(), graph::PropertyValue(int64_t{45}));
	EXPECT_THROW(static_cast<void>(engine.execute("CALL dbms.getConfig('missing.key')")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(engine.execute("CALL dbms.setConfig('missing.key', 1)")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(engine.execute("CALL dbms.setConfig('query.timeout_ms', true)")),
				 std::invalid_argument);

	const graph::query::QueryResult listed = engine.execute("CALL dbms.listConfig()");
	ASSERT_EQ(listed.getColumns(), (std::vector<std::string>{"key", "value"}));
	ASSERT_EQ(listed.rowCount(), 6U);
	EXPECT_EQ(listed.getRows()[0U].at("key").asPrimitive(), graph::PropertyValue("query.max_memory_mb"));
	EXPECT_EQ(listed.getRows()[0U].at("value").asPrimitive(), graph::PropertyValue(int64_t{0}));
	EXPECT_EQ(listed.getRows()[1U].at("key").asPrimitive(), graph::PropertyValue("query.max_var_length_depth"));
	EXPECT_EQ(listed.getRows()[1U].at("value").asPrimitive(), graph::PropertyValue(int64_t{50}));
	EXPECT_EQ(listed.getRows()[2U].at("key").asPrimitive(), graph::PropertyValue("query.slow_log.enabled"));
	EXPECT_EQ(listed.getRows()[2U].at("value").asPrimitive(), graph::PropertyValue(false));
	EXPECT_EQ(listed.getRows()[3U].at("key").asPrimitive(), graph::PropertyValue("query.slow_log.threshold_ms"));
	EXPECT_EQ(listed.getRows()[3U].at("value").asPrimitive(), graph::PropertyValue(int64_t{1'000}));
	EXPECT_EQ(listed.getRows()[4U].at("key").asPrimitive(), graph::PropertyValue("query.timeout_ms"));
	EXPECT_EQ(listed.getRows()[5U].at("key").asPrimitive(), graph::PropertyValue("stats.enabled"));
	EXPECT_EQ(listed.getRows()[5U].at("value").asPrimitive(), graph::PropertyValue(true));
	const graph::query::QueryResult statistics = engine.execute("CALL dbms.showStats()");
	ASSERT_EQ(statistics.getColumns(), (std::vector<std::string>{"category", "metric", "value"}));
	EXPECT_FALSE(statistics.isEmpty());
	const graph::query::QueryResult reset = engine.execute("CALL dbms.resetStats()");
	ASSERT_EQ(reset.getColumns(), (std::vector<std::string>{"result"}));
	EXPECT_EQ(reset.getRows().front().at("result").asPrimitive(), graph::PropertyValue("Statistics reset"));

	graph::query::QueryContext readOnly;
	readOnly.execMode = graph::query::ExecMode::EM_READ_ONLY;
	EXPECT_THROW(static_cast<void>(engine.execute("CALL dbms.setConfig('query.timeout_ms', 60)", readOnly)),
				 std::runtime_error);
	backend->close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   ConfigurationCandidateShortWriteRetainsTheCompleteOldValueAndReopenCanRetry) {
	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	backend->setConfiguration("query.timeout_ms", graph::PropertyValue(int64_t{30}));

	std::atomic<bool> injected = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting shortCandidate([&injected](const file_handle_t fd, const void *bytes,
																			  const size_t byteCount,
																			  const pwrite_off_t offset) {
			if (byteCount == RSS_PAGE_SIZE && !injected.exchange(true, std::memory_order_relaxed)) {
				return portable_pwrite(fd, bytes, 16U, offset);
			}
			return portable_pwrite(fd, bytes, byteCount, offset);
		});
		EXPECT_THROW(backend->setConfiguration("query.timeout_ms", graph::PropertyValue(int64_t{45})),
					 std::runtime_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_EQ(backend->findConfiguration("query.timeout_ms"), graph::PropertyValue(int64_t{30}));
	// A candidate-write failure occurred before any Superblock publication, so
	// the same handle remains usable and can retry without a needless reopen.
	EXPECT_NO_THROW(backend->setConfiguration("query.timeout_ms", graph::PropertyValue(int64_t{45})));
	EXPECT_EQ(backend->findConfiguration("query.timeout_ms"), graph::PropertyValue(int64_t{45}));
	backend->close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_EQ(reopened.findConfiguration("query.timeout_ms"), graph::PropertyValue(int64_t{45}));
	reopened.close();

	RelocatableStorageBackend retried(options());
	retried.open();
	EXPECT_EQ(retried.findConfiguration("query.timeout_ms"), graph::PropertyValue(int64_t{45}));
	retried.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   ScalarIndexDdlCandidateShortWriteLeavesNoDefinitionAndReopenCanRetry) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Person"}, {{"score", graph::PropertyValue(int64_t{17})}}}});
	ASSERT_EQ(nodes.size(), 1U);

	std::atomic<bool> injected = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
				[&injected](const file_handle_t fd, const void *bytes, const size_t byteCount, const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.exchange(true, std::memory_order_relaxed)) {
						return portable_pwrite(fd, bytes, 16U, offset);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}})),
					 std::runtime_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	// The interrupted first allocation can leave a truncated private pool control
	// page. Do not reuse that process-local allocator state; recovery starts from
	// the last complete Superblock and reclaims the unreachable candidate safely.
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
	const LogicalGraphIndexDefinition index =
			reopened.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	EXPECT_EQ(readIndexEntries(index.index_id),
			  (std::vector<BTreePageEntry>{
					  {IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{17}), nodes.front()), nodes.front()}}));
	EXPECT_EQ(reopened.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{index});
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ScalarIndexDdlCandidateDiskFullLeavesNoDefinitionAndReopenCanRetry) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Person"}, {{"score", graph::PropertyValue(int64_t{17})}}}});
	ASSERT_EQ(nodes.size(), 1U);

	std::atomic<bool> injected = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullCandidate(
				[&injected](const file_handle_t fd, const void *bytes, const size_t byteCount, const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}})),
					 std::runtime_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
	const LogicalGraphIndexDefinition index =
			reopened.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	EXPECT_EQ(readIndexEntries(index.index_id),
			  (std::vector<BTreePageEntry>{
					  {IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{17}), nodes.front()), nodes.front()}}));
	EXPECT_EQ(reopened.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{index});
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   UniqueConstraintDdlCandidateShortWriteLeavesNoDefinitionOrSupportTreeAndReopenCanRetry) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphConstraintCreateRequest request{
			"user_email_unique", LogicalGraphConstraintEntityKind::LGCEK_NODE, LogicalGraphConstraintKind::LGCK_UNIQUE,
			"User", {"email"}, LogicalGraphConstraintPropertyType::LGCPT_NONE};

	std::atomic<bool> injected = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
				[&injected](const file_handle_t fd, const void *bytes, const size_t byteCount, const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.exchange(true, std::memory_order_relaxed)) {
						return portable_pwrite(fd, bytes, 16U, offset);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(backend.createLogicalConstraint(request)), std::runtime_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_TRUE(reopened.listLogicalConstraints(8U).empty());
	const LogicalGraphConstraintDefinition constraint = reopened.createLogicalConstraint(request);
	const std::vector<graph::PropertyValue> uniqueValues = {graph::PropertyValue(std::string("ada@example.test"))};
	EXPECT_EQ(readIndexEntries(constraint.constraint_id),
			  (std::vector<BTreePageEntry>{{ConstraintSupportTree::entryKey(
																ConstraintSupportTree::canonicalKey(uniqueValues), nodes.front()),
											nodes.front()}}));
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   UniqueConstraintDdlCandidateDiskFullLeavesNoDefinitionOrSupportTreeAndReopenCanRetry) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphConstraintCreateRequest request{
			"user_email_unique", LogicalGraphConstraintEntityKind::LGCEK_NODE, LogicalGraphConstraintKind::LGCK_UNIQUE,
			"User", {"email"}, LogicalGraphConstraintPropertyType::LGCPT_NONE};

	std::atomic<bool> injected = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullCandidate(
				[&injected](const file_handle_t fd, const void *bytes, const size_t byteCount, const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(backend.createLogicalConstraint(request)), std::runtime_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_TRUE(reopened.listLogicalConstraints(8U).empty());
	const LogicalGraphConstraintDefinition constraint = reopened.createLogicalConstraint(request);
	const std::vector<graph::PropertyValue> uniqueValues = {graph::PropertyValue(std::string("ada@example.test"))};
	EXPECT_EQ(readIndexEntries(constraint.constraint_id),
			  (std::vector<BTreePageEntry>{{ConstraintSupportTree::entryKey(
											  ConstraintSupportTree::canonicalKey(uniqueValues), nodes.front()),
									  nodes.front()}}));
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   VectorIndexDdlCandidateShortWriteLeavesNoDefinitionOrManifestAndReopenCanRetry) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Embedding"}, {{"value", vectorValue(1.0, -2.0)}}}});
	ASSERT_EQ(nodes.size(), 1U);

	std::atomic<bool> injected = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
				[&injected](const file_handle_t fd, const void *bytes, const size_t byteCount, const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.exchange(true, std::memory_order_relaxed)) {
						return portable_pwrite(fd, bytes, 16U, offset);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(
						backend.createLogicalNodeVectorIndex({"embedding_value", "Embedding", "value", 2U, "l2"})),
					 std::runtime_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
	const LogicalGraphIndexDefinition index =
			reopened.createLogicalNodeVectorIndex({"embedding_value", "Embedding", "value", 2U, "l2"});
	EXPECT_EQ(index.kind, LogicalGraphIndexKind::LGIK_VECTOR);
	EXPECT_EQ(index.vector_dimensions, 2U);
	EXPECT_EQ(index.vector_metric, "l2");
	const VectorManifest manifest = readVectorManifest(index.index_id);
	EXPECT_EQ(manifest.index_id, index.index_id);
	EXPECT_EQ(manifest.dimensions, 2U);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));

	auto snapshot = reopened.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, -2.0F};
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 4U;
	const LogicalGraphVectorSearchResult result = snapshot->searchLogicalVector(request);
	ASSERT_EQ(result.hits.size(), 1U);
	EXPECT_EQ(result.hits.front().node_id, nodes.front());
	snapshot.reset();
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   VectorIndexDdlCandidateDiskFullLeavesNoDefinitionOrManifestAndReopenCanRetry) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Embedding"}, {{"value", vectorValue(1.0, -2.0)}}}});
	ASSERT_EQ(nodes.size(), 1U);

	std::atomic<bool> injected = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullCandidate(
				[&injected](const file_handle_t fd, const void *bytes, const size_t byteCount, const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(
						backend.createLogicalNodeVectorIndex({"embedding_value", "Embedding", "value", 2U, "l2"})),
					 std::runtime_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
	const LogicalGraphIndexDefinition index =
			reopened.createLogicalNodeVectorIndex({"embedding_value", "Embedding", "value", 2U, "l2"});
	EXPECT_EQ(index.kind, LogicalGraphIndexKind::LGIK_VECTOR);
	const VectorManifest manifest = readVectorManifest(index.index_id);
	EXPECT_EQ(manifest.index_id, index.index_id);
	EXPECT_EQ(manifest.dimensions, 2U);

	auto snapshot = reopened.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, -2.0F};
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 4U;
	const LogicalGraphVectorSearchResult result = snapshot->searchLogicalVector(request);
	ASSERT_EQ(result.hits.size(), 1U);
	EXPECT_EQ(result.hits.front().node_id, nodes.front());
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   ConfigurationFinalPublicationSyncFailureReportsUncertainDurabilityAndReopensTheCompleteNewValue) {
	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	backend->setConfiguration("query.timeout_ms", graph::PropertyValue(int64_t{30}));

	std::atomic<uint32_t> syncCalls = 0U;
	{
		StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence([&syncCalls](const file_handle_t fd) {
			if (syncCalls.fetch_add(1U, std::memory_order_relaxed) == 1U) {
#ifdef _WIN32
				SetLastError(ERROR_WRITE_FAULT);
#else
				errno = EIO;
#endif
				return -1;
			}
			return portable_fsync(fd);
		});
		EXPECT_THROW(backend->setConfiguration("query.timeout_ms", graph::PropertyValue(int64_t{45})),
					 std::runtime_error);
	}
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	// The call can report an error after the new configuration is already
	// durable. No other public writer may allocate or publish until recovery
	// selects the canonical generation on a fresh handle.
	EXPECT_THROW(backend->setConfiguration("query.timeout_ms", graph::PropertyValue(int64_t{60})), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend->beginLogicalGraphTransaction(false)), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend->createLogicalNodeIndex({"blocked_scalar", "Person", {"score"}})),
				 std::logic_error);
	EXPECT_THROW(static_cast<void>(
						 backend->createLogicalNodeVectorIndex({"blocked_vector", "Person", "embedding", 2U, "l2"})),
				 std::logic_error);
	EXPECT_THROW(static_cast<void>(backend->createLogicalConstraint({"blocked_constraint",
																	 LogicalGraphConstraintEntityKind::LGCEK_NODE,
																	 LogicalGraphConstraintKind::LGCK_NOT_NULL,
																	 "Person",
																	 {"score"},
																	 LogicalGraphConstraintPropertyType::LGCPT_NONE})),
				 std::logic_error);
	EXPECT_TRUE(backend->listLogicalIndexes(8U).empty());
	EXPECT_TRUE(backend->listLogicalConstraints(8U).empty());
	// The complete inactive Superblock was written before its final fence failed,
	// so a fresh handle must recover either one complete generation, here the new
	// value, rather than an in-memory or partial Catalog state.
	backend->close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_EQ(reopened.findConfiguration("query.timeout_ms"), graph::PropertyValue(int64_t{45}));
	// Retrying an uncertain caller result is idempotent at the public configuration
	// boundary and must leave one complete durable value after another reopen.
	reopened.setConfiguration("query.timeout_ms", graph::PropertyValue(int64_t{45}));
	reopened.close();

	RelocatableStorageBackend retried(options());
	retried.open();
	EXPECT_EQ(retried.findConfiguration("query.timeout_ms"), graph::PropertyValue(int64_t{45}));
	retried.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   ScalarIndexDdlFinalPublicationSyncFailureRequiresReopenAndExposesOnlyACompleteSchemaGeneration) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Person"}, {{"score", graph::PropertyValue(int64_t{17})}}}});
	ASSERT_EQ(nodes.size(), 1U);

	std::atomic<bool> inactiveSuperblockWritten = false;
	std::atomic<uint32_t> syncCalls = 0U;
	{
		StorageIO::ScopedNativeWriteHookForTesting observeInactiveSuperblock(
				[&inactiveSuperblockWritten](const file_handle_t fd, const void *bytes, const size_t byteCount,
											 const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						inactiveSuperblockWritten.store(true, std::memory_order_relaxed);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence(
				[&inactiveSuperblockWritten, &syncCalls](const file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					if (inactiveSuperblockWritten.load(std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}})),
					 std::runtime_error);
	}
	EXPECT_TRUE(inactiveSuperblockWritten.load(std::memory_order_relaxed));
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);

	// A DDL caller cannot know whether the returned error followed a durable
	// catalog publication. The current handle must not publish any more roots.
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"BlockedUntilReopen"}, {}}})), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndex({"blocked_scalar", "Person", {"score"}})),
				 std::logic_error);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	const std::vector<LogicalGraphIndexDefinition> indexes = reopened.listLogicalIndexes(8U);
	ASSERT_LE(indexes.size(), 1U);
	if (indexes.empty()) {
		const LogicalGraphIndexDefinition retried =
				reopened.createLogicalNodeIndex({"person_score", "Person", {"score"}});
		EXPECT_EQ(readIndexEntries(retried.index_id),
				  (std::vector<BTreePageEntry>{
						  {IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{17}), nodes.front()),
						   nodes.front()}}));
	} else {
		const LogicalGraphIndexDefinition &published = indexes.front();
		EXPECT_EQ(published.name, "person_score");
		EXPECT_EQ(published.label, "Person");
		EXPECT_EQ(published.property_keys, (std::vector<std::string>{"score"}));
		EXPECT_EQ(readIndexEntries(published.index_id),
				  (std::vector<BTreePageEntry>{
						  {IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{17}), nodes.front()),
						   nodes.front()}}));
	}
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   VectorIndexDdlFinalPublicationSyncFailureRequiresReopenAndExposesOnlyACompleteSchemaGeneration) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Embedding"}, {{"value", vectorValue(1.0, -2.0)}}}});
	ASSERT_EQ(nodes.size(), 1U);

	std::atomic<bool> inactiveSuperblockWritten = false;
	std::atomic<uint32_t> syncCalls = 0U;
	{
		StorageIO::ScopedNativeWriteHookForTesting observeInactiveSuperblock(
				[&inactiveSuperblockWritten](const file_handle_t fd, const void *bytes, const size_t byteCount,
											 const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						inactiveSuperblockWritten.store(true, std::memory_order_relaxed);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence(
				[&inactiveSuperblockWritten, &syncCalls](const file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					if (inactiveSuperblockWritten.load(std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(
							 backend.createLogicalNodeVectorIndex({"embedding_value", "Embedding", "value", 2U, "l2"})),
					 std::runtime_error);
	}
	EXPECT_TRUE(inactiveSuperblockWritten.load(std::memory_order_relaxed));
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);

	// Manifest, mapping roots, and the catalog definition share one publication.
	// An ambiguous outcome cannot be followed by another write on this handle.
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"BlockedUntilReopen"}, {}}})), std::logic_error);
	EXPECT_THROW(
			static_cast<void>(backend.createLogicalNodeVectorIndex({"blocked_vector", "Embedding", "value", 2U, "l2"})),
			std::logic_error);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	std::vector<LogicalGraphIndexDefinition> indexes = reopened.listLogicalIndexes(8U);
	ASSERT_LE(indexes.size(), 1U);
	if (indexes.empty()) {
		indexes.push_back(reopened.createLogicalNodeVectorIndex({"embedding_value", "Embedding", "value", 2U, "l2"}));
	}
	ASSERT_EQ(indexes.size(), 1U);
	const LogicalGraphIndexDefinition &published = indexes.front();
	EXPECT_EQ(published.name, "embedding_value");
	EXPECT_EQ(published.kind, LogicalGraphIndexKind::LGIK_VECTOR);
	EXPECT_EQ(published.label, "Embedding");
	EXPECT_EQ(published.property_keys, (std::vector<std::string>{"value"}));
	EXPECT_EQ(published.vector_dimensions, 2U);
	EXPECT_EQ(published.vector_metric, "l2");

	auto snapshot = reopened.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = published.name;
	request.query_vector = {1.0F, -2.0F};
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 4U;
	const LogicalGraphVectorSearchResult result = snapshot->searchLogicalVector(request);
	ASSERT_EQ(result.hits.size(), 1U);
	EXPECT_EQ(result.hits.front().node_id, nodes.front());
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   UniqueConstraintDdlFinalPublicationSyncFailureRequiresReopenAndExposesOnlyACompleteSchemaGeneration) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphConstraintCreateRequest request{"user_email_unique",
													  LogicalGraphConstraintEntityKind::LGCEK_NODE,
													  LogicalGraphConstraintKind::LGCK_UNIQUE,
													  "User",
													  {"email"},
													  LogicalGraphConstraintPropertyType::LGCPT_NONE};

	std::atomic<bool> inactiveSuperblockWritten = false;
	std::atomic<uint32_t> syncCalls = 0U;
	{
		StorageIO::ScopedNativeWriteHookForTesting observeInactiveSuperblock(
				[&inactiveSuperblockWritten](const file_handle_t fd, const void *bytes, const size_t byteCount,
											 const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						inactiveSuperblockWritten.store(true, std::memory_order_relaxed);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence(
				[&inactiveSuperblockWritten, &syncCalls](const file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					if (inactiveSuperblockWritten.load(std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(backend.createLogicalConstraint(request)), std::runtime_error);
	}
	EXPECT_TRUE(inactiveSuperblockWritten.load(std::memory_order_relaxed));
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"BlockedUntilReopen"}, {}}})), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.createLogicalConstraint(request)), std::logic_error);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	std::vector<LogicalGraphConstraintDefinition> constraints = reopened.listLogicalConstraints(8U);
	ASSERT_LE(constraints.size(), 1U);
	if (constraints.empty()) {
		constraints.push_back(reopened.createLogicalConstraint(request));
	}
	ASSERT_EQ(constraints.size(), 1U);
	const LogicalGraphConstraintDefinition &published = constraints.front();
	EXPECT_EQ(published.name, request.name);
	EXPECT_EQ(published.entity_kind, request.entity_kind);
	EXPECT_EQ(published.kind, request.kind);
	EXPECT_EQ(published.label, request.label);
	EXPECT_EQ(published.property_keys, request.property_keys);
	const std::vector<graph::PropertyValue> uniqueValues = {graph::PropertyValue(std::string("ada@example.test"))};
	EXPECT_EQ(readIndexEntries(published.constraint_id),
			  (std::vector<BTreePageEntry>{{ConstraintSupportTree::entryKey(
													ConstraintSupportTree::canonicalKey(uniqueValues), nodes.front()),
											nodes.front()}}));
	EXPECT_THROW(static_cast<void>(reopened.createLogicalNodes(
						 {{{"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}}})),
				 std::runtime_error);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   GraphMutationFinalPublicationSyncFailureReopensOneCompleteLargeValueScalarIndexAndVectorGeneration) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	const std::string largePayload(8U * 1024U, 'x');
	RelocatableStorageBackend backend(options());
	backend.open();
	const LogicalGraphIndexDefinition scalar = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	const LogicalGraphIndexDefinition vector =
			backend.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	const LogicalGraphNodeCreateRequest nodeCreate{{"Person"},
												   {{"embedding", vectorValue(1.0, -2.0)},
													{"payload", graph::PropertyValue(largePayload)},
													{"score", graph::PropertyValue(int64_t{17})}}};

	std::atomic<bool> inactiveSuperblockWritten = false;
	std::atomic<uint32_t> syncCalls = 0U;
	{
		StorageIO::ScopedNativeWriteHookForTesting observeInactiveSuperblock(
				[&inactiveSuperblockWritten](const file_handle_t fd, const void *bytes, const size_t byteCount,
											 const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						inactiveSuperblockWritten.store(true, std::memory_order_relaxed);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence(
				[&inactiveSuperblockWritten, &syncCalls](const file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					if (inactiveSuperblockWritten.load(std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({nodeCreate})), std::runtime_error);
	}
	EXPECT_TRUE(inactiveSuperblockWritten.load(std::memory_order_relaxed));
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> graphSnapshot = reopened.openLogicalGraphReadSnapshot();
	ASSERT_NE(graphSnapshot, nullptr);
	const LogicalGraphNodePage nodes = graphSnapshot->scanNodesAfter(0U, 2U, 16U);
	graphSnapshot.reset();
	auto vectorSnapshot = reopened.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(vectorSnapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = vector.name;
	request.query_vector = {1.0F, -2.0F};
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 4U;
	const LogicalGraphVectorSearchResult result = vectorSnapshot->searchLogicalVector(request);
	vectorSnapshot.reset();

	// A failed final sync leaves caller durability uncertain: reopening may select
	// the complete old generation or the complete new generation, never a mix.
	if (nodes.nodes.empty()) {
		EXPECT_TRUE(readIndexEntries(scalar.index_id).empty());
		EXPECT_TRUE(result.hits.empty());
	} else {
		ASSERT_EQ(nodes.nodes.size(), 1U);
		const LogicalGraphNode &node = nodes.nodes.front();
		EXPECT_EQ(node.labels, std::vector<std::string>{"Person"});
		const auto payload = std::ranges::find_if(
				node.properties, [](const LogicalGraphPropertyEntry &entry) { return entry.key == "payload"; });
		ASSERT_NE(payload, node.properties.end());
		EXPECT_EQ(payload->value, graph::PropertyValue(largePayload));
		EXPECT_EQ(readIndexEntries(scalar.index_id),
				  (std::vector<BTreePageEntry>{
						  {IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{17}), node.node_id),
						   node.node_id}}));
		EXPECT_NO_THROW(static_cast<void>(readVectorManifest(vector.index_id)));
		ASSERT_EQ(result.hits.size(), 1U);
		EXPECT_EQ(result.hits.front().node_id, node.node_id);
	}
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, AppliesDurableQueryMemoryPolicyToTheEngineDefaultGuard) {
	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	graph::query::QueryEngine engine(backend, backend);
	static_cast<void>(engine.execute("CALL dbms.setConfig('query.max_memory_mb', 1)"));

	// range() materializes its rows during execution, after QueryGuard captures
	// the query baseline. This verifies a durable per-query memory policy rather
	// than rejecting the database process's preexisting resident set.
	// Keep the successful run below the independent 64 MiB execution materialization limit.
	const std::string query = "UNWIND range(0, 65536) AS value RETURN value";
	EXPECT_THROW(static_cast<void>(engine.execute(query)), graph::query::QueryMemoryExceededException);

	static_cast<void>(engine.execute("CALL dbms.setConfig('query.max_memory_mb', 0)"));
	EXPECT_NO_THROW(static_cast<void>(engine.execute(query)));
	backend->close();
}

TEST_F(RelocatableStorageBackendSchemaTest, AppliesDurableVariableLengthDepthAsAnUnbypassableExecutionCeiling) {
	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	const std::vector<uint64_t> nodes = backend->createLogicalNodes({{{"Start"}, {}}, {{"Hop"}, {}}, {{"End"}, {}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(
			backend->createLogicalEdges({{nodes[0U], nodes[1U], "LINK", {}}, {nodes[1U], nodes[2U], "LINK", {}}}));
	graph::query::QueryEngine engine(backend, backend);
	const std::string query = "MATCH (a:Start)-[:LINK*1..2]->(b) RETURN b";

	static_cast<void>(engine.execute("CALL dbms.setConfig('query.max_var_length_depth', 1)"));
	EXPECT_EQ(engine.execute(query).rowCount(), 1U);
	graph::query::QueryContext callerLimit;
	callerLimit.maxVarLengthDepth = 2;
	EXPECT_EQ(engine.execute(query, callerLimit).rowCount(), 1U);

	static_cast<void>(engine.execute("CALL dbms.setConfig('query.max_var_length_depth', 2)"));
	EXPECT_EQ(engine.execute(query).rowCount(), 2U);
	callerLimit.maxVarLengthDepth = 1;
	EXPECT_EQ(engine.execute(query, callerLimit).rowCount(), 1U);
	backend->close();
}
