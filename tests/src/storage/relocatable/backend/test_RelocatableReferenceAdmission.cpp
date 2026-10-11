/**
 * @brief Native reference-page allocation admission and continuation regression.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalVarLengthTraversal.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ShortestPathExpression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/execution/LogicalGraphReadExecutor.hpp"
#include "graph/query/QueryGuard.hpp"
#include "query/parser/cypher/CypherParserImpl.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/gds/LogicalProjectionCatalog.hpp"
#include "graph/query/api/QueryEngine.hpp"

using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest, MutationConstraintReadControlsRejectBeforePublicationAndAllowRetry) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const PropertyValue original(std::string(512U, 'k'));
	const PropertyValue changed(std::string(512U, 'v'));
	const PropertyValue payload(std::string(16000U, 'p'));
	const auto nodes = backend.createLogicalNodes({{{"MutationAdmission"}, {{"key", original}, {"unused", payload}}}});
	const auto edges = backend.createLogicalEdges({{nodes[0], nodes[0], "MutationAdmission", {{"key", original}, {"unused", payload}}}});
	for (bool edge: {false, true}) {
		SCOPED_TRACE(edge);
		static_cast<void>(backend.createLogicalConstraint({edge ? "edge_admission" : "node_admission",
			edge ? LogicalGraphConstraintEntityKind::LGCEK_EDGE : LogicalGraphConstraintEntityKind::LGCEK_NODE,
			LogicalGraphConstraintKind::LGCK_UNIQUE, "MutationAdmission", {"key"}}));
		const auto batchFor = [&](const PropertyValue &value) {
			LogicalGraphMutationBatch batch;
			batch.property_replacements.push_back({{
				edge ? LogicalGraphMutationEntityKind::LGMK_EDGE : LogicalGraphMutationEntityKind::LGMK_NODE,
				LogicalGraphMutationReferenceKind::LGRK_PERSISTED, edge ? edges[0] : nodes[0]},
				{{"key", value}, {"unused", payload}}});
			return batch;
		};
		const auto verify = [&](const PropertyValue &value) {
			auto snapshot = backend.openLogicalGraphReadSnapshot();
			const auto properties = edge ? snapshot->readEdgeShaped(edges[0], {.all_properties = true}).properties
				: snapshot->readNodeShaped(nodes[0], {.all_properties = true}).properties;
			const auto key = std::ranges::find(properties, "key", &LogicalGraphPropertyEntry::key);
			ASSERT_NE(key, properties.end()); EXPECT_EQ(key->value, value);
			const auto unused = std::ranges::find(properties, "unused", &LogicalGraphPropertyEntry::key);
			ASSERT_NE(unused, properties.end()); EXPECT_EQ(unused->value, payload);
		};
		const auto changedBatch = batchFor(changed);
		size_t admissions = 0U;
		LogicalGraphMutationReadControl control;
		control.property_admission = [&](size_t bytes) {
			++admissions;
			if (bytes > 8U) throw std::length_error("selected constraint source rejected");
		};
		EXPECT_THROW(static_cast<void>(backend.executeLogicalGraphMutationBatchWithReadControl(changedBatch, {}, control)), std::length_error);
		EXPECT_GT(admissions, 0U); verify(original);
		size_t checkpoints = 0U;
		control.checkpoint = [&] { ++checkpoints; };
		control.property_admission = [](size_t bytes) {
			if (bytes > 4096U) throw std::length_error("selected source included unrelated payload");
		};
		static_cast<void>(backend.executeLogicalGraphMutationBatchWithReadControl(changedBatch, {}, control));
		verify(changed); ASSERT_GT(checkpoints, 2U);
		static_cast<void>(backend.executeLogicalGraphMutationBatch(batchFor(original)));
		for (size_t rejectAt = 1U; rejectAt <= checkpoints; ++rejectAt) {
			SCOPED_TRACE(rejectAt);
			size_t current = 0U;
			control.checkpoint = [&] { if (++current == rejectAt) throw std::length_error("mutation cancelled"); };
			EXPECT_THROW(static_cast<void>(backend.executeLogicalGraphMutationBatchWithReadControl(changedBatch, {}, control)), std::length_error);
			EXPECT_EQ(current, rejectAt); verify(original);
		}
		control.checkpoint = {};
		for (size_t retry = 0U; retry < 2U; ++retry) {
			static_cast<void>(backend.executeLogicalGraphMutationBatchWithReadControl(changedBatch, {}, control));
			verify(changed);
			static_cast<void>(backend.executeLogicalGraphMutationBatch(batchFor(original)));
		}
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, SchemaPropertySourceAdmissionRejectsBeforePublicationAndAllowsRetry) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options)); backend->open();
	const auto nodes = backend->createLogicalNodes({{{"SchemaAdmission"}, {
		{"key", PropertyValue(std::string(512U, 'k'))}, {"other", PropertyValue(std::string(512U, 'o'))},
		{"embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue(2.0)})},
		{"unused", PropertyValue(std::string(16000U, 'p'))}}}});
	static_cast<void>(backend->createLogicalEdges({{nodes[0], nodes[0], "SchemaAdmission",
		{{"key", PropertyValue(std::string(512U, 'k'))}, {"unused", PropertyValue(std::string(16000U, 'p'))}}}}));
	for (size_t mode = 0U; mode < 5U; ++mode) {
		SCOPED_TRACE(mode);
		const auto create = [&](const std::function<void(size_t)> &admission) {
			if (mode == 0U) static_cast<void>(backend->createLogicalNodeIndexWithCheckpoint(
				{"source_scalar", "SchemaAdmission", {"key"}}, {}, admission));
			else if (mode == 1U) {
				const std::array<LogicalGraphNodeIndexCreateRequest, 2U> indexes{{
					{"source_scalar", "SchemaAdmission", {"key"}}, {"source_other", "SchemaAdmission", {"other"}}}};
				static_cast<void>(backend->createLogicalNodeIndexesWithCheckpoint(indexes, {}, admission));
			} else if (mode == 2U) static_cast<void>(backend->createLogicalNodeVectorIndexWithCheckpoint(
				{"source_vector", "SchemaAdmission", "embedding", 2U, "l2"}, {}, admission));
			else static_cast<void>(backend->createLogicalConstraintWithCheckpoint({"source_constraint",
				mode == 3U ? LogicalGraphConstraintEntityKind::LGCEK_NODE : LogicalGraphConstraintEntityKind::LGCEK_EDGE,
				LogicalGraphConstraintKind::LGCK_UNIQUE, "SchemaAdmission", {"key"}}, {}, admission));
		};
		size_t calls = 0U;
		EXPECT_THROW(create([&](size_t bytes) {
			++calls;
			// Each scalar field fits separately; the batch must also sum both fields.
			if (bytes > (mode == 1U ? 600U : 8U)) throw std::length_error("source budget rejected");
		}), std::length_error);
		EXPECT_GT(calls, 0U);
		EXPECT_TRUE(backend->listLogicalIndexes(8U).empty());
		EXPECT_TRUE(backend->listLogicalConstraints(8U).empty());
		for (size_t retry = 0U; retry < 2U; ++retry) {
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
			create([](size_t bytes) { if (bytes > 4096U) throw std::length_error("unexpected source growth"); });
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
			if (mode < 3U) for (const auto &index: backend->listLogicalIndexes(8U)) EXPECT_TRUE(backend->dropLogicalIndexByName(index.name));
			else EXPECT_TRUE(backend->dropLogicalConstraintByName("source_constraint"));
		}
	}
	graph::query::LogicalReadExecutionOptions narrow;
	narrow.bounds.maximum_materialized_bytes = 8U;
	graph::query::QueryEngine rejected(backend, narrow);
	EXPECT_THROW(static_cast<void>(rejected.execute("CREATE INDEX engine_source FOR (n:SchemaAdmission) ON (n.key)")), std::runtime_error);
	EXPECT_TRUE(backend->listLogicalIndexes(8U).empty());
	narrow.bounds.maximum_materialized_bytes = 4096U;
	graph::query::QueryEngine retried(backend, narrow);
	EXPECT_EQ(retried.execute("CREATE INDEX engine_source FOR (n:SchemaAdmission) ON (n.key)").rowCount(), 1U);
	EXPECT_TRUE(backend->dropLogicalIndexByName("engine_source"));
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, SchemaPropertySourceBudgetResetsBetweenBoundedIndexBatches) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	std::vector<LogicalGraphNodeCreateRequest> nodes;
	for (size_t i = 0U; i < 1025U; ++i) nodes.push_back({{"BatchSource"}, {
		{"key", PropertyValue(std::string(64U, 'k') + std::to_string(i))}}});
	static_cast<void>(backend.createLogicalNodes(nodes));
	size_t peak = 0U;
	size_t previous = 0U;
	size_t batchRestarts = 0U;
	// Include both the token and canonical value header, not just String payload.
	const size_t sourceLimit = 512U * (sizeof(uint32_t) + CanonicalPropertyValueCodec::encodedSize(
		PropertyValue(std::string(64U, 'k') + "1024")));
	const auto index = backend.createLogicalNodeIndexWithCheckpoint({"batch_source", "BatchSource", {"key"}}, {},
		[&](size_t bytes) {
			if (bytes < previous) ++batchRestarts;
			previous = bytes;
			peak = std::max(peak, bytes);
			if (bytes > sourceLimit) throw std::length_error("released batches must not accumulate");
		});
	EXPECT_GT(peak, 32U * 1024U);
	EXPECT_GE(batchRestarts, 2U);
	EXPECT_EQ(backend.listLogicalIndexes(8U).front().index_id, index.index_id);
	EXPECT_TRUE(backend.dropLogicalIndexByName(index.name));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ConstraintSupportTreeReadsSelectedFieldsForNodeCompositeAndEdgeKeys) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto properties = [](int first, int second) {
		return std::vector<LogicalGraphWriteProperty>{{"first", PropertyValue(first)}, {"second", PropertyValue(second)},
			{"unused", PropertyValue(std::string(16000U, 'p'))}};
	};
	const auto nodes = backend.createLogicalNodes({{{"SelectedKey"}, properties(1, 11)},
		{{"SelectedKey"}, properties(2, 22)}, {{"OutsideKey"}, properties(1, 11)},
		{{"OutsideKey"}, {{"unused", PropertyValue(std::string(16000U, 'p'))}}}});
	static_cast<void>(backend.createLogicalEdges({{nodes[0], nodes[1], "SelectedKey", properties(1, 11)},
		{nodes[1], nodes[0], "SelectedKey", properties(2, 22)}, {nodes[0], nodes[0], "OutsideKey", properties(1, 11)},
		{nodes[1], nodes[1], "SelectedKey", {{"unused", PropertyValue(std::string(16000U, 'p'))}}}}));
	for (bool edge: {false, true}) {
		SCOPED_TRACE(edge);
		const auto request = LogicalGraphConstraintCreateRequest{edge ? "selected_edge_key" : "selected_node_key",
			edge ? LogicalGraphConstraintEntityKind::LGCEK_EDGE : LogicalGraphConstraintEntityKind::LGCEK_NODE,
			edge ? LogicalGraphConstraintKind::LGCK_UNIQUE : LogicalGraphConstraintKind::LGCK_NODE_KEY,
			"SelectedKey", edge ? std::vector<std::string>{"first"} : std::vector<std::string>{"second", "first"}};
		for (size_t retry = 0U; retry < 2U; ++retry) {
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
			const auto definition = backend.createLogicalConstraint(request);
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			EXPECT_EQ(definition.property_keys, request.property_keys);
			EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
			// Validation reads all four entities. Construction reads only matching
			// references (two Nodes / three Edges) and reuses those columns for canonical validation.
			ASSERT_TRUE(trace.contains("native_graph.read.property_selected_column_batch_owners"));
			EXPECT_EQ(trace.at("native_graph.read.property_selected_column_batch_owners").totalValue, edge ? 7 : 6);
			if (edge) EXPECT_THROW(static_cast<void>(backend.createLogicalEdges({
				{nodes[0], nodes[1], "SelectedKey", properties(1, 11)}})), std::runtime_error);
			else EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"SelectedKey"}, properties(1, 11)}})), std::runtime_error);
			EXPECT_TRUE(backend.dropLogicalConstraintByName(request.name));
		}
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, SchemaCreationCancellationLeavesNoPublishedDefinitionAndAllowsRetry) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	for (int i = 0; i < 4; ++i) requests.push_back({{"CancelableSchema"}, {
		{"key", PropertyValue(i)}, {"other", PropertyValue(i + 100)},
		{"embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(i + 1.0), PropertyValue(1.0)})}}});
	const auto nodes = backend.createLogicalNodes(requests);
	for (size_t mode = 0U; mode < 5U; ++mode) {
		SCOPED_TRACE(mode);
		const auto create = [&](const std::function<void()> &checkpoint) {
			if (mode == 0U) static_cast<void>(backend.createLogicalNodeIndexWithCheckpoint({"cancel_scalar", "CancelableSchema", {"key"}}, checkpoint));
			else if (mode == 1U) {
				const std::array<LogicalGraphNodeIndexCreateRequest, 2U> indexes{{
					{"cancel_scalar", "CancelableSchema", {"key"}}, {"cancel_other", "CancelableSchema", {"other"}}}};
				static_cast<void>(backend.createLogicalNodeIndexesWithCheckpoint(indexes, checkpoint));
			} else if (mode == 2U) static_cast<void>(backend.createLogicalNodeVectorIndexWithCheckpoint(
				{"cancel_vector", "CancelableSchema", "embedding", 2U, "l2"}, checkpoint));
			else static_cast<void>(backend.createLogicalConstraintWithCheckpoint({"cancel_constraint",
				LogicalGraphConstraintEntityKind::LGCEK_NODE,
				mode == 3U ? LogicalGraphConstraintKind::LGCK_UNIQUE : LogicalGraphConstraintKind::LGCK_NOT_NULL,
				"CancelableSchema", {"key"}}, checkpoint));
		};
		const auto drop = [&] {
			if (mode < 3U) for (const auto &index: backend.listLogicalIndexes(8U)) EXPECT_TRUE(backend.dropLogicalIndexByName(index.name));
			else EXPECT_TRUE(backend.dropLogicalConstraintByName("cancel_constraint"));
		};
		size_t baselineCalls = 0U;
		create([&] { ++baselineCalls; });
		ASSERT_GT(baselineCalls, 2U);
		drop();
		for (size_t cancellationCall = 1U; cancellationCall <= baselineCalls; ++cancellationCall) {
			SCOPED_TRACE(cancellationCall);
			size_t calls = 0U;
			EXPECT_THROW(create([&] {
				if (++calls == cancellationCall) throw std::length_error("cancel schema before publication");
			}), std::length_error);
			EXPECT_EQ(calls, cancellationCall);
			EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());
			EXPECT_TRUE(backend.listLogicalConstraints(8U).empty());
		}
		for (size_t retry = 0U; retry < 2U; ++retry) {
			EXPECT_NO_THROW(create({}));
			if (mode < 3U) EXPECT_EQ(backend.listLogicalIndexes(8U).size(), mode == 1U ? 2U : 1U);
			else EXPECT_EQ(backend.listLogicalConstraints(8U).size(), 1U);
			drop();
		}
	}
	backend.close();
	auto reopenOptions = backendOptions(); reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions)); reopened.open();
	EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
	EXPECT_TRUE(reopened.listLogicalConstraints(8U).empty());
	EXPECT_EQ(reopened.openLogicalGraphReadSnapshot()->readNodeShaped(nodes[0], {}).node_id, nodes[0]);
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, ProjectionBuildRejectsRevisionChangesAndCancellationBeforePublication) {
	using namespace graph::query::gds;
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto nodes = backend.createLogicalNodes({{{"Projection"}, {}}, {{"Projection"}, {}}});
	const auto edge = backend.createLogicalEdges({{nodes[0], nodes[1], "WEIGHTED",
		{{"weight", PropertyValue(2.0)}, {"unused", PropertyValue(std::string(16000U, 'p'))}}}})[0];
	LogicalGraphProjectionSpec spec;
	spec.name = "checked-build"; spec.node_labels = {"Projection"};
	spec.relationships = {{.type = "WEIGHTED", .weight = {
		.kind = LogicalProjectionWeightKind::LPWK_PROPERTY, .property_name = "weight"}}};
	const LogicalProjectionBuildBounds bounds{.maximum_nodes = 8U, .maximum_metadata_pages = 8U,
		.maximum_edges_per_node = 8U, .maximum_property_bytes = 1024U};
	for (bool vectorSnapshot: {false, true}) {
		SCOPED_TRACE(vectorSnapshot);
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		auto snapshot = vectorSnapshot ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
			: overlay.openLogicalGraphReadSnapshot();
		size_t baselineCalls = 0U;
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
		const auto baseline = LogicalGraphProjection::build(*snapshot, spec, bounds, [&] { ++baselineCalls; });
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		ASSERT_EQ(baseline.outgoing(nodes[0]).size(), 1U);
		EXPECT_EQ(baseline.outgoing(nodes[0])[0].weight, 2.0);
		EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
		ASSERT_GT(baselineCalls, 2U);
		// Exercise every build checkpoint, including source admission and the final return boundary.
		// Catalog preparation invokes one checkpoint before entering the builder.
		for (size_t cancellationCall = 1U; cancellationCall <= baselineCalls + 3U; ++cancellationCall) {
			SCOPED_TRACE(cancellationCall);
			LogicalProjectionCatalog catalog;
			size_t calls = 0U;
			EXPECT_THROW(static_cast<void>(catalog.create(spec, *snapshot, bounds, [&] {
				if (++calls == cancellationCall) throw std::length_error("cancel projection");
			})), std::length_error);
			EXPECT_EQ(calls, cancellationCall);
			EXPECT_FALSE(catalog.exists(spec.name));
		}
		for (size_t boundary = 0U; boundary < 4U; ++boundary) {
			baselineCalls = 0U;
			static_cast<void>(LogicalGraphProjection::build(*snapshot, spec, bounds, [&] { ++baselineCalls; }));
			const size_t mutationCall = boundary == 0U ? 2U : baselineCalls + boundary;
			SCOPED_TRACE(mutationCall);
			LogicalProjectionCatalog catalog;
			size_t calls = 0U;
			EXPECT_THROW(static_cast<void>(catalog.create(spec, *snapshot, bounds, [&] {
				if (++calls == mutationCall) {
					overlay.replaceLogicalEdgeProperties({{edge, {{"weight", PropertyValue(3.0)}}}});
					// Refresh a vector delegate while the outer build still owns its old revision.
					EXPECT_EQ(snapshot->readEdgeShaped(edge, {.all_properties = true}).properties[0].value, PropertyValue(3.0));
				}
			})), std::runtime_error);
			EXPECT_EQ(calls, mutationCall);
			EXPECT_FALSE(catalog.exists(spec.name));
			for (size_t retry = 0U; retry < 2U; ++retry) {
				const auto rebuilt = LogicalGraphProjection::build(*snapshot, spec, bounds);
				ASSERT_EQ(rebuilt.outgoing(nodes[0]).size(), 1U);
				EXPECT_EQ(rebuilt.outgoing(nodes[0])[0].weight, 3.0);
			}
			// Restore base visibility so the next attempt follows the same build path.
			overlay.replaceLogicalEdgeProperties({{edge, {{"weight", PropertyValue(2.0)},
				{"unused", PropertyValue(std::string(16000U, 'p'))}}}});
		}
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ReferenceAdmissionTracksConversionPeakAndRetriesRejectedContinuation) {
	const auto fixture = materializeNativeGraphFixture();
	auto config = backendOptions();
	config.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(config);
	backend.open();
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_TRUE(snapshot->supportsAdjacencyReferenceBufferAdmission());
	const auto read = [&](std::optional<LogicalGraphAdjacencyContinuation> continuation,
		const std::function<void(size_t)> &admission) {
		return snapshot->scanAdjacentEdgeRefsWithBufferAdmission(fixture.person_node_id,
			LogicalGraphReadDirection::LGRD_OUTGOING, continuation, 1U, admission);
	};
	EXPECT_THROW(static_cast<void>(read({}, [](size_t) {
		throw std::runtime_error("reference allocation rejected");
	})), std::runtime_error);
	size_t retained = 0U;
	size_t peak = 0U;
	const auto admit = [&](size_t bytes) { retained = bytes; peak = std::max(peak, bytes); };
	const auto first = read({}, admit);
	ASSERT_EQ(first.edges.size(), 1U);
	EXPECT_EQ(first.edges.front().entity_id, fixture.knows_edge_id);
	EXPECT_EQ(retained, first.edges.capacity() * sizeof(EntityRef));
	EXPECT_GT(peak, retained);
	ASSERT_TRUE(first.continuation);
	EXPECT_THROW(static_cast<void>(read(first.continuation, [](size_t) {
		throw std::runtime_error("continuation allocation rejected");
	})), std::runtime_error);
	const auto tail = read(first.continuation, admit);
	ASSERT_EQ(tail.edges.size(), 1U);
	EXPECT_EQ(tail.edges.front().entity_id, fixture.mentions_edge_id);
	EXPECT_FALSE(tail.continuation);
	EXPECT_EQ(retained, tail.edges.capacity() * sizeof(EntityRef));
	const auto replay = read(first.continuation, admit);
	ASSERT_EQ(replay.edges.size(), 1U);
	EXPECT_EQ(replay.edges.front().entity_id, tail.edges.front().entity_id);
	EXPECT_EQ(replay.edges.front().type_token, tail.edges.front().type_token);
}

TEST_F(RelocatableStorageBackendTest, NativeReferenceShortestPathCompletesHighFanoutUnderSharedBudget) {
	using namespace graph::query;
	using namespace graph::query::execution;
	const auto fixture = materializeNativeGraphFixture();
	auto config = backendOptions();
	config.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(config);
	backend.open();
	std::vector<LogicalGraphEdgeCreateRequest> edges(199U,
		{fixture.person_node_id, fixture.person_node_id, "KNOWS", {}});
	edges.push_back({fixture.person_node_id, fixture.unlabeled_node_id, "KNOWS", {}});
	ASSERT_EQ(backend.createLogicalEdges(edges).size(), edges.size());
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	LogicalPlanReadRow row;
	row.values.emplace("a", EntityRef{fixture.person_node_id, false, 0U, 0U, 0U});
	row.values.emplace("b", EntityRef{fixture.unlabeled_node_id, false, 0U, 0U, 0U});
	std::vector<std::unique_ptr<expressions::Expression>> arguments;
	arguments.push_back(std::make_unique<expressions::ShortestPathExpression>(
		"a", "b", "KNOWS", expressions::PatternDirection::PAT_OUTGOING, 0, 1, false));
	expressions::FunctionCallExpression length("length", std::move(arguments));
	LogicalPlanReadRuntimeBounds bounds{.maximum_node_records = 16U,
		.maximum_node_page_records = 2U, .maximum_metadata_pages_per_node_page = 8U,
		.maximum_edges_per_node = 256U, .maximum_edge_records = 256U,
		.maximum_adjacency_page_records = 256U, .maximum_property_entries = 8U,
		.maximum_rows = 256U, .maximum_distinct_rows = 256U,
		.maximum_materialized_bytes = 8192U};
	const bool oldValues = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	PropertyValue result;
	try {
		result = std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			*snapshot, length, row, bounds, QueryContext{}));
	} catch (...) {
		graph::debug::PerfTrace::setValueCollectionEnabled(oldValues);
		throw;
	}
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(oldValues);
	EXPECT_EQ(result, PropertyValue(1));
	ASSERT_TRUE(trace.contains("native_graph.read.adjacency_topology_scan_calls"));
	EXPECT_GT(trace.at("native_graph.read.adjacency_topology_scan_calls").totalValue, 2);
	EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
}

TEST_F(RelocatableStorageBackendTest, OverlayReferenceAdmissionPreservesCreatedDeletedOrderAndReplay) {
	const auto fixture = materializeNativeGraphFixture();
	auto config = backendOptions();
	config.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(config);
	backend.open();
	for (const bool vectorSnapshot: {false, true}) {
		SCOPED_TRACE(vectorSnapshot);
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		auto snapshot = vectorSnapshot
			? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
			: overlay.openLogicalGraphReadSnapshot();
		ASSERT_TRUE(snapshot->supportsAdjacencyReferenceBufferAdmission());
		const auto read = [&](std::optional<LogicalGraphAdjacencyContinuation> continuation,
			const std::function<void(size_t)> &admission) {
			return snapshot->scanAdjacentEdgeRefsWithBufferAdmission(fixture.person_node_id,
				LogicalGraphReadDirection::LGRD_OUTGOING, continuation, 1U, admission);
		};
		EXPECT_EQ(read({}, [](size_t) {}).edges.front().entity_id, fixture.knows_edge_id);
		const auto created = overlay.createLogicalEdges({
			{fixture.person_node_id, fixture.person_node_id, "MENTIONS", {}},
			{fixture.person_node_id, fixture.person_node_id, "KNOWS", {}}});
		ASSERT_EQ(created.size(), 2U);
		overlay.deleteLogicalEdges({{fixture.knows_edge_id}});
		size_t retained = 0U;
		size_t peak = 0U;
		size_t calls = 0U;
		const auto admit = [&](size_t bytes) { ++calls; retained = bytes; peak = std::max(peak, bytes); };
		const auto first = read({}, admit);
		ASSERT_EQ(first.edges.size(), 1U);
		EXPECT_EQ(first.edges.front().entity_id, fixture.mentions_edge_id);
		EXPECT_EQ(retained, first.edges.capacity() * sizeof(EntityRef));
		EXPECT_GT(peak, retained);
		ASSERT_TRUE(first.continuation);
		const auto tail = read(first.continuation, admit);
		ASSERT_EQ(tail.edges.size(), 1U);
		EXPECT_EQ(tail.edges.front().entity_id, created[0]);
		ASSERT_TRUE(tail.continuation);
		const auto last = read(tail.continuation, admit);
		ASSERT_EQ(last.edges.size(), 1U);
		EXPECT_EQ(last.edges.front().entity_id, created[1]);
		EXPECT_FALSE(last.continuation);
		EXPECT_NE(last.edges.front().type_token, tail.edges.front().type_token);
		calls = 0U;
		static_cast<void>(read(first.continuation, admit));
		const size_t successfulCalls = calls;
		ASSERT_GT(successfulCalls, 2U);
		for (const size_t rejectAt: {size_t{1U}, successfulCalls - 1U, successfulCalls}) {
			size_t attemptCalls = 0U;
			EXPECT_THROW(static_cast<void>(read(first.continuation, [&](size_t) {
				if (++attemptCalls == rejectAt) throw std::runtime_error("overlay reference rejected");
			})), std::runtime_error);
			const auto retry = read(first.continuation, admit);
			ASSERT_EQ(retry.edges.size(), 1U);
			EXPECT_EQ(retry.edges.front().entity_id, tail.edges.front().entity_id);
			EXPECT_EQ(retry.edges.front().type_token, tail.edges.front().type_token);
			EXPECT_EQ(retained, retry.edges.capacity() * sizeof(EntityRef));
		}
		const auto all = snapshot->scanAdjacentEdgeRefsWithBufferAdmission(fixture.person_node_id,
			LogicalGraphReadDirection::LGRD_OUTGOING, {}, 8U, admit);
		ASSERT_EQ(all.edges.size(), 3U);
		EXPECT_EQ(all.edges[0].entity_id, fixture.mentions_edge_id);
		EXPECT_EQ(all.edges[1].entity_id, created[0]);
		EXPECT_EQ(all.edges[2].entity_id, created[1]);
		EXPECT_EQ(retained, all.edges.capacity() * sizeof(EntityRef));
		transaction->rollback();
	}
}

TEST_F(RelocatableStorageBackendTest, OverlayReferenceCpuCancellationAllowsContinuationRetry) {
	const auto fixture = materializeNativeGraphFixture();
	auto config = backendOptions();
	config.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(config);
	backend.open();
	auto transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphTransactionOverlay overlay(*transaction);
	std::vector<LogicalGraphEdgeCreateRequest> requests;
	for (size_t index = 0U; index < 600U; ++index) {
		requests.push_back({fixture.person_node_id, fixture.person_node_id,
			index % 2U ? "KNOWS" : "MENTIONS", {}});
	}
	const auto created = overlay.createLogicalEdges(requests);
	ASSERT_EQ(created.size(), requests.size());
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const auto first = snapshot->scanAdjacentEdgeRefsWithBufferAdmission(fixture.person_node_id,
		LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U, [](size_t) {});
	ASSERT_TRUE(first.continuation);
	const auto topology = snapshot->scanAdjacentEdgeTopologiesWithBufferAdmission(fixture.person_node_id,
		LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 1024U, [](size_t) {});
	size_t workingBytes = topology.edges.capacity() * sizeof(LogicalGraphEdgeTopology);
	for (const auto &edge: topology.edges) workingBytes += edge.type.capacity();
	workingBytes += topology.edges.size() * (sizeof(EntityRef) + sizeof(size_t));
	const auto read = [&](const std::function<void(size_t)> &admission) {
		return snapshot->scanAdjacentEdgeRefsWithBufferAdmission(fixture.person_node_id,
			LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 1024U, admission);
	};
	size_t cpuChecks = 0U;
	const auto expected = read([&](size_t bytes) { if (bytes == workingBytes) ++cpuChecks; });
	ASSERT_EQ(expected.edges.size(), 601U);
	ASSERT_GT(cpuChecks, 8U);
	// Fifth check is within sort comparisons; the penultimate CPU check is
	// within ordinal scatter. Both follow successful source allocation.
	for (const size_t rejectAt: {size_t{5U}, cpuChecks - 1U}) {
		size_t checks = 0U;
		EXPECT_THROW(static_cast<void>(read([&](size_t bytes) {
			if (bytes == workingBytes && ++checks == rejectAt)
				throw std::runtime_error("cancelled during reference CPU work");
		})), std::runtime_error);
		EXPECT_EQ(checks, rejectAt);
		const auto retry = read([](size_t) {});
		ASSERT_EQ(retry.edges.size(), expected.edges.size());
		EXPECT_FALSE(retry.continuation);
		for (size_t index = 0U; index < retry.edges.size(); ++index) {
			EXPECT_EQ(retry.edges[index].entity_id, expected.edges[index].entity_id);
			EXPECT_EQ(retry.edges[index].type_token, expected.edges[index].type_token);
		}
	}
	transaction->rollback();
}

TEST_F(RelocatableStorageBackendTest, PatternLabelFilteringRetainsCrossPageOrderDuplicatesAndBudgetRetry) {
	using namespace graph::query;
	using namespace graph::query::execution;
	const auto fixture = materializeNativeGraphFixture();
	auto config = backendOptions();
	config.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(config);
	backend.open();
	std::vector<LogicalGraphEdgeCreateRequest> requests;
	for (size_t index = 0U; index < 8U; ++index)
		requests.push_back({fixture.person_node_id,
			index % 2U ? fixture.person_node_id : fixture.unlabeled_node_id, "KNOWS", {}});
	const auto created = backend.createLogicalEdges(requests);
	ASSERT_EQ(created.size(), requests.size());
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	LogicalPlanReadRow row;
	row.values.emplace("a", EntityRef{fixture.person_node_id, false, 0U, 0U, 0U});
	std::vector<std::unique_ptr<expressions::Expression>> arguments;
	arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("r"));
	expressions::PatternComprehensionExpression expression("(a)-[r]->(m)", "a", "m", "", "Person",
		expressions::PatternDirection::PAT_OUTGOING,
		std::make_unique<expressions::FunctionCallExpression>("id", std::move(arguments)), nullptr, "r");
	LogicalPlanReadRuntimeBounds bounds{.maximum_node_records = 16U,
		.maximum_node_page_records = 2U, .maximum_metadata_pages_per_node_page = 8U,
		.maximum_edges_per_node = 16U, .maximum_edge_records = 16U,
		.maximum_adjacency_page_records = 2U, .maximum_property_entries = 8U,
		.maximum_rows = 16U, .maximum_distinct_rows = 16U, .maximum_materialized_bytes = 64U};
	const auto evaluate = [&] {
		return std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			*snapshot, expression, row, bounds, QueryContext{}));
	};
	EXPECT_THROW(static_cast<void>(evaluate()), std::runtime_error);
	bounds.maximum_materialized_bytes = 16384U;
	const auto result = evaluate();
	const auto &values = result.getList();
	ASSERT_EQ(values.size(), 6U);
	EXPECT_EQ(values[0], PropertyValue(static_cast<int64_t>(fixture.knows_edge_id)));
	EXPECT_EQ(values[1], PropertyValue(static_cast<int64_t>(fixture.mentions_edge_id)));
	for (size_t index = 0U; index < 4U; ++index)
		EXPECT_EQ(values[index + 2U], PropertyValue(static_cast<int64_t>(created[index * 2U + 1U])));
	EXPECT_EQ(evaluate(), result);
}

TEST_F(RelocatableStorageBackendTest, BatchReferenceAdmissionRetainsSourceDuringVisitorAndRetriesRejection) {
	const auto fixture = materializeNativeGraphFixture();
	auto config = backendOptions(); config.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(config); backend.open();
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_TRUE(snapshot->supportsBatchAdjacencyReferenceBufferAdmission());
	const std::array<uint64_t, 1U> sources{fixture.person_node_id};
	size_t delivered = 0U;
	EXPECT_THROW(snapshot->visitAdjacentEdgeRefsForNodesWithBufferAdmission(sources,
		LogicalGraphReadDirection::LGRD_OUTGOING, 8U,
		[&](const EntityRef &) { ++delivered; return true; },
		[](size_t) { throw std::runtime_error("source admission rejected"); }), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	size_t retained = 0U;
	std::vector<uint64_t> ids;
	snapshot->visitAdjacentEdgeRefsForNodesWithBufferAdmission(sources,
		LogicalGraphReadDirection::LGRD_OUTGOING, 8U,
		[&](const EntityRef &reference) {
			EXPECT_GT(retained, 0U); ids.push_back(reference.entity_id); return true;
		}, [&](size_t bytes) { retained = bytes; });
	EXPECT_EQ(ids, (std::vector<uint64_t>{fixture.knows_edge_id, fixture.mentions_edge_id}));
	EXPECT_EQ(retained, 0U);
}

TEST_F(RelocatableStorageBackendTest, BatchReferenceAdmissionAllocatesForActualEdgesRatherThanConfiguredLimit) {
	const auto fixture = materializeNativeGraphFixture();
	auto config = backendOptions(); config.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(config); backend.open();
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const std::array<uint64_t, 1U> sources{fixture.person_node_id};
	std::vector<uint64_t> ids;
	size_t peak = 0U;
	snapshot->visitAdjacentEdgeRefsForNodesWithBufferAdmission(sources,
		LogicalGraphReadDirection::LGRD_OUTGOING, 4096U,
		[&](const EntityRef &reference) { ids.push_back(reference.entity_id); return true; },
		[&](size_t bytes) {
			peak = std::max(peak, bytes);
			if (bytes > 4096U) throw std::runtime_error("source exceeded allowance");
		});
	EXPECT_EQ(ids, (std::vector<uint64_t>{fixture.knows_edge_id, fixture.mentions_edge_id}));
	EXPECT_GT(peak, 0U); EXPECT_LE(peak, 4096U);
}

TEST_F(RelocatableStorageBackendTest, NativeTwoHopCountPreservesParallelPathsAfterBudgetFailure) {
	using namespace graph::query;
	using namespace graph::query::execution;
	static_cast<void>(materializeNativeGraphFixture());
	auto config = backendOptions(); config.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(config); backend.open();
	const auto nodes = backend.createLogicalNodes({{{"BudgetSeed"}, {}}, {{"BudgetMid"}, {}}, {{"BudgetEnd"}, {}}});
	ASSERT_EQ(nodes.size(), 3U);
	std::vector<LogicalGraphEdgeCreateRequest> edges(100U, {nodes[0], nodes[1], "BUDGET_HOP", {}});
	edges.push_back({nodes[1], nodes[2], "BUDGET_HOP", {}});
	ASSERT_EQ(backend.createLogicalEdges(edges).size(), 101U);
	auto inner = std::make_unique<logical::LogicalTraversal>(
		std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"BudgetSeed"}),
		"a", "r", "b", "BUDGET_HOP", "out", std::vector<std::string>{"BudgetMid"});
	auto outer = std::make_unique<logical::LogicalTraversal>(std::move(inner), "b", "s", "c", "BUDGET_HOP", "out",
		std::vector<std::string>{"BudgetEnd"});
	logical::LogicalAggregate aggregate(std::move(outer), {},
		{{"count", std::make_shared<expressions::VariableReferenceExpression>("c"), "count", false}});
	LogicalPlanReadRuntimeBounds bounds{.maximum_node_records = 16U,
		.maximum_node_page_records = 2U, .maximum_metadata_pages_per_node_page = 8U,
		.maximum_edges_per_node = 128U, .maximum_edge_records = 128U,
		.maximum_adjacency_page_records = 128U, .maximum_property_entries = 8U,
		.maximum_rows = 128U, .maximum_distinct_rows = 128U,
		.maximum_materialized_bytes = 512U};
	LogicalPlanReadRuntime runtime(backend);
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.execute(aggregate, bounds,
		[&](const LogicalPlanReadRow &) { ++delivered; return true; })), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	bounds.maximum_materialized_bytes = 32768U;
	static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &row) {
		++delivered; EXPECT_EQ(std::get<PropertyValue>(row.values.at("count")), PropertyValue(100)); return true;
	}));
	EXPECT_EQ(delivered, 1U);
}

TEST_F(RelocatableStorageBackendTest, BatchReferencePressureFlushesWithoutReplayingDeliveredEdges) {
	const auto fixture = materializeNativeGraphFixture();
	auto config = backendOptions(); config.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(config); backend.open();
	std::vector<LogicalGraphEdgeCreateRequest> edges(100U,
		{fixture.person_node_id, fixture.unlabeled_node_id, "KNOWS", {}});
	ASSERT_EQ(backend.createLogicalEdges(edges).size(), 100U);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const std::array<uint64_t, 1U> sources{fixture.person_node_id};
	std::vector<uint64_t> ids;
	size_t pressures = 0U;
	snapshot->visitAdjacentEdgeRefsForNodesWithBufferAdmission(sources,
		LogicalGraphReadDirection::LGRD_OUTGOING, 256U,
		[&](const EntityRef &reference) { ids.push_back(reference.entity_id); return true; },
		[&](size_t bytes) {
			if (bytes > 4096U) { ++pressures; throw LogicalGraphBufferPressure(); }
		});
	EXPECT_GT(pressures, 0U);
	ASSERT_EQ(ids.size(), 102U);
	EXPECT_TRUE(std::ranges::is_sorted(ids));
	EXPECT_EQ(std::ranges::adjacent_find(ids), ids.end());
}

TEST_F(RelocatableStorageBackendTest, BatchReferenceVisitorFailuresAreNotRetriedByPressureFlush) {
	const auto fixture = materializeNativeGraphFixture();
	auto config = backendOptions(); config.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(config); backend.open();
	std::vector<LogicalGraphEdgeCreateRequest> edges(100U,
		{fixture.person_node_id, fixture.unlabeled_node_id, "KNOWS", {}});
	ASSERT_EQ(backend.createLogicalEdges(edges).size(), 100U);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const std::array<uint64_t, 1U> sources{fixture.person_node_id};
	const auto admission = [](size_t bytes) {
		if (bytes > 4096U) throw LogicalGraphBufferPressure();
	};
	for (bool typedPressure : {false, true}) {
		size_t delivered = 0U;
		EXPECT_THROW(snapshot->visitAdjacentEdgeRefsForNodesWithBufferAdmission(sources,
			LogicalGraphReadDirection::LGRD_OUTGOING, 256U,
			[&](const EntityRef &) -> bool {
				++delivered;
				if (typedPressure) throw LogicalGraphBufferPressure();
				throw std::runtime_error("visitor failed");
			}, admission), std::runtime_error);
		EXPECT_EQ(delivered, 1U);
		std::vector<uint64_t> ids;
		snapshot->visitAdjacentEdgeRefsForNodesWithBufferAdmission(sources,
			LogicalGraphReadDirection::LGRD_OUTGOING, 256U,
			[&](const EntityRef &reference) { ids.push_back(reference.entity_id); return true; }, admission);
		ASSERT_EQ(ids.size(), 102U);
		EXPECT_TRUE(std::ranges::is_sorted(ids));
		EXPECT_EQ(std::ranges::adjacent_find(ids), ids.end());
	}
}

TEST_F(RelocatableStorageBackendTest, CompositeRangeBoundsLargeValueBuffersAndRetriesCachedCounts) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto ids = backend.createLogicalNodes({{{"LargeCompositeBuffers", "ExtraLabelA", "ExtraLabelB", "ExtraLabelC"},
		{{"tenant", PropertyValue("a")}, {"rank", PropertyValue(int64_t{7})},
		 {"payload", PropertyValue(std::string(16000U, 'p'))}}}});
	ASSERT_EQ(ids.size(), 1U);
	static_cast<void>(backend.createLogicalNodeIndex({"large_composite_buffers", "LargeCompositeBuffers", {"tenant", "rank"}}));
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphCompositePropertyRange range{{{"tenant", PropertyValue("a")}},
		{"rank", PropertyValue(int64_t{7}), PropertyValue(), true, true}};
	for (int retry = 0; retry < 2; ++retry) {
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByCompositePropertyRange({"LargeCompositeBuffers"}, range,
			8U, 4U, 4096U, [&](const IndexCandidateBatch &) { ++delivered; return true; })), FormatException);
		EXPECT_EQ(delivered, 0U);
		std::vector<uint64_t> found;
		EXPECT_NO_THROW(static_cast<void>(snapshot->visitNodeCandidatesByCompositePropertyRange({"LargeCompositeBuffers"}, range,
			8U, 4U, 65536U, [&](const IndexCandidateBatch &batch) {
				for (const auto &candidate : batch.candidates) found.push_back(candidate.entity_id);
				return true;
			})));
		EXPECT_EQ(found, ids);
	}
	snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, ExactCandidatesShareLargeValueBufferBudgetForScalarAndCompositeIndexes) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	for (bool composite : {false, true}) {
		const std::string label = composite ? "LargeExactCompositeBuffers" : "LargeExactScalarBuffers";
		const auto ids = backend.createLogicalNodes({{{label},
			{{"tenant", PropertyValue("a")}, {"rank", PropertyValue(int64_t{7})},
			 {"payload", PropertyValue(std::string(16000U, 'p'))}}}});
		const std::vector<std::string> fields = composite ? std::vector<std::string>{"tenant", "rank"} : std::vector<std::string>{"rank"};
		static_cast<void>(backend.createLogicalNodeIndex({label, label, fields}));
		auto snapshot = backend.openLogicalGraphReadSnapshot();
		std::vector<std::pair<std::string, PropertyValue>> predicates{{"rank", PropertyValue(int64_t{7})}};
		if (composite) predicates.push_back({"tenant", PropertyValue("a")});
		for (int retry = 0; retry < 2; ++retry) {
			size_t delivered = 0U;
			EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByExactProperties({label}, predicates,
				8U, 4U, 4096U, [&](const IndexCandidateBatch &) { ++delivered; return true; })), FormatException);
			EXPECT_EQ(delivered, 0U);
			std::vector<uint64_t> found;
			EXPECT_NO_THROW(static_cast<void>(snapshot->visitNodeCandidatesByExactProperties({label}, predicates,
				8U, 4U, 65536U, [&](const IndexCandidateBatch &batch) {
					for (const auto &candidate : batch.candidates) found.push_back(candidate.entity_id);
					return true;
				})));
			EXPECT_EQ(found, ids);
		}
		snapshot.reset();
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ScalarAndCompositeRangesBudgetOverflowLabelsWithSmallProperties) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto ids = backend.createLogicalNodes({{{"RangeLabelBudget", "ExtraA", "ExtraB", "ExtraC"},
		{{"tenant", PropertyValue("a")}, {"rank", PropertyValue(int64_t{7})}}}});
	ASSERT_EQ(ids.size(), 1U);
	static_cast<void>(backend.createLogicalNodeIndex({"range_label_scalar", "RangeLabelBudget", {"rank"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"range_label_composite", "RangeLabelBudget", {"tenant", "rank"}}));
	const LogicalGraphPropertyRange range{"rank", PropertyValue(int64_t{7}), PropertyValue(), true, true};
	const LogicalGraphCompositePropertyRange composite{{{"tenant", PropertyValue("a")}}, range};
	for (const bool useComposite: {false, true}) {
		for (int retry = 0; retry < 2; ++retry) {
			// A fresh snapshot/session prevents a prior successful read from hiding
			// the overflow page behind its validated label cache.
			auto snapshot = backend.openLogicalGraphReadSnapshot();
			size_t delivered = 0U;
			const auto visitor = [&](const IndexCandidateBatch &) { ++delivered; return true; };
			if (useComposite) {
				EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByCompositePropertyRange(
					{"RangeLabelBudget"}, composite, 8U, 4U, 2048U, visitor)), FormatException);
			} else {
				EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByPropertyRange(
					{"RangeLabelBudget"}, range, 8U, 4U, 2048U, visitor)), FormatException);
			}
			EXPECT_EQ(delivered, 0U);
			std::vector<uint64_t> found;
			const auto collect = [&](const IndexCandidateBatch &batch) {
				for (const auto &candidate: batch.candidates) found.push_back(candidate.entity_id);
				return true;
			};
			if (useComposite) {
				EXPECT_NO_THROW(static_cast<void>(snapshot->visitNodeCandidatesByCompositePropertyRange(
					{"RangeLabelBudget"}, composite, 8U, 4U, 65536U, collect)));
			} else {
				EXPECT_NO_THROW(static_cast<void>(snapshot->visitNodeCandidatesByPropertyRange(
					{"RangeLabelBudget"}, range, 8U, 4U, 65536U, collect)));
			}
			EXPECT_EQ(found, ids);
		}
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, RangeRejectsStaleIndexedLabelsBeforePropertyReads) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	uint64_t id = 0U;
	{
		RelocatableStorageBackend backend(options); backend.open();
		const auto ids = backend.createLogicalNodes({{{"StaleRangeLabel"}, {{"rank", PropertyValue(int64_t{7})}, {"tenant", PropertyValue("a")}}}});
		ASSERT_EQ(ids.size(), 1U); id = ids[0];
		static_cast<void>(backend.createLogicalNodeIndex({"stale_range_label", "StaleRangeLabel", {"rank"}}));
		static_cast<void>(backend.createLogicalNodeIndex({"stale_range_composite", "StaleRangeLabel", {"tenant", "rank"}}));
		backend.close();
	}
	{
		auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(file->is_open());
		auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		SlottedPagePool pages(io, *metadata); EntityLabelSetStore labels(io, *metadata, pages);
		auto pin = metadata->openSnapshot();
		static_cast<void>(labels.updateNodeLabels(pin, id, std::span<const uint64_t>{}, {},
			LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1U)));
	}
	RelocatableStorageBackend backend(options); backend.open();
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const LogicalGraphPropertyRange range{"rank", PropertyValue(int64_t{7}), PropertyValue(), true, true};
	const LogicalGraphCompositePropertyRange composite{{{"tenant", PropertyValue("a")}}, range};
	for (const bool useComposite: {false, true}) {
		for (int retry = 0; retry < 2; ++retry) {
			graph::debug::PerfTrace::reset();
			size_t delivered = 0U;
			const auto visitor = [&](const IndexCandidateBatch &) { ++delivered; return true; };
			if (useComposite) {
				EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByCompositePropertyRange(
					{"StaleRangeLabel"}, composite, 8U, 4U, 65536U, visitor)), FormatException);
			} else {
				EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByPropertyRange(
					{"StaleRangeLabel"}, range, 8U, 4U, 65536U, visitor)), FormatException);
			}
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			EXPECT_EQ(delivered, 0U);
			EXPECT_FALSE(trace.contains("native_graph.read.property_combined_batch_owners"));
		}
	}
	graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
	snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, ScalarRangeCompleteCandidatesKeepColumnsAlignedAcrossPermutationCycles) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const std::array<int64_t, 6U> scores{4, 1, 6, 2, 5, 3};
	std::vector<LogicalGraphNodeCreateRequest> requests;
	for (const auto score: scores) requests.push_back({{"RangeCycles"}, {{"score", PropertyValue(score)}}});
	const auto nodes = backend.createLogicalNodes(requests);
	static_cast<void>(backend.createLogicalNodeIndex({"range_cycles", "RangeCycles", {"score"}}));
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	for (size_t retry = 0U; retry < 2U; ++retry) {
		for (int64_t minimum: {1, 3, 7}) {
			const auto result = snapshot->findNodeCandidatesByPropertyRange({"RangeCycles"},
				{"score", PropertyValue(minimum), PropertyValue(int64_t{7}), true, true}, 8U, 1U);
			ASSERT_TRUE(result.has_value());
			size_t row = 0U;
			for (size_t index = 0U; index < scores.size(); ++index) {
				if (scores[index] < minimum) continue;
				ASSERT_LT(row, result->candidates.size());
				ASSERT_EQ(result->verified_property_columns.size(), 1U);
				ASSERT_LT(row, result->verified_property_columns.front().values.size());
				EXPECT_EQ(result->candidates[row].entity_id, nodes[index]);
				EXPECT_EQ(result->verified_property_columns.front().values[row], PropertyValue(scores[index]));
				++row;
			}
			EXPECT_EQ(result->candidates.size(), row);
		}
	}
	snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, RangeDedupSharesByteBudgetAcrossBatchesAndRetriesAfterRejection) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	for (int64_t rank = 0; rank < 512; ++rank) {
		requests.push_back({{"RangeDedup"}, {{"tenant", PropertyValue("a")}, {"rank", PropertyValue(rank)}}});
	}
	const auto nodes = backend.createLogicalNodes(requests);
	static_cast<void>(backend.createLogicalNodeIndex({"range_dedup_scalar", "RangeDedup", {"rank"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"range_dedup_composite", "RangeDedup", {"tenant", "rank"}}));
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphPropertyRange range{"rank", PropertyValue(int64_t{0}), PropertyValue(int64_t{511}), true, true};
	const auto visit = [&](bool composite, size_t bytes, const IndexCandidateBatchVisitor &visitor) {
		return composite
			? snapshot->visitNodeCandidatesByCompositePropertyRange({"RangeDedup"}, {{{"tenant", PropertyValue("a")}}, range},
				512U, 2U, bytes, visitor)
			: snapshot->visitNodeCandidatesByPropertyRange({"RangeDedup"}, range, 512U, 2U, bytes, visitor);
	};
	for (bool composite: {false, true}) {
		for (size_t retry = 0U; retry < 2U; ++retry) {
			size_t delivered = 0U;
			EXPECT_THROW(static_cast<void>(visit(composite, 16U * 1024U, [&](const IndexCandidateBatch &batch) {
				delivered += batch.candidates.size(); return true;
			})), FormatException);
			EXPECT_GT(delivered, 0U);
			EXPECT_LT(delivered, nodes.size());
			std::vector<uint64_t> actual;
			EXPECT_NO_THROW(static_cast<void>(visit(composite, 256U * 1024U, [&](const IndexCandidateBatch &batch) {
				for (const auto &candidate: batch.candidates) actual.push_back(candidate.entity_id);
				return true;
			})));
			EXPECT_EQ(actual, nodes);
			delivered = 0U;
			EXPECT_NO_THROW(static_cast<void>(visit(composite, 16U * 1024U, [&](const IndexCandidateBatch &batch) {
				delivered += batch.candidates.size(); return false;
			})));
			EXPECT_GT(delivered, 0U);
			EXPECT_LT(delivered, nodes.size());
		}
	}
	snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, RangeLowBudgetRejectsBeforeDeliveryAndAllowsRetry) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const std::string key(1024U, 'k');
	const auto nodes = backend.createLogicalNodes({{{"OutputAdmission"},
		{{"tenant", PropertyValue("a")}, {key, PropertyValue(int64_t{7})}}}});
	static_cast<void>(backend.createLogicalNodeIndex({"output_scalar", "OutputAdmission", {key}}));
	static_cast<void>(backend.createLogicalNodeIndex({"output_composite", "OutputAdmission", {"tenant", key}}));
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphPropertyRange range{key, PropertyValue(int64_t{7}), PropertyValue(), true, true};
	const auto visit = [&](bool composite, size_t bytes, const IndexCandidateBatchVisitor &visitor) {
		return composite
			? snapshot->visitNodeCandidatesByCompositePropertyRange({"OutputAdmission"}, {{{"tenant", PropertyValue("a")}}, range},
				8U, 2U, bytes, visitor)
			: snapshot->visitNodeCandidatesByPropertyRange({"OutputAdmission"}, range, 8U, 2U, bytes, visitor);
	};
	for (bool composite: {false, true}) {
		for (size_t retry = 0U; retry < 2U; ++retry) {
			EXPECT_NO_THROW(static_cast<void>(visit(composite, 64U * 1024U, [](const IndexCandidateBatch &) { return true; })));
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
			size_t delivered = 0U;
			EXPECT_THROW(static_cast<void>(visit(composite, 1024U, [&](const IndexCandidateBatch &) {
				++delivered; return true;
			})), FormatException);
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			EXPECT_EQ(delivered, 0U);
			// The 1 KiB limit can reject live source buffers before payload validation.
			EXPECT_TRUE(trace.contains("native_graph.read.index_candidate_root_descent"));
			EXPECT_NO_THROW(static_cast<void>(visit(composite, 64U * 1024U, [&](const IndexCandidateBatch &batch) {
				EXPECT_EQ(batch.candidates.front().entity_id, nodes.front());
				EXPECT_EQ(batch.verified_property_columns.back().key, key);
				EXPECT_EQ(batch.verified_property_columns.back().values.front(), PropertyValue(int64_t{7}));
				++delivered; return false;
			})));
			EXPECT_EQ(delivered, 1U);
		}
	}
	snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, RangeBoundsPreserveDuplicateValuesAndEscapedCompositePrefixes) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const PropertyValue tenant(std::string(256U, 'a') + std::string("\0z", 2U));
	const std::array<int64_t, 4U> scores{3, 1, 2, 2};
	std::vector<LogicalGraphNodeCreateRequest> requests;
	for (auto score: scores) requests.push_back({{"RangeBounds"}, {{"tenant", tenant}, {"score", PropertyValue(score)}}});
	const auto nodes = backend.createLogicalNodes(requests);
	static_cast<void>(backend.createLogicalNodeIndex({"bounds_scalar", "RangeBounds", {"score"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"bounds_composite", "RangeBounds", {"tenant", "score"}}));
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	for (size_t retry = 0U; retry < 2U; ++retry) {
		for (bool lowerInclusive: {false, true}) {
			for (bool upperInclusive: {false, true}) {
				const LogicalGraphPropertyRange range{"score", PropertyValue(1), PropertyValue(3), lowerInclusive, upperInclusive};
				for (bool composite: {false, true}) {
					const auto result = composite
						? snapshot->findNodeCandidatesByCompositePropertyRange({"RangeBounds"}, {{{"tenant", tenant}}, range}, 8U, 2U)
						: snapshot->findNodeCandidatesByPropertyRange({"RangeBounds"}, range, 8U, 2U);
					ASSERT_TRUE(result.has_value());
					ASSERT_EQ(result->verified_property_columns.size(), composite ? 2U : 1U);
					size_t row = 0U;
					for (size_t index = 0U; index < scores.size(); ++index) {
						if ((!lowerInclusive && scores[index] == 1) || (!upperInclusive && scores[index] == 3)) continue;
						ASSERT_LT(row, result->candidates.size());
						ASSERT_LT(row, result->verified_property_columns.back().values.size());
						EXPECT_EQ(result->candidates[row].entity_id, nodes[index]);
						EXPECT_EQ(result->verified_property_columns.back().values[row], PropertyValue(scores[index]));
						if (composite) EXPECT_EQ(result->verified_property_columns.front().values[row], tenant);
						++row;
					}
					EXPECT_EQ(result->candidates.size(), row);
				}
			}
		}
	}
	snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, OverlayExactLookupHydratesOnlyUnchangedBaseMatches) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto nodes = backend.createLogicalNodes({
		{{"OverlayExact"}, {{"id", PropertyValue("wanted")}, {"payload", PropertyValue(std::string(16000U, 'a'))}}},
		{{"OverlayExact"}, {{"id", PropertyValue("wanted")}, {"payload", PropertyValue(std::string(16000U, 'b'))}}},
		{{"OverlayExact"}, {{"id", PropertyValue("wanted")}, {"payload", PropertyValue(std::string(16000U, 'c'))}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend.createLogicalNodeIndex({"overlay_exact_id", "OverlayExact", {"id"}}));
	for (const bool vectorSnapshot: {false, true}) {
		SCOPED_TRACE(vectorSnapshot);
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		auto snapshot = vectorSnapshot
			? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
			: overlay.openLogicalGraphReadSnapshot();
		overlay.replaceLogicalNodeProperties({{nodes[0], {{"id", PropertyValue("changed")}}}});
		overlay.deleteLogicalNodes({{nodes[1], false, 0U}});
		const auto created = overlay.createLogicalNodes({{{"OverlayExact"}, {{"id", PropertyValue("wanted")}}}}).front();
		const auto find = [&](std::string_view value) {
			return snapshot->findNodesByExactProperties({"OverlayExact"},
				{{"id", PropertyValue(std::string(value))}}, 8U, 8U);
		};
		for (size_t retry = 0U; retry < 2U; ++retry) {
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
			const auto result = find("wanted");
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			ASSERT_TRUE(result);
			ASSERT_EQ(result->size(), 2U);
			EXPECT_EQ((*result)[0].node_id, nodes[2]);
			EXPECT_EQ((*result)[1].node_id, created);
			const auto payload = std::ranges::find((*result)[0].properties, "payload", &LogicalGraphPropertyEntry::key);
			ASSERT_NE(payload, (*result)[0].properties.end());
			EXPECT_EQ(payload->value, PropertyValue(std::string(16000U, 'c')));
			ASSERT_TRUE(trace.contains("native_graph.read.property_full_decode"));
			EXPECT_EQ(trace.at("native_graph.read.property_full_decode").totalValue, 1);
			const auto changed = find("changed");
			ASSERT_TRUE(changed); ASSERT_EQ(changed->size(), 1U);
			EXPECT_EQ(changed->front().node_id, nodes[0]);
			const auto missing = find("missing");
			ASSERT_TRUE(missing); EXPECT_TRUE(missing->empty());
		}
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, OverlayExactStreamPreservesFinalStateBudgetStopAndRevision) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto nodes = backend.createLogicalNodes({
		{{"OverlayStream"}, {{"id", PropertyValue("wanted")}, {"payload", PropertyValue(std::string(16000U, 'a'))}}},
		{{"OverlayStream"}, {{"id", PropertyValue("wanted")}, {"payload", PropertyValue(std::string(16000U, 'b'))}}},
		{{"OverlayStream"}, {{"id", PropertyValue("other")}, {"payload", PropertyValue(std::string(16000U, 'c'))}}},
		{{"OverlayStream"}, {{"id", PropertyValue("wanted")}, {"payload", PropertyValue(std::string(16000U, 'd'))}}}});
	static_cast<void>(backend.createLogicalNodeIndex({"overlay_stream_id", "OverlayStream", {"id"}}));
	for (bool vectorSnapshot: {false, true}) {
		SCOPED_TRACE(vectorSnapshot);
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		auto snapshot = vectorSnapshot ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
			: overlay.openLogicalGraphReadSnapshot();
		ASSERT_TRUE(snapshot->supportsNativeExactNodeCandidates());
		const auto originalGeneration = snapshot->nativeSnapshotGeneration();
		overlay.replaceLogicalNodeProperties({{nodes[0], {{"id", PropertyValue("changed")}}},
			{nodes[2], {{"id", PropertyValue("wanted")}, {"payload", PropertyValue(std::string(16000U, 'x'))}}}});
		overlay.deleteLogicalNodes({{nodes[1], false, 0U}});
		const auto created = overlay.createLogicalNodes({{{"OverlayStream"}, {{"id", PropertyValue("wanted")},
			{"payload", PropertyValue(std::string(16000U, 'y'))}}}}).front();
		EXPECT_NE(originalGeneration, snapshot->nativeSnapshotGeneration());
		graph::query::execution::LogicalPlanReadRuntime runtime(overlay);
		graph::query::execution::LogicalPlanReadRuntimeBounds bounds{.maximum_node_records = 8U,
			.maximum_node_page_records = 4U, .maximum_metadata_pages_per_node_page = 8U,
			.maximum_edges_per_node = 8U, .maximum_edge_records = 8U,
			.maximum_adjacency_page_records = 4U, .maximum_property_entries = 8U,
			.maximum_rows = 8U, .maximum_distinct_rows = 8U, .maximum_materialized_bytes = 128U * 1024U};
		std::vector<graph::query::logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<graph::query::expressions::VariableReferenceExpression>("n", "id"), "id");
		graph::query::logical::LogicalProject project(
			std::make_unique<graph::query::logical::LogicalNodeScan>("n", std::vector<std::string>{"OverlayStream"},
				std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue("wanted")}}), std::move(items));
		const auto read = [&](size_t bytes, const IndexCandidateBatchVisitor &visitor) {
			return snapshot->visitNodeCandidatesByExactProperties({"OverlayStream"}, {{"id", PropertyValue("wanted")}}, 8U, 8U, bytes, visitor);
		};
		for (size_t retry = 0U; retry < 2U; ++retry) {
			std::vector<uint64_t> ids;
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
			const auto done = read(64U * 1024U, [&](const IndexCandidateBatch &batch) {
				EXPECT_EQ(batch.snapshot_generation, snapshot->nativeSnapshotGeneration());
				EXPECT_TRUE(batch.exact_match_verified && batch.predicates_verified && batch.label_scope_verified);
				if (batch.verified_property_columns.size() != 1U) throw std::runtime_error("expected one verified predicate column");
				EXPECT_EQ(batch.verified_property_columns.front().key, "id");
				for (size_t i = 0U; i < batch.candidates.size(); ++i) {
					ids.push_back(batch.candidates[i].entity_id);
					EXPECT_EQ(batch.verified_property_columns.front().values[i], PropertyValue("wanted"));
				}
				return true;
			});
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			ASSERT_EQ(done, std::optional<bool>{true});
			std::ranges::sort(ids);
			EXPECT_EQ(ids, (std::vector<uint64_t>{nodes[2], nodes[3], created}));
			EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
			graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
			const auto result = runtime.executeToQueryResult(project, bounds);
			const auto queryTrace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			ASSERT_EQ(result.rowCount(), 3U);
			for (const auto &row: result.getRows()) EXPECT_EQ(row.at("id").asPrimitive(), PropertyValue("wanted"));
			EXPECT_FALSE(queryTrace.contains("native_graph.read.property_full_decode"));
			EXPECT_TRUE(queryTrace.contains("logical_plan.index_candidate.materialized_bytes"));
			const auto complete = snapshot->findNodeCandidatesByExactProperties({"OverlayStream"},
				{{"id", PropertyValue("wanted")}}, 8U, 8U);
			ASSERT_TRUE(complete);
			ASSERT_EQ(complete->candidates.size(), 3U);
			EXPECT_EQ(complete->candidates[0].entity_id, nodes[2]);
			EXPECT_EQ(complete->candidates[1].entity_id, nodes[3]);
			EXPECT_EQ(complete->candidates[2].entity_id, created);
			for (const auto &value: complete->verified_property_columns.front().values) EXPECT_EQ(value, PropertyValue("wanted"));
			size_t calls = 0U;
			EXPECT_EQ(read(64U * 1024U, [&](const auto &) { ++calls; return false; }), std::optional<bool>{false});
			EXPECT_EQ(calls, 1U);
			calls = 0U;
			EXPECT_THROW(static_cast<void>(read(64U, [&](const auto &) { ++calls; return true; })), std::exception);
			EXPECT_EQ(calls, 0U);
		}
		const auto beforeMutation = snapshot->nativeSnapshotGeneration();
		EXPECT_THROW(static_cast<void>(read(64U * 1024U, [&](const auto &) {
			overlay.replaceLogicalNodeProperties({{created, {{"id", PropertyValue("changed")}}}});
			return true;
		})), std::runtime_error);
		EXPECT_NE(beforeMutation, snapshot->nativeSnapshotGeneration());
		std::vector<uint64_t> ids;
		EXPECT_EQ(read(64U * 1024U, [&](const auto &batch) {
			for (const auto &ref: batch.candidates) ids.push_back(ref.entity_id);
			return true;
		}), std::optional<bool>{true});
		std::ranges::sort(ids);
		EXPECT_EQ(ids, (std::vector<uint64_t>{nodes[2], nodes[3]}));
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, OverlayRangesPreserveBoundariesFinalStateAndDeferredProjection) {
	using namespace graph::query;
	using namespace graph::query::execution;
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	for (int rank: {4, 1, 2, 3, 2, 2, 1}) {
		requests.push_back({{"OverlayRanges"}, {{"tenant", PropertyValue(requests.size() == 4U ? "b" : "a")},
			{"rank", PropertyValue(rank)}, {"payload", PropertyValue(std::string(16000U, 'p'))}}});
	}
	const auto nodes = backend.createLogicalNodes(requests);
	static_cast<void>(backend.createLogicalNodeIndex({"overlay_ranges_scalar", "OverlayRanges", {"rank"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"overlay_ranges_composite", "OverlayRanges", {"tenant", "rank"}}));
	for (bool vectorSnapshot: {false, true}) {
		SCOPED_TRACE(vectorSnapshot);
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		auto snapshot = vectorSnapshot ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
			: overlay.openLogicalGraphReadSnapshot();
		ASSERT_TRUE(snapshot->supportsNativeRangeNodeCandidates());
		overlay.replaceLogicalNodeProperties({
			{nodes[0], {{"tenant", PropertyValue("a")}, {"rank", PropertyValue(2)}}},
			{nodes[1], {{"tenant", PropertyValue("a")}, {"rank", PropertyValue(5)}}},
			{nodes[4], {{"tenant", PropertyValue("a")}, {"rank", PropertyValue(2)}}}});
		overlay.deleteLogicalNodes({{nodes[2], false, 0U}});
		LogicalGraphMutationBatch labels;
		labels.node_label_replacements = {{{LogicalGraphMutationEntityKind::LGMK_NODE,
			LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodes[3]}, {"Other"}}};
		static_cast<void>(overlay.executeLogicalGraphMutationBatch(labels));
		const auto created = overlay.createLogicalNodes({
			{{"OverlayRanges"}, {{"tenant", PropertyValue("a")}, {"rank", PropertyValue(2)}, {"payload", PropertyValue(std::string(16000U, 'x'))}}},
			{{"OverlayRanges"}, {{"tenant", PropertyValue("a")}, {"rank", PropertyValue(3)}}},
			{{"OverlayRanges"}, {{"tenant", PropertyValue("b")}, {"rank", PropertyValue(2)}}},
			{{"OverlayRanges"}, {{"tenant", PropertyValue("a")}, {"rank", PropertyValue()}}},
			{{"OverlayRanges"}, {{"tenant", PropertyValue("a")}}}});
		LogicalPlanReadRuntime runtime(overlay);
		LogicalPlanReadRuntimeBounds bounds{.maximum_node_records = 16U, .maximum_node_page_records = 4U,
			.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U,
			.maximum_edge_records = 8U, .maximum_adjacency_page_records = 4U,
			.maximum_property_entries = 8U, .maximum_rows = 16U, .maximum_distinct_rows = 16U,
			.maximum_materialized_bytes = 256U * 1024U};
		for (bool composite: {false, true}) {
			SCOPED_TRACE(composite);
			for (bool lower: {false, true}) for (bool upper: {false, true}) {
				SCOPED_TRACE(lower);
				SCOPED_TRACE(upper);
				const LogicalGraphPropertyRange range{"rank", PropertyValue(1), PropertyValue(3), lower, upper};
				const LogicalGraphCompositePropertyRange request{{{"tenant", PropertyValue("a")}}, range};
				std::vector<uint64_t> expected{nodes[0], nodes[4], nodes[5], created[0]};
				if (!composite) expected.push_back(created[2]);
				if (lower) expected.push_back(nodes[6]);
				if (upper) expected.push_back(created[1]);
				std::ranges::sort(expected);
				const auto read = [&](size_t bytes, const IndexCandidateBatchVisitor &visitor) {
					return composite ? snapshot->visitNodeCandidatesByCompositePropertyRange({"OverlayRanges"}, request, 16U, 8U, bytes, visitor)
						: snapshot->visitNodeCandidatesByPropertyRange({"OverlayRanges"}, range, 16U, 8U, bytes, visitor);
				};
				for (size_t retry = 0U; retry < 2U; ++retry) {
					std::vector<uint64_t> ids;
					const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
					graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
					EXPECT_EQ(read(128U * 1024U, [&](const IndexCandidateBatch &batch) {
						EXPECT_FALSE(batch.exact_match_verified);
						EXPECT_TRUE(batch.predicates_verified && batch.label_scope_verified);
						EXPECT_EQ(batch.snapshot_generation, snapshot->nativeSnapshotGeneration());
						const auto ranks = std::ranges::find(batch.verified_property_columns, "rank", &IndexCandidateBatch::VerifiedPropertyColumn::key);
						if (ranks == batch.verified_property_columns.end()) throw std::runtime_error("missing rank column");
						for (size_t i = 0U; i < batch.candidates.size(); ++i) {
							ids.push_back(batch.candidates[i].entity_id);
							const int value = ids.back() == nodes[6] ? 1 : ids.back() == created[1] ? 3 : 2;
							EXPECT_EQ(ranks->values[i], PropertyValue(value));
						}
						return true;
					}), std::optional<bool>{true});
					const auto trace = graph::debug::PerfTrace::snapshotAndReset();
					graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
					std::ranges::sort(ids); EXPECT_EQ(ids, expected);
					EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
					const auto complete = composite ? snapshot->findNodeCandidatesByCompositePropertyRange({"OverlayRanges"}, request, 16U, 8U)
						: snapshot->findNodeCandidatesByPropertyRange({"OverlayRanges"}, range, 16U, 8U);
					ASSERT_TRUE(complete);
					ids.clear(); for (const auto &ref: complete->candidates) ids.push_back(ref.entity_id);
					EXPECT_EQ(ids, expected);
					const auto rankColumn = std::ranges::find(complete->verified_property_columns, "rank", &IndexCandidateBatch::VerifiedPropertyColumn::key);
					ASSERT_NE(rankColumn, complete->verified_property_columns.end());
					for (size_t i = 0U; i < ids.size(); ++i) {
						EXPECT_EQ(rankColumn->values[i], PropertyValue(ids[i] == nodes[6] ? 1 : ids[i] == created[1] ? 3 : 2));
					}
					size_t calls = 0U;
					EXPECT_EQ(read(128U * 1024U, [&](const auto &) { ++calls; return false; }), std::optional<bool>{false});
					EXPECT_EQ(calls, 1U);
					calls = 0U;
					EXPECT_THROW(static_cast<void>(read(64U, [&](const auto &) { ++calls; return true; })), std::exception);
					EXPECT_EQ(calls, 0U);
				}
				auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"OverlayRanges"});
				scan->setRangePredicates({{"rank", PropertyValue(1), PropertyValue(3), lower, upper}});
				if (composite) scan->setPropertyPredicates({{"tenant", PropertyValue("a")}});
				std::vector<logical::LogicalProjectItem> items;
				items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank");
				logical::LogicalProject project(std::move(scan), std::move(items));
				const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
				graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
				const auto result = runtime.executeToQueryResult(project, bounds);
				const auto trace = graph::debug::PerfTrace::snapshotAndReset();
				graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
				EXPECT_EQ(result.rowCount(), expected.size());
				EXPECT_TRUE(trace.contains("logical_plan.index_candidate.materialized_bytes"));
				EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
			}
			const LogicalGraphPropertyRange range{"rank", PropertyValue(1), PropertyValue(3), true, true};
			const auto read = [&](const IndexCandidateBatchVisitor &visitor) {
				return composite ? snapshot->visitNodeCandidatesByCompositePropertyRange({"OverlayRanges"},
					{{{"tenant", PropertyValue("a")}}, range}, 16U, 8U, 128U * 1024U, visitor)
					: snapshot->visitNodeCandidatesByPropertyRange({"OverlayRanges"}, range, 16U, 8U, 128U * 1024U, visitor);
			};
			EXPECT_THROW(static_cast<void>(read([&](const auto &) {
				overlay.replaceLogicalNodeProperties({{created[0], {{"tenant", PropertyValue("a")}, {"rank", PropertyValue(2)}}}});
				// Reentrant reads refresh a vector snapshot; the in-flight reader must stay alive until rejection.
				EXPECT_EQ(snapshot->readNodeShaped(created[0], {.all_properties = true}).node_id, created[0]);
				return true;
			})), std::runtime_error);
			EXPECT_EQ(read([](const auto &) { return true; }), std::optional<bool>{true});
		}
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, OverlayShapedScanFiltersDeletedAndReplacedRowsBeforeBaseHydration) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto nodes = backend.createLogicalNodes({
		{{"OverlayScan"}, {{"rank", PropertyValue(1)}, {"payload", PropertyValue(std::string(16000U, 'a'))}}},
		{{"OverlayScan"}, {{"rank", PropertyValue(2)}, {"payload", PropertyValue(std::string(16000U, 'b'))}}},
		{{"OverlayScan"}, {{"rank", PropertyValue(3)}, {"payload", PropertyValue(std::string(16000U, 'c'))}}}});
	for (bool vectorSnapshot: {false, true}) {
		SCOPED_TRACE(vectorSnapshot);
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		overlay.replaceLogicalNodeProperties({{nodes[0], {{"rank", PropertyValue(99)}}}});
		overlay.deleteLogicalNodes({{nodes[1], false, 0U}});
		const auto created = overlay.createLogicalNodes({{{"OverlayScan"},
			{{"rank", PropertyValue(4)}, {"payload", PropertyValue(std::string(16000U, 'd'))}}}}).front();
		auto snapshot = vectorSnapshot ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
			: overlay.openLogicalGraphReadSnapshot();
		const auto rankToken = snapshot->findPropertyToken("rank");
		ASSERT_TRUE(rankToken);
		const std::vector<EntityReadShape> shapes{{.property_tokens = {*rankToken}},
			{.include_labels = true, .all_properties = true}, {.include_labels = true}, {}};
		for (const auto &shape: shapes) {
			SCOPED_TRACE(shape.all_properties);
			for (size_t pageRecords: {1U, 8U}) for (size_t retry = 0U; retry < 2U; ++retry) {
				SCOPED_TRACE(pageRecords);
				std::vector<LogicalGraphNode> rows;
				uint64_t after = 0U;
				const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
				graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
				for (size_t pages = 0U;; ++pages) {
					ASSERT_LT(pages, 8U);
					const auto page = snapshot->scanNodesAfter(after, pageRecords, 8U, shape);
					EXPECT_LE(page.nodes.size(), pageRecords);
					EXPECT_LE(page.metadata_pages_visited, 8U);
					for (const auto &node: page.nodes) { EXPECT_GT(node.node_id, after); after = node.node_id; rows.push_back(node); }
					if (!page.may_have_more) break;
					ASSERT_FALSE(page.nodes.empty());
				}
				const auto trace = graph::debug::PerfTrace::snapshotAndReset();
				graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
				ASSERT_EQ(rows.size(), 3U);
				EXPECT_EQ(rows[0].node_id, nodes[0]); EXPECT_EQ(rows[1].node_id, nodes[2]); EXPECT_EQ(rows[2].node_id, created);
				for (size_t i = 0U; i < rows.size(); ++i) {
					EXPECT_EQ(rows[i].labels, shape.include_labels ? std::vector<std::string>{"OverlayScan"} : std::vector<std::string>{});
					if (!shape.all_properties && shape.property_tokens.empty()) { EXPECT_TRUE(rows[i].properties.empty()); continue; }
					const auto rank = std::ranges::find(rows[i].properties, "rank", &LogicalGraphPropertyEntry::key);
					ASSERT_NE(rank, rows[i].properties.end());
					EXPECT_EQ(rank->value, PropertyValue(i == 0U ? 99 : i == 1U ? 3 : 4));
					EXPECT_EQ(rows[i].properties.size(), shape.all_properties && i != 0U ? 2U : 1U);
				}
				if (shape.all_properties) {
					ASSERT_TRUE(trace.contains("native_graph.read.property_full_decode"));
					EXPECT_EQ(trace.at("native_graph.read.property_full_decode").totalValue, 1);
				} else {
					EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
				}
			}
		}
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ReversePruningUsesExactCandidatesWithoutHydratingUnrelatedTargetProperties) {
	using namespace graph::query;
	using namespace graph::query::execution;
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto nodes = backend.createLogicalNodes({{{"ReverseSource"}, {}},
		{{"ReverseTarget"}, {{"id", PropertyValue("wanted")}, {"payload", PropertyValue(std::string(16000U, 'p'))}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend.createLogicalEdges({{nodes[0], nodes[1], "REVERSE_HOP", {}}}));
	static_cast<void>(backend.createLogicalNodeIndex({"reverse_target_id", "ReverseTarget", {"id"}}));
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds bounds{.maximum_node_records = 8U,
		.maximum_node_page_records = 2U, .maximum_metadata_pages_per_node_page = 8U,
		.maximum_edges_per_node = 8U, .maximum_edge_records = 8U,
		.maximum_adjacency_page_records = 4U, .maximum_property_entries = 8U,
		.maximum_rows = 8U, .maximum_distinct_rows = 8U, .maximum_materialized_bytes = 64U * 1024U};
	for (size_t retry = 0U; retry < 2U; ++retry) {
		for (bool missing: {false, true}) {
			auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
				std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"ReverseSource"}),
				"source", "", "target", "REVERSE_HOP", "out", 1, 2,
				std::vector<std::string>{"ReverseTarget"},
				std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue(missing ? "missing" : "wanted")}});
			std::vector<logical::LogicalProjectItem> items;
			items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("target", "id"), "id");
			logical::LogicalProject project(std::move(traversal), std::move(items));
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
			const auto result = runtime.executeToQueryResult(project, bounds);
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			ASSERT_EQ(result.rowCount(), missing ? 0U : 1U);
			if (!missing) EXPECT_EQ(result.getRows().front().at("id").asPrimitive(), PropertyValue("wanted"));
			EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
			if (!missing) {
				ASSERT_TRUE(trace.contains("logical_plan.reverse_target_candidate_rows"));
				EXPECT_EQ(trace.at("logical_plan.reverse_target_candidate_rows").totalValue, 1);
			}
		}
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ConstraintValidationReadsOnlyRequiredFieldsAndPreservesViolations) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto nodes = backend.createLogicalNodes({
		{{"SelectedConstraint"}, {{"name", PropertyValue("a")}, {"payload", PropertyValue(std::string(16000U, 'a'))}}},
		{{"SelectedConstraint"}, {{"name", PropertyValue("b")}, {"payload", PropertyValue(std::string(16000U, 'b'))}}}});
	static_cast<void>(backend.createLogicalEdges({{nodes[0], nodes[1], "SELECTED_CONSTRAINT", {
		{"weight", PropertyValue(3)}, {"payload", PropertyValue(std::string(16000U, 'e'))}}}}));
	const auto check = [&](const std::function<void()> &operation) {
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
		operation();
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
	};
	check([&] {
		static_cast<void>(backend.createLogicalConstraint({"selected_name_required", LogicalGraphConstraintEntityKind::LGCEK_NODE,
			LogicalGraphConstraintKind::LGCK_NOT_NULL, "SelectedConstraint", {"name"}, LogicalGraphConstraintPropertyType::LGCPT_NONE}));
	});
	check([&] {
		static_cast<void>(backend.createLogicalConstraint({"selected_weight_required", LogicalGraphConstraintEntityKind::LGCEK_EDGE,
			LogicalGraphConstraintKind::LGCK_NOT_NULL, "SELECTED_CONSTRAINT", {"weight"}, LogicalGraphConstraintPropertyType::LGCPT_NONE}));
	});
	check([&] {
		static_cast<void>(backend.createLogicalConstraint({"selected_name_type", LogicalGraphConstraintEntityKind::LGCEK_NODE,
			LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE, "SelectedConstraint", {"name"}, LogicalGraphConstraintPropertyType::LGCPT_STRING}));
	});
	static_cast<void>(backend.createLogicalConstraint({"selected_name_unique", LogicalGraphConstraintEntityKind::LGCEK_NODE,
		LogicalGraphConstraintKind::LGCK_UNIQUE, "SelectedConstraint", {"name"}, LogicalGraphConstraintPropertyType::LGCPT_NONE}));
	for (size_t retry = 0U; retry < 2U; ++retry) {
		check([&] {
			EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"SelectedConstraint"}, {{"name", PropertyValue("a")}}}})), std::runtime_error);
			EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"SelectedConstraint"}, {}}})), std::runtime_error);
			EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"SelectedConstraint"}, {{"name", PropertyValue(7)}}}})), std::runtime_error);
			EXPECT_THROW(static_cast<void>(backend.createLogicalEdges({{nodes[0], nodes[1], "SELECTED_CONSTRAINT", {}}})), std::runtime_error);
		});
	}
	check([&] {
		try {
			static_cast<void>(backend.createLogicalConstraint({"selected_new_field_required", LogicalGraphConstraintEntityKind::LGCEK_NODE,
				LogicalGraphConstraintKind::LGCK_NOT_NULL, "SelectedConstraint", {"new_field"}, LogicalGraphConstraintPropertyType::LGCPT_NONE}));
			FAIL() << "missing persisted field must violate the new constraint";
		} catch (const std::runtime_error &error) {
			EXPECT_NE(std::string(error.what()).find("Constraint violation"), std::string::npos);
		}
	});
	const auto created = backend.createLogicalNodes({{{"SelectedConstraint"}, {{"name", PropertyValue("c")}}}});
	ASSERT_EQ(created.size(), 1U);
	EXPECT_EQ(backend.listLogicalConstraints(8U).size(), 4U);
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ReadExecutorHydratesCompleteRowsOnlyAfterTargetFiltersMatch) {
	using namespace graph::query::execution;
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto nodes = backend.createLogicalNodes({
		{{"ExecutorSource"}, {{"id", PropertyValue(7)}, {"payload", PropertyValue(std::string(16000U, 's'))}}},
		{{"ExecutorTarget"}, {{"score", PropertyValue(0)}, {"payload", PropertyValue(std::string(16000U, 'r'))}}},
		{{"ExecutorTarget"}, {{"score", PropertyValue(1)}, {"payload", PropertyValue(std::string(16000U, 't'))}}}});
	const auto edges = backend.createLogicalEdges({
		{nodes[0], nodes[1], "EXECUTOR_MATCH", {{"pass", PropertyValue(1)}, {"payload", PropertyValue(std::string(16000U, 'r'))}}},
		{nodes[0], nodes[2], "EXECUTOR_MATCH", {{"pass", PropertyValue(1)}, {"payload", PropertyValue(std::string(16000U, 'e'))}}},
		{nodes[0], nodes[2], "EXECUTOR_OTHER", {{"pass", PropertyValue(1)}, {"payload", PropertyValue(std::string(16000U, 'o'))}}}});
	LogicalGraphReadExecutor executor(backend);
	for (bool selectedSource: {false, true}) {
		SCOPED_TRACE(selectedSource);
		for (size_t scenario = 0U; scenario < 4U; ++scenario) for (size_t retry = 0U; retry < 2U; ++retry) {
			SCOPED_TRACE(scenario);
			LogicalGraphReadRequest request;
			request.source_filter.required_labels = {"ExecutorSource"};
			if (selectedSource) request.source_filter.property_predicates = {{"id", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(7), {}}};
			LogicalGraphExpansion expansion;
			expansion.edge_filter.relationship_type = "EXECUTOR_MATCH";
			expansion.edge_filter.property_predicates = {{"pass", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(1), {}}};
			expansion.target_filter.required_labels = {scenario == 1U ? "Missing" : "ExecutorTarget"};
			if (scenario != 1U) expansion.target_filter.property_predicates = {{"score", LogicalPropertyPredicateOp::LPP_EQ,
				PropertyValue(scenario == 0U ? 9 : 1), {}}};
			if (scenario == 2U) expansion.direction = LogicalGraphReadDirection::LGRD_INCOMING;
			request.expansion = expansion;
			request.bounds = {8U, 8U, 8U, 1U};
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
			std::vector<LogicalGraphReadRow> rows;
			const auto stats = executor.execute(request, [&](const auto &row) { rows.push_back(row); });
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			EXPECT_EQ(stats.nodes_matched, 1U);
			if (scenario != 3U) {
				EXPECT_TRUE(rows.empty());
				EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
			} else {
				ASSERT_EQ(rows.size(), 1U);
				EXPECT_EQ(rows.front().source_node.node_id, nodes[0]);
				ASSERT_TRUE(rows.front().edge); EXPECT_EQ(rows.front().edge->edge_id, edges[1]);
				ASSERT_TRUE(rows.front().target_node); EXPECT_EQ(rows.front().target_node->node_id, nodes[2]);
				const auto checkPayload = [](const auto &properties, char expected) {
					const auto payload = std::ranges::find(properties, "payload", &LogicalGraphPropertyEntry::key);
					ASSERT_NE(payload, properties.end()); EXPECT_EQ(payload->value, PropertyValue(std::string(16000U, expected)));
				};
				checkPayload(rows.front().source_node.properties, 's');
				checkPayload(rows.front().edge->properties, 'e');
				checkPayload(rows.front().target_node->properties, 't');
				ASSERT_TRUE(trace.contains("native_graph.read.property_full_decode"));
				EXPECT_EQ(trace.at("native_graph.read.property_full_decode").totalValue, 3);
			}
		}
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, OverlayCompleteEdgeVisitHydratesOnlyVisibleUnchangedRows) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto nodes = backend.createLogicalNodes({{{"Visit"}, {}}, {{"Visit"}, {}}, {{"Visit"}, {}}});
	const auto edges = backend.createLogicalEdges({
		{nodes[0], nodes[1], "VISIT", {{"payload", PropertyValue(std::string(16000U, 'a'))}}},
		{nodes[0], nodes[1], "VISIT", {{"payload", PropertyValue(std::string(16000U, 'b'))}}},
		{nodes[0], nodes[2], "VISIT", {{"payload", PropertyValue(std::string(16000U, 'c'))}}},
		{nodes[1], nodes[0], "VISIT", {{"payload", PropertyValue(std::string(16000U, 'd'))}}}});
	ASSERT_EQ(edges.size(), 4U);
	for (bool vectorSnapshot: {false, true}) {
		SCOPED_TRACE(vectorSnapshot);
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		overlay.replaceLogicalEdgeProperties({{edges[0], {{"payload", PropertyValue("replacement")}}}});
		overlay.deleteLogicalEdges({{edges[1]}});
		overlay.deleteLogicalNodes({{nodes[2], true, 8U}});
		const auto created = overlay.createLogicalEdges({{nodes[0], nodes[0], "VISIT",
			{{"payload", PropertyValue(std::string(16000U, 'e'))}}}}).front();
		auto snapshot = vectorSnapshot ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
			: overlay.openLogicalGraphReadSnapshot();
		for (bool stop: {true, false}) for (size_t retry = 0U; retry < 2U; ++retry) {
			SCOPED_TRACE(stop);
			std::vector<LogicalGraphEdge> rows;
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
			snapshot->visitEdges(3U, [&](const LogicalGraphEdge &edge) { rows.push_back(edge); return !stop; });
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			ASSERT_EQ(rows.size(), stop ? 1U : 3U);
			EXPECT_EQ(rows[0].edge_id, edges[0]);
			EXPECT_EQ(rows[0].source_node_id, nodes[0]); EXPECT_EQ(rows[0].target_node_id, nodes[1]);
			EXPECT_EQ(rows[0].type, "VISIT");
			ASSERT_EQ(rows[0].properties.size(), 1U);
			EXPECT_EQ(rows[0].properties[0].value, PropertyValue("replacement"));
			if (stop) {
				EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
			} else {
				EXPECT_EQ(rows[1].edge_id, edges[3]); EXPECT_EQ(rows[2].edge_id, created);
				EXPECT_EQ(rows[1].type, "VISIT"); EXPECT_EQ(rows[2].type, "VISIT");
				ASSERT_EQ(rows[1].properties.size(), 1U); ASSERT_EQ(rows[2].properties.size(), 1U);
				EXPECT_EQ(rows[1].properties[0].value, PropertyValue(std::string(16000U, 'd')));
				EXPECT_EQ(rows[2].properties[0].value, PropertyValue(std::string(16000U, 'e')));
				ASSERT_TRUE(trace.contains("native_graph.read.property_full_decode"));
				EXPECT_EQ(trace.at("native_graph.read.property_full_decode").totalValue, 1);
			}
		}
		EXPECT_THROW(snapshot->visitEdges(2U, [](const auto &) { return true; }), std::runtime_error);
		for (const auto direction: {LogicalGraphReadDirection::LGRD_OUTGOING, LogicalGraphReadDirection::LGRD_INCOMING}) {
			for (size_t pageSize: {1U, 8U}) for (size_t mode = 0U; mode < 3U; ++mode) {
				SCOPED_TRACE(pageSize);
				SCOPED_TRACE(mode);
				size_t retained = 0U;
				size_t propertyCalls = 0U;
				const auto propertyAdmission = [&](size_t) { ++propertyCalls; };
				const auto bufferAdmission = [&](size_t bytes) { retained = bytes; };
				const auto read = [&](std::optional<LogicalGraphAdjacencyContinuation> continuation) {
					if (mode == 0U) return snapshot->scanAdjacentEdges(nodes[0], direction, continuation, pageSize);
					if (mode == 1U) return snapshot->scanAdjacentEdgesWithPropertyAdmission(nodes[0], direction,
						continuation, pageSize, propertyAdmission);
					return snapshot->scanAdjacentEdgesWithAdmission(nodes[0], direction, continuation, pageSize,
						propertyAdmission, bufferAdmission);
				};
				std::vector<LogicalGraphEdge> rows;
				std::optional<LogicalGraphAdjacencyContinuation> continuation;
				const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
				graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
				for (size_t pages = 0U;; ++pages) {
					ASSERT_LT(pages, 4U);
					const auto page = read(continuation);
					EXPECT_LE(page.edges.size(), pageSize);
					if (mode == 2U) EXPECT_GE(retained, page.edges.capacity() * sizeof(LogicalGraphEdge));
					for (const auto &edge: page.edges) rows.push_back(edge);
					continuation = page.continuation;
					if (!continuation) break;
				}
				const auto trace = graph::debug::PerfTrace::snapshotAndReset();
				graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
				ASSERT_EQ(rows.size(), 2U);
				const bool outgoing = direction == LogicalGraphReadDirection::LGRD_OUTGOING;
				EXPECT_EQ(rows[0].edge_id, outgoing ? edges[0] : edges[3]);
				EXPECT_EQ(rows[1].edge_id, created);
				EXPECT_EQ(rows[0].type, "VISIT"); EXPECT_EQ(rows[1].type, "VISIT");
				ASSERT_EQ(rows[0].properties.size(), 1U); ASSERT_EQ(rows[1].properties.size(), 1U);
				EXPECT_EQ(rows[0].properties[0].value, outgoing ? PropertyValue("replacement") : PropertyValue(std::string(16000U, 'd')));
				EXPECT_EQ(rows[1].properties[0].value, PropertyValue(std::string(16000U, 'e')));
				if (outgoing) EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
				else {
					ASSERT_TRUE(trace.contains("native_graph.read.property_full_decode"));
					EXPECT_EQ(trace.at("native_graph.read.property_full_decode").totalValue, 1);
				}
				if (mode != 0U) EXPECT_GT(propertyCalls, 0U);
				const auto replay = read({});
				ASSERT_FALSE(replay.edges.empty()); EXPECT_EQ(replay.edges.front().edge_id, rows[0].edge_id);
			}
		}
		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgesWithAdmission(nodes[0],
			LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U, [](size_t) {}, [](size_t) {
				throw std::runtime_error("buffer rejected");
			})), std::runtime_error);
		const auto retry = snapshot->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U);
		ASSERT_EQ(retry.edges.size(), 1U); EXPECT_EQ(retry.edges.front().edge_id, edges[0]);

	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, OverlayPropertyAdmissionRejectsReentrantRevisionChangesAndAllowsRetry) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto nodes = backend.createLogicalNodes({{{"Admission"}, {{"payload", PropertyValue(std::string(16000U, 'n'))}}},
		{{"Admission"}, {}}});
	const auto edge = backend.createLogicalEdges({{nodes[0], nodes[1], "ADMISSION",
		{{"payload", PropertyValue(std::string(16000U, 'e'))}}}}).front();
	for (bool vectorSnapshot: {false, true}) for (bool staged: {false, true}) {
		for (bool isEdge: {false, true}) for (bool columns: {false, true}) {
			SCOPED_TRACE(vectorSnapshot);
			SCOPED_TRACE(staged);
			SCOPED_TRACE(isEdge);
			SCOPED_TRACE(columns);
			auto transaction = backend.beginLogicalGraphTransaction(false);
			LogicalGraphTransactionOverlay overlay(*transaction);
			const auto replace = [&](const char *value) {
				if (isEdge) overlay.replaceLogicalEdgeProperties({{edge, {{"payload", PropertyValue(value)}}}});
				else overlay.replaceLogicalNodeProperties({{nodes[0], {{"payload", PropertyValue(value)}}}});
			};
			if (staged) replace("staged");
			auto snapshot = vectorSnapshot ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
				: overlay.openLogicalGraphReadSnapshot();
			ASSERT_TRUE(snapshot->supportsShapedPropertyReadAdmission());
			ASSERT_TRUE(snapshot->supportsPropertyReadAdmission());
			const auto token = snapshot->findPropertyToken("payload"); ASSERT_TRUE(token);
			const std::array<uint64_t, 1U> ids{isEdge ? edge : nodes[0]};
			const std::array<std::string_view, 1U> keys{"payload"};
			const EntityReadShape shape{.property_tokens = {*token}};
			const auto read = [&](const std::function<void(size_t)> &admission) {
				if (columns) {
					return isEdge ? snapshot->readEdgePropertyColumnsSortedWithAdmission(ids, keys, admission)[0][0]
						: snapshot->readNodePropertyColumnsSortedWithAdmission(ids, keys, admission)[0][0];
				}
				return isEdge ? snapshot->readEdgesShapedSortedWithAdmission(ids, shape, admission)[0].properties[0].value
					: snapshot->readNodesShapedSortedWithAdmission(ids, shape, admission)[0].properties[0].value;
			};
			size_t callbacks = 0U;
			EXPECT_THROW(static_cast<void>(read([&](size_t) {
				++callbacks;
				replace("changed");
				// A nested vector read refreshes its cached delegate while the outer call is still active.
				if (isEdge) EXPECT_EQ(snapshot->readEdgeShaped(edge, shape).properties[0].value, PropertyValue("changed"));
				else EXPECT_EQ(snapshot->readNodeShaped(nodes[0], shape).properties[0].value, PropertyValue("changed"));
			})), std::runtime_error);
			EXPECT_EQ(callbacks, 1U);
			for (size_t retry = 0U; retry < 2U; ++retry) {
				EXPECT_EQ(read([](size_t) {}), PropertyValue("changed"));
				EXPECT_THROW(static_cast<void>(read([](size_t) { throw std::length_error("cancelled"); })), std::length_error);
			}
			EXPECT_THROW(static_cast<void>(read([&](size_t) { transaction->rollback(); })), std::logic_error);
			EXPECT_THROW(static_cast<void>(read([](size_t) {})), std::logic_error);
		}
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, OverlayAdjacencyAdmissionRejectsReentrantChangesAndRetriesContinuation) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto nodes = backend.createLogicalNodes({{{"Admission"}, {}}, {{"Admission"}, {}}});
	const auto edges = backend.createLogicalEdges({
		{nodes[0], nodes[1], "ADMISSION", {{"payload", PropertyValue(std::string(16000U, 'a'))}}},
		{nodes[0], nodes[1], "ADMISSION", {{"payload", PropertyValue(std::string(16000U, 'b'))}}}});
	for (bool vectorSnapshot: {false, true}) for (bool staged: {false, true}) for (size_t mode = 0U; mode < 5U; ++mode) {
		SCOPED_TRACE(vectorSnapshot);
		SCOPED_TRACE(staged);
		SCOPED_TRACE(mode);
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		if (staged) overlay.replaceLogicalEdgeProperties({{edges[0], {{"payload", PropertyValue("staged")}}}});
		auto snapshot = vectorSnapshot ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
			: overlay.openLogicalGraphReadSnapshot();
		const auto first = snapshot->scanAdjacentEdgeRefs(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U);
		ASSERT_TRUE(first.continuation);
		const auto read = [&](const std::function<void(size_t)> &admission) {
			const auto collect = [](const auto &page) {
				std::vector<uint64_t> ids;
				for (const auto &edge: page.edges) {
					if constexpr (requires { edge.entity_id; }) ids.push_back(edge.entity_id);
					else ids.push_back(edge.edge_id);
				}
				return ids;
			};
			const auto direction = LogicalGraphReadDirection::LGRD_OUTGOING;
			if (mode == 0U) return collect(snapshot->scanAdjacentEdgeRefsWithBufferAdmission(nodes[0], direction, first.continuation, 1U, admission));
			if (mode == 1U) return collect(snapshot->scanAdjacentEdgeTopologiesWithBufferAdmission(nodes[0], direction, first.continuation, 1U, admission));
			if (mode == 2U) return collect(snapshot->scanAdjacentEdgesWithPropertyAdmission(nodes[0], direction, first.continuation, 1U, admission));
			return collect(snapshot->scanAdjacentEdgesWithAdmission(nodes[0], direction, first.continuation, 1U,
				mode == 3U ? admission : std::function<void(size_t)>([](size_t) {}),
				mode == 4U ? admission : std::function<void(size_t)>([](size_t) {})));
		};
		size_t callbacks = 0U;
		EXPECT_THROW(static_cast<void>(read([&](size_t) {
			++callbacks;
			overlay.replaceLogicalEdgeProperties({{edges[1], {{"payload", PropertyValue("changed")}}}});
			EXPECT_EQ(snapshot->readEdgeShaped(edges[1], {.all_properties = true}).properties[0].value, PropertyValue("changed"));
		})), std::runtime_error);
		EXPECT_EQ(callbacks, 1U);
		for (size_t retry = 0U; retry < 2U; ++retry) {
			EXPECT_EQ(read([](size_t) {}), (std::vector<uint64_t>{edges[1]}));
			EXPECT_THROW(static_cast<void>(read([](size_t) { throw std::length_error("cancelled"); })), std::length_error);
		}
		EXPECT_THROW(static_cast<void>(read([&](size_t) { transaction->rollback(); })), std::logic_error);
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, OverlayVisitorsRejectCallbackMutationAndRollbackBeforeContinuing) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto nodes = backend.createLogicalNodes({{{"Visitor"}, {{"payload", PropertyValue("base")}}},
		{{"Visitor"}, {}}, {{"Visitor"}, {}}});
	const auto edges = backend.createLogicalEdges({{nodes[0], nodes[1], "VISITOR", {{"payload", PropertyValue("base")}}},
		{nodes[0], nodes[2], "VISITOR", {}}});
	for (bool vectorSnapshot: {false, true}) for (bool staged: {false, true}) for (size_t mode = 0U; mode < 9U; ++mode) {
		for (bool keepGoing: {false, true}) {
			SCOPED_TRACE(vectorSnapshot);
			SCOPED_TRACE(staged);
			SCOPED_TRACE(mode);
			SCOPED_TRACE(keepGoing);
			auto transaction = backend.beginLogicalGraphTransaction(false);
			LogicalGraphTransactionOverlay overlay(*transaction);
			if (staged) overlay.replaceLogicalNodeProperties({{nodes[0], {{"payload", PropertyValue("staged")}}}});
			auto snapshot = vectorSnapshot ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
				: overlay.openLogicalGraphReadSnapshot();
			std::vector<uint64_t> seen;
			const auto scan = [&](const std::function<void()> &action, bool proceed) {
				const auto visitor = [&](const auto &entity) {
					if constexpr (requires { entity.entity_id; }) seen.push_back(entity.entity_id);
					else if constexpr (requires { entity.edge_id; }) seen.push_back(entity.edge_id);
					else seen.push_back(entity.node_id);
					action();
					return proceed;
				};
				if (mode == 0U) snapshot->visitNodeRefs(8U, visitor);
				else if (mode == 1U) snapshot->visitNodeRefsWithLabelMatches(8U, {"Visitor"}, [&](const EntityRef &ref, bool matches) {
					EXPECT_TRUE(matches); return visitor(ref);
				});
				else if (mode == 2U) snapshot->visitEdgeRefs(8U, visitor);
				else if (mode == 3U) snapshot->visitNodes(8U, visitor);
				else if (mode == 4U) snapshot->visitEdges(8U, visitor);
				else if (mode == 5U) snapshot->visitAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, 8U, visitor);
				else if (mode == 6U) snapshot->visitEdgeTopologies(8U, visitor);
				else if (mode == 7U) snapshot->visitAdjacentEdgeTopologies(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, 8U, visitor);
				else snapshot->visitAdjacentEdgeRefsForNodes(std::array<uint64_t, 1U>{nodes[0]}, LogicalGraphReadDirection::LGRD_OUTGOING, 8U, visitor);
			};
			const bool isEdge = mode == 2U || mode >= 4U;
			EXPECT_THROW(scan([&] {
				if (isEdge) overlay.replaceLogicalEdgeProperties({{edges[0], {{"payload", PropertyValue("changed")}}}});
				else overlay.replaceLogicalNodeProperties({{nodes[0], {{"payload", PropertyValue("changed")}}}});
				// Refresh the vector delegate during the active visitor call.
				if (isEdge) EXPECT_EQ(snapshot->readEdgeShaped(edges[0], {.all_properties = true}).properties[0].value, PropertyValue("changed"));
				else EXPECT_EQ(snapshot->readNodeShaped(nodes[0], {.all_properties = true}).properties[0].value, PropertyValue("changed"));
			}, keepGoing), std::runtime_error);
			ASSERT_EQ(seen.size(), 1U);
			for (size_t retry = 0U; retry < 2U; ++retry) {
				seen.clear(); scan([] {}, true);
				EXPECT_EQ(seen, isEdge ? edges : nodes);
				seen.clear(); scan([] {}, false); EXPECT_EQ(seen.size(), 1U);
				seen.clear();
				EXPECT_THROW(scan([] { throw std::length_error("cancelled"); }, true), std::length_error);
				EXPECT_EQ(seen.size(), 1U);
			}
			seen.clear();
			EXPECT_THROW(scan([&] { transaction->rollback(); }, keepGoing), std::logic_error);
			EXPECT_EQ(seen.size(), 1U);
			EXPECT_THROW(scan([] {}, true), std::logic_error);
		}
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ReadRuntimeChecksSnapshotAndGuardAfterFinalOutputCallback) {
	using namespace graph::query;
	using namespace graph::query::execution;
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto nodes = backend.createLogicalNodes({{{"Output"}, {{"payload", PropertyValue("base")}}}, {{"Output"}, {}}});
	graph::parser::cypher::CypherParserImpl parser;
	const auto countPlan = parser.parseToLogical("MATCH (n:Output) RETURN count(n) AS value");
	const auto selectedPlan = parser.parseToLogical("MATCH (n:Output) RETURN n.payload AS value LIMIT 1");
	LogicalPlanReadRuntimeBounds bounds{.maximum_node_records = 8U, .maximum_node_page_records = 2U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U, .maximum_edge_records = 8U,
		.maximum_adjacency_page_records = 2U, .maximum_property_entries = 8U, .maximum_rows = 8U,
		.maximum_distinct_rows = 8U, .maximum_materialized_bytes = 64U * 1024U};
	for (bool vectorSnapshot: {false, true}) for (size_t mode = 0U; mode < 4U; ++mode) {
		for (bool keepGoing: {false, true}) for (size_t action = 0U; action < 3U; ++action) {
			if (mode == 0U && action == 2U) continue; // The context-free overload has no QueryGuard.
			SCOPED_TRACE(vectorSnapshot);
			SCOPED_TRACE(mode);
			SCOPED_TRACE(keepGoing);
			SCOPED_TRACE(action);
			auto transaction = backend.beginLogicalGraphTransaction(false);
			LogicalGraphTransactionOverlay overlay(*transaction);
			auto snapshot = vectorSnapshot ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
				: overlay.openLogicalGraphReadSnapshot();
			ASSERT_TRUE(snapshot->mutationFence());
			LogicalPlanReadRuntime runtime(overlay);
			size_t resident = 0U;
			QueryGuard guard(0, 1, [&] { return resident; });
			QueryContext context; context.guard = &guard;
			LogicalPlanReadRow input;
			input.values.emplace("n", EntityRef{nodes[0], false, 0U, 0U, 0U});
			logical::LogicalProject inputPlan(std::make_unique<logical::LogicalSingleRow>(),
				{{std::make_shared<expressions::VariableReferenceExpression>("n", "payload"), "value"}});
			const auto run = [&](const LogicalPlanReadRuntime::RowVisitor &visitor) {
				if (mode == 0U) return runtime.execute(*countPlan.root, bounds, visitor);
				if (mode == 1U) return runtime.execute(*countPlan.root, bounds, visitor, context);
				if (mode == 2U) return runtime.executeWithSnapshot(*snapshot, *selectedPlan.root, bounds, visitor, context);
				return runtime.executeWithSnapshotAndInputRow(*snapshot, inputPlan, bounds, input, visitor, context);
			};
			size_t delivered = 0U;
			const auto callback = [&](const LogicalPlanReadRow &) {
				++delivered;
				if (action == 0U) {
					overlay.replaceLogicalNodeProperties({{nodes[0], {{"payload", PropertyValue("changed")}}}});
					EXPECT_EQ(snapshot->readNodeShaped(nodes[0], {.all_properties = true}).properties[0].value, PropertyValue("changed"));
				} else if (action == 1U) transaction->rollback();
				else resident = 2U * 1024U * 1024U;
				return keepGoing;
			};
			if (action == 0U) EXPECT_THROW(static_cast<void>(run(callback)), std::runtime_error);
			else if (action == 1U) EXPECT_THROW(static_cast<void>(run(callback)), std::logic_error);
			else EXPECT_THROW(static_cast<void>(run(callback)), QueryMemoryExceededException);
			EXPECT_EQ(delivered, 1U);
			if (action == 1U) continue;
			resident = 0U;
			for (size_t retry = 0U; retry < 2U; ++retry) {
				delivered = 0U;
				EXPECT_NO_THROW(static_cast<void>(run([&](const LogicalPlanReadRow &row) {
					++delivered;
					const auto &value = std::get<PropertyValue>(row.values.at("value"));
					EXPECT_EQ(value, mode < 2U ? PropertyValue(2) : PropertyValue(action == 0U ? "changed" : "base"));
					return true;
				})));
				EXPECT_EQ(delivered, 1U);
			}
		}
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ReadRuntimeFailureDiscardsDeferredOutputWithoutHydratingIt) {
	using namespace graph::query;
	using namespace graph::query::execution;
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto nodes = backend.createLogicalNodes({{{"DeferredFailure"}, {{"payload", PropertyValue(std::string(16000U, 'p'))}}}});
	graph::parser::cypher::CypherParserImpl parser;
	const auto failing = parser.parseToLogical("MATCH (n:DeferredFailure) RETURN n AS value UNION ALL RETURN 1 / 0 AS value");
	const auto successful = parser.parseToLogical("MATCH (n:DeferredFailure) RETURN n AS value");
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds bounds{.maximum_node_records = 8U, .maximum_node_page_records = 2U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U, .maximum_edge_records = 8U,
		.maximum_adjacency_page_records = 2U, .maximum_property_entries = 8U, .maximum_rows = 8U,
		.maximum_distinct_rows = 8U, .maximum_materialized_bytes = 128U * 1024U};
	for (size_t retry = 0U; retry < 2U; ++retry) {
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
		try {
			static_cast<void>(runtime.executeToQueryResult(*failing.root, bounds));
			FAIL() << "division by zero must reject the buffered query";
		} catch (const std::runtime_error &error) {
			EXPECT_EQ(std::string(error.what()), "logical-plan arithmetic attempted division by zero");
		}
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
		const auto result = runtime.executeToQueryResult(*successful.root, bounds);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows()[0].at("value").asLogicalNode().node_id, nodes[0]);
	}
	backend.close();
}
