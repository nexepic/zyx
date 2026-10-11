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
	const LogicalGraphIndexDefinition vector =
			backend.createLogicalNodeVectorIndex({"", "Person", "embedding", 2U, "INNER_PRODUCT"});
	EXPECT_EQ(vector.name, "index_vector_node_6_Person_9_embedding_2_ip");
	EXPECT_EQ(vector.vector_metric, "ip");
	EXPECT_EQ(backend.listLogicalIndexes(8U), (std::vector<LogicalGraphIndexDefinition>{scalar, vector}));
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
	EXPECT_EQ(backend.listLogicalConstraints(16U), created);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_EQ(reopened.listLogicalConstraints(16U), created);
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, PublishesAndReopensDurableConfigurationWithoutMixingRuntimeStatistics) {
	RelocatableStorageBackend backend(options());
	backend.open();
	EXPECT_EQ(backend.findConfiguration("query.timeout_ms"), std::nullopt);
	backend.setConfiguration("query.timeout_ms", graph::PropertyValue(int64_t{30}));
	backend.setConfiguration("query.slow_log.enabled", graph::PropertyValue(true));
	backend.setConfiguration("query.timeout_ms", graph::PropertyValue(int64_t{45}));
	EXPECT_EQ(backend.readConfigurations(4U),
			  (std::vector<LogicalGraphConfiguration>{{"query.slow_log.enabled", graph::PropertyValue(true)},
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
	const std::string query = "UNWIND range(0, 262144) AS value RETURN value";
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
	const std::vector<LogicalGraphIndexDefinition> created = backend.createLogicalNodeIndexes(requests);
	ASSERT_EQ(created.size(), 2U);
	EXPECT_LT(created[0].index_id, created[1].index_id);
	EXPECT_EQ(created[0].kind, LogicalGraphIndexKind::LGIK_PROPERTY);
	EXPECT_EQ(created[1].kind, LogicalGraphIndexKind::LGIK_PROPERTY);
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

TEST_F(RelocatableStorageBackendSchemaTest, RejectsInvalidOrExistingScalarIndexBatchesBeforePublication) {
	RelocatableStorageBackend backend(options());
	backend.open();
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

TEST_F(RelocatableStorageBackendSchemaTest, BuildsHistoricalVectorStateAndPublishesItsDefinitionAtomically) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, -2.0)}}},
										{{"Other"}, {{"embedding", vectorValue(5.0, 8.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);

	const LogicalGraphIndexDefinition created =
			backend.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "INNER_PRODUCT"});
	EXPECT_GE(created.index_id, RSS_SYSTEM_CATALOG_INDEX_ID_FIRST);
	EXPECT_EQ(created.kind, LogicalGraphIndexKind::LGIK_VECTOR);
	EXPECT_EQ(created.vector_dimensions, 2U);
	EXPECT_EQ(created.vector_metric, "ip");
	EXPECT_EQ(backend.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{created});

	const std::vector<uint64_t> later =
			backend.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(-3.0, 4.0)}}}});
	ASSERT_EQ(later.size(), 1U);
	backend.close();

	const std::vector<BTreePageEntry> mappings = readIndexEntries(created.index_id);
	ASSERT_EQ(mappings.size(), 2U);
	EXPECT_EQ(mappings[0], (BTreePageEntry{VectorSemanticCodec::nodeMappingKey(nodes.front()), 1U}));
	EXPECT_EQ(mappings[1], (BTreePageEntry{VectorSemanticCodec::nodeMappingKey(later.front()), 2U}));
	const VectorManifest manifest = readVectorManifest(created.index_id);
	EXPECT_EQ(manifest.index_id, created.index_id);
	EXPECT_EQ(manifest.dimensions, 2U);
	EXPECT_EQ(manifest.metric, VectorDistanceMetric::VDM_INNER_PRODUCT);
	EXPECT_TRUE(manifest.node_mapping_root_page_id.isValid());
	EXPECT_TRUE(manifest.raw_write_tail_page_id.isValid());

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_EQ(reopened.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{created});
	EXPECT_TRUE(reopened.dropLogicalIndexByName(created.name));
	EXPECT_FALSE(reopened.dropLogicalIndexByName(created.name));
	EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
	const VectorIndexStateReclaimReport reclamation =
			reopened.reclaimObsoleteVectorIndexState(vectorStateReclaimOptions());
	EXPECT_EQ(reclamation.manifests.dropped_manifests_retired, 1U);
	EXPECT_GT(reclamation.raw.raw_pages_retired, 0U);
	EXPECT_EQ(reclamation.raw.vector_locations_erased, 2U);
	// A Vector index now owns two semantic B+Trees: NodeId-to-VectorId and
	// VectorId-to-NodeId. Dropping it retires both immutable roots together.
	EXPECT_EQ(reclamation.node_mappings.dropped_index_roots_retired, 2U);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, SearchesSnapshotPinnedLogicalVectorStateWithBoundedExactRefinement) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, -2.0)}}},
										{{"Person"}, {{"embedding", vectorValue(-3.0, 4.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(
			backend.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "inner_product"}));

	auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = "person_embedding";
	request.query_vector = {1.0F, -2.0F};
	request.top_k = 2U;
	request.maximum_mapping_entries = 2U;
	request.maximum_raw_vector_reads = 2U;
	const std::vector<LogicalGraphVectorSearchHit> hits = snapshot->searchLogicalVector(request).hits;
	ASSERT_EQ(hits.size(), 2U);
	EXPECT_EQ(hits[0].node_id, nodes[0]);
	EXPECT_EQ(hits[1].node_id, nodes[1]);
	EXPECT_LT(hits[0].score, hits[1].score);
	EXPECT_EQ(snapshot->readNode(hits.front().node_id).node_id, nodes.front());

	request.maximum_mapping_entries = 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::runtime_error);
	request.maximum_mapping_entries = 2U;
	request.maximum_raw_vector_reads = 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::runtime_error);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, UsesPublishedAdjacencyCandidatesWithExactRawRefinement) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto bfloatRecord = [](uint64_t vectorId, float first, float second) {
		CanonicalVectorRecord record;
		record.vector_id = vectorId;
		record.raw_bfloat16.resize(2U * sizeof(uint16_t));
		LittleEndianCodec::writeU16(record.raw_bfloat16, 0U,
									static_cast<uint16_t>(std::bit_cast<uint32_t>(first) >> 16U));
		LittleEndianCodec::writeU16(record.raw_bfloat16, sizeof(uint16_t),
									static_cast<uint16_t>(std::bit_cast<uint32_t>(second) >> 16U));
		return record;
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	const std::vector<CanonicalVectorRecord> records = {bfloatRecord(1U, 0.0F, 0.0F), bfloatRecord(2U, 0.0F, 1.0F)};
	publishVectorAdjacency(index.index_id, records);

	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	graph::query::QueryEngine engine(backend, backend);
	const graph::query::QueryResult result = engine.execute(
			"CALL db.index.vector.queryNodes('person_embedding', 2, [0.0, 1.0]) YIELD node, score RETURN node, score");
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows()[0].at("node").asLogicalNode().node_id, nodes[1]);
	EXPECT_EQ(result.getRows()[1].at("node").asLogicalNode().node_id, nodes[0]);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getRows()[0].at("score").asPrimitive().getVariant()), 0.0);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getRows()[1].at("score").asPrimitive().getVariant()), 1.0);
	auto snapshot = backend->openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest bounded;
	bounded.index_name = index.name;
	bounded.query_vector = {0.0F, 1.0F};
	bounded.top_k = 1U;
	bounded.maximum_mapping_entries = 2U;
	bounded.maximum_raw_vector_reads = 2U;
	bounded.candidate_search_budget = 1U;
	const LogicalGraphVectorSearchResult budgetLimited = snapshot->searchLogicalVector(bounded);
	EXPECT_EQ(budgetLimited.hits.size(), 1U);
	EXPECT_EQ(budgetLimited.stats.candidate_discoveries, 2U);
	EXPECT_EQ(budgetLimited.stats.candidate_expansions, 1U);

	bounded.maximum_mapping_entries = 1U;
	bounded.maximum_raw_vector_reads = 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(bounded)), std::runtime_error);
	// Keep the mapping ceiling above the frontier size so this exercises the
	// independent raw-record safety ceiling while expanding the first neighbor.
	bounded.maximum_mapping_entries = 2U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(bounded)), std::runtime_error);
	snapshot.reset();
	backend->close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RanksPersistentAdjacencyCandidatesWithGenerationCheckedPq) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto bfloatRecord = [](uint64_t vectorId, float first, float second) {
		CanonicalVectorRecord record;
		record.vector_id = vectorId;
		record.raw_bfloat16.resize(2U * sizeof(uint16_t));
		LittleEndianCodec::writeU16(record.raw_bfloat16, 0U,
									static_cast<uint16_t>(std::bit_cast<uint32_t>(first) >> 16U));
		LittleEndianCodec::writeU16(record.raw_bfloat16, sizeof(uint16_t),
									static_cast<uint16_t>(std::bit_cast<uint32_t>(second) >> 16U));
		return record;
	};

	RelocatableStorageBackend writer(options());
	writer.open();
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(10.0, 10.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(9.0, 10.0)}}}});
	ASSERT_EQ(nodes.size(), 4U);
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	const std::vector<CanonicalVectorRecord> records = {bfloatRecord(1U, 0.0F, 0.0F), bfloatRecord(2U, 0.0F, 1.0F),
														bfloatRecord(3U, 10.0F, 10.0F), bfloatRecord(4U, 9.0F, 10.0F)};
	publishVectorPq(index.index_id, records);
	publishVectorAdjacency(index.index_id, records, 3U);

	RelocatableStorageBackend backend(options());
	backend.open();
	auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {9.0F, 10.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 8U;
	request.maximum_raw_vector_reads = 1U;
	request.candidate_search_budget = 1U;
	request.candidate_refinement_budget = 1U;
	const LogicalGraphVectorSearchResult search = snapshot->searchLogicalVector(request);
	const std::vector<LogicalGraphVectorSearchHit> &hits = search.hits;
	ASSERT_EQ(hits.size(), 1U);
	EXPECT_EQ(hits.front().node_id, nodes[3]);
	EXPECT_FLOAT_EQ(hits.front().score, 0.0F);
	EXPECT_TRUE(search.stats.used_adjacency);
	EXPECT_TRUE(search.stats.used_pq);
	EXPECT_EQ(search.stats.candidate_expansions, 1U);
	EXPECT_EQ(search.stats.candidate_discoveries, records.size());
	EXPECT_EQ(search.stats.pq_record_reads, records.size());
	EXPECT_EQ(search.stats.raw_vector_reads, 1U);
	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.maximum_raw_vector_reads = records.size();
	const LogicalGraphVectorSearchResult exact = snapshot->searchLogicalVector(request);
	ASSERT_EQ(exact.hits.size(), 1U);
	EXPECT_EQ(exact.hits.front(), hits.front());
	EXPECT_EQ(exact.stats.scanned_mapping_entries, records.size());
	EXPECT_EQ(exact.stats.raw_vector_reads, records.size());
	EXPECT_FALSE(exact.stats.used_adjacency);
	EXPECT_FALSE(exact.stats.used_pq);

	// Updating an indexed Vector must retain the immutable PQ/adjacency base for
	// pinned readers and add the replacement only through the current delta.
	backend.replaceLogicalNodeProperties({{nodes[3], {{"embedding", vectorValue(9.0, 11.0)}}}});
	auto currentSnapshot = backend.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(currentSnapshot, nullptr);
	request.mode = LogicalGraphVectorSearchMode::LGVSM_AUTO;
	request.query_vector = {9.0F, 11.0F};
	request.maximum_mapping_entries = 8U;
	request.maximum_raw_vector_reads = 2U;
	request.candidate_search_budget = 1U;
	request.candidate_refinement_budget = 2U;
	const LogicalGraphVectorSearchResult current = currentSnapshot->searchLogicalVector(request);
	ASSERT_EQ(current.hits.size(), 1U);
	EXPECT_EQ(current.hits.front().node_id, nodes[3]);
	EXPECT_FLOAT_EQ(current.hits.front().score, 0.0F);
	EXPECT_TRUE(current.stats.used_adjacency);
	EXPECT_TRUE(current.stats.used_pq);
	EXPECT_TRUE(current.stats.used_delta);
	EXPECT_EQ(current.stats.scanned_delta_entries, 2U);

	// The removed base record remains physically present for the pinned reader,
	// but AUTO search must suppress it before looking up its now-erased semantic
	// mapping. Vector 3 and the replacement tie at distance one; stable Node-ID
	// ordering therefore makes the expected current result unambiguous.
	request.query_vector = {9.0F, 10.0F};
	const LogicalGraphVectorSearchResult tombstoneMerged = currentSnapshot->searchLogicalVector(request);
	ASSERT_EQ(tombstoneMerged.hits.size(), 1U);
	EXPECT_EQ(tombstoneMerged.hits.front().node_id, nodes[2]);
	EXPECT_FLOAT_EQ(tombstoneMerged.hits.front().score, 1.0F);
	EXPECT_TRUE(tombstoneMerged.stats.used_adjacency);
	EXPECT_TRUE(tombstoneMerged.stats.used_pq);
	EXPECT_TRUE(tombstoneMerged.stats.used_delta);

	// Replacing the base entry point is legal. The old adjacency generation no
	// longer has a valid traversal root, so AUTO search deliberately falls back
	// to the authoritative current semantic mapping rather than following it.
	backend.replaceLogicalNodeProperties({{nodes[0], {{"embedding", vectorValue(0.0, 0.25)}}}});
	auto entryPointTombstonedSnapshot = backend.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(entryPointTombstonedSnapshot, nullptr);
	request.query_vector = {0.0F, 0.25F};
	request.maximum_raw_vector_reads = records.size();
	request.candidate_refinement_budget = records.size();
	const LogicalGraphVectorSearchResult entryPointTombstoned =
			entryPointTombstonedSnapshot->searchLogicalVector(request);
	ASSERT_EQ(entryPointTombstoned.hits.size(), 1U);
	EXPECT_EQ(entryPointTombstoned.hits.front().node_id, nodes[0]);
	EXPECT_FLOAT_EQ(entryPointTombstoned.hits.front().score, 0.0F);
	EXPECT_TRUE(entryPointTombstoned.stats.used_delta);
	EXPECT_EQ(entryPointTombstoned.stats.scanned_delta_entries, 4U);
	EXPECT_FALSE(entryPointTombstoned.stats.used_adjacency);
	EXPECT_FALSE(entryPointTombstoned.stats.used_pq);
	EXPECT_EQ(entryPointTombstoned.stats.scanned_mapping_entries, records.size());
	EXPECT_EQ(entryPointTombstoned.stats.raw_vector_reads, records.size());
	entryPointTombstonedSnapshot.reset();

	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.query_vector = {9.0F, 11.0F};
	request.maximum_raw_vector_reads = records.size();
	const LogicalGraphVectorSearchResult pinnedExact = snapshot->searchLogicalVector(request);
	ASSERT_EQ(pinnedExact.hits.size(), 1U);
	EXPECT_EQ(pinnedExact.hits.front().node_id, nodes[3]);
	EXPECT_FLOAT_EQ(pinnedExact.hits.front().score, 1.0F);
	currentSnapshot.reset();
	snapshot.reset();
	backend.close();

	advanceManifestCodebookGeneration(index.index_id);
	request.mode = LogicalGraphVectorSearchMode::LGVSM_AUTO;
	request.maximum_raw_vector_reads = 2U;
	request.candidate_refinement_budget = 2U;
	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	auto corruptedSnapshot = corrupted.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(corruptedSnapshot, nullptr);
	EXPECT_THROW(static_cast<void>(corruptedSnapshot->searchLogicalVector(request)), FormatException);
	corruptedSnapshot.reset();
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsDerivedVectorSearchBudgetCombinationsBeforeCandidateTraversal) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto bfloatRecord = [](uint64_t vectorId, float first, float second) {
		CanonicalVectorRecord record;
		record.vector_id = vectorId;
		record.raw_bfloat16.resize(2U * sizeof(uint16_t));
		LittleEndianCodec::writeU16(record.raw_bfloat16, 0U,
									static_cast<uint16_t>(std::bit_cast<uint32_t>(first) >> 16U));
		LittleEndianCodec::writeU16(record.raw_bfloat16, sizeof(uint16_t),
									static_cast<uint16_t>(std::bit_cast<uint32_t>(second) >> 16U));
		return record;
	};

	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
												 {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	const std::vector<CanonicalVectorRecord> records = {bfloatRecord(1U, 0.0F, 0.0F), bfloatRecord(2U, 0.0F, 1.0F)};
	publishVectorPq(index.index_id, records);
	publishVectorAdjacency(index.index_id, records);

	RelocatableStorageBackend backend(options());
	backend.open();
	auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 1.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = records.size();
	request.maximum_raw_vector_reads = records.size();
	request.candidate_search_budget = 1U;
	request.candidate_refinement_budget = 1U;

	request.candidate_search_budget = 0U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::invalid_argument);
	request.candidate_search_budget = request.maximum_mapping_entries + 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::invalid_argument);
	request.candidate_search_budget = 1U;
	request.candidate_refinement_budget = 0U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::invalid_argument);
	request.candidate_refinement_budget = request.maximum_raw_vector_reads + 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::invalid_argument);
	request.candidate_refinement_budget = 1U;
	request.top_k = 2U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::invalid_argument);

	request.top_k = 1U;
	EXPECT_NO_THROW(static_cast<void>(snapshot->searchLogicalVector(request)));
	request.top_k = records.size();
	request.candidate_search_budget = records.size();
	request.candidate_refinement_budget = records.size();
	request.query_vector = {0.0F, 0.5F};
	const LogicalGraphVectorSearchResult expanded = snapshot->searchLogicalVector(request);
	EXPECT_EQ(expanded.hits.size(), records.size());
	EXPECT_GT(expanded.stats.candidate_expansions, 1U);
	EXPECT_FLOAT_EQ(expanded.hits[0].score, expanded.hits[1].score);
	EXPECT_LT(expanded.hits[0].node_id, expanded.hits[1].node_id);

	request.top_k = 1U;
	request.maximum_mapping_entries = 1U;
	request.maximum_raw_vector_reads = 1U;
	request.candidate_search_budget = 1U;
	request.candidate_refinement_budget = 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::runtime_error);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ValidatesNonFiniteAndZeroCosineQueriesAtTheStorageSearchBoundary) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, 0.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "cosine"});
	writer.close();

	const VectorManifest manifest = readVectorManifest(index.index_id);
	IndexDefinition definition;
	definition.index_id = index.index_id;
	definition.kind = IndexDefinitionKind::IDK_VECTOR;
	definition.vector_dimensions = manifest.dimensions;
	definition.vector_metric = "cosine";
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 0.0F};
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.maximum_mapping_entries = 1U;
	request.maximum_raw_vector_reads = 1U;

	expectDirectVectorSearchInvalidArgument(definition, manifest, request,
											"relocatable cosine Vector search query must have a nonzero L2 norm");
	request.query_vector = {std::numeric_limits<float>::quiet_NaN(), 0.0F};
	expectDirectVectorSearchInvalidArgument(
			definition, manifest, request,
			"relocatable logical Vector search requires a finite query matching the persisted dimensions");
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsDeltaManifestCountThatDisagreesWithItsPersistedOverlay) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto bfloatRecord = [](uint64_t vectorId, float first, float second) {
		CanonicalVectorRecord record;
		record.vector_id = vectorId;
		record.raw_bfloat16.resize(2U * sizeof(uint16_t));
		LittleEndianCodec::writeU16(record.raw_bfloat16, 0U,
									static_cast<uint16_t>(std::bit_cast<uint32_t>(first) >> 16U));
		LittleEndianCodec::writeU16(record.raw_bfloat16, sizeof(uint16_t),
									static_cast<uint16_t>(std::bit_cast<uint32_t>(second) >> 16U));
		return record;
	};

	RelocatableStorageBackend writer(options());
	writer.open();
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();
	const std::array<CanonicalVectorRecord, 2U> records = {bfloatRecord(1U, 0.0F, 0.0F), bfloatRecord(2U, 0.0F, 1.0F)};
	publishVectorAdjacency(index.index_id, records);

	RelocatableStorageBackend updater(options());
	updater.open();
	updater.replaceLogicalNodeProperties({{nodes.back(), {{"embedding", vectorValue(1.0, 1.0)}}}});
	updater.close();

	const VectorManifest manifest = readVectorManifest(index.index_id);
	ASSERT_NE(manifest.delta_mapping_index_id, 0U);
	ASSERT_GT(manifest.delta_entry_count, 1U);
	ASSERT_LT(manifest.delta_entry_count, std::numeric_limits<uint64_t>::max());
	IndexDefinition definition;
	definition.index_id = index.index_id;
	definition.kind = IndexDefinitionKind::IDK_VECTOR;
	definition.vector_dimensions = manifest.dimensions;
	definition.vector_metric = "l2";
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 1.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 8U;
	request.maximum_raw_vector_reads = 8U;
	request.candidate_search_budget = 1U;
	VectorManifest corrupt = manifest;
	++corrupt.delta_entry_count;
	expectDirectVectorSearchRejected(
			definition, corrupt, request,
			"relocatable logical Vector search delta overlay count disagrees with its Manifest");

	request.maximum_mapping_entries = manifest.delta_entry_count - 1U;
	RelocatableStorageBackend bounded(options());
	bounded.open();
	auto snapshot = bounded.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::runtime_error);
	snapshot.reset();
	bounded.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsPartialAndStaleVectorManifestIdentitiesAtTheSearchBoundary) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	const VectorManifest original = readVectorManifest(index.index_id);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 1U;
	request.maximum_raw_vector_reads = 1U;

	const auto expectRejected = [&] {
		RelocatableStorageBackend backend(options());
		backend.open();
		auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
		ASSERT_NE(snapshot, nullptr);
		EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), FormatException);
		snapshot.reset();
		backend.close();
	};

	VectorManifest corrupt = original;
	corrupt.node_mapping_root_generation = Generation(original.node_mapping_root_generation.value() + 1U);
	overwriteVectorManifest(corrupt);
	expectRejected();
	overwriteVectorManifest(original);

	corrupt = original;
	corrupt.metric = VectorDistanceMetric::VDM_INNER_PRODUCT;
	overwriteVectorManifest(corrupt);
	expectRejected();
	overwriteVectorManifest(original);

	IndexDefinition definition;
	definition.index_id = index.index_id;
	definition.kind = IndexDefinitionKind::IDK_VECTOR;
	definition.vector_dimensions = original.dimensions;
	definition.vector_metric = "l2";
	IndexDefinition zeroDimensionDefinition = definition;
	zeroDimensionDefinition.vector_dimensions = 0U;
	corrupt = original;
	corrupt.dimensions = 0U;
	expectDirectVectorSearchRejected(zeroDimensionDefinition, corrupt, request,
									 "relocatable logical Vector search found an invalid semantic Manifest");

	corrupt = original;
	corrupt.vector_node_mapping_root_page_id = PageId(1U);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a stale Vector-to-Node mapping root");

	// A public Index ID cannot masquerade as a delta root, even when the rest
	// of this forged identity is incomplete.
	corrupt = original;
	corrupt.delta_mapping_index_id = original.index_id;
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found an invalid semantic Manifest");

	corrupt = original;
	corrupt.adjacency_generation = Generation(1U);
	expectDirectVectorSearchRejected(
			definition, corrupt, request,
			"relocatable logical Vector search found a partial or incompatible adjacency generation");

	corrupt = original;
	corrupt.adjacency_mapping_index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xD5);
	expectDirectVectorSearchRejected(
			definition, corrupt, request,
			"relocatable logical Vector search found a partial or incompatible adjacency generation");
	corrupt = original;
	corrupt.adjacency_mapping_root_page_id = PageId(1U);
	expectDirectVectorSearchRejected(
			definition, corrupt, request,
			"relocatable logical Vector search found a partial or incompatible adjacency generation");
	corrupt = original;
	corrupt.adjacency_mapping_root_generation = Generation(1U);
	expectDirectVectorSearchRejected(
			definition, corrupt, request,
			"relocatable logical Vector search found a partial or incompatible adjacency generation");
	corrupt = original;
	corrupt.entry_point_vector_id = 1U;
	expectDirectVectorSearchRejected(
			definition, corrupt, request,
			"relocatable logical Vector search found a partial or incompatible adjacency generation");

	corrupt = original;
	corrupt.adjacency_generation = Generation(1U);
	corrupt.entry_point_vector_id = 1U;
	corrupt.adjacency_mapping_index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xD3);
	corrupt.adjacency_mapping_root_page_id = PageId(1U);
	corrupt.adjacency_mapping_root_generation = Generation(1U);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a stale adjacency mapping root");

	corrupt = original;
	corrupt.codebook_id = 1U;
	corrupt.codebook_generation = Generation(1U);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a partial PQ generation");

	corrupt = original;
	corrupt.codebook_generation = Generation(1U);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a partial PQ generation");
	corrupt = original;
	corrupt.pq_mapping_index_id = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xD6);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a partial PQ generation");
	corrupt = original;
	corrupt.pq_mapping_root_page_id = PageId(1U);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a partial PQ generation");
	corrupt = original;
	corrupt.pq_mapping_root_generation = Generation(1U);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a partial PQ generation");

	corrupt = original;
	corrupt.codebook_id = 1U;
	corrupt.codebook_generation = Generation(1U);
	corrupt.pq_mapping_index_id = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xD4);
	corrupt.pq_mapping_root_page_id = PageId(1U);
	corrupt.pq_mapping_root_generation = Generation(1U);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a stale PQ mapping root");
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsStaleAndNonFiniteRawVectorRecordsBeforeScoring) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 0.0F};
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.maximum_mapping_entries = 1U;
	request.maximum_raw_vector_reads = 1U;
	const auto expectRejected = [&](const char *expectedMessage) {
		RelocatableStorageBackend backend(options());
		backend.open();
		auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
		ASSERT_NE(snapshot, nullptr);
		try {
			static_cast<void>(snapshot->searchLogicalVector(request));
			FAIL() << "expected the corrupted raw Vector record to be rejected";
		} catch (const FormatException &error) {
			EXPECT_STREQ(error.what(), expectedMessage);
		}
		snapshot.reset();
		backend.close();
	};

	const std::vector<std::byte> original = readVectorRecord(1U);
	std::vector<std::byte> corrupt = original;
	LittleEndianCodec::writeU64(corrupt, 0U, 2U);
	overwriteVectorRecord(1U, corrupt);
	expectRejected("relocatable logical Vector search resolved a stale raw Vector record");
	overwriteVectorRecord(1U, original);

	corrupt = original;
	LittleEndianCodec::writeU16(corrupt, sizeof(uint64_t), 0x7F80U);
	overwriteVectorRecord(1U, corrupt);
	expectRejected("relocatable logical Vector search decoded a non-finite raw coordinate");
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsMalformedAndNonBijectiveSemanticVectorMappings) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 0.0F};
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.maximum_mapping_entries = 1U;
	request.maximum_raw_vector_reads = 1U;
	const auto expectRejected = [&](const char *expectedMessage) {
		RelocatableStorageBackend backend(options());
		backend.open();
		auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
		ASSERT_NE(snapshot, nullptr);
		try {
			static_cast<void>(snapshot->searchLogicalVector(request));
			FAIL() << "expected the corrupted semantic mapping to be rejected";
		} catch (const FormatException &error) {
			EXPECT_STREQ(error.what(), expectedMessage);
		}
		snapshot.reset();
		backend.close();
	};

	const BTreePageEntry nodeMapping{VectorSemanticCodec::nodeMappingKey(1U), 1U};
	const BTreePageEntry vectorMapping{VectorSemanticCodec::vectorMappingKey(1U), 1U};
	const VectorManifest manifest = readVectorManifest(index.index_id);

	overwriteBTreeLeafEntry(index.index_id, nodeMapping.key, {nodeMapping.key, 0U});
	expectRejected("relocatable logical Vector search found a malformed Node-to-Vector mapping");
	overwriteBTreeLeafEntry(index.index_id, nodeMapping.key, nodeMapping);

	const std::vector<std::byte> zeroNodeKey(sizeof(uint64_t), std::byte{});
	overwriteBTreeLeafEntry(index.index_id, nodeMapping.key, {zeroNodeKey, 1U});
	expectRejected("relocatable logical Vector search found a noncanonical Node mapping key");
	overwriteBTreeLeafEntry(index.index_id, zeroNodeKey, nodeMapping);

	const BTreePageEntry wrongNodeMapping{VectorSemanticCodec::nodeMappingKey(2U), 1U};
	overwriteBTreeLeafEntry(index.index_id, nodeMapping.key, wrongNodeMapping);
	expectRejected("relocatable logical Vector search found inconsistent semantic mapping directions");
	overwriteBTreeLeafEntry(index.index_id, wrongNodeMapping.key, nodeMapping);

	overwriteBTreeLeafEntry(manifest.vector_node_mapping_index_id, vectorMapping.key, {vectorMapping.key, 0U});
	expectRejected("relocatable Vector-to-Node mapping B+Tree contains an invalid Node ID value");
	overwriteBTreeLeafEntry(manifest.vector_node_mapping_index_id, vectorMapping.key, vectorMapping);
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsMissingAdjacencyMappingBeforeCandidateExpansion) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto bfloatRecord = [](uint64_t vectorId, float first, float second) {
		CanonicalVectorRecord record;
		record.vector_id = vectorId;
		record.raw_bfloat16.resize(2U * sizeof(uint16_t));
		LittleEndianCodec::writeU16(record.raw_bfloat16, 0U,
									static_cast<uint16_t>(std::bit_cast<uint32_t>(first) >> 16U));
		LittleEndianCodec::writeU16(record.raw_bfloat16, sizeof(uint16_t),
									static_cast<uint16_t>(std::bit_cast<uint32_t>(second) >> 16U));
		return record;
	};

	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();
	const std::vector<CanonicalVectorRecord> records = {bfloatRecord(1U, 0.0F, 0.0F)};
	publishVectorAdjacency(index.index_id, records);

	const VectorManifest manifest = readVectorManifest(index.index_id);
	const std::vector<std::byte> entryKey = VectorSemanticCodec::vectorMappingKey(manifest.entry_point_vector_id);
	const std::vector<std::byte> replacementKey =
			VectorSemanticCodec::vectorMappingKey(manifest.entry_point_vector_id + 1U);
	const std::vector<BTreePageEntry> mappings = readIndexEntries(manifest.adjacency_mapping_index_id);
	ASSERT_EQ(mappings.size(), 1U);
	ASSERT_EQ(mappings.front().key, entryKey);
	const BTreePageEntry mapping = mappings.front();
	overwriteBTreeLeafEntry(manifest.adjacency_mapping_index_id, entryKey, {replacementKey, mapping.value_or_child});

	RelocatableStorageBackend backend(options());
	backend.open();
	auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 2U;
	request.maximum_raw_vector_reads = 2U;
	request.candidate_search_budget = 1U;
	try {
		static_cast<void>(snapshot->searchLogicalVector(request));
		FAIL() << "expected the missing adjacency mapping to be rejected";
	} catch (const FormatException &error) {
		EXPECT_STREQ(error.what(), "relocatable logical Vector search found no adjacency record for a candidate");
	}
	snapshot.reset();
	backend.close();
	overwriteBTreeLeafEntry(manifest.adjacency_mapping_index_id, replacementKey, mapping);
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsCorruptPqInventoryMappingsAndCodesBeforeRefinement) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto bfloatRecord = [](uint64_t vectorId, float first, float second) {
		CanonicalVectorRecord record;
		record.vector_id = vectorId;
		record.raw_bfloat16.resize(2U * sizeof(uint16_t));
		LittleEndianCodec::writeU16(record.raw_bfloat16, 0U,
									static_cast<uint16_t>(std::bit_cast<uint32_t>(first) >> 16U));
		LittleEndianCodec::writeU16(record.raw_bfloat16, sizeof(uint16_t),
									static_cast<uint16_t>(std::bit_cast<uint32_t>(second) >> 16U));
		return record;
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
												 {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();
	const std::vector<CanonicalVectorRecord> records = {bfloatRecord(1U, 0.0F, 0.0F), bfloatRecord(2U, 0.0F, 1.0F)};
	publishVectorPq(index.index_id, records);
	publishVectorAdjacency(index.index_id, records);

	const VectorManifest manifest = readVectorManifest(index.index_id);
	const auto pqEntry = std::ranges::find_if(manifest.entries, [](const VectorManifestEntry &entry) {
		return entry.storage_kind == VectorStorageKind::VSK_PQ;
	});
	ASSERT_NE(pqEntry, manifest.entries.end());
	ASSERT_NE(manifest.entry_point_vector_id, 0U);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = records.size();
	request.maximum_raw_vector_reads = 1U;
	request.candidate_search_budget = 1U;
	request.candidate_refinement_budget = 1U;
	const auto expectRejected = [&](const char *expectedMessage) {
		RelocatableStorageBackend backend(options());
		backend.open();
		auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
		ASSERT_NE(snapshot, nullptr);
		try {
			static_cast<void>(snapshot->searchLogicalVector(request));
			FAIL() << "expected the corrupt PQ state to be rejected";
		} catch (const FormatException &error) {
			EXPECT_STREQ(error.what(), expectedMessage);
		}
		snapshot.reset();
		backend.close();
	};

	IndexDefinition definition;
	definition.index_id = index.index_id;
	definition.kind = IndexDefinitionKind::IDK_VECTOR;
	definition.vector_dimensions = manifest.dimensions;
	definition.vector_metric = "l2";
	VectorManifest missingInventory = manifest;
	missingInventory.entries.clear();
	expectDirectVectorSearchRejected(definition, missingInventory, request,
									 "relocatable logical Vector search PQ generation has no Manifest page inventory");

	const BTreePageEntry pqMapping{VectorSemanticCodec::vectorMappingKey(manifest.entry_point_vector_id),
								   pqEntry->page_id.value()};
	overwriteBTreeLeafEntry(manifest.pq_mapping_index_id, pqMapping.key,
							{pqMapping.key, manifest.raw_write_tail_page_id.value()});
	expectRejected("relocatable logical Vector search PQ mapping escaped its Manifest inventory");
	overwriteBTreeLeafEntry(manifest.pq_mapping_index_id, pqMapping.key, pqMapping);

	// A missing mapping is distinct from a mapping that targets a page outside
	// the Manifest inventory. Both states must be rejected before page I/O.
	const std::vector<std::byte> missingMappingKey =
			VectorSemanticCodec::vectorMappingKey(manifest.entry_point_vector_id + 100U);
	overwriteBTreeLeafEntry(manifest.pq_mapping_index_id, pqMapping.key, {missingMappingKey, pqMapping.value_or_child});
	expectRejected("relocatable logical Vector search PQ mapping escaped its Manifest inventory");
	overwriteBTreeLeafEntry(manifest.pq_mapping_index_id, missingMappingKey, pqMapping);

	const std::vector<std::byte> original = readVectorPageRecord(pqEntry->page_id, manifest.entry_point_vector_id);
	std::vector<std::byte> corrupt = original;
	const uint64_t staleVectorId = manifest.entry_point_vector_id + 100U;
	LittleEndianCodec::writeU64(corrupt, 0U, staleVectorId);
	overwriteVectorPageRecord(pqEntry->page_id, manifest.entry_point_vector_id, corrupt);
	expectRejected("relocatable logical Vector search resolved an incompatible PQ page");
	overwriteVectorPageRecord(pqEntry->page_id, staleVectorId, original);

	corrupt = original;
	corrupt[sizeof(uint64_t)] = std::byte{0xFFU};
	overwriteVectorPageRecord(pqEntry->page_id, manifest.entry_point_vector_id, corrupt);
	expectRejected("relocatable logical Vector search found an out-of-range PQ centroid code");
}

TEST_F(RelocatableStorageBackendSchemaTest, ExecutesTheExistingVectorProcedureOverTheLogicalSearchBoundary) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	const std::vector<uint64_t> nodes =
			backend->createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, -2.0)}}},
										 {{"Person"}, {{"embedding", vectorValue(-3.0, 4.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(
			backend->createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "inner_product"}));

	graph::query::QueryEngine engine(backend, backend);
	const graph::query::QueryResult result = engine.execute(
			"CALL db.index.vector.queryNodes('person_embedding', 2, [1.0, -2.0]) YIELD node, score RETURN node, score");
	ASSERT_EQ(result.getColumns(), (std::vector<std::string>{"node", "score"}));
	ASSERT_EQ(result.rowCount(), 2U);
	ASSERT_TRUE(result.getRows()[0].at("node").isLogicalNode());
	EXPECT_EQ(result.getRows()[0].at("node").asLogicalNode().node_id, nodes[0]);
	EXPECT_EQ(result.getRows()[1].at("node").asLogicalNode().node_id, nodes[1]);
	EXPECT_LT(std::get<double>(result.getRows()[0].at("score").asPrimitive().getVariant()),
			  std::get<double>(result.getRows()[1].at("score").asPrimitive().getVariant()));
	backend->close();
}

TEST_F(RelocatableStorageBackendSchemaTest, TrainsAndReusesAPersistentPqGenerationThroughTheLogicalProcedure) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	static_cast<void>(backend->createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
												   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}},
												   {{"Person"}, {{"embedding", vectorValue(10.0, 10.0)}}}}));
	const LogicalGraphIndexDefinition index =
			backend->createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});

	graph::query::QueryEngine engine(backend, backend);
	const graph::query::QueryResult procedure = engine.execute("CALL db.index.vector.train('person_embedding')");
	ASSERT_EQ(procedure.getColumns(), (std::vector<std::string>{"status"}));
	ASSERT_EQ(procedure.rowCount(), 1U);
	EXPECT_EQ(procedure.getRows().front().at("status").asPrimitive(), graph::PropertyValue("Success"));
	const graph::query::QueryResult projectedProcedure =
			engine.execute("CALL db.index.vector.train('person_embedding') YIELD status "
						   "WHERE status = 'Success' RETURN status AS training_status");
	ASSERT_EQ(projectedProcedure.getColumns(), (std::vector<std::string>{"training_status"}));
	ASSERT_EQ(projectedProcedure.rowCount(), 1U);
	EXPECT_EQ(projectedProcedure.getRows().front().at("training_status").asPrimitive(),
			  graph::PropertyValue("Success"));
	const graph::query::QueryResult skippedProcedure =
			engine.execute("CALL db.index.vector.train('person_embedding') YIELD status RETURN status SKIP 1 LIMIT 1");
	EXPECT_TRUE(skippedProcedure.isEmpty());
	static_cast<void>(backend->createLogicalNodeVectorIndex({"empty_embedding", "Empty", "embedding", 2U, "l2"}));
	const graph::query::QueryResult emptyProcedure =
			engine.execute("CALL db.index.vector.train('empty_embedding') YIELD status RETURN status");
	ASSERT_EQ(emptyProcedure.getColumns(), (std::vector<std::string>{"status"}));
	ASSERT_EQ(emptyProcedure.rowCount(), 1U);
	EXPECT_EQ(emptyProcedure.getRows().front().at("status").asPrimitive(),
			  graph::PropertyValue("Skipped (Empty Data)"));

	const VectorManifest first = readVectorManifest(index.index_id);
	EXPECT_NE(first.codebook_id, 0U);
	EXPECT_TRUE(first.codebook_generation.isValid());
	EXPECT_TRUE(VectorPqMapping::isMappingIndexId(first.pq_mapping_index_id));
	EXPECT_TRUE(first.pq_mapping_root_page_id.isValid());
	EXPECT_TRUE(first.pq_mapping_root_generation.isValid());
	EXPECT_TRUE(first.adjacency_generation.isValid());
	EXPECT_TRUE(VectorAdjacencyMapping::isMappingIndexId(first.adjacency_mapping_index_id));
	EXPECT_TRUE(first.adjacency_mapping_root_page_id.isValid());
	EXPECT_TRUE(first.adjacency_mapping_root_generation.isValid());
	EXPECT_EQ(first.delta_entry_count, 0U);
	EXPECT_TRUE(std::ranges::any_of(first.entries, [](const VectorManifestEntry &entry) {
		return entry.storage_kind == VectorStorageKind::VSK_PQ;
	}));
	EXPECT_NO_THROW(static_cast<void>(backend->verifyIntegrity()));

	const LogicalGraphVectorTrainingResult retry = backend->trainLogicalVector({"person_embedding", 16U, 16U});
	EXPECT_TRUE(retry.trained);
	EXPECT_TRUE(retry.reused_existing_generation);
	EXPECT_EQ(readVectorManifest(index.index_id), first);

	auto exactSnapshot = backend->openLogicalGraphVectorSearchSnapshot();
	LogicalGraphVectorSearchRequest exact;
	exact.index_name = "person_embedding";
	exact.query_vector = {0.0F, 1.0F};
	exact.top_k = 1U;
	exact.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	exact.maximum_mapping_entries = 16U;
	exact.maximum_raw_vector_reads = 16U;
	EXPECT_EQ(exactSnapshot->searchLogicalVector(exact).hits.size(), 1U);
	exactSnapshot.reset();

	graph::query::QueryContext readOnly;
	readOnly.execMode = graph::query::ExecMode::EM_READ_ONLY;
	EXPECT_THROW(static_cast<void>(engine.execute("CALL db.index.vector.train('person_embedding')", readOnly)),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(backend->trainLogicalVector({"missing", 16U, 16U})), std::runtime_error);
	backend->close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	const LogicalGraphVectorTrainingResult afterReopen = reopened.trainLogicalVector({"person_embedding", 16U, 16U});
	EXPECT_TRUE(afterReopen.trained);
	EXPECT_TRUE(afterReopen.reused_existing_generation);
	reopened.close();
}
