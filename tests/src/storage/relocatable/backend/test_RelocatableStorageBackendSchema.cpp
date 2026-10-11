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
	RelocatableStorageBackend writer(options());
	writer.open();
	const std::vector<uint64_t> nodes = writer.createLogicalNodes(
			{{{"Document"}, {{"body", graph::PropertyValue(std::string(PAYLOAD_BYTES, 'x'))}}}});
	ASSERT_EQ(nodes.size(), 1U);
	writer.close();

	const uint64_t fileBytes = std::filesystem::file_size(data_path_);
	ASSERT_GT(fileBytes, PAYLOAD_BYTES);
	RelocatableStorageBackend reopened(options());
	reopened.open();
	const StorageIOStats openStatistics = reopened.ioStats().data;
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

TEST_F(RelocatableStorageBackendSchemaTest, ResolvesExactUniqueNodeLookupsThroughThePinnedSupportTree) {
	RelocatableStorageBackend backend(options());
	backend.open();
	static_cast<void>(backend.createLogicalConstraint({"user_email_unique",
													   LogicalGraphConstraintEntityKind::LGCEK_NODE,
													   LogicalGraphConstraintKind::LGCK_UNIQUE,
													   "User",
													   {"email"},
													   LogicalGraphConstraintPropertyType::LGCPT_NONE}));
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"User"},
										 {{"email", graph::PropertyValue(std::string("ada@example.test"))},
										  {"payload", graph::PropertyValue(std::string("large retained text"))}}}});
	ASSERT_EQ(nodes.size(), 1U);

	const auto lookup = [&backend](const std::string &email) {
		const std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
		return snapshot->findNodesByExactProperties({"User"}, {{"email", graph::PropertyValue(email)}}, 1U, 2U);
	};
	const auto matched = lookup("ada@example.test");
	ASSERT_TRUE(matched.has_value());
	ASSERT_EQ(matched->size(), 1U);
	EXPECT_EQ(matched->front().node_id, nodes.front());
	const auto missing = lookup("missing@example.test");
	ASSERT_TRUE(missing.has_value());
	EXPECT_TRUE(missing->empty());

	{
		const std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
		EXPECT_EQ(snapshot->findNodesByExactProperties(
						  {"User"}, {{"payload", graph::PropertyValue(std::string("large retained text"))}}, 1U, 2U),
				  std::nullopt);
	}

	backend.replaceLogicalNodeProperties(
			{{nodes.front(),
			  {{"email", graph::PropertyValue(std::string("grace@example.test"))},
			   {"payload", graph::PropertyValue(std::string("replacement retained text"))}}}});
	const auto oldKey = lookup("ada@example.test");
	ASSERT_TRUE(oldKey.has_value());
	EXPECT_TRUE(oldKey->empty());
	const auto updated = lookup("grace@example.test");
	ASSERT_TRUE(updated.has_value());
	ASSERT_EQ(updated->size(), 1U);
	EXPECT_EQ(updated->front().node_id, nodes.front());

	backend.deleteLogicalNodes({{nodes.front(), false, 0U}});
	const auto deleted = lookup("grace@example.test");
	ASSERT_TRUE(deleted.has_value());
	EXPECT_TRUE(deleted->empty());
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsPersistedUniqueSupportTreeEntriesWithMismatchedEntityValues) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const LogicalGraphConstraintDefinition constraint =
			backend.createLogicalConstraint({"user_email_unique",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_UNIQUE,
											 "User",
											 {"email"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE});
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}}});
	ASSERT_EQ(nodes.size(), 1U);
	backend.close();

	const std::vector<graph::PropertyValue> values = {graph::PropertyValue(std::string("ada@example.test"))};
	const std::vector<std::byte> key =
			ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(values), nodes.front());
	// Preserve a valid B+Tree page encoding while modelling a stale support-tree payload.
	overwriteBTreeLeafEntry(constraint.constraint_id, key, {key, nodes.front() + 1U});

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	RelocatableReadSnapshot snapshot = corrupted.openSnapshot();
	EXPECT_THROW(
			static_cast<void>(corrupted.findNodesByExactProperties(
					snapshot, {"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}, 1U, 2U)),
			FormatException);
	snapshot.release();
	EXPECT_THROW(static_cast<void>(corrupted.verifyIntegrity()), FormatException);
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsPersistedScalarIndexEntriesWithMismatchedEntityValues) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphIndexDefinition index = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	backend.close();

	const std::vector<std::byte> key = IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{42}), nodes.front());
	// Keep the page structurally valid while modelling a stale durable payload
	// that cannot be created through the public graph writer.
	overwriteBTreeLeafEntry(index.index_id, key, {key, nodes.front() + 1U});

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	RelocatableReadSnapshot snapshot = corrupted.openSnapshot();
	std::unique_ptr<LogicalGraphReadSnapshot> graphSnapshot = corrupted.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(corrupted.findNodesByExactProperties(
				snapshot, {"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 1U, 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(graphSnapshot->visitNodeCandidatesByExactProperties(
				{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 1U, 1U,
				[](const graph::storage::IndexCandidateBatch &) { return true; })),
			 FormatException);
	snapshot.release();
	graphSnapshot.reset();
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsPersistedScalarIndexKeysThatDisagreeWithThePropertyRecord) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphIndexDefinition index = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	backend.close();

	const std::vector<std::byte> storedKey = IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{42}), nodes.front());
	const std::vector<std::byte> staleKey = IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{43}), nodes.front());
	// A page checksum authenticates this replacement key, but not its agreement
	// with the separate authoritative Property collection.
	overwriteBTreeLeafEntry(index.index_id, storedKey, {staleKey, nodes.front()});

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	RelocatableReadSnapshot snapshot = corrupted.openSnapshot();
	EXPECT_THROW(static_cast<void>(corrupted.findNodesByExactProperties(
				snapshot, {"Person"}, {{"score", graph::PropertyValue(int64_t{43})}}, 1U, 1U)),
			 FormatException);
	snapshot.release();
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsPersistedScalarRangeIndexEntriesWithMismatchedEntityValues) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphIndexDefinition index = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	backend.close();

	const std::vector<std::byte> key = IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{42}), nodes.front());
	overwriteBTreeLeafEntry(index.index_id, key, {key, nodes.front() + 1U});

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	RelocatableReadSnapshot snapshot = corrupted.openSnapshot();
	std::unique_ptr<LogicalGraphReadSnapshot> graphSnapshot = corrupted.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(corrupted.findNodesByPropertyRange(
				snapshot, {"Person"},
				{"score", graph::PropertyValue(int64_t{42}), graph::PropertyValue(), true, true}, 1U, 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(graphSnapshot->visitNodeCandidatesByPropertyRange(
				{"Person"},
				{"score", graph::PropertyValue(int64_t{42}), graph::PropertyValue(), true, true}, 1U, 1U,
				[](const graph::storage::IndexCandidateBatch &) { return true; })),
			 FormatException);
	snapshot.release();
	graphSnapshot.reset();
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsPersistedCompositeRangeIndexEntriesWithMismatchedEntityValues) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{30})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphIndexDefinition index =
			backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}});
	backend.close();

	const std::vector<graph::PropertyValue> values = {graph::PropertyValue("Shanghai"), graph::PropertyValue(int64_t{30})};
	const std::vector<std::byte> key = IndexKeyCodec::encodeCompositeEntryKey(values, nodes.front());
	overwriteBTreeLeafEntry(index.index_id, key, {key, nodes.front() + 1U});

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	RelocatableReadSnapshot snapshot = corrupted.openSnapshot();
	std::unique_ptr<LogicalGraphReadSnapshot> graphSnapshot = corrupted.openLogicalGraphReadSnapshot();
	const LogicalGraphCompositePropertyRange request{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{30}), graph::PropertyValue(), true, true}};
	EXPECT_THROW(static_cast<void>(corrupted.findNodesByCompositePropertyRange(snapshot, {"Person"}, request, 1U, 2U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(graphSnapshot->visitNodeCandidatesByCompositePropertyRange(
				{"Person"}, request, 1U, 2U,
				[](const graph::storage::IndexCandidateBatch &) { return true; })),
			 FormatException);
	snapshot.release();
	graphSnapshot.reset();
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsIndexCandidatePagesWithMismatchedOwners) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})},
																{"city", graph::PropertyValue("Shanghai")},
																{"age", graph::PropertyValue(int64_t{30})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphIndexDefinition scalar = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	const LogicalGraphIndexDefinition composite =
			backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}});
	backend.close();

	mutateBTreeRootOwner(scalar.index_id, [](PageOwnerHeader &owner) { ++owner.owner_id; });
	RelocatableStorageBackend scalarCorrupted(options());
	scalarCorrupted.open();
	RelocatableReadSnapshot scalarSnapshot = scalarCorrupted.openSnapshot();
	EXPECT_THROW(static_cast<void>(scalarCorrupted.findNodesByExactProperties(
				scalarSnapshot, {"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 1U, 4U)), FormatException);
	scalarSnapshot.release();
	scalarCorrupted.close();

	mutateBTreeRootOwner(scalar.index_id, [](PageOwnerHeader &owner) { owner.kind = PageOwnerKind::POK_ENTITY; });
	RelocatableStorageBackend scalarRangeCorrupted(options());
	scalarRangeCorrupted.open();
	RelocatableReadSnapshot scalarRangeSnapshot = scalarRangeCorrupted.openSnapshot();
	EXPECT_THROW(static_cast<void>(scalarRangeCorrupted.findNodesByPropertyRange(
				scalarRangeSnapshot, {"Person"},
				{"score", graph::PropertyValue(int64_t{0}), graph::PropertyValue(int64_t{100}), true, true}, 1U, 4U)),
				FormatException);
	scalarRangeSnapshot.release();
	scalarRangeCorrupted.close();

	mutateBTreeRootOwner(composite.index_id, [](PageOwnerHeader &owner) { owner.kind = PageOwnerKind::POK_ENTITY; });
	RelocatableStorageBackend compositeCorrupted(options());
	compositeCorrupted.open();
	RelocatableReadSnapshot compositeSnapshot = compositeCorrupted.openSnapshot();
	const LogicalGraphCompositePropertyRange request{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{30}), graph::PropertyValue(), true, true}};
	EXPECT_THROW(static_cast<void>(compositeCorrupted.findNodesByCompositePropertyRange(
				compositeSnapshot, {"Person"}, request, 1U, 4U)), FormatException);
	compositeSnapshot.release();
	compositeCorrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsIndexCandidatePagesWithChecksumCorruption) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphIndexDefinition index = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	backend.close();
	flipBTreeRootPayloadByte(index.index_id);

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = corrupted.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByPropertyRange(
				{"Person"}, {"score", graph::PropertyValue(int64_t{0}), graph::PropertyValue(int64_t{100}), true, true}, 1U,
				4U, [](const graph::storage::IndexCandidateBatch &) { return true; })), FormatException);
	snapshot.reset();
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ResolvesExactScalarAndCompositeNodeLookupsThroughPinnedIndexes) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"Person"},
																	 {{"score", graph::PropertyValue(int64_t{42})},
																	  {"city", graph::PropertyValue("Shanghai")},
																	  {"age", graph::PropertyValue(int64_t{30})}}},
																	{{"Person"},
																	 {{"score", graph::PropertyValue(int64_t{42})},
																	  {"city", graph::PropertyValue("Beijing")},
																	  {"age", graph::PropertyValue(int64_t{31})}}},
																	{{"Person"},
																	 {{"score", graph::PropertyValue(int64_t{7})},
																	  {"city", graph::PropertyValue("Shanghai")},
																	  {"age", graph::PropertyValue(int64_t{29})}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));

	{
		const std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
		ASSERT_NE(snapshot, nullptr);
		const auto scoreMatches = snapshot->findNodesByExactProperties(
				{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 3U, 3U);
		ASSERT_TRUE(scoreMatches.has_value());
		ASSERT_EQ(scoreMatches->size(), 2U);
		EXPECT_EQ(scoreMatches->at(0).node_id, nodes.at(0));
		EXPECT_EQ(scoreMatches->at(1).node_id, nodes.at(1));
		EXPECT_THROW(static_cast<void>(snapshot->findNodesByExactProperties(
							 {"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 1U, 3U)),
					 FormatException);
		// The overflow-safe probe must reject an unrepresentable result bound
		// instead of wrapping maximum-size + 1 to zero and returning a false miss.
		EXPECT_THROW(static_cast<void>(snapshot->findNodesByExactProperties(
							 {"Person"}, {{"score", graph::PropertyValue(int64_t{42})}},
							 std::numeric_limits<size_t>::max(), 3U)), FormatException);

		const auto compositeMatch = snapshot->findNodesByExactProperties(
				{"Person"}, {{"age", graph::PropertyValue(int64_t{30})}, {"city", graph::PropertyValue("Shanghai")}},
				3U, 3U);
		ASSERT_TRUE(compositeMatch.has_value());
		ASSERT_EQ(compositeMatch->size(), 1U);
		EXPECT_EQ(compositeMatch->front().node_id, nodes.at(0));
		const graph::storage::LogicalGraphCompositePropertyRange compositeRangeRequest{
				{{"city", graph::PropertyValue("Shanghai")}},
				{"age", graph::PropertyValue(int64_t{29}), graph::PropertyValue(int64_t{31}), true, false}};
		const auto compositeRange =
				snapshot->findNodesByCompositePropertyRange({"Person"}, compositeRangeRequest, 3U, 3U);
		ASSERT_TRUE(compositeRange.has_value());
		ASSERT_EQ(compositeRange->size(), 2U);
		EXPECT_EQ(compositeRange->at(0).node_id, nodes.at(0));
		EXPECT_EQ(compositeRange->at(1).node_id, nodes.at(2));
		const auto compositeRangeLookup = [&snapshot](graph::PropertyValue minValue, graph::PropertyValue maxValue,
																	 bool minInclusive = true, bool maxInclusive = true,
																	 size_t maximumNodes = 3U) {
			return snapshot->findNodesByCompositePropertyRange(
					{"Person"}, {{{"city", graph::PropertyValue("Shanghai")}},
								 {"age", std::move(minValue), std::move(maxValue), minInclusive, maxInclusive}},
					maximumNodes, 3U);
		};
		const auto lowerOnly = compositeRangeLookup(graph::PropertyValue(int64_t{30}), graph::PropertyValue());
		ASSERT_TRUE(lowerOnly.has_value());
		ASSERT_EQ(lowerOnly->size(), 1U);
		EXPECT_EQ(lowerOnly->front().node_id, nodes.at(0));
		const auto upperInclusive = compositeRangeLookup(graph::PropertyValue(), graph::PropertyValue(int64_t{30}));
		ASSERT_TRUE(upperInclusive.has_value());
		ASSERT_EQ(upperInclusive->size(), 2U);
		EXPECT_EQ(upperInclusive->at(0).node_id, nodes.at(0));
		EXPECT_EQ(upperInclusive->at(1).node_id, nodes.at(2));
		const auto openInterval = compositeRangeLookup(
				graph::PropertyValue(int64_t{29}), graph::PropertyValue(int64_t{31}), false, false);
		ASSERT_TRUE(openInterval.has_value());
		ASSERT_EQ(openInterval->size(), 1U);
		EXPECT_EQ(openInterval->front().node_id, nodes.at(0));
		EXPECT_THROW(static_cast<void>(compositeRangeLookup(
					 graph::PropertyValue(int64_t{29}), graph::PropertyValue(), true, true, 1U)),
				 FormatException);
		EXPECT_EQ(snapshot->findNodesByCompositePropertyRange(
						  {"Person"}, {{{"age", graph::PropertyValue(int64_t{30})}},
										  {"city", graph::PropertyValue("Shanghai"), graph::PropertyValue(), true, true}},
						  3U, 3U),
				  std::nullopt);
		const auto unknownProperty = snapshot->findNodesByCompositePropertyRange(
				{"Person"}, {{{"city", graph::PropertyValue("Shanghai")}},
							 {"missing", graph::PropertyValue(int64_t{1}), graph::PropertyValue(), true, true}},
				3U, 3U);
		ASSERT_TRUE(unknownProperty.has_value());
		EXPECT_TRUE(unknownProperty->empty());
		EXPECT_EQ(snapshot->findNodesByCompositePropertyRange(
						  {"Person"}, {{{"city", graph::PropertyValue(std::vector<graph::PropertyValue>{
										  graph::PropertyValue("Shanghai")})}},
										  {"age", graph::PropertyValue(int64_t{29}), graph::PropertyValue(), true, true}},
						  3U, 3U),
				  std::nullopt);
		const auto missing = snapshot->findNodesByExactProperties(
				{"Person"}, {{"score", graph::PropertyValue(int64_t{999})}}, 3U, 3U);
		ASSERT_TRUE(missing.has_value());
		EXPECT_TRUE(missing->empty());
		EXPECT_EQ(
				snapshot->findNodesByExactProperties({"Person"},
													 {{"score", graph::PropertyValue(std::vector<graph::PropertyValue>{
																		graph::PropertyValue(int64_t{42})})}},
													 3U, 3U),
				std::nullopt);
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ReusesTheSnapshotReadSessionForExactIndexCandidates) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}},
			 {{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	EXPECT_TRUE(snapshot->supportsNativeExactNodeCandidates());
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const auto first = snapshot->findNodeCandidatesByExactProperties(
			{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 2U, 1U);
	const auto second = snapshot->findNodeCandidatesByExactProperties(
			{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 2U, 1U);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(second.has_value());
	ASSERT_TRUE(snapshot->nativeSnapshotGeneration().has_value());
	EXPECT_EQ(first->snapshot_generation, *snapshot->nativeSnapshotGeneration());
	EXPECT_EQ(second->snapshot_generation, *snapshot->nativeSnapshotGeneration());
	EXPECT_TRUE(first->exact_match_verified);
	EXPECT_TRUE(second->exact_match_verified);
	ASSERT_EQ(first->candidates.size(), nodes.size());
	EXPECT_EQ(first->candidates, second->candidates);
	EXPECT_EQ(first->candidates.at(0).entity_id, nodes.at(0));
	EXPECT_EQ(first->candidates.at(1).entity_id, nodes.at(1));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_label_token_resolution"));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_constraint_definition_read"));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_definition_read"));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_property_token_resolution"));
	ASSERT_TRUE(trace.contains("native_graph.read.property_selected_column_batch_owners"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_label_token_resolution").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_property_token_resolution").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_constraint_definition_read").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_definition_read").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.property_selected_column_batch_owners").totalValue,
			  static_cast<int64_t>(nodes.size() * 2U));

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ReusesTheSnapshotReadSessionForScalarRangeIndexes) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}},
			 {{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const auto first = snapshot->findNodesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}, 2U, 1U);
	const auto second = snapshot->findNodesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}, 2U, 1U);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(second.has_value());
	ASSERT_EQ(first->size(), nodes.size());
	EXPECT_EQ(*first, *second);
	ASSERT_TRUE(trace.contains("native_graph.read.range_index_label_token_resolution"));
	ASSERT_TRUE(trace.contains("native_graph.read.range_index_property_token_resolution"));
	ASSERT_TRUE(trace.contains("native_graph.read.range_index_definition_read"));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_root_descent"));
	ASSERT_TRUE(trace.contains("native_graph.read.property_selected_batch_owners"));
	EXPECT_EQ(trace.at("native_graph.read.range_index_label_token_resolution").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.range_index_property_token_resolution").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.range_index_definition_read").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_root_descent").totalValue, 2);
	// The first range lookup fills the snapshot-owned selected-property cache;
	// the repeated lookup reuses those values and performs no second owner batch.
	EXPECT_EQ(trace.at("native_graph.read.property_selected_batch_owners").totalValue, 2);

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ScalarRangeVerificationUsesOneGlobalPropertyBatch) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	requests.reserve(300U);
	for (int64_t score = 1; score <= 300; ++score) {
		requests.push_back({{"Person"}, {{"score", graph::PropertyValue(score)}}});
	}
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), requests.size());
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const auto candidates = snapshot->findNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{1}), graph::PropertyValue(int64_t{300}), true, true},
			requests.size(), 1U);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_TRUE(candidates.has_value());
	ASSERT_EQ(candidates->candidates.size(), requests.size());
	ASSERT_EQ(candidates->verified_property_columns.size(), 1U);
	EXPECT_EQ(candidates->verified_property_columns.front().values.size(), requests.size());
	ASSERT_TRUE(trace.contains("native_graph.read.property_selected_batch_owners"));
	EXPECT_EQ(trace.at("native_graph.read.property_selected_batch_owners").totalValue,
			  static_cast<int64_t>(requests.size()));
	ASSERT_TRUE(trace.contains("native_graph.read.directory_range_cursor_root_descents"));
	// Label validation and Property validation use separate authenticated
	// directory namespaces; each is descended once for the complete batch.
	EXPECT_LE(trace.at("native_graph.read.directory_range_cursor_root_descents").totalValue, 2);
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_liveness_probes"));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_verified_rows"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_liveness_probes").totalValue,
			  static_cast<int64_t>(requests.size()));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_verified_rows").totalValue,
			  static_cast<int64_t>(requests.size()));
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ScalarRangeCandidateStreamStopsBeforeReadingLaterBatches) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	requests.reserve(600U);
	for (int64_t score = 1; score <= 600; ++score) {
		requests.push_back({{"Person"}, {{"score", graph::PropertyValue(score)}}});
	}
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), requests.size());
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	ASSERT_TRUE(snapshot->supportsNativeRangeNodeCandidates());

	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	size_t callbackCount = 0U;
	size_t callbackEntries = 0U;
	const auto stopped = snapshot->visitNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{1}), graph::PropertyValue(int64_t{600}), true, true},
			requests.size(), 1U, [&](const graph::storage::IndexCandidateBatch &batch) -> bool {
				++callbackCount;
				callbackEntries += batch.candidates.size();
				EXPECT_EQ(batch.snapshot_generation, snapshot->nativeSnapshotGeneration().value());
				EXPECT_TRUE(batch.predicates_verified);
				EXPECT_EQ(batch.verified_property_columns.size(), 1U);
				if (batch.verified_property_columns.size() != 1U) {
					return false;
				}
				EXPECT_EQ(batch.verified_property_columns.front().values.size(), batch.candidates.size());
				EXPECT_TRUE(std::ranges::is_sorted(batch.candidates, {}, &graph::storage::EntityRef::entity_id));
				return false;
			});
		const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(stopped.has_value());
	EXPECT_FALSE(*stopped);
	EXPECT_EQ(callbackCount, 1U);
	EXPECT_EQ(callbackEntries, graph::storage::BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES);
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_root_descent"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_root_descent").totalValue, 1);
	// Early termination must not validate a second Property batch or read the
	// remaining index leaves after the first bounded batch was accepted.
	ASSERT_TRUE(trace.contains("native_graph.read.property_selected_batch_owners"));
	EXPECT_EQ(trace.at("native_graph.read.property_selected_batch_owners").totalValue,
			static_cast<int64_t>(graph::storage::BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES));

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, CandidateStreamsEnforceNodeResultBounds) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<LogicalGraphNodeCreateRequest> requests{
			{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
								 {"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{7})}}},
			{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
								 {"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{42})}}}};
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);

	const auto acceptAll = [](const graph::storage::IndexCandidateBatch &) { return true; };
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByExactProperties(
			{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}, 1U, 8U, acceptAll)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{7}), true, true},
			1U, 8U, acceptAll)), FormatException);
	const LogicalGraphCompositePropertyRange composite{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{0}), graph::PropertyValue(int64_t{100}), true, true}};
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByCompositePropertyRange(
			{"Person"}, composite, 1U, 8U, acceptAll)), FormatException);

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, CandidateStreamsEnforceProducerByteBounds) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::string largeValue(256U, 'x');
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({
			{{"Person"}, {{"code", graph::PropertyValue(largeValue)},
								 {"city", graph::PropertyValue(largeValue)}, {"age", graph::PropertyValue(int64_t{7})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_code", "Person", {"code"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const auto acceptAll = [](const graph::storage::IndexCandidateBatch &) { return true; };
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByExactProperties(
			{"Person"}, {{"code", graph::PropertyValue(largeValue)}}, 1U, 8U, 1U, acceptAll)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByPropertyRange(
			{"Person"}, {"code", graph::PropertyValue(std::string("a")), graph::PropertyValue(), true, true}, 1U, 8U,
			1U, acceptAll)), FormatException);
	const LogicalGraphCompositePropertyRange composite{
			{{"city", graph::PropertyValue(largeValue)}},
			{"age", graph::PropertyValue(int64_t{0}), graph::PropertyValue(int64_t{10}), true, true}};
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByCompositePropertyRange(
			{"Person"}, composite, 1U, 8U, 1U, acceptAll)), FormatException);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RangeIndexCandidatesRetainOnlyVerifiedIndexedProperties) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
								{"unused", graph::PropertyValue("large payload")}}},
			 { {"Person"}, {{"score", graph::PropertyValue(int64_t{42})},
								{"unused", graph::PropertyValue("another payload")}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	ASSERT_TRUE(snapshot->supportsNativeRangeNodeCandidates());
	const auto candidates = snapshot->findNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}, 2U, 2U);
	ASSERT_TRUE(candidates.has_value());
	EXPECT_TRUE(candidates->predicates_verified);
	EXPECT_FALSE(candidates->exact_match_verified);
	ASSERT_EQ(candidates->candidates.size(), nodes.size());
	ASSERT_EQ(candidates->verified_property_columns.size(), 1U);
	EXPECT_EQ(candidates->verified_property_columns.front().key, "score");
	ASSERT_EQ(candidates->verified_property_columns.front().values.size(), nodes.size());
	for (size_t index = 0U; index < nodes.size(); ++index) {
		EXPECT_EQ(candidates->verified_property_columns.front().values[index],
				  graph::PropertyValue(int64_t{index == 0U ? 7 : 42}));
	}
	EXPECT_EQ(candidates->candidates.front().entity_id, nodes.front());
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ScalarRangeCandidateStreamConsumesMultipleBatchesInLogicalOrder) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	requests.reserve(513U);
	for (int64_t score = 1; score <= 513; ++score) {
		requests.push_back({{"Person"}, {{"score", graph::PropertyValue(score)}}});
	}
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), requests.size());
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	size_t callbackCount = 0U;
	size_t callbackEntries = 0U;
	uint64_t previousInBatch = 0U;
	const auto completed = snapshot->visitNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{1}), graph::PropertyValue(int64_t{513}), true, true},
			requests.size(), 1U, [&](const graph::storage::IndexCandidateBatch &batch) {
				++callbackCount;
				callbackEntries += batch.candidates.size();
				uint64_t previous = 0U;
				for (size_t index = 0U; index < batch.candidates.size(); ++index) {
					EXPECT_GT(batch.candidates[index].entity_id, previous);
					previous = batch.candidates[index].entity_id;
					EXPECT_EQ(batch.verified_property_columns.front().values[index],
							graph::PropertyValue(static_cast<int64_t>(batch.candidates[index].entity_id)));
				}
				EXPECT_GT(previous, previousInBatch);
				previousInBatch = previous;
				return true;
			});
	ASSERT_TRUE(completed.has_value());
	EXPECT_TRUE(*completed);
	EXPECT_EQ(callbackCount, 3U);
	EXPECT_EQ(callbackEntries, requests.size());
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ExactCandidateStreamDescendsIndexRootOnceAcrossBatches) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	requests.reserve(600U);
	for (size_t index = 0U; index < 600U; ++index) {
		requests.push_back({{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}});
	}
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), requests.size());
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	size_t callbackCount = 0U;
	size_t candidateCount = 0U;
	const auto completed = snapshot->visitNodeCandidatesByExactProperties(
			{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}, requests.size(), 1U,
			[&](const graph::storage::IndexCandidateBatch &batch) {
				++callbackCount;
				candidateCount += batch.candidates.size();
				EXPECT_TRUE(batch.exact_match_verified);
				EXPECT_TRUE(batch.predicates_verified);
				EXPECT_EQ(batch.verified_property_columns.size(), 1U);
				if (batch.verified_property_columns.size() != 1U) {
					return false;
				}
				EXPECT_EQ(batch.candidates.size(), batch.verified_property_columns.front().values.size());
				EXPECT_TRUE(std::ranges::is_sorted(batch.candidates, {}, &graph::storage::EntityRef::entity_id));
				return true;
			});
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_TRUE(completed.has_value());
	EXPECT_TRUE(*completed);
	EXPECT_GT(callbackCount, 1U);
	EXPECT_EQ(candidateCount, nodes.size());
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_root_descent"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_root_descent").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_batches"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_batches").totalValue,
			static_cast<int64_t>(callbackCount));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_emitted_rows"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_emitted_rows").totalValue,
			static_cast<int64_t>(nodes.size()));
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ScalarRangeCandidateStreamPreservesOrderWithinValueBatches) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	requests.reserve(600U);
	for (int64_t score = 600; score >= 1; --score) {
		requests.push_back({{"Person"}, {{"score", graph::PropertyValue(score)}}});
	}
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), requests.size());
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	std::vector<uint64_t> observed;
	const auto completed = snapshot->visitNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{1}), graph::PropertyValue(int64_t{600}), true, true},
			requests.size(), 1U, 1U << 20U, [&](const graph::storage::IndexCandidateBatch &batch) {
				EXPECT_TRUE(std::ranges::is_sorted(batch.candidates, {}, &EntityRef::entity_id));
				for (const EntityRef &candidate: batch.candidates) {
					observed.push_back(candidate.entity_id);
				}
				return true;
			});
	ASSERT_TRUE(completed.has_value());
	EXPECT_TRUE(*completed);
	std::vector<uint64_t> expected = nodes;
	std::ranges::sort(observed);
	std::ranges::sort(expected);
	EXPECT_EQ(observed, expected);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
		CandidateStreamsRemainBoundToTheirSnapshotAcrossIndexUpdates) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> initialNodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
									 {"city", graph::PropertyValue("Shanghai")},
									 {"age", graph::PropertyValue(int64_t{7})}}}});
	ASSERT_EQ(initialNodes.size(), 1U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));

	std::unique_ptr<LogicalGraphReadSnapshot> oldSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(oldSnapshot, nullptr);
	ASSERT_TRUE(oldSnapshot->nativeSnapshotGeneration().has_value());
	const uint64_t oldGeneration = *oldSnapshot->nativeSnapshotGeneration();

	const std::vector<uint64_t> laterNodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
									  {"city", graph::PropertyValue("Shanghai")},
									  {"age", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(laterNodes.size(), 1U);

	const LogicalGraphPropertyRange scalarRange{
			"score", graph::PropertyValue(int64_t{0}), graph::PropertyValue(int64_t{100}), true, true};
	const LogicalGraphCompositePropertyRange compositeRange{
			{{"city", graph::PropertyValue("Shanghai")} },
			{"age", graph::PropertyValue(int64_t{0}), graph::PropertyValue(int64_t{100}), true, true}};
	const auto collectExact = [](const LogicalGraphReadSnapshot &snapshot) -> std::vector<uint64_t> {
		std::vector<uint64_t> ids;
		const auto completed = snapshot.visitNodeCandidatesByExactProperties(
				{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}, 16U, 16U,
				[&](const graph::storage::IndexCandidateBatch &batch) -> bool {
					if (!batch.exact_match_verified || !batch.predicates_verified ||
							batch.candidates.size() != batch.verified_property_columns.front().values.size()) {
						return false;
					}
					EXPECT_EQ(batch.snapshot_generation, snapshot.nativeSnapshotGeneration().value());
					for (size_t index = 0U; index < batch.candidates.size(); ++index) {
						if (!ids.empty()) {
							EXPECT_LT(ids.back(), batch.candidates[index].entity_id);
						}
						ids.push_back(batch.candidates[index].entity_id);
					}
					return true;
				});
		if (!completed.has_value()) {
			ADD_FAILURE() << "exact candidate stream unexpectedly declined";
			return {};
		}
		EXPECT_TRUE(*completed);
		return ids;
	};

	const auto collectScalar = [](const LogicalGraphReadSnapshot &snapshot,
									 const LogicalGraphPropertyRange &range) -> std::vector<uint64_t> {
			std::vector<uint64_t> ids;
			const auto completed = snapshot.visitNodeCandidatesByPropertyRange(
					{"Person"}, range, 16U, 16U, [&](const graph::storage::IndexCandidateBatch &batch) -> bool {
						EXPECT_TRUE(batch.predicates_verified);
						EXPECT_TRUE(batch.label_scope_verified);
						EXPECT_EQ(batch.snapshot_generation, snapshot.nativeSnapshotGeneration().value());
						if (batch.verified_property_columns.size() != 1U ||
								batch.candidates.size() != batch.verified_property_columns.front().values.size()) {
							return false;
						}
						for (size_t index = 0U; index < batch.candidates.size(); ++index) {
							if (!ids.empty()) {
								EXPECT_LT(ids.back(), batch.candidates[index].entity_id);
							}
							ids.push_back(batch.candidates[index].entity_id);
							EXPECT_EQ(batch.verified_property_columns.front().values[index].getType(),
															  graph::PropertyType::INTEGER);
						}
						return true;
					});
		if (!completed.has_value()) {
			ADD_FAILURE() << "scalar candidate stream unexpectedly declined";
			return {};
		}
		EXPECT_TRUE(*completed);
		return ids;
	};
	const auto collectComposite = [](const LogicalGraphReadSnapshot &snapshot,
									 const LogicalGraphCompositePropertyRange &range) -> std::vector<uint64_t> {
			std::vector<uint64_t> ids;
			const auto completed = snapshot.visitNodeCandidatesByCompositePropertyRange(
					{"Person"}, range, 16U, 16U, [&](const graph::storage::IndexCandidateBatch &batch) -> bool {
						EXPECT_TRUE(batch.predicates_verified);
						EXPECT_EQ(batch.snapshot_generation, snapshot.nativeSnapshotGeneration().value());
						if (batch.verified_property_columns.size() != 2U ||
								batch.candidates.size() != batch.verified_property_columns[0].values.size() ||
								batch.candidates.size() != batch.verified_property_columns[1].values.size()) {
							return false;
						}
						for (size_t index = 0U; index < batch.candidates.size(); ++index) {
							if (!ids.empty()) {
								EXPECT_LT(ids.back(), batch.candidates[index].entity_id);
							}
							ids.push_back(batch.candidates[index].entity_id);
							EXPECT_EQ(batch.verified_property_columns[0].values[index], graph::PropertyValue("Shanghai"));
						}
						return true;
					});
		if (!completed.has_value()) {
			ADD_FAILURE() << "composite candidate stream unexpectedly declined";
			return {};
		}
		EXPECT_TRUE(*completed);
		return ids;
	};

	const std::vector<uint64_t> oldExact = collectExact(*oldSnapshot);
	const std::vector<uint64_t> oldScalar = collectScalar(*oldSnapshot, scalarRange);
	const std::vector<uint64_t> oldComposite = collectComposite(*oldSnapshot, compositeRange);
	EXPECT_EQ(oldExact, initialNodes);
	EXPECT_EQ(oldScalar, initialNodes);
	EXPECT_EQ(oldComposite, initialNodes);

	std::unique_ptr<LogicalGraphReadSnapshot> currentSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(currentSnapshot, nullptr);
	ASSERT_TRUE(currentSnapshot->nativeSnapshotGeneration().has_value());
	EXPECT_GT(*currentSnapshot->nativeSnapshotGeneration(), oldGeneration);
	const std::vector<uint64_t> currentExact = collectExact(*currentSnapshot);
	const std::vector<uint64_t> currentScalar = collectScalar(*currentSnapshot, scalarRange);
	const std::vector<uint64_t> currentComposite = collectComposite(*currentSnapshot, compositeRange);
	EXPECT_EQ(currentExact, (std::vector<uint64_t>{initialNodes.front(), laterNodes.front()}));
	EXPECT_EQ(currentScalar, (std::vector<uint64_t>{initialNodes.front(), laterNodes.front()}));
	EXPECT_EQ(currentComposite, (std::vector<uint64_t>{initialNodes.front(), laterNodes.front()}));

	currentSnapshot.reset();
	oldSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
		CandidateStreamKeepsConcurrentWriterInvisibleToPinnedSnapshot) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> initialRequests;
	initialRequests.reserve(128U);
	for (int64_t score = 1; score <= 128; ++score) {
		initialRequests.push_back({{"Person"}, {{"score", graph::PropertyValue(score)}}});
	}
	const std::vector<uint64_t> initialNodes = backend.createLogicalNodes(initialRequests);
	ASSERT_EQ(initialNodes.size(), initialRequests.size());
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> oldSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(oldSnapshot, nullptr);

	std::vector<uint64_t> observedByOldSnapshot;
	std::exception_ptr readerFailure;
	std::atomic<bool> readerStarted{false};
	std::thread reader([&] {
		try {
			readerStarted.store(true, std::memory_order_release);
			const auto result = oldSnapshot->findNodeCandidatesByPropertyRange(
					{"Person"}, {"score", graph::PropertyValue(int64_t{1}), graph::PropertyValue(int64_t{256}), true, true},
					256U, 1U);
			if (!result.has_value()) {
				throw std::runtime_error("concurrent candidate lookup unexpectedly declined");
			}
			for (const EntityRef &candidate: result->candidates) {
				observedByOldSnapshot.push_back(candidate.entity_id);
			}
		} catch (...) {
			readerFailure = std::current_exception();
		}
	});
	while (!readerStarted.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}

	const std::vector<uint64_t> laterNodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{200})}}},
			 { {"Person"}, {{"score", graph::PropertyValue(int64_t{201})}}}});
	ASSERT_EQ(laterNodes.size(), 2U);
	reader.join();
	if (readerFailure != nullptr) {
		std::rethrow_exception(readerFailure);
	}

	EXPECT_EQ(observedByOldSnapshot, initialNodes);
	std::unique_ptr<LogicalGraphReadSnapshot> currentSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(currentSnapshot, nullptr);
	const auto current = currentSnapshot->findNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{1}), graph::PropertyValue(int64_t{256}), true, true},
			256U, 1U);
	ASSERT_TRUE(current.has_value());
	EXPECT_EQ(current->candidates.size(), initialNodes.size() + laterNodes.size());
	currentSnapshot.reset();
	oldSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
		CandidateStreamsApplyLivenessWithoutBreakingAnOlderSnapshot) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({
			{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
										{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{7})}}},
			{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
										{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{7})}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> oldSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(oldSnapshot, nullptr);
	backend.deleteLogicalNodes({{nodes.back(), false, 0U}});

	const LogicalGraphPropertyRange range{
			"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{7}), true, true};
	const LogicalGraphCompositePropertyRange compositeRange{
			{{"city", graph::PropertyValue("Shanghai")} },
			{"age", graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{7}), true, true}};
	const auto collectExact = [&](const LogicalGraphReadSnapshot &snapshot) {
		std::vector<uint64_t> ids;
		const auto completed = snapshot.visitNodeCandidatesByExactProperties(
				{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}, 8U, 8U,
				[&](const graph::storage::IndexCandidateBatch &batch) -> bool {
					if (!batch.exact_match_verified || !batch.predicates_verified ||
							batch.verified_property_columns.size() != 1U ||
							batch.candidates.size() != batch.verified_property_columns.front().values.size()) {
						return false;
					}
					for (size_t index = 0U; index < batch.candidates.size(); ++index) {
						ids.push_back(batch.candidates[index].entity_id);
						EXPECT_EQ(batch.verified_property_columns.front().values[index], graph::PropertyValue(int64_t{7}));
					}
					return true;
				});
		if (!completed.has_value()) {
			ADD_FAILURE() << "exact liveness candidate stream unexpectedly declined";
			return std::vector<uint64_t>{};
		}
		EXPECT_TRUE(*completed);
		return ids;
	};
	const auto collect = [&](const LogicalGraphReadSnapshot &snapshot) {
		std::vector<uint64_t> ids;
		const auto completed = snapshot.visitNodeCandidatesByPropertyRange(
				{"Person"}, range, 8U, 8U, [&](const graph::storage::IndexCandidateBatch &batch) -> bool {
					if (!batch.predicates_verified || batch.snapshot_generation == 0U ||
							batch.verified_property_columns.size() != 1U ||
							batch.candidates.size() != batch.verified_property_columns.front().values.size()) {
						return false;
					}
					for (size_t index = 0U; index < batch.candidates.size(); ++index) {
						ids.push_back(batch.candidates[index].entity_id);
						EXPECT_EQ(batch.verified_property_columns.front().values[index], graph::PropertyValue(int64_t{7}));
					}
					return true;
				});
		if (!completed.has_value()) {
			ADD_FAILURE() << "liveness candidate stream unexpectedly declined";
			return std::vector<uint64_t>{};
		}
		EXPECT_TRUE(*completed);
		return ids;
	};
	const auto collectComposite = [&](const LogicalGraphReadSnapshot &snapshot) {
		std::vector<uint64_t> ids;
		const auto completed = snapshot.visitNodeCandidatesByCompositePropertyRange(
				{"Person"}, compositeRange, 8U, 8U, [&](const graph::storage::IndexCandidateBatch &batch) {
					if (batch.verified_property_columns.size() != 2U ||
							batch.candidates.size() != batch.verified_property_columns[0].values.size() ||
							batch.candidates.size() != batch.verified_property_columns[1].values.size()) {
						return false;
					}
					for (size_t index = 0U; index < batch.candidates.size(); ++index) {
						ids.push_back(batch.candidates[index].entity_id);
						EXPECT_EQ(batch.verified_property_columns[0].values[index], graph::PropertyValue("Shanghai"));
					}
					return true;
				});
		if (!completed.has_value()) {
			ADD_FAILURE() << "composite liveness candidate stream unexpectedly declined";
			return ids;
		}
		EXPECT_TRUE(*completed);
		return ids;
	};

	EXPECT_EQ(collectExact(*oldSnapshot), nodes);
	EXPECT_EQ(collect(*oldSnapshot), nodes);
	EXPECT_EQ(collectComposite(*oldSnapshot), nodes);
	std::unique_ptr<LogicalGraphReadSnapshot> currentSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(currentSnapshot, nullptr);
	EXPECT_EQ(collectExact(*currentSnapshot), std::vector<uint64_t>{nodes.front()});
	EXPECT_EQ(collect(*currentSnapshot), std::vector<uint64_t>{nodes.front()});
	EXPECT_EQ(collectComposite(*currentSnapshot), std::vector<uint64_t>{nodes.front()});
	currentSnapshot.reset();
	oldSnapshot.reset();
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> reopenedSnapshot = reopened.openLogicalGraphReadSnapshot();
	ASSERT_NE(reopenedSnapshot, nullptr);
	EXPECT_EQ(collectExact(*reopenedSnapshot), std::vector<uint64_t>{nodes.front()});
	EXPECT_EQ(collect(*reopenedSnapshot), std::vector<uint64_t>{nodes.front()});
	EXPECT_EQ(collectComposite(*reopenedSnapshot), std::vector<uint64_t>{nodes.front()});
	reopenedSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
		CandidateStreamsRejectAnIndexedPropertyAfterReplacementAndPreserveOldSnapshot) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({
			{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> oldSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(oldSnapshot, nullptr);

	// Replacing the complete Property collection removes the indexed field. The
	// old snapshot must retain the prior generation, while the current generation
	// must not return a stale index candidate or silently treat the field as live.
	backend.replaceLogicalNodeProperties({{nodes.front(), {}}});

	const auto oldExact = oldSnapshot->findNodeCandidatesByExactProperties(
			{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 2U, 2U);
	ASSERT_TRUE(oldExact.has_value());
	ASSERT_EQ(oldExact->candidates.size(), 1U);
	EXPECT_EQ(oldExact->candidates.front().entity_id, nodes.front());

	std::unique_ptr<LogicalGraphReadSnapshot> currentSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(currentSnapshot, nullptr);
	const auto currentExact = currentSnapshot->findNodeCandidatesByExactProperties(
			{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 2U, 2U);
	ASSERT_TRUE(currentExact.has_value());
	EXPECT_TRUE(currentExact->candidates.empty());
	const auto currentRange = currentSnapshot->findNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{0}), graph::PropertyValue(int64_t{100}), true, true}, 2U,
			2U);
	ASSERT_TRUE(currentRange.has_value());
	EXPECT_TRUE(currentRange->candidates.empty());

	currentSnapshot.reset();
	oldSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> reopenedSnapshot = reopened.openLogicalGraphReadSnapshot();
	ASSERT_NE(reopenedSnapshot, nullptr);
	const auto reopenedExact = reopenedSnapshot->findNodeCandidatesByExactProperties(
			{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 2U, 2U);
	ASSERT_TRUE(reopenedExact.has_value());
	EXPECT_TRUE(reopenedExact->candidates.empty());
	reopenedSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, UniqueCandidateStreamEnforcesProducerByteBound) {
	RelocatableStorageBackend backend(options());
	backend.open();
	static_cast<void>(backend.createLogicalConstraint({"person_code_unique",
			LogicalGraphConstraintEntityKind::LGCEK_NODE, LogicalGraphConstraintKind::LGCK_UNIQUE,
			"Person", {"code"}, LogicalGraphConstraintPropertyType::LGCPT_NONE}));
	const std::string largeValue(256U, 'x');
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({
			{{"Person"}, {{"code", graph::PropertyValue(largeValue)}}}});
	ASSERT_EQ(nodes.size(), 1U);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	ASSERT_TRUE(snapshot->supportsNativeExactNodeCandidates());
	const auto acceptAll = [](const graph::storage::IndexCandidateBatch &) { return true; };
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByExactProperties(
			{"Person"}, {{"code", graph::PropertyValue(largeValue)}}, 1U, 4U, 1U, acceptAll)), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ReusesCatalogDescriptorAcrossEntityPagesDuringInsertionPlanning) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> initial;
	initial.reserve(600U);
	for (size_t index = 0U; index < 600U; ++index) {
		initial.push_back({{"CatalogReuse"}, {}});
	}
	const std::vector<uint64_t> initialNodes = backend.createLogicalNodes(initial);
	ASSERT_EQ(initialNodes.size(), initial.size());

	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	ASSERT_EQ(backend.createLogicalNodes({{{"CatalogReuse"}, {}}}).size(), 1U);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	// The planner keeps a snapshot-pinned insertion target cache after the
	// initial batch. A subsequent insert should reuse it without rescanning the
	// complete Entity Directory or reloading the pool descriptor.
	EXPECT_TRUE(trace.contains("native_graph.commit.entity_insert_target_cache_hits"));
	EXPECT_EQ(trace.at("native_graph.commit.entity_insert_target_cache_hits").totalValue, 1);
	EXPECT_FALSE(trace.contains("native_graph.commit.entity_insert_node_directory_pages"));
	EXPECT_FALSE(trace.contains("native_graph.commit.entity_insert_catalog_lookups"));

	// A delete-only publication must preserve the untouched insertion targets so
	// the following insert still avoids a full Entity Directory scan.
	backend.deleteLogicalNodes({{initialNodes.front()}});
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	ASSERT_EQ(backend.createLogicalNodes({{{"CatalogReuse"}, {}}}).size(), 1U);
	const graph::debug::PerfTrace::Snapshot afterDelete = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_TRUE(afterDelete.contains("native_graph.commit.entity_insert_target_cache_hits"));
	EXPECT_EQ(afterDelete.at("native_graph.commit.entity_insert_target_cache_hits").totalValue, 1);
	EXPECT_FALSE(afterDelete.contains("native_graph.commit.entity_insert_node_directory_pages"));
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, GroupsBulkNodeInsertionsByBoundedEntityPageTargets) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	requests.reserve(600U);
	for (size_t index = 0U; index < 600U; ++index) {
		requests.push_back({{"GroupedInsertions"}, {}});
	}
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	ASSERT_EQ(backend.createLogicalNodes(requests).size(), requests.size());
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(trace.contains("native_graph.commit.entity_insert_target_pages"));
	ASSERT_TRUE(trace.contains("native_graph.commit.entity_insert_target_records"));
	EXPECT_EQ(trace.at("native_graph.commit.entity_insert_target_records").totalValue,
			static_cast<int64_t>(requests.size()));
	EXPECT_GT(trace.at("native_graph.commit.entity_insert_target_pages").totalValue, 0);
	EXPECT_LT(trace.at("native_graph.commit.entity_insert_target_pages").totalValue,
			trace.at("native_graph.commit.entity_insert_target_records").totalValue);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ResolvesBulkNodeMutationsThroughOneSortedDirectoryBatch) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> creates;
	creates.reserve(600U);
	for (size_t index = 0U; index < 600U; ++index) {
		creates.push_back({{"BulkMutation"}, {{"value", graph::PropertyValue(static_cast<int64_t>(index))}}});
	}
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(creates);
	ASSERT_EQ(nodes.size(), creates.size());

	std::vector<LogicalGraphPropertyReplaceRequest> replacements;
	replacements.reserve(nodes.size());
	for (const uint64_t nodeId: nodes) {
		replacements.push_back({nodeId, {{"value", graph::PropertyValue(int64_t{-1})}}});
	}
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	backend.replaceLogicalNodeProperties(replacements);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(trace.contains("native_graph.read.entity_directory_batch_requests"));
	EXPECT_EQ(trace.at("native_graph.read.entity_directory_batch_requests").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.read.entity_directory_batch_ids"));
	EXPECT_EQ(trace.at("native_graph.read.entity_directory_batch_ids").totalValue,
			static_cast<int64_t>(nodes.size()));
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ResolvesBulkEdgeMutationsThroughOneSortedDirectoryBatch) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkMutation"}, {}}, {{"BulkMutation"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	std::vector<LogicalGraphEdgeCreateRequest> creates;
	creates.reserve(600U);
	for (size_t index = 0U; index < 600U; ++index) {
		creates.push_back({nodes.front(), nodes.back(), "LINK", {{"value", graph::PropertyValue(static_cast<int64_t>(index))}}});
	}
	const std::vector<uint64_t> edges = backend.createLogicalEdges(creates);
	ASSERT_EQ(edges.size(), creates.size());

	std::vector<LogicalGraphPropertyReplaceRequest> replacements;
	replacements.reserve(edges.size());
	for (const uint64_t edgeId: edges) {
		replacements.push_back({edgeId, {{"value", graph::PropertyValue(int64_t{-1})}}});
	}
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	backend.replaceLogicalEdgeProperties(replacements);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(trace.contains("native_graph.read.entity_directory_batch_requests"));
	EXPECT_EQ(trace.at("native_graph.read.entity_directory_batch_requests").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.read.entity_directory_batch_ids"));
	EXPECT_EQ(trace.at("native_graph.read.entity_directory_batch_ids").totalValue,
			static_cast<int64_t>(edges.size()));
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, CompositeRangeCandidatesRetainLogicalIndexFieldOrder) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{7})}}},
			 { {"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const LogicalGraphCompositePropertyRange request{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}};
	const auto candidates = snapshot->findNodeCandidatesByCompositePropertyRange({"Person"}, request, 2U, 2U);
	ASSERT_TRUE(candidates.has_value());
	EXPECT_TRUE(candidates->predicates_verified);
	ASSERT_EQ(candidates->candidates.size(), nodes.size());
	ASSERT_EQ(candidates->verified_property_columns.size(), 2U);
	EXPECT_EQ(candidates->verified_property_columns[0].key, "city");
	EXPECT_EQ(candidates->verified_property_columns[1].key, "age");
	ASSERT_EQ(candidates->verified_property_columns[0].values.size(), nodes.size());
	ASSERT_EQ(candidates->verified_property_columns[1].values.size(), nodes.size());
	for (size_t index = 0U; index < nodes.size(); ++index) {
		EXPECT_EQ(candidates->verified_property_columns[0].values[index], graph::PropertyValue("Shanghai"));
		EXPECT_EQ(candidates->verified_property_columns[1].values[index],
				  graph::PropertyValue(int64_t{index == 0U ? 7 : 42}));
	}
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, CompositeRangeCandidateStreamStopsAtFirstBatch) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	requests.reserve(600U);
	for (int64_t age = 1; age <= 600; ++age) {
		requests.push_back({{"Person"}, {{"city", graph::PropertyValue("Shanghai")},
				{"age", graph::PropertyValue(age)}}});
	}
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), requests.size());
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const LogicalGraphCompositePropertyRange request{
			{{"city", graph::PropertyValue("Shanghai")} },
			{"age", graph::PropertyValue(int64_t{1}), graph::PropertyValue(int64_t{600}), true, true}};
	size_t callbackCount = 0U;
	size_t callbackEntries = 0U;
	const auto completed = snapshot->visitNodeCandidatesByCompositePropertyRange(
			{"Person"}, request, requests.size(), 2U,
			[&](const graph::storage::IndexCandidateBatch &batch) -> bool {
				++callbackCount;
				callbackEntries += batch.candidates.size();
				EXPECT_TRUE(batch.predicates_verified);
				EXPECT_EQ(batch.verified_property_columns.size(), 2U);
				if (batch.verified_property_columns.size() != 2U) return false;
				EXPECT_EQ(batch.candidates.size(), batch.verified_property_columns.front().values.size());
				EXPECT_EQ(batch.candidates.size(), batch.verified_property_columns.back().values.size());
				EXPECT_TRUE(std::ranges::is_sorted(batch.candidates, {}, &graph::storage::EntityRef::entity_id));
				return false;
			});
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_TRUE(completed.has_value());
	EXPECT_FALSE(*completed);
	EXPECT_EQ(callbackCount, 1U);
	EXPECT_EQ(callbackEntries, graph::storage::BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES);
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_root_descent"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_root_descent").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.read.property_selected_column_batch_owners"));
	EXPECT_EQ(trace.at("native_graph.read.property_selected_column_batch_owners").totalValue,
			static_cast<int64_t>(graph::storage::BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_liveness_probes"));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_batches"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_liveness_probes").totalValue,
			  static_cast<int64_t>(graph::storage::BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_batches").totalValue, 1);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, CompositeRangeAppliesUpperBoundBeforeCandidateBudget) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{7})}}},
			 { {"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{42})}}},
			 { {"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{99})}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);

	// Only the first indexed value belongs to this range.  The cursor must stop
	// at the encoded upper key before counting later leaf entries against the
	// caller's result bound.
	const LogicalGraphCompositePropertyRange request{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{7}), true, true}};
	const auto result = snapshot->findNodesByCompositePropertyRange({"Person"}, request, 1U, 4U);
	ASSERT_TRUE(result.has_value());
	ASSERT_EQ(result->size(), 1U);
	EXPECT_EQ(result->front().node_id, nodes.front());

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ReusesTheSnapshotReadSessionForCompositeRangeIndexes) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{7})}}},
			 {{"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{42})}}},
			 {{"Person"}, {{"city", graph::PropertyValue("Beijing")}, {"age", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const LogicalGraphCompositePropertyRange request{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}};
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const auto first = snapshot->findNodesByCompositePropertyRange({"Person"}, request, 2U, 2U);
	const auto second = snapshot->findNodesByCompositePropertyRange({"Person"}, request, 2U, 2U);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(second.has_value());
	ASSERT_EQ(first->size(), 2U);
	EXPECT_EQ(*first, *second);
	EXPECT_EQ(first->at(0).node_id, nodes.at(0));
	EXPECT_EQ(first->at(1).node_id, nodes.at(1));
	ASSERT_TRUE(trace.contains("native_graph.read.composite_range_index_label_token_resolution"));
	ASSERT_TRUE(trace.contains("native_graph.read.composite_range_index_property_token_resolution"));
	ASSERT_TRUE(trace.contains("native_graph.read.composite_range_index_definition_read"));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_root_descent"));
	ASSERT_TRUE(trace.contains("native_graph.read.property_selected_column_batch_owners"));
	EXPECT_EQ(trace.at("native_graph.read.composite_range_index_label_token_resolution").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.composite_range_index_property_token_resolution").totalValue, 2);
	EXPECT_EQ(trace.at("native_graph.read.composite_range_index_definition_read").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_root_descent").totalValue, 2);
	// Both composite indexed fields are read once for the first lookup and then
	// served from the same snapshot-owned cache on the repeated lookup.
	EXPECT_EQ(trace.at("native_graph.read.property_selected_column_batch_owners").totalValue,
			  static_cast<int64_t>(first->size()));

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ResolvesScalarNodeRangesThroughPinnedIndexes) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}},
			 {{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}},
			 {{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}},
			 {{"Person"}, {{"score", graph::PropertyValue(int64_t{50})}}}});
	ASSERT_EQ(nodes.size(), 4U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));

	{
	const std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const auto lookup = [&snapshot](graph::PropertyValue minValue, graph::PropertyValue maxValue, bool minInclusive = true,
										 bool maxInclusive = true, size_t maximumNodes = 4U) {
		return snapshot->findNodesByPropertyRange(
				{"Person"}, {"score", std::move(minValue), std::move(maxValue), minInclusive, maxInclusive}, maximumNodes,
				1U);
	};

	const auto lower = lookup(graph::PropertyValue(int64_t{42}), graph::PropertyValue());
	ASSERT_TRUE(lower.has_value());
	ASSERT_EQ(lower->size(), 3U);
	EXPECT_EQ(lower->at(0).node_id, nodes.at(1));
	EXPECT_EQ(lower->at(1).node_id, nodes.at(2));
	EXPECT_EQ(lower->at(2).node_id, nodes.at(3));

	const auto upper = lookup(graph::PropertyValue(), graph::PropertyValue(int64_t{42}));
	ASSERT_TRUE(upper.has_value());
	ASSERT_EQ(upper->size(), 3U);
	EXPECT_EQ(upper->at(0).node_id, nodes.at(0));
	EXPECT_EQ(upper->at(1).node_id, nodes.at(1));
	EXPECT_EQ(upper->at(2).node_id, nodes.at(2));

	const auto open = lookup(graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{50}), false, false);
	ASSERT_TRUE(open.has_value());
	ASSERT_EQ(open->size(), 2U);
	EXPECT_EQ(open->at(0).node_id, nodes.at(1));
	EXPECT_EQ(open->at(1).node_id, nodes.at(2));
	const auto reversed = lookup(graph::PropertyValue(int64_t{50}), graph::PropertyValue(int64_t{7}));
	ASSERT_TRUE(reversed.has_value());
	EXPECT_TRUE(reversed->empty());
	const auto emptyExclusive =
			lookup(graph::PropertyValue(int64_t{42}), graph::PropertyValue(int64_t{42}), false, true);
	ASSERT_TRUE(emptyExclusive.has_value());
	EXPECT_TRUE(emptyExclusive->empty());
	const auto unknownLabel = snapshot->findNodesByPropertyRange(
			{"MissingLabel"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}, 4U,
			1U);
	ASSERT_TRUE(unknownLabel.has_value());
	EXPECT_TRUE(unknownLabel->empty());
	const auto unknownProperty = snapshot->findNodesByPropertyRange(
			{"Person"}, {"missing", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}, 4U, 1U);
	ASSERT_TRUE(unknownProperty.has_value());
	EXPECT_TRUE(unknownProperty->empty());
	EXPECT_EQ(snapshot->findNodesByPropertyRange(
				  {"Person", "Other"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true},
				  4U, 1U),
			  std::nullopt);
	EXPECT_EQ(snapshot->findNodesByPropertyRange(
				  {"Person"}, {"", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}, 4U, 1U),
			  std::nullopt);
	EXPECT_THROW(static_cast<void>(lookup(graph::PropertyValue(int64_t{42}), graph::PropertyValue(), true, true, 2U)),
				 FormatException);
	EXPECT_EQ(lookup(graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(int64_t{7})}),
						 graph::PropertyValue()),
				  std::nullopt);
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, PermitsAtomicUniqueKeySwapAndPublishesOnlyTheFinalSupportTreeState) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const LogicalGraphConstraintDefinition constraint =
			backend.createLogicalConstraint({"user_email_unique",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_UNIQUE,
											 "User",
											 {"email"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE});
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"User"}, {{"email", graph::PropertyValue(std::string("a@example.test"))}}},
										{{"User"}, {{"email", graph::PropertyValue(std::string("b@example.test"))}}}});
	ASSERT_EQ(nodes.size(), 2U);

	LogicalGraphMutationBatch swap;
	swap.property_replacements = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodes[0]},
			 {{"email", graph::PropertyValue(std::string("b@example.test"))}}},
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodes[1]},
			 {{"email", graph::PropertyValue(std::string("a@example.test"))}}},
	};
	EXPECT_NO_THROW(static_cast<void>(backend.executeLogicalGraphMutationBatch(swap)));
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	const std::vector<graph::PropertyValue> a = {graph::PropertyValue(std::string("a@example.test"))};
	const std::vector<graph::PropertyValue> b = {graph::PropertyValue(std::string("b@example.test"))};
	std::vector<BTreePageEntry> expected = {
			{ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(a), nodes[1]), nodes[1]},
			{ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(b), nodes[0]), nodes[0]},
	};
	std::ranges::sort(expected, {}, &BTreePageEntry::key);
	EXPECT_EQ(readIndexEntries(constraint.constraint_id), expected);
}

TEST_F(RelocatableStorageBackendSchemaTest, DetachDeleteErasesEdgeUniqueSupportEntryInTheSamePublication) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> firstNodes = backend.createLogicalNodes({{{"User"}, {}}, {{"User"}, {}}});
	ASSERT_EQ(firstNodes.size(), 2U);
	const LogicalGraphConstraintDefinition constraint =
			backend.createLogicalConstraint({"knows_code_unique",
											 LogicalGraphConstraintEntityKind::LGCEK_EDGE,
											 LogicalGraphConstraintKind::LGCK_UNIQUE,
											 "KNOWS",
											 {"code"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE});
	const std::vector<uint64_t> firstEdge = backend.createLogicalEdges(
			{{firstNodes[0], firstNodes[1], "KNOWS", {{"code", graph::PropertyValue(std::string("edge-code"))}}}});
	ASSERT_EQ(firstEdge.size(), 1U);
	backend.deleteLogicalNodes({{firstNodes[0], true, 4U}});

	const std::vector<uint64_t> laterNodes = backend.createLogicalNodes({{{"User"}, {}}, {{"User"}, {}}});
	ASSERT_EQ(laterNodes.size(), 2U);
	const std::vector<uint64_t> replacement = backend.createLogicalEdges(
			{{laterNodes[0], laterNodes[1], "KNOWS", {{"code", graph::PropertyValue(std::string("edge-code"))}}}});
	ASSERT_EQ(replacement.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	const std::vector<graph::PropertyValue> values = {graph::PropertyValue(std::string("edge-code"))};
	const std::vector<std::byte> canonical = ConstraintSupportTree::canonicalKey(values);
	EXPECT_EQ(readIndexEntries(constraint.constraint_id),
			  (std::vector<BTreePageEntry>{
					  {ConstraintSupportTree::entryKey(canonical, replacement.front()), replacement.front()}}));
}

TEST_F(RelocatableStorageBackendSchemaTest, NamedRelationshipConstraintsExcludeUntypedRelationships) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"User"}, {}}, {{"User"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	// Creating the constraint after this edge exercises full-scan validation;
	// subsequent creates exercise the affected-entity validation path.
	const std::vector<uint64_t> firstUntyped = backend.createLogicalEdges(
			{{nodes[0], nodes[1], "", {{"code", graph::PropertyValue(std::string("shared-code"))}}}});
	ASSERT_EQ(firstUntyped.size(), 1U);
	static_cast<void>(backend.createLogicalConstraint({"knows_code_unique",
													   LogicalGraphConstraintEntityKind::LGCEK_EDGE,
													   LogicalGraphConstraintKind::LGCK_UNIQUE,
													   "KNOWS",
													   {"code"},
													   LogicalGraphConstraintPropertyType::LGCPT_NONE}));

	const std::vector<uint64_t> secondUntyped = backend.createLogicalEdges(
			{{nodes[0], nodes[1], "", {{"code", graph::PropertyValue(std::string("shared-code"))}}}});
	ASSERT_EQ(secondUntyped.size(), 1U);
	const std::vector<uint64_t> named = backend.createLogicalEdges(
			{{nodes[0], nodes[1], "KNOWS", {{"code", graph::PropertyValue(std::string("shared-code"))}}}});
	ASSERT_EQ(named.size(), 1U);
	EXPECT_THROW(
			static_cast<void>(backend.createLogicalEdges(
					{{nodes[0], nodes[1], "KNOWS", {{"code", graph::PropertyValue(std::string("shared-code"))}}}})),
			std::runtime_error);

	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	EXPECT_TRUE(backend.readEdge(snapshot, firstUntyped.front()).type.empty());
	EXPECT_TRUE(backend.readEdge(snapshot, secondUntyped.front()).type.empty());
	EXPECT_EQ(backend.readEdge(snapshot, named.front()).type, "KNOWS");
	snapshot.release();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, PublishesScalarIndexBatchesOnlyAfterEveryHistoricalTreeBuilds) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}, {"rank", graph::PropertyValue(int64_t{7})}}},
			 {{"Person"}, {{"rank", graph::PropertyValue(int64_t{9})}}}});
	ASSERT_EQ(nodes.size(), 2U);

	const std::vector<LogicalGraphNodeIndexCreateRequest> requests = {{"person_score", "Person", {"score"}},
																	  {"person_rank", "Person", {"rank"}}};
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const std::vector<LogicalGraphIndexDefinition> created = backend.createLogicalNodeIndexes(requests);
	const graph::debug::PerfTrace::Snapshot indexBuildTrace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_EQ(created.size(), 2U);
	ASSERT_TRUE(indexBuildTrace.contains("native_graph.read.property_selected_column_batch_owners"));
	// Both definitions share one bounded Property-column pass over the matching
	// Node owners; the old per-definition implementation performed two passes.
	EXPECT_EQ(indexBuildTrace.at("native_graph.read.property_selected_column_batch_owners").totalValue, 2);
	EXPECT_FALSE(indexBuildTrace.contains("native_graph.read.property_optional_lookup"));
	EXPECT_LT(created[0].index_id, created[1].index_id);
	EXPECT_EQ(created[0].kind, LogicalGraphIndexKind::LGIK_PROPERTY);
	EXPECT_EQ(created[1].kind, LogicalGraphIndexKind::LGIK_PROPERTY);
	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	EXPECT_THROW(static_cast<void>(backend.readIndexDefinitions(snapshot, 1U)), FormatException);
	EXPECT_EQ(backend.readIndexDefinitions(snapshot, 2U).size(), 2U);
	snapshot.release();
	EXPECT_EQ(backend.listLogicalIndexes(8U), created);
	backend.close();

	const std::vector<BTreePageEntry> scoreEntries = readIndexEntries(created[0].index_id);
	ASSERT_EQ(scoreEntries.size(), 1U);
	EXPECT_EQ(scoreEntries.front(),
			  (BTreePageEntry{IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{42}), nodes.front()),
							  nodes.front()}));
	const std::vector<BTreePageEntry> rankEntries = readIndexEntries(created[1].index_id);
	ASSERT_EQ(rankEntries.size(), 2U);

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_EQ(reopened.listLogicalIndexes(8U), created);
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsFailedScalarIndexBatchWithoutPublishingAPartialDefinitionSet) {
	RelocatableStorageBackend backend(options());
	backend.open();
	static_cast<void>(backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}, {"rank", graph::PropertyValue(int64_t{7})}}},
			 {{"Person"}, {{"rank", graph::PropertyValue(int64_t{9})}}}}));
	backend.close();

	RelocatableStorageBackendOptions constrainedOptions = options();
	constrainedOptions.maximum_online_index_entries = 1U;
	RelocatableStorageBackend constrained(constrainedOptions);
	constrained.open();
	const std::vector<LogicalGraphNodeIndexCreateRequest> requests = {{"person_score", "Person", {"score"}},
																	  {"person_rank", "Person", {"rank"}}};
	EXPECT_THROW(static_cast<void>(constrained.createLogicalNodeIndexes(requests)), FormatException);
	EXPECT_TRUE(constrained.listLogicalIndexes(8U).empty());
	constrained.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsScalarIndexesThatExceedThePublishedDefinitionCapacity) {
	RelocatableStorageBackendOptions constrainedOptions = options();
	constrainedOptions.maximum_index_definitions = 1U;
	RelocatableStorageBackend backend(std::move(constrainedOptions));
	backend.open();

	const LogicalGraphIndexDefinition first = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndex({"person_rank", "Person", {"rank"}})),
			 FormatException);
	EXPECT_EQ(backend.listLogicalIndexes(1U), std::vector<LogicalGraphIndexDefinition>{first});
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsInvalidOrExistingScalarIndexBatchesBeforePublication) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<LogicalGraphNodeIndexCreateRequest> empty;
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndexes(empty)), std::invalid_argument);
	EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());
	const std::vector<LogicalGraphNodeIndexCreateRequest> tooMany(options().maximum_index_definitions + 1U);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndexes(tooMany)), std::invalid_argument);
	EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());

	const std::vector<LogicalGraphNodeIndexCreateRequest> duplicateSchema = {
			{"person_score_first", "Person", {"score"}}, {"person_score_second", "Person", {"score"}}};
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndexes(duplicateSchema)), std::invalid_argument);
	const std::vector<LogicalGraphNodeIndexCreateRequest> duplicateProperty = {
			{"person_score_pair", "Person", {"score", "score"}}};
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndexes(duplicateProperty)), std::invalid_argument);
	EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());

	const LogicalGraphIndexDefinition original = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	const std::vector<LogicalGraphNodeIndexCreateRequest> conflictsWithExisting = {
			{"person_rank", "Person", {"rank"}}, {"person_score_again", "Person", {"score"}}};
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndexes(conflictsWithExisting)), FormatException);
	EXPECT_EQ(backend.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{original});
	backend.close();
}
