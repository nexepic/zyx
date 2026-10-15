/**
 * @file test_LogicalGraphReadExecutor_RuntimeContracts.cpp
 * @brief Native index and scalar runtime contracts for the logical graph executor.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "query/execution/LogicalGraphReadExecutorTestFixture.hpp"

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/optimization/LogicalPlanOptimizer.hpp"
#include "graph/query/logical/operators/LogicalUnion.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsNativeExactCandidatesOutsidePinnedSnapshot) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_exact_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.exact_candidate_result = IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 16U, true, false, {}};
	auto candidateLookupCount = std::make_shared<size_t>(0U);
	auto nodeReadCount = std::make_shared<size_t>(0U);
	data.exact_candidate_lookup_count = candidateLookupCount;
	data.node_read_count = nodeReadCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"}, {{"age", PropertyValue(42)}});

	EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
		return true;
	})), std::runtime_error);
	EXPECT_EQ(*candidateLookupCount, 1U);
	EXPECT_EQ(*nodeReadCount, 0U);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsNativeExactCandidatesWithoutSnapshotGeneration) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_exact_node_candidates = true;
	data.exact_candidate_result = IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 0U, true, false, {}};
	auto candidateLookupCount = std::make_shared<size_t>(0U);
	auto nodeReadCount = std::make_shared<size_t>(0U);
	data.exact_candidate_lookup_count = candidateLookupCount;
	data.node_read_count = nodeReadCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"}, {{"age", PropertyValue(42)}});

	EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
		return true;
	})), std::runtime_error);
	EXPECT_EQ(*candidateLookupCount, 1U);
	EXPECT_EQ(*nodeReadCount, 0U);
}

TEST(LogicalGraphReadExecutorTest, OptimizedFilterReusesVerifiedNativeExactCandidateFields) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_exact_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.exact_candidate_result = IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, true, false, {}};
	auto candidateLookupCount = std::make_shared<size_t>(0U);
	auto nodeReadCount = std::make_shared<size_t>(0U);
	auto nodeBatchReadCount = std::make_shared<size_t>(0U);
	data.exact_candidate_lookup_count = candidateLookupCount;
	data.node_read_count = nodeReadCount;
	data.node_batch_read_count = nodeBatchReadCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t{42}));
	auto root = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}), predicate);
	const auto optimized = graph::query::optimization::LogicalPlanOptimizer{}.optimize(std::move(root));
	ASSERT_NE(optimized.root, nullptr);
	ASSERT_EQ(optimized.rewrite_count, 1U);

	std::vector<uint64_t> nodeIds;
	const LogicalPlanReadRuntimeStats stats =
			runtime.execute(*optimized.root, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*candidateLookupCount, 1U);
	// One complete shaped Node batch remains necessary at the public result boundary;
	// the retained Filter must reuse the candidate's selected field instead of
	// causing another point read.
	EXPECT_EQ(*nodeBatchReadCount, 1U);
	EXPECT_EQ(*nodeReadCount, 0U);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{1U, 0U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, BatchesUnverifiedNativeExactCandidatesBeforeCompleteMaterialization) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_exact_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.exact_candidate_result = IndexCandidateBatch{
			{{1U, false, 0U, 0U, 0U}, {2U, false, 0U, 0U, 0U}}, 17U, true, false, {}};
	auto nodeReadCount = std::make_shared<size_t>(0U);
	auto nodeBatchReadCount = std::make_shared<size_t>(0U);
	auto nodeLabelBatchReadCount = std::make_shared<size_t>(0U);
	auto nodePropertyColumnReadCount = std::make_shared<size_t>(0U);
	data.node_read_count = nodeReadCount;
	data.node_batch_read_count = nodeBatchReadCount;
	data.node_label_batch_read_count = nodeLabelBatchReadCount;
	data.node_property_column_read_count = nodePropertyColumnReadCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"},
			{{"age", PropertyValue(42)}});

	std::vector<uint64_t> nodeIds;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			scan, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			});

	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*nodePropertyColumnReadCount, 1U);
	EXPECT_EQ(*nodeLabelBatchReadCount, 1U);
	// Only the surviving candidate reaches the complete result boundary. The
	// rejected candidate is validated from the selected Property column batch.
	EXPECT_EQ(*nodeReadCount, 0U);
	EXPECT_EQ(*nodeBatchReadCount, 1U);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{2U, 0U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, NativeExactCandidateStreamStopsAtDownstreamLimit) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(2U).properties = {{"age", PropertyValue(42)}};
	data.enable_native_exact_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.exact_candidate_stream_results =
			std::vector<IndexCandidateBatch>{IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, true, false, {}},
											 IndexCandidateBatch{{{2U, false, 0U, 0U, 0U}}, 17U, true, false, {}}};
	auto streamBatchCount = std::make_shared<size_t>(0U);
	data.exact_candidate_stream_batch_count = streamBatchCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto limited = std::make_unique<logical::LogicalNodeScan>(
			"n", std::vector<std::string>{"Person"},
			std::vector<std::pair<std::string, PropertyValue>>{{"age", PropertyValue(42)}});
	logical::LogicalLimit root(std::move(limited), 1);
	std::vector<uint64_t> ids;
	static_cast<void>(runtime.execute(root, runtimeBounds(), [&ids](const LogicalPlanReadRow &row) {
		ids.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
		return true;
	}));
	EXPECT_EQ(ids, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*streamBatchCount, 1U);
}

TEST(LogicalGraphReadExecutorTest, NativeExactCandidateStreamEnforcesTheCrossBatchNodeBound) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(2U).properties = {{"age", PropertyValue(42)}};
	data.enable_native_exact_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.exact_candidate_stream_results =
			std::vector<IndexCandidateBatch>{IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, true, false, {}},
											 IndexCandidateBatch{{{2U, false, 0U, 0U, 0U}}, 17U, true, false, {}}};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	bounds.maximum_node_records = 1U;
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"},
								  std::vector<std::pair<std::string, PropertyValue>>{{"age", PropertyValue(42)}});

	try {
		static_cast<void>(runtime.execute(scan, bounds, [](const LogicalPlanReadRow &) { return true; }));
		FAIL() << "cross-batch candidates must honor the Node scan bound";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical graph indexed Node lookup exceeded the Node scan bound");
	}
}

TEST(LogicalGraphReadExecutorTest, NativeRangeCandidateStreamStopsAtDownstreamLimit) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(1U).properties = {{"age", PropertyValue(42)}};
	data.nodes.at(2U).properties = {{"age", PropertyValue(43)}};
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.range_candidate_stream_results = std::vector<IndexCandidateBatch>{
			IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, false, true, {{"age", {PropertyValue(42)}}}},
			IndexCandidateBatch{{{2U, false, 0U, 0U, 0U}}, 17U, false, true, {{"age", {PropertyValue(43)}}}}};
	data.range_candidate_stream_batch_count = std::make_shared<size_t>(0U);
	auto streamBatchCount = data.range_candidate_stream_batch_count;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	scan->setRangePredicates({{"age", PropertyValue(42), PropertyValue(), true, true}});
	logical::LogicalLimit root(std::move(scan), 1);
	std::vector<uint64_t> ids;
	static_cast<void>(runtime.execute(root, runtimeBounds(), [&ids](const LogicalPlanReadRow &row) {
		const auto &value = row.values.at("n");
		ids.push_back(std::holds_alternative<storage::EntityRef>(value)
				? std::get<storage::EntityRef>(value).entity_id
				: std::get<storage::LogicalGraphNode>(value).node_id);
		return true;
	}));
	EXPECT_EQ(ids, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*streamBatchCount, 1U);
}

TEST(LogicalGraphReadExecutorTest, NativeCompositeRangeCandidateStreamStopsAtDownstreamLimit) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(1U).properties = {{"status", PropertyValue("online")}, {"age", PropertyValue(42)}};
	data.nodes.at(2U).properties = {{"status", PropertyValue("online")}, {"age", PropertyValue(43)}};
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	IndexCandidateBatch first{{{1U, false, 0U, 0U, 0U}}, 17U, false, true,
			{{"status", {PropertyValue("online")}}, {"age", {PropertyValue(42)}}}};
	IndexCandidateBatch second{{{2U, false, 0U, 0U, 0U}}, 17U, false, true,
			{{"status", {PropertyValue("online")}}, {"age", {PropertyValue(43)}}}};
	data.composite_range_candidate_stream_results = std::vector<IndexCandidateBatch>{first, second};
	data.composite_range_candidate_stream_batch_count = std::make_shared<size_t>(0U);
	auto streamBatchCount = data.composite_range_candidate_stream_batch_count;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>(
			"n", std::vector<std::string>{"Person"},
			std::vector<std::pair<std::string, PropertyValue>>{{"status", PropertyValue("online")}});
	scan->setRangePredicates({{"age", PropertyValue(42), PropertyValue(), true, true}});
	logical::LogicalLimit root(std::move(scan), 1);
	std::vector<uint64_t> ids;
	static_cast<void>(runtime.execute(root, runtimeBounds(), [&ids](const LogicalPlanReadRow &row) {
		const auto &value = row.values.at("n");
		ids.push_back(std::holds_alternative<storage::EntityRef>(value)
							  ? std::get<storage::EntityRef>(value).entity_id
							  : std::get<storage::LogicalGraphNode>(value).node_id);
		return true;
	}));
	EXPECT_EQ(ids, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*streamBatchCount, 1U);
}

TEST(LogicalGraphReadExecutorTest, NativeRangeCandidateStreamAllowsValueMajorOrderAcrossBatches) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	// An ordered B+Tree range is value-major. Entity IDs may therefore move
	// backwards when the next value batch is delivered; each batch remains
	// strictly ordered and the stream must not impose a false global ID order.
	data.range_candidate_stream_results = std::vector<IndexCandidateBatch>{
			IndexCandidateBatch{{{2U, false, 0U, 0U, 0U}}, 17U, false, true,
					{{"age", {PropertyValue(30)}}}},
			IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, false, true,
					{{"age", {PropertyValue(42)}}}}};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"});
	scan.setRangePredicates({{"age", PropertyValue(30), PropertyValue(42), true, true}});

	std::vector<uint64_t> nodeIds;
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
		const auto &value = row.values.at("n");
		nodeIds.push_back(std::holds_alternative<storage::EntityRef>(value)
				? std::get<storage::EntityRef>(value).entity_id
				: std::get<storage::LogicalGraphNode>(value).node_id);
		return true;
	})));
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{2U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, NativeRangeCandidateStreamRejectsDuplicatesAcrossBatches) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.range_candidate_stream_results = std::vector<IndexCandidateBatch>{
			IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, false, true,
					{{"age", {PropertyValue(42)}}}},
			IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, false, true,
					{{"age", {PropertyValue(42)}}}}};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"});
	scan.setRangePredicates({{"age", PropertyValue(30), PropertyValue(42), true, true}});

	try {
		static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; }));
		FAIL() << "duplicate stream candidates must fail closed";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical graph indexed Node lookup returned duplicate Node references");
	}
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeBoundsVerifiedIndexCandidateBytes) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.range_candidate_result = IndexCandidateBatch{
			{{1U, false, 0U, 0U, 0U}}, 17U, false, true, {{"age", {PropertyValue(std::string(128U, 'x'))}}}};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	bounds.maximum_candidate_bytes = 64U;
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"});
	scan.setRangePredicates({{"age", PropertyValue(int64_t{30}), PropertyValue(), true, true}});

	try {
		static_cast<void>(runtime.execute(scan, bounds, [](const LogicalPlanReadRow &) { return true; }));
		FAIL() << "oversized streamed candidates must fail closed";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical graph indexed Node candidate batch exceeded its byte bound");
	}
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeBoundsNonStreamingIndexCandidateBytes) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_range_node_candidates = true;
	data.disable_native_candidate_stream = true;
	data.native_snapshot_generation = 17U;
	data.range_candidate_result = IndexCandidateBatch{
			{{1U, false, 0U, 0U, 0U}}, 17U, false, true, {{"age", {PropertyValue(std::string(128U, 'x'))}}}};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	bounds.maximum_candidate_bytes = 64U;
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"});
	scan.setRangePredicates({{"age", PropertyValue(int64_t{30}), PropertyValue(), true, true}});

	try {
		static_cast<void>(runtime.execute(scan, bounds, [](const LogicalPlanReadRow &) { return true; }));
		FAIL() << "oversized non-streaming candidates must fail closed";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical graph indexed Node candidate batch exceeded its byte bound");
	}
}

TEST(LogicalGraphReadExecutorTest, ReadsUnverifiedNativeIndexResidualColumnsInBoundedBatches) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.clear();
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.expected_range_lookup = LogicalGraphPropertyRange{
			"age", PropertyValue(int64_t{1}), PropertyValue(int64_t{600}), true, true};
	auto propertyColumnReads = std::make_shared<size_t>(0U);
	data.node_property_column_read_count = propertyColumnReads;
	std::vector<EntityRef> candidates;
	candidates.reserve(600U);
	for (uint64_t nodeId = 1U; nodeId <= 600U; ++nodeId) {
		LogicalGraphNode node;
		node.node_id = nodeId;
		node.labels = {"Person"};
		node.properties = {{"age", PropertyValue(static_cast<int64_t>(nodeId))}};
		data.nodes.emplace(nodeId, std::move(node));
		candidates.push_back({nodeId, false, 0U, 0U, 0U});
	}
	data.range_candidate_result = IndexCandidateBatch{std::move(candidates), 17U, false, false, {}, true};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"});
	scan.setRangePredicates({{"age", PropertyValue(int64_t{1}), PropertyValue(int64_t{600}), true, true}});
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	bounds.maximum_node_records = 600U;
	bounds.maximum_rows = 600U;

	const graph::query::QueryResult result = runtime.executeToQueryResult(scan, bounds);

	EXPECT_EQ(result.rowCount(), 600U);
	EXPECT_EQ(*propertyColumnReads, 2U);
}

TEST(LogicalGraphReadExecutorTest, ReducesIndexResidualBatchSizeForByteBudgetBeforeLimit) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.clear();
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.expected_range_lookup = LogicalGraphPropertyRange{
			"age", PropertyValue(int64_t{250}), PropertyValue(int64_t{600}), true, true};
	auto propertyColumnReads = std::make_shared<size_t>(0U);
	data.node_property_column_read_count = propertyColumnReads;
	auto batchSizes = std::make_shared<std::vector<size_t>>();
	data.node_property_column_batch_sizes = batchSizes;
	std::vector<EntityRef> candidates;
	candidates.reserve(600U);
	for (uint64_t nodeId = 1U; nodeId <= 600U; ++nodeId) {
		LogicalGraphNode node;
		node.node_id = nodeId;
		node.labels = {"Person"};
		node.properties = {{"age", PropertyValue(static_cast<int64_t>(nodeId))}};
		data.nodes.emplace(nodeId, std::move(node));
		candidates.push_back({nodeId, false, 0U, 0U, 0U});
	}
	data.range_candidate_result = IndexCandidateBatch{std::move(candidates), 17U, false, false, {}, true};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	scan->setRangePredicates({{"age", PropertyValue(int64_t{250}), PropertyValue(int64_t{600}), true, true}});
	logical::LogicalLimit root(std::move(scan), 1);
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	bounds.maximum_node_records = 600U;
	bounds.maximum_rows = 600U;
	bounds.maximum_materialized_bytes = 8U * 1024U;

	const graph::query::QueryResult result = runtime.executeToQueryResult(root, bounds);

	ASSERT_EQ(result.rowCount(), 1U);
	ASSERT_TRUE(result.getRows().front().at("n").isLogicalNode());
	EXPECT_EQ(result.getRows().front().at("n").asNode().getId(), 250U);
	// Object slots and map buckets differ across standard libraries. Check
	// bounded batching and LIMIT termination rather than one platform's count.
	ASSERT_EQ(batchSizes->size(), *propertyColumnReads);
	EXPECT_GT(*propertyColumnReads, 1U);
	size_t visitedCandidates = 0U;
	for (size_t size: *batchSizes) {
		EXPECT_GT(size, 0U);
		EXPECT_LT(size, 600U);
		EXPECT_LE(size * sizeof(PropertyValue), bounds.maximum_materialized_bytes);
		visitedCandidates += size;
	}
	EXPECT_GE(visitedCandidates, 250U);
	EXPECT_LT(visitedCandidates, 600U);
}

TEST(LogicalGraphReadExecutorTest, RejectsOversizedNativeIndexResidualPayloadBeforeVisitingRows) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.clear();
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.expected_range_lookup = LogicalGraphPropertyRange{
			"age", PropertyValue(int64_t{1}), PropertyValue(int64_t{1}), true, true};
	auto propertyColumnReads = std::make_shared<size_t>(0U);
	data.node_property_column_read_count = propertyColumnReads;
	LogicalGraphNode node;
	node.node_id = 1U;
	node.labels = {"Person"};
	node.properties = {{"age", PropertyValue(std::string(256U, 'x'))}};
	data.nodes.emplace(node.node_id, std::move(node));
	data.range_candidate_result = IndexCandidateBatch{
			{{1U, false, 0U, 0U, 0U}}, 17U, false, false, {}, true};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"});
	scan.setRangePredicates({{"age", PropertyValue(int64_t{1}), PropertyValue(int64_t{1}), true, true}});
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	// Advance past the resident output-batch slot so this exercises the selected
	// Property-column payload admission boundary itself.
	bounds.maximum_materialized_bytes = 512U;
	size_t visitedRows = 0U;

	try {
		static_cast<void>(runtime.execute(scan, bounds, [&visitedRows](const LogicalPlanReadRow &) {
			++visitedRows;
			return true;
		}));
		FAIL() << "oversized selected candidate payloads must fail closed";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical-plan indexed Node Property batch exceeded its byte bound");
	}
	// The structural row alone exceeds 64 bytes, so reject before reading its payload.
	EXPECT_EQ(*propertyColumnReads, 0U);
	EXPECT_EQ(visitedRows, 0U);
}

TEST(LogicalGraphReadExecutorTest, BatchesUnverifiedNativeExactCandidateResidualColumns) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.clear();
	data.enable_native_exact_node_candidates = true;
	data.native_snapshot_generation = 17U;
	auto propertyColumnReads = std::make_shared<size_t>(0U);
	data.node_property_column_read_count = propertyColumnReads;
	std::vector<EntityRef> candidates;
	candidates.reserve(600U);
	for (uint64_t nodeId = 1U; nodeId <= 600U; ++nodeId) {
		LogicalGraphNode node;
		node.node_id = nodeId;
		node.labels = {"Person"};
		node.properties = {{"age", PropertyValue(static_cast<int64_t>(nodeId))}};
		data.nodes.emplace(nodeId, std::move(node));
		candidates.push_back({nodeId, false, 0U, 0U, 0U});
	}
	data.exact_candidate_result = IndexCandidateBatch{std::move(candidates), 17U, false, false, {}, true};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"}, {{"age", PropertyValue(int64_t{42})}});
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	bounds.maximum_node_records = 600U;
	bounds.maximum_rows = 600U;

	const graph::query::QueryResult result = runtime.executeToQueryResult(scan, bounds);

	ASSERT_EQ(result.rowCount(), 1U);
	ASSERT_TRUE(result.getRows().front().at("n").isLogicalNode());
	EXPECT_EQ(result.getRows().front().at("n").asNode().getId(), 42U);
	EXPECT_EQ(*propertyColumnReads, 2U);
}

TEST(LogicalGraphReadExecutorTest, BatchesUnverifiedNativeCompositeCandidateResidualColumns) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.clear();
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.expected_composite_range_lookup = LogicalGraphCompositePropertyRange{
			{{"status", PropertyValue("online")}},
			{"age", PropertyValue(int64_t{250}), PropertyValue(int64_t{600}), true, true}};
	auto propertyColumnReads = std::make_shared<size_t>(0U);
	data.node_property_column_read_count = propertyColumnReads;
	std::vector<EntityRef> candidates;
	candidates.reserve(600U);
	for (uint64_t nodeId = 1U; nodeId <= 600U; ++nodeId) {
		LogicalGraphNode node;
		node.node_id = nodeId;
		node.labels = {"Person"};
		node.properties = {{"status", PropertyValue("online")},
				{"age", PropertyValue(static_cast<int64_t>(nodeId))}};
		data.nodes.emplace(nodeId, std::move(node));
		candidates.push_back({nodeId, false, 0U, 0U, 0U});
	}
	data.composite_range_candidate_result =
			IndexCandidateBatch{std::move(candidates), 17U, false, false, {}, true};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"},
			{{"status", PropertyValue("online")}});
	scan.setRangePredicates({{"age", PropertyValue(int64_t{250}), PropertyValue(int64_t{600}), true, true}});
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	bounds.maximum_node_records = 600U;
	bounds.maximum_rows = 600U;

	const graph::query::QueryResult result = runtime.executeToQueryResult(scan, bounds);

	EXPECT_EQ(result.rowCount(), 351U);
	EXPECT_EQ(*propertyColumnReads, 2U);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsMalformedNativeCandidateColumnsAndOrder) {
	{
		LogicalGraphFixtureData data = graphData();
		data.enable_native_range_node_candidates = true;
		data.native_snapshot_generation = 17U;
		data.range_candidate_result = IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, false, true, {}};
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"});
		scan.setRangePredicates({{"age", PropertyValue(0), PropertyValue(), true, true}});
		EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
			return true;
		})), std::runtime_error);
	}

	for (const std::vector<EntityRef> &malformedCandidates: {
				std::vector<EntityRef>{{1U, false, 0U, 0U, 0U}, {1U, false, 0U, 0U, 0U}},
				std::vector<EntityRef>{{2U, false, 0U, 0U, 0U}, {1U, false, 0U, 0U, 0U}}}) {
		LogicalGraphFixtureData data = graphData();
		data.enable_native_exact_node_candidates = true;
		data.native_snapshot_generation = 17U;
		data.exact_candidate_result = IndexCandidateBatch{malformedCandidates, 17U, true, false, {}};
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"}, {{"age", PropertyValue(42)}});
		EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
			return true;
		})), std::runtime_error);
	}
}

TEST(LogicalGraphReadExecutorTest, OptimizedFilterNodeScanUsesExactLookupAndRetainsRuntimePredicateValidation) {
	LogicalGraphFixtureData data = graphData();
	data.enable_exact_node_lookup = true;
	auto exactLookupCount = std::make_shared<size_t>(0U);
	data.exact_lookup_count = exactLookupCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t{42}));
	auto root = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}), predicate);
	const auto optimized = graph::query::optimization::LogicalPlanOptimizer{}.optimize(std::move(root));
	ASSERT_NE(optimized.root, nullptr);
	EXPECT_EQ(optimized.rewrite_count, 1U);

	std::vector<uint64_t> nodeIds;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			*optimized.root, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*exactLookupCount, 1U);
	// The index candidate is revalidated by the NodeScan and then by the
	// retained Filter, so both layers account for their Property reads.
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{1U, 0U, 2U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, OptimizedFilterNodeScanUsesRangeLookupAndRetainsRuntimePredicateValidation) {
	LogicalGraphFixtureData data = graphData();
	data.enable_range_node_lookup = true;
	auto rangeLookupCount = std::make_shared<size_t>(0U);
	data.range_lookup_count = rangeLookupCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t{30}));
	auto root = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}), predicate);
	const auto optimized = graph::query::optimization::LogicalPlanOptimizer{}.optimize(std::move(root));
	ASSERT_NE(optimized.root, nullptr);
	EXPECT_EQ(optimized.rewrite_count, 1U);

	std::vector<uint64_t> nodeIds;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			*optimized.root, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U, 2U}));
	EXPECT_EQ(*rangeLookupCount, 1U);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{2U, 0U, 4U, 2U}));
}

TEST(LogicalGraphReadExecutorTest, OptimizedFilterReusesVerifiedNativeRangeCandidateFields) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.range_candidate_result = IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, false, true,
			{{"age", {PropertyValue(42)}}}};
	auto candidateLookupCount = std::make_shared<size_t>(0U);
	auto nodeReadCount = std::make_shared<size_t>(0U);
	auto nodeBatchReadCount = std::make_shared<size_t>(0U);
	data.range_candidate_lookup_count = candidateLookupCount;
	data.node_read_count = nodeReadCount;
	data.node_batch_read_count = nodeBatchReadCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t{30}));
	auto root = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}), predicate);
	const auto optimized = graph::query::optimization::LogicalPlanOptimizer{}.optimize(std::move(root));
	ASSERT_NE(optimized.root, nullptr);
	ASSERT_EQ(optimized.rewrite_count, 1U);

	std::vector<uint64_t> nodeIds;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			*optimized.root, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*candidateLookupCount, 1U);
	// The filter consumes the authenticated selected range field. The public
	// result boundary uses one complete shaped batch and no point read.
	EXPECT_EQ(*nodeBatchReadCount, 1U);
	EXPECT_EQ(*nodeReadCount, 0U);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{1U, 0U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, OptimizedFilterNodeScanUsesCanonicalConjunctiveRangeLookup) {
	LogicalGraphFixtureData data = graphData();
	data.enable_range_node_lookup = true;
	data.expected_range_lookup = {"age", PropertyValue(30), PropertyValue(42), true, false};
	auto rangeLookupCount = std::make_shared<size_t>(0U);
	data.range_lookup_count = rangeLookupCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto lower = std::make_unique<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t{30}));
	auto upper = std::make_unique<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_LESS,
			std::make_unique<expressions::LiteralExpression>(int64_t{42}));
	auto root = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}),
			std::make_shared<expressions::BinaryOpExpression>(
					std::move(lower), expressions::BinaryOperatorType::BOP_AND, std::move(upper)));
	const auto optimized = graph::query::optimization::LogicalPlanOptimizer{}.optimize(std::move(root));
	ASSERT_NE(optimized.root, nullptr);
	EXPECT_EQ(optimized.rewrite_count, 1U);

	std::vector<uint64_t> nodeIds;
	static_cast<void>(runtime.execute(*optimized.root, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
		nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
		return true;
	}));
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{2U}));
	EXPECT_EQ(*rangeLookupCount, 1U);
}

TEST(LogicalGraphReadExecutorTest, OptimizedFilterNodeScanUsesCompositeRangeLookupAndRetainsPredicateValidation) {
	LogicalGraphFixtureData data = graphData();
	data.enable_composite_range_node_lookup = true;
	data.expected_composite_range_lookup = {
			{{"status", PropertyValue("online")}}, {"age", PropertyValue(30), PropertyValue(), true, true}};
	auto lookupCount = std::make_shared<size_t>(0U);
	data.composite_range_lookup_count = lookupCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto equality = std::make_unique<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "status"),
			expressions::BinaryOperatorType::BOP_EQUAL,
			std::make_unique<expressions::LiteralExpression>(std::string{"online"}));
	auto range = std::make_unique<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t{30}));
	auto root = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}),
			std::make_shared<expressions::BinaryOpExpression>(
					std::move(equality), expressions::BinaryOperatorType::BOP_AND, std::move(range)));
	const auto optimized = graph::query::optimization::LogicalPlanOptimizer{}.optimize(std::move(root));
	ASSERT_NE(optimized.root, nullptr);
	EXPECT_EQ(optimized.rewrite_count, 2U);

	std::vector<uint64_t> nodeIds;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			*optimized.root, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*lookupCount, 1U);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{1U, 0U, 2U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, ParserPushesParameterizedInlinePropertyEqualityIntoExactLookup) {
	LogicalGraphFixtureData data = graphData();
	data.enable_exact_node_lookup = true;
	auto exactLookupCount = std::make_shared<size_t>(0U);
	data.exact_lookup_count = exactLookupCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	graph::query::QueryPlan plan = parser.parseToLogical("MATCH (n:Person {age: $age}) RETURN n");
	ASSERT_NE(plan.root, nullptr);

	graph::query::QueryContext context;
	context.parameters.emplace("age", PropertyValue(42));
	std::vector<uint64_t> nodeIds;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			*plan.root, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			},
			context);
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*exactLookupCount, 1U);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{1U, 0U, 1U, 1U}));

	context.parameters["age"] = PropertyValue();
	EXPECT_EQ(runtime.execute(*plan.root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; }, context),
			  (LogicalPlanReadRuntimeStats{}));
	context.parameters.clear();
	EXPECT_THROW(static_cast<void>(runtime.execute(*plan.root, runtimeBounds(),
															 [](const LogicalPlanReadRow &) { return true; }, context)),
				 std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsArithmeticOverflowBeforeProducingAResult) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto overflow = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_ADD,
			std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::max()));
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::move(overflow), "overflow");
	logical::LogicalProject root(std::move(scan), std::move(items));
	EXPECT_THROW(
			static_cast<void>(runtime.execute(root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
			std::overflow_error);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeEvaluatesSupportedScalarExpressionsAndNullSemantics) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto binary = [](std::unique_ptr<expressions::Expression> left, expressions::BinaryOperatorType operation,
						   std::unique_ptr<expressions::Expression> right) {
		return sharedExpression(
				std::make_unique<expressions::BinaryOpExpression>(std::move(left), operation, std::move(right)));
	};
	const auto unary = [](expressions::UnaryOperatorType operation, std::unique_ptr<expressions::Expression> operand) {
		return sharedExpression(std::make_unique<expressions::UnaryOpExpression>(operation, std::move(operand)));
	};
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(2)),
							  expressions::BinaryOperatorType::BOP_ADD,
							  std::make_unique<expressions::LiteralExpression>(int64_t(3))),
					   "add");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(std::string("zy")),
							  expressions::BinaryOperatorType::BOP_ADD,
							  std::make_unique<expressions::LiteralExpression>(std::string("x"))),
					   "concat");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(9)),
							  expressions::BinaryOperatorType::BOP_SUBTRACT,
							  std::make_unique<expressions::LiteralExpression>(int64_t(4))),
					   "subtract");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(4)),
							  expressions::BinaryOperatorType::BOP_MULTIPLY,
							  std::make_unique<expressions::LiteralExpression>(int64_t(2))),
					   "multiply");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(7)),
							  expressions::BinaryOperatorType::BOP_DIVIDE,
							  std::make_unique<expressions::LiteralExpression>(int64_t(2))),
					   "divide");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(7)),
							  expressions::BinaryOperatorType::BOP_MODULO,
							  std::make_unique<expressions::LiteralExpression>(int64_t(4))),
					   "modulo");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(2)),
							  expressions::BinaryOperatorType::BOP_POWER,
							  std::make_unique<expressions::LiteralExpression>(int64_t(3))),
					   "power");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(2)),
							  expressions::BinaryOperatorType::BOP_LESS,
							  std::make_unique<expressions::LiteralExpression>(int64_t(3))),
					   "less");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(3)),
							  expressions::BinaryOperatorType::BOP_GREATER,
							  std::make_unique<expressions::LiteralExpression>(int64_t(2))),
					   "greater");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(2)),
							  expressions::BinaryOperatorType::BOP_LESS_EQUAL,
							  std::make_unique<expressions::LiteralExpression>(int64_t(2))),
					   "lessEqual");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(3)),
							  expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
							  std::make_unique<expressions::LiteralExpression>(int64_t(3))),
					   "greaterEqual");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(3)),
							  expressions::BinaryOperatorType::BOP_EQUAL,
							  std::make_unique<expressions::LiteralExpression>(int64_t(3))),
					   "equal");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(3)),
							  expressions::BinaryOperatorType::BOP_NOT_EQUAL,
							  std::make_unique<expressions::LiteralExpression>(int64_t(4))),
					   "notEqual");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(false),
							  expressions::BinaryOperatorType::BOP_AND,
							  std::make_unique<expressions::LiteralExpression>()),
					   "andFalse");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(true),
							  expressions::BinaryOperatorType::BOP_AND,
							  std::make_unique<expressions::LiteralExpression>(true)),
					   "andTrue");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(true),
							  expressions::BinaryOperatorType::BOP_OR,
							  std::make_unique<expressions::LiteralExpression>()),
					   "orTrue");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(false),
							  expressions::BinaryOperatorType::BOP_OR,
							  std::make_unique<expressions::LiteralExpression>(false)),
					   "orFalse");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(true),
							  expressions::BinaryOperatorType::BOP_XOR,
							  std::make_unique<expressions::LiteralExpression>(false)),
					   "xor");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(std::string("abcdef")),
							  expressions::BinaryOperatorType::BOP_STARTS_WITH,
							  std::make_unique<expressions::LiteralExpression>(std::string("abc"))),
					   "startsWith");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(std::string("abcdef")),
							  expressions::BinaryOperatorType::BOP_ENDS_WITH,
							  std::make_unique<expressions::LiteralExpression>(std::string("def"))),
					   "endsWith");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(std::string("abcdef")),
							  expressions::BinaryOperatorType::BOP_CONTAINS,
							  std::make_unique<expressions::LiteralExpression>(std::string("cd"))),
					   "contains");
	items.emplace_back(unary(expressions::UnaryOperatorType::UOP_MINUS,
							 std::make_unique<expressions::LiteralExpression>(int64_t(3))),
					   "negativeInteger");
	items.emplace_back(
			unary(expressions::UnaryOperatorType::UOP_MINUS, std::make_unique<expressions::LiteralExpression>(1.5)),
			"negativeDouble");
	items.emplace_back(
			unary(expressions::UnaryOperatorType::UOP_NOT, std::make_unique<expressions::LiteralExpression>(true)),
			"not");
	items.emplace_back(sharedExpression(std::make_unique<expressions::InExpression>(
							   std::make_unique<expressions::LiteralExpression>(int64_t(2)),
							   std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)})),
					   "in");
	items.emplace_back(sharedExpression(std::make_unique<expressions::IsNullExpression>(
							   std::make_unique<expressions::LiteralExpression>(), false)),
					   "isNull");
	items.emplace_back(sharedExpression(std::make_unique<expressions::IsNullExpression>(
							   std::make_unique<expressions::LiteralExpression>(int64_t(1)), true)),
					   "isNotNull");
	logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
	std::vector<LogicalPlanReadRow> rows;

	const LogicalPlanReadRuntimeStats stats =
			runtime.execute(root, runtimeBounds(), [&rows](const LogicalPlanReadRow &row) {
				rows.push_back(row);
				return true;
			});

	ASSERT_EQ(rows.size(), 1U);
	const auto &values = rows.front().values;
	EXPECT_EQ(std::get<PropertyValue>(values.at("add")), PropertyValue(5));
	EXPECT_EQ(std::get<PropertyValue>(values.at("concat")), PropertyValue("zyx"));
	EXPECT_EQ(std::get<PropertyValue>(values.at("subtract")), PropertyValue(5));
	EXPECT_EQ(std::get<PropertyValue>(values.at("multiply")), PropertyValue(8));
	EXPECT_EQ(std::get<PropertyValue>(values.at("divide")), PropertyValue(3.5));
	EXPECT_EQ(std::get<PropertyValue>(values.at("modulo")), PropertyValue(3));
	EXPECT_EQ(std::get<PropertyValue>(values.at("power")), PropertyValue(8.0));
	for (const char *name: {"less", "greater", "lessEqual", "greaterEqual", "equal", "notEqual", "andTrue", "orTrue",
							"xor", "startsWith", "endsWith", "contains", "isNull", "isNotNull"}) {
		EXPECT_EQ(std::get<PropertyValue>(values.at(name)), PropertyValue(true)) << name;
	}
	EXPECT_EQ(std::get<PropertyValue>(values.at("andFalse")), PropertyValue(false));
	EXPECT_EQ(std::get<PropertyValue>(values.at("orFalse")), PropertyValue(false));
	EXPECT_EQ(std::get<PropertyValue>(values.at("negativeInteger")), PropertyValue(-3));
	EXPECT_EQ(std::get<PropertyValue>(values.at("negativeDouble")), PropertyValue(-1.5));
	EXPECT_EQ(std::get<PropertyValue>(values.at("not")), PropertyValue(false));
	EXPECT_EQ(std::get<PropertyValue>(values.at("in")), PropertyValue(true));
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimePreservesThreeValuedFiltersAndStopsAtTheVisitorBoundary) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	auto nullFilter = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalSingleRow>(),
			sharedExpression(std::make_unique<expressions::LiteralExpression>()));
	size_t nullRows = 0U;
	const LogicalPlanReadRuntimeStats nullStats =
			runtime.execute(*nullFilter, runtimeBounds(), [&nullRows](const LogicalPlanReadRow &) {
				++nullRows;
				return true;
			});
	EXPECT_EQ(nullRows, 0U);
	EXPECT_EQ(nullStats, (LogicalPlanReadRuntimeStats{}));

	logical::LogicalSingleRow single;
	const LogicalPlanReadRuntimeStats stopped =
			runtime.execute(single, runtimeBounds(), [](const LogicalPlanReadRow &) { return false; });
	EXPECT_EQ(stopped, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsInvalidScalarOperationsBeforeEmittingRows) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto executeExpression = [&](std::unique_ptr<expressions::Expression> expression) {
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(sharedExpression(std::move(expression)), "value");
		logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
		return runtime.execute(root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; });
	};

	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(int64_t(1)),
						 expressions::BinaryOperatorType::BOP_DIVIDE,
						 std::make_unique<expressions::LiteralExpression>(int64_t(0))))),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(int64_t(1)),
						 expressions::BinaryOperatorType::BOP_MODULO,
						 std::make_unique<expressions::LiteralExpression>(0.5)))),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::min()),
						 expressions::BinaryOperatorType::BOP_MODULO,
						 std::make_unique<expressions::LiteralExpression>(int64_t(-1))))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<double>::max()),
						 expressions::BinaryOperatorType::BOP_MULTIPLY,
						 std::make_unique<expressions::LiteralExpression>(2.0)))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::UnaryOpExpression>(
						 expressions::UnaryOperatorType::UOP_MINUS,
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::min())))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::UnaryOpExpression>(
						 expressions::UnaryOperatorType::UOP_NOT,
						 std::make_unique<expressions::LiteralExpression>(int64_t(1))))),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::InExpression>(
						 std::make_unique<expressions::LiteralExpression>(int64_t(1)),
						 std::make_unique<expressions::LiteralExpression>(int64_t(1))))),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::min()),
						 expressions::BinaryOperatorType::BOP_SUBTRACT,
						 std::make_unique<expressions::LiteralExpression>(int64_t(1))))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::max()),
						 expressions::BinaryOperatorType::BOP_SUBTRACT,
						 std::make_unique<expressions::LiteralExpression>(int64_t(-1))))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::min()),
						 expressions::BinaryOperatorType::BOP_MULTIPLY,
						 std::make_unique<expressions::LiteralExpression>(int64_t(-1))))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::max()),
						 expressions::BinaryOperatorType::BOP_MULTIPLY,
						 std::make_unique<expressions::LiteralExpression>(int64_t(-2))))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::min()),
						 expressions::BinaryOperatorType::BOP_MULTIPLY,
						 std::make_unique<expressions::LiteralExpression>(int64_t(2))))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::min()),
						 expressions::BinaryOperatorType::BOP_MULTIPLY,
						 std::make_unique<expressions::LiteralExpression>(int64_t(-2))))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(int64_t(1)),
						 expressions::BinaryOperatorType::BOP_MODULO,
						 std::make_unique<expressions::LiteralExpression>(int64_t(0))))),
				 std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeCoversScalarNullAndFailClosedBoundaries) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto evaluate = [&](std::unique_ptr<expressions::Expression> expression) {
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(sharedExpression(std::move(expression)), "value");
		logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
		std::vector<LogicalPlanReadRow> rows;
		static_cast<void>(runtime.execute(root, runtimeBounds(), [&rows](const LogicalPlanReadRow &row) {
			rows.push_back(row);
			return true;
		}));
		if (rows.size() != 1U) {
			throw std::runtime_error("scalar test plan did not produce one row");
		}
		return std::get<PropertyValue>(rows.front().values.at("value"));
	};
	const auto binary = [](std::unique_ptr<expressions::Expression> left, expressions::BinaryOperatorType operation,
						   std::unique_ptr<expressions::Expression> right) {
		return std::make_unique<expressions::BinaryOpExpression>(std::move(left), operation, std::move(right));
	};

	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(),
							  expressions::BinaryOperatorType::BOP_ADD,
							  std::make_unique<expressions::LiteralExpression>(int64_t(1)))),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(1)),
							  expressions::BinaryOperatorType::BOP_ADD,
							  std::make_unique<expressions::LiteralExpression>())),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(),
							  expressions::BinaryOperatorType::BOP_EQUAL,
							  std::make_unique<expressions::LiteralExpression>(int64_t(1)))),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(1)),
							  expressions::BinaryOperatorType::BOP_EQUAL,
							  std::make_unique<expressions::LiteralExpression>())),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(1.5),
							  expressions::BinaryOperatorType::BOP_ADD,
							  std::make_unique<expressions::LiteralExpression>(2.0))),
			  PropertyValue(3.5));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(4.5),
							  expressions::BinaryOperatorType::BOP_SUBTRACT,
							  std::make_unique<expressions::LiteralExpression>(1.0))),
			  PropertyValue(3.5));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(-2)),
							  expressions::BinaryOperatorType::BOP_ADD,
							  std::make_unique<expressions::LiteralExpression>(int64_t(-3)))),
			  PropertyValue(-5));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(2)),
							  expressions::BinaryOperatorType::BOP_ADD,
							  std::make_unique<expressions::LiteralExpression>(int64_t(-1)))),
			  PropertyValue(1));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(-5)),
							  expressions::BinaryOperatorType::BOP_SUBTRACT,
							  std::make_unique<expressions::LiteralExpression>(int64_t(-2)))),
			  PropertyValue(-3));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(0)),
							  expressions::BinaryOperatorType::BOP_MULTIPLY,
							  std::make_unique<expressions::LiteralExpression>(int64_t(42)))),
			  PropertyValue(0));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(42)),
							  expressions::BinaryOperatorType::BOP_MULTIPLY,
							  std::make_unique<expressions::LiteralExpression>(int64_t(0)))),
			  PropertyValue(0));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(2)),
							  expressions::BinaryOperatorType::BOP_MULTIPLY,
							  std::make_unique<expressions::LiteralExpression>(int64_t(-3)))),
			  PropertyValue(-6));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(-2)),
							  expressions::BinaryOperatorType::BOP_MULTIPLY,
							  std::make_unique<expressions::LiteralExpression>(int64_t(3)))),
			  PropertyValue(-6));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(-2)),
							  expressions::BinaryOperatorType::BOP_MULTIPLY,
							  std::make_unique<expressions::LiteralExpression>(int64_t(-3)))),
			  PropertyValue(6));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(false),
							  expressions::BinaryOperatorType::BOP_AND,
							  std::make_unique<expressions::LiteralExpression>(true))),
			  PropertyValue(false));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(true),
							  expressions::BinaryOperatorType::BOP_AND,
							  std::make_unique<expressions::LiteralExpression>(false))),
			  PropertyValue(false));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(true),
							  expressions::BinaryOperatorType::BOP_AND,
							  std::make_unique<expressions::LiteralExpression>())),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(false),
							  expressions::BinaryOperatorType::BOP_OR,
							  std::make_unique<expressions::LiteralExpression>())),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(false),
							  expressions::BinaryOperatorType::BOP_OR,
							  std::make_unique<expressions::LiteralExpression>(true))),
			  PropertyValue(true));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(),
							  expressions::BinaryOperatorType::BOP_XOR,
							  std::make_unique<expressions::LiteralExpression>(true))),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(false),
							  expressions::BinaryOperatorType::BOP_XOR,
							  std::make_unique<expressions::LiteralExpression>(true))),
			  PropertyValue(true));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(std::string("abcdef")),
							  expressions::BinaryOperatorType::BOP_CONTAINS,
							  std::make_unique<expressions::LiteralExpression>(std::string("zz")))),
			  PropertyValue(false));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(),
							  expressions::BinaryOperatorType::BOP_STARTS_WITH,
							  std::make_unique<expressions::LiteralExpression>(std::string("z")))),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(std::string("z")),
							  expressions::BinaryOperatorType::BOP_STARTS_WITH,
							  std::make_unique<expressions::LiteralExpression>())),
			  PropertyValue());
	EXPECT_EQ(evaluate(std::make_unique<expressions::InExpression>(
					  std::make_unique<expressions::LiteralExpression>(int64_t(3)),
					  std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)})),
			  PropertyValue(false));
	EXPECT_EQ(evaluate(std::make_unique<expressions::InExpression>(std::make_unique<expressions::LiteralExpression>(),
																   std::vector<PropertyValue>{PropertyValue(1)})),
			  PropertyValue());
	EXPECT_EQ(evaluate(std::make_unique<expressions::UnaryOpExpression>(
					  expressions::UnaryOperatorType::UOP_NOT, std::make_unique<expressions::LiteralExpression>())),
			  PropertyValue());

	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(std::string("x")),
								 expressions::BinaryOperatorType::BOP_ADD,
								 std::make_unique<expressions::LiteralExpression>(int64_t(1)))),
			  PropertyValue("x1"));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(1)),
								 expressions::BinaryOperatorType::BOP_STARTS_WITH,
								 std::make_unique<expressions::LiteralExpression>(int64_t(1)))),
			  PropertyValue(true));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(std::string("x")),
								 expressions::BinaryOperatorType::BOP_STARTS_WITH,
								 std::make_unique<expressions::LiteralExpression>(int64_t(1)))),
			  PropertyValue(false));
	EXPECT_THROW(
			static_cast<void>(evaluate(binary(
					std::make_unique<expressions::LiteralExpression>(std::numeric_limits<double>::infinity()),
					expressions::BinaryOperatorType::BOP_ADD, std::make_unique<expressions::LiteralExpression>(1.0)))),
			std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate(binary(std::make_unique<expressions::LiteralExpression>(0.5),
												   expressions::BinaryOperatorType::BOP_MODULO,
												   std::make_unique<expressions::LiteralExpression>(int64_t(1))))),
				 std::runtime_error);
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(1)),
								 expressions::BinaryOperatorType::BOP_REGEX_MATCH,
								 std::make_unique<expressions::LiteralExpression>(int64_t(1)))),
			  PropertyValue(true));
	EXPECT_THROW(static_cast<void>(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(1)),
												   expressions::BinaryOperatorType::BOP_IN,
												   std::make_unique<expressions::LiteralExpression>(int64_t(1))))),
				 std::logic_error);
	EXPECT_THROW(static_cast<void>(evaluate(std::make_unique<expressions::UnaryOpExpression>(
						 expressions::UnaryOperatorType::UOP_MINUS,
						 std::make_unique<expressions::LiteralExpression>(std::string("x"))))),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate(std::make_unique<expressions::UnaryOpExpression>(
						 static_cast<expressions::UnaryOperatorType>(99),
						 std::make_unique<expressions::LiteralExpression>(int64_t(1))))),
				 std::logic_error);
}

TEST(LogicalGraphReadExecutorTest, EvaluatesEntityIntrospectionFunctionsAgainstSnapshotBindings) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);

	LogicalPlanReadRow row;
	row.values.emplace("node", snapshot->readNode(1U));
	row.values.emplace("edge", snapshot->readEdge(10U));
	row.values.emplace("scalar", PropertyValue(7));
	row.values.emplace("map", PropertyValue(PropertyValue::MapType{{"enabled", PropertyValue(true)}}));
	graph::query::QueryContext context;
	const auto evaluate = [&](std::string function, std::string variable) {
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>(std::move(variable)));
		expressions::FunctionCallExpression expression(std::move(function), std::move(arguments));
		return LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(*snapshot, expression, row, runtimeBounds(), context);
	};

	EXPECT_EQ(evaluate("id", "node"), LogicalPlanReadValue(PropertyValue(1)));
	EXPECT_EQ(evaluate("id", "edge"), LogicalPlanReadValue(PropertyValue(10)));
	EXPECT_EQ(evaluate("type", "edge"), LogicalPlanReadValue(PropertyValue("KNOWS")));
	EXPECT_EQ(evaluate("labels", "node"),
			  LogicalPlanReadValue(PropertyValue(std::vector<PropertyValue>{PropertyValue("Person"), PropertyValue("VIP")})));
	const PropertyValue keys = std::get<PropertyValue>(evaluate("keys", "node"));
	EXPECT_EQ(keys.getType(), PropertyType::LIST);
	EXPECT_EQ(keys.getList().size(), 2U);
	const PropertyValue properties = std::get<PropertyValue>(evaluate("properties", "edge"));
	EXPECT_EQ(properties.getType(), PropertyType::MAP);
	EXPECT_EQ(properties.getMap().at("weight"), PropertyValue(2));
	EXPECT_EQ(evaluate("properties", "map"),
			  LogicalPlanReadValue(PropertyValue(PropertyValue::MapType{{"enabled", PropertyValue(true)}})));
	EXPECT_EQ(evaluate("keys", "map"),
			  LogicalPlanReadValue(PropertyValue(std::vector<PropertyValue>{PropertyValue("enabled")})));

	EXPECT_THROW(static_cast<void>(evaluate("labels", "edge")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate("type", "node")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate("id", "scalar")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate("keys", "scalar")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate("properties", "scalar")), std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeResolvesNodeEdgeAndMapPropertiesWithoutDuplicateReads) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(1U).properties.emplace_back("attributes",
												  PropertyValue(PropertyValue::MapType{{"answer", PropertyValue(42)}}));
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto nodeScan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"});
	std::vector<logical::LogicalProjectItem> nodeItems;
	nodeItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "missing"), "missing");
	nodeItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "age"), "age");
	nodeItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "attributes"), "attributes");
	nodeItems.emplace_back(std::make_shared<expressions::BinaryOpExpression>(
								   std::make_unique<expressions::VariableReferenceExpression>("n"),
								   expressions::BinaryOperatorType::BOP_EQUAL,
								   std::make_unique<expressions::LiteralExpression>(int64_t(1))),
						   "nodeIdMatches");
	auto nodeProject = std::make_unique<logical::LogicalProject>(std::move(nodeScan), std::move(nodeItems));
	std::vector<logical::LogicalProjectItem> mapItems;
	mapItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("attributes", "missing"),
						  "mapMissing");
	mapItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("attributes", "answer"), "answer");
	std::vector<std::unique_ptr<expressions::Expression>> propertyArguments;
	propertyArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("attributes"));
	mapItems.emplace_back(std::make_shared<expressions::FunctionCallExpression>(
							 std::string("properties"), std::move(propertyArguments)),
						  "allAttributes");
	std::vector<std::unique_ptr<expressions::Expression>> keyArguments;
	keyArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("attributes"));
	mapItems.emplace_back(std::make_shared<expressions::FunctionCallExpression>(std::string("keys"),
							 std::move(keyArguments)),
						  "attributeKeys");
	mapItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("age", "anything"),
						  "scalarMissing");
	logical::LogicalProject mapRoot(std::move(nodeProject), std::move(mapItems));
	std::vector<LogicalPlanReadRow> mapRows;
	const LogicalPlanReadRuntimeStats mapStats =
			runtime.execute(mapRoot, runtimeBounds(), [&mapRows](const LogicalPlanReadRow &row) {
				mapRows.push_back(row);
				return true;
			});
	ASSERT_EQ(mapRows.size(), 1U);
	EXPECT_EQ(std::get<PropertyValue>(mapRows.front().values.at("answer")), PropertyValue(42));
	EXPECT_EQ(std::get<PropertyValue>(mapRows.front().values.at("mapMissing")), PropertyValue());
	EXPECT_EQ(std::get<PropertyValue>(mapRows.front().values.at("allAttributes")),
			  PropertyValue(PropertyValue::MapType{{"answer", PropertyValue(42)}}));
	EXPECT_EQ(std::get<PropertyValue>(mapRows.front().values.at("attributeKeys")),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue("answer")}));
	EXPECT_EQ(std::get<PropertyValue>(mapRows.front().values.at("scalarMissing")), PropertyValue());
	EXPECT_EQ(mapStats, (LogicalPlanReadRuntimeStats{3U, 0U, 1U, 1U}));

	auto edgeScan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"});
	auto traversal = std::make_unique<logical::LogicalTraversal>(std::move(edgeScan), "n", "r", "m", "KNOWS", "out");
	std::vector<logical::LogicalProjectItem> edgeItems;
	edgeItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r", "missing"), "missing");
	edgeItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r", "weight"), "weight");
	edgeItems.emplace_back(std::make_shared<expressions::BinaryOpExpression>(
								   std::make_unique<expressions::VariableReferenceExpression>("r"),
								   expressions::BinaryOperatorType::BOP_EQUAL,
								   std::make_unique<expressions::LiteralExpression>(int64_t(10))),
						   "edgeIdMatches");
	logical::LogicalProject edgeRoot(std::move(traversal), std::move(edgeItems));
	std::vector<LogicalPlanReadRow> edgeRows;
	const LogicalPlanReadRuntimeStats edgeStats =
			runtime.execute(edgeRoot, runtimeBounds(), [&edgeRows](const LogicalPlanReadRow &row) {
				edgeRows.push_back(row);
				return true;
			});
	ASSERT_EQ(edgeRows.size(), 1U);
	EXPECT_EQ(std::get<PropertyValue>(edgeRows.front().values.at("weight")), PropertyValue(2));
	EXPECT_EQ(std::get<PropertyValue>(edgeRows.front().values.at("missing")), PropertyValue());
	EXPECT_EQ(std::get<PropertyValue>(edgeRows.front().values.at("edgeIdMatches")), PropertyValue(true));
	// The topology-only traversal resolves the two requested fields through the
	// selected-property API and does not materialize a complete Edge Property map.
	EXPECT_EQ(edgeStats, (LogicalPlanReadRuntimeStats{3U, 2U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeUsesStructuralDistinctKeysAndEnforcesItsBound) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(1U).properties.emplace_back("ambiguous",
												  PropertyValue(std::vector<PropertyValue>{PropertyValue("a, b")}));
	data.nodes.at(2U).properties.emplace_back(
			"ambiguous", PropertyValue(std::vector<PropertyValue>{PropertyValue("a"), PropertyValue("b")}));
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "ambiguous"), "value");
	logical::LogicalProject structuralRoot(std::move(scan), std::move(items), true);
	std::vector<PropertyValue> values;
	const LogicalPlanReadRuntimeStats structuralStats =
			runtime.execute(structuralRoot, runtimeBounds(), [&values](const LogicalPlanReadRow &row) {
				values.push_back(std::get<PropertyValue>(row.values.at("value")));
				return true;
			});
	EXPECT_EQ(values, (std::vector<PropertyValue>{
							  PropertyValue(std::vector<PropertyValue>{PropertyValue("a, b")}),
							  PropertyValue(std::vector<PropertyValue>{PropertyValue("a"), PropertyValue("b")})}));
	EXPECT_EQ(structuralStats, (LogicalPlanReadRuntimeStats{3U, 0U, 2U, 2U}));

	auto boundedScan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<logical::LogicalProjectItem> boundedItems;
	boundedItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "age"), "age");
	logical::LogicalProject boundedRoot(std::move(boundedScan), std::move(boundedItems), true);
	LogicalPlanReadRuntimeBounds limitedDistinct = runtimeBounds();
	limitedDistinct.maximum_distinct_rows = 1U;
	EXPECT_THROW(static_cast<void>(runtime.execute(boundedRoot, limitedDistinct,
												   [](const LogicalPlanReadRow &) { return true; })),
				 std::runtime_error);

	auto duplicateScan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<logical::LogicalProjectItem> duplicateItems;
	duplicateItems.emplace_back(std::make_shared<expressions::LiteralExpression>("same"), "value");
	logical::LogicalProject duplicateRoot(std::move(duplicateScan), std::move(duplicateItems), true);
	size_t duplicateRows = 0U;
	const LogicalPlanReadRuntimeStats duplicateStats =
			runtime.execute(duplicateRoot, runtimeBounds(), [&duplicateRows](const LogicalPlanReadRow &) {
				++duplicateRows;
				return true;
			});
	EXPECT_EQ(duplicateRows, 1U);
	EXPECT_EQ(duplicateStats, (LogicalPlanReadRuntimeStats{3U, 0U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeAppliesAndValidatesAllNodeScanPredicateForms) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto countMatches = [&runtime](std::unique_ptr<logical::LogicalNodeScan> scan) {
		size_t count = 0U;
		static_cast<void>(runtime.execute(*scan, runtimeBounds(), [&count](const LogicalPlanReadRow &) {
			++count;
			return true;
		}));
		return count;
	};

	EXPECT_EQ(countMatches(std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Missing"})), 0U);
	EXPECT_EQ(countMatches(std::make_unique<logical::LogicalNodeScan>(
					  "n", std::vector<std::string>{"Person"},
					  std::vector<std::pair<std::string, PropertyValue>>{{"missing", PropertyValue(1)}})),
			  0U);
	EXPECT_EQ(countMatches(std::make_unique<logical::LogicalNodeScan>(
					  "n", std::vector<std::string>{"Person"},
					  std::vector<std::pair<std::string, PropertyValue>>{{"age", PropertyValue(99)}})),
			  0U);

	auto belowRange = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	belowRange->setRangePredicates({logical::RangePredicate{"age", PropertyValue(50), PropertyValue(), true, true}});
	EXPECT_EQ(countMatches(std::move(belowRange)), 0U);
	auto aboveRange = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	aboveRange->setRangePredicates({logical::RangePredicate{"age", PropertyValue(), PropertyValue(40), true, true}});
	EXPECT_EQ(countMatches(std::move(aboveRange)), 1U);
	auto exclusiveRange = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	exclusiveRange->setRangePredicates(
			{logical::RangePredicate{"age", PropertyValue(42), PropertyValue(42), false, false}});
	EXPECT_EQ(countMatches(std::move(exclusiveRange)), 0U);
	auto absentRange = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	absentRange->setRangePredicates(
			{logical::RangePredicate{"missing", PropertyValue(1), PropertyValue(), true, true}});
	EXPECT_EQ(countMatches(std::move(absentRange)), 0U);

	auto invalidLabel = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{""});
	EXPECT_THROW(static_cast<void>(countMatches(std::move(invalidLabel))), std::invalid_argument);
	auto duplicateLabel = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person", "Person"});
	EXPECT_THROW(static_cast<void>(countMatches(std::move(duplicateLabel))), std::invalid_argument);
	auto emptyProperty = std::make_unique<logical::LogicalNodeScan>(
			"n", std::vector<std::string>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"", PropertyValue(1)}});
	EXPECT_THROW(static_cast<void>(countMatches(std::move(emptyProperty))), std::invalid_argument);
	auto invalidRange = std::make_unique<logical::LogicalNodeScan>("n");
	invalidRange->setRangePredicates({logical::RangePredicate{"", PropertyValue(1), PropertyValue(), true, true}});
	EXPECT_THROW(static_cast<void>(countMatches(std::move(invalidRange))), std::invalid_argument);
	auto invalidComposite = std::make_unique<logical::LogicalNodeScan>("n");
	invalidComposite->setCompositeEquality(logical::CompositeEqualityPredicate{{"age"}, {}});
	EXPECT_THROW(static_cast<void>(countMatches(std::move(invalidComposite))), std::invalid_argument);
	auto emptyComposite = std::make_unique<logical::LogicalNodeScan>("n");
	emptyComposite->setCompositeEquality(logical::CompositeEqualityPredicate{{""}, {PropertyValue(42)}});
	EXPECT_THROW(static_cast<void>(countMatches(std::move(emptyComposite))), std::invalid_argument);
	auto unmatchedComposite = std::make_unique<logical::LogicalNodeScan>("n");
	unmatchedComposite->setCompositeEquality(logical::CompositeEqualityPredicate{{"age"}, {PropertyValue(99)}});
	EXPECT_EQ(countMatches(std::move(unmatchedComposite)), 0U);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeBuildsDistinctKeysForEverySupportedPropertyAndEntityValue) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(1U).properties.emplace_back("null", PropertyValue());
	data.nodes.at(1U).properties.emplace_back("boolean", PropertyValue(false));
	data.nodes.at(1U).properties.emplace_back("double", PropertyValue(-0.0));
	data.nodes.at(1U).properties.emplace_back(
			"map", PropertyValue(PropertyValue::MapType{{"z", PropertyValue(1)}, {"a", PropertyValue("value")}}));
	data.nodes.at(1U).properties.emplace_back("date", PropertyValue(TemporalDate{1}));
	data.nodes.at(1U).properties.emplace_back("datetime", PropertyValue(TemporalDateTime{1}));
	data.nodes.at(1U).properties.emplace_back("duration", PropertyValue(TemporalDuration{1, 2, 3}));
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds propertyBounds = runtimeBounds();
	propertyBounds.maximum_property_entries = 16U;
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"});
	std::vector<logical::LogicalProjectItem> values;
	for (const std::string &key: {"null", "boolean", "double", "map", "date", "datetime", "duration"}) {
		values.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", key), key);
	}
	logical::LogicalProject propertyRoot(std::move(scan), std::move(values), true);
	const LogicalPlanReadRuntimeStats propertyStats =
			runtime.execute(propertyRoot, propertyBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(propertyStats, (LogicalPlanReadRuntimeStats{3U, 0U, 1U, 1U}));

	auto nodeScan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"});
	std::vector<logical::LogicalProjectItem> nodeItems;
	nodeItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "node");
	logical::LogicalProject nodeRoot(std::move(nodeScan), std::move(nodeItems), true);
	const LogicalPlanReadRuntimeStats nodeStats =
			runtime.execute(nodeRoot, propertyBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(nodeStats, (LogicalPlanReadRuntimeStats{3U, 0U, 0U, 1U}));

	auto edgeScan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"});
	auto traversal = std::make_unique<logical::LogicalTraversal>(std::move(edgeScan), "n", "r", "m", "KNOWS", "out");
	std::vector<logical::LogicalProjectItem> edgeItems;
	edgeItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r"), "edge");
	logical::LogicalProject edgeRoot(std::move(traversal), std::move(edgeItems), true);
	const LogicalPlanReadRuntimeStats edgeStats =
			runtime.execute(edgeRoot, propertyBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(edgeStats, (LogicalPlanReadRuntimeStats{3U, 2U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsMalformedOperatorsExpressionsAndBounds) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto visitor = [](const LogicalPlanReadRow &) { return true; };

	logical::LogicalFilter missingPredicate(std::make_unique<logical::LogicalSingleRow>(), nullptr);
	EXPECT_THROW(static_cast<void>(runtime.execute(missingPredicate, runtimeBounds(), visitor)), std::invalid_argument);
	logical::LogicalProject emptyProject(std::make_unique<logical::LogicalSingleRow>(), {});
	EXPECT_THROW(static_cast<void>(runtime.execute(emptyProject, runtimeBounds(), visitor)), std::invalid_argument);
	std::vector<logical::LogicalProjectItem> missingExpressionItems;
	missingExpressionItems.emplace_back(std::shared_ptr<expressions::Expression>{}, "value");
	logical::LogicalProject missingExpressionProject(std::make_unique<logical::LogicalSingleRow>(),
													 std::move(missingExpressionItems));
	EXPECT_THROW(static_cast<void>(runtime.execute(missingExpressionProject, runtimeBounds(), visitor)),
				 std::invalid_argument);
	std::vector<logical::LogicalProjectItem> emptyAliasItems;
	emptyAliasItems.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t(1)), "");
	logical::LogicalProject emptyAliasProject(std::make_unique<logical::LogicalSingleRow>(),
											  std::move(emptyAliasItems));
	EXPECT_THROW(static_cast<void>(runtime.execute(emptyAliasProject, runtimeBounds(), visitor)),
				 std::invalid_argument);
	std::vector<logical::LogicalProjectItem> duplicateAliasItems;
	duplicateAliasItems.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t(1)), "value");
	duplicateAliasItems.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t(2)), "value");
	logical::LogicalProject duplicateAliasProject(std::make_unique<logical::LogicalSingleRow>(),
												  std::move(duplicateAliasItems));
	EXPECT_THROW(static_cast<void>(runtime.execute(duplicateAliasProject, runtimeBounds(), visitor)),
				 std::invalid_argument);
	logical::LogicalLimit missingChild(std::unique_ptr<logical::LogicalOperator>{}, 1);
	EXPECT_THROW(static_cast<void>(runtime.execute(missingChild, runtimeBounds(), visitor)), std::runtime_error);

	const auto evaluateFailure = [&](std::shared_ptr<expressions::Expression> expression) {
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::move(expression), "value");
		logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
		return runtime.execute(root, runtimeBounds(), visitor);
	};
	EXPECT_THROW(static_cast<void>(evaluateFailure(
						 sharedExpression(std::make_unique<expressions::VariableReferenceExpression>("missing")))),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluateFailure(
						 sharedExpression(std::make_unique<expressions::VariableReferenceExpression>("value", "")))),
				 std::invalid_argument);
	EXPECT_THROW(
			static_cast<void>(evaluateFailure(sharedExpression(std::make_unique<expressions::FunctionCallExpression>(
					"unsupported", std::vector<std::unique_ptr<expressions::Expression>>{})))),
			std::runtime_error);

	const auto mutateBounds = [](LogicalPlanReadRuntimeBounds &bounds, size_t variant) {
		if (variant == 0U) {
			bounds.maximum_node_records = 0U;
		} else if (variant == 1U) {
			bounds.maximum_node_page_records = 0U;
		} else if (variant == 2U) {
			bounds.maximum_metadata_pages_per_node_page = 0U;
		} else if (variant == 3U) {
			bounds.maximum_edges_per_node = 0U;
		} else if (variant == 4U) {
			bounds.maximum_edge_records = 0U;
		} else if (variant == 5U) {
			bounds.maximum_adjacency_page_records = 0U;
		} else if (variant == 6U) {
			bounds.maximum_property_entries = 0U;
		} else if (variant == 7U) {
			bounds.maximum_rows = 0U;
		} else if (variant == 8U) {
			bounds.maximum_distinct_rows = 0U;
		} else if (variant == 9U) {
			bounds.maximum_node_page_records = bounds.maximum_node_records + 1U;
		} else {
			bounds.maximum_adjacency_page_records = bounds.maximum_edges_per_node + 1U;
		}
	};
	for (size_t variant: {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U}) {
		LogicalPlanReadRuntimeBounds invalid = runtimeBounds();
		mutateBounds(invalid, variant);
		logical::LogicalSingleRow root;
		EXPECT_THROW(static_cast<void>(runtime.execute(root, invalid, visitor)), std::invalid_argument);
	}
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsMalformedNodePagesAndLogicalShapes) {
	{
		LogicalGraphFixtureData data = graphData();
		data.return_oversized_node_page = true;
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n");
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.return_nonmonotonic_node_page = true;
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		LogicalPlanReadRuntimeBounds pageBounds = runtimeBounds();
		pageBounds.maximum_node_page_records = 2U;
		logical::LogicalNodeScan scan("n");
		EXPECT_THROW(
				static_cast<void>(runtime.execute(scan, pageBounds, [](const LogicalPlanReadRow &) { return true; })),
				std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.return_empty_nonterminal_page = true;
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n");
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
	}
	{
		TestLogicalGraphBackend backend(graphData(), true);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n");
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
	}
	{
		TestLogicalGraphBackend backend(graphData());
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan invalidScan("");
		EXPECT_THROW(static_cast<void>(runtime.execute(invalidScan, runtimeBounds(),
													   [](const LogicalPlanReadRow &) { return true; })),
					 std::invalid_argument);
		logical::LogicalSkip invalidSkip(std::make_unique<logical::LogicalSingleRow>(), -1);
		EXPECT_THROW(static_cast<void>(runtime.execute(invalidSkip, runtimeBounds(),
													   [](const LogicalPlanReadRow &) { return true; })),
					 std::invalid_argument);
		logical::LogicalLimit zeroLimit(std::make_unique<logical::LogicalNodeScan>("n"), 0);
		const LogicalPlanReadRuntimeStats zeroStats =
				runtime.execute(zeroLimit, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; });
		EXPECT_EQ(zeroStats, (LogicalPlanReadRuntimeStats{}));
		UnsupportedLogicalReadOperator unsupported;
		EXPECT_THROW(static_cast<void>(runtime.execute(unsupported, runtimeBounds(),
													   [](const LogicalPlanReadRow &) { return true; })),
					 std::logic_error);
	}
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsMalformedAdjacencyPagesAndBindings) {
	const auto traversal = []() {
		return std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "m", "",
				"out");
	};
	const auto setCorruption = [](LogicalGraphFixtureData &data, size_t variant) {
		if (variant == 0U) {
			data.return_empty_nonterminal_adjacency_page = true;
		} else if (variant == 1U) {
			data.return_oversized_adjacency_page = true;
		} else if (variant == 2U) {
			data.return_invalid_adjacency_page_endpoint = true;
		} else {
			data.return_invalid_adjacency_continuation = true;
			data.invalid_adjacency_continuation_mode = static_cast<uint8_t>(variant - 3U);
		}
	};
	for (size_t variant: {0U, 1U, 2U, 3U, 4U, 5U}) {
		LogicalGraphFixtureData data = graphData();
		setCorruption(data, variant);
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		std::unique_ptr<logical::LogicalTraversal> root = traversal();
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(*root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
	}

	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(2U).labels = {"Person", "Person"};
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		std::unique_ptr<logical::LogicalTraversal> root = traversal();
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(*root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
	}
	{
		TestLogicalGraphBackend backend(graphData());
		LogicalPlanReadRuntime runtime(backend);
		auto invalidTargetLabel = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "m",
				"KNOWS", "out", std::vector<std::string>{""});
		EXPECT_THROW(static_cast<void>(runtime.execute(*invalidTargetLabel, runtimeBounds(),
													   [](const LogicalPlanReadRow &) { return true; })),
					 std::invalid_argument);
		auto root = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "m", "",
				"sideways");
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(*root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
					 std::invalid_argument);
	}
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsCorruptEntitiesPropertiesAndTraversalBindings) {
	const auto expectPropertyScanFailure = [](LogicalGraphFixtureData data,
											  LogicalPlanReadRuntimeBounds bounds = runtimeBounds()) {
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n", {}, {{"age", PropertyValue(42)}});
		return runtime.execute(scan, bounds, [](const LogicalPlanReadRow &) { return true; });
	};
	const auto expectTraversalFailure = [](LogicalGraphFixtureData data) {
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		auto root = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "m",
				"KNOWS", "out", std::vector<std::string>{}, std::vector<std::pair<std::string, PropertyValue>>{},
				std::unordered_map<std::string, PropertyValue>{{"weight", PropertyValue(2)}});
		return runtime.execute(*root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; });
	};

	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).node_id = 0U;
		EXPECT_THROW(static_cast<void>(expectPropertyScanFailure(std::move(data))), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).properties = {{"age", PropertyValue(42)}, {"age", PropertyValue(43)}};
		EXPECT_THROW(static_cast<void>(expectPropertyScanFailure(std::move(data))), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).labels = {""};
		EXPECT_THROW(static_cast<void>(expectPropertyScanFailure(std::move(data))), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).properties.front().key.clear();
		EXPECT_THROW(static_cast<void>(expectPropertyScanFailure(std::move(data))), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).properties = {
				{"age", PropertyValue(42)}, {"status", PropertyValue("online")}, {"tier", PropertyValue("gold")}};
		LogicalPlanReadRuntimeBounds limitedProperties = runtimeBounds();
		limitedProperties.maximum_property_entries = 1U;
		EXPECT_THROW(static_cast<void>(expectPropertyScanFailure(std::move(data), limitedProperties)),
					 std::runtime_error);
	}

	const auto corruptEdge = [](LogicalGraphEdge &edge, size_t variant) {
		if (variant == 0U) {
			edge.edge_id = 0U;
		} else if (variant == 1U) {
			edge.target_node_id = 0U;
		} else {
			edge.properties = {{"", PropertyValue(1)}};
		}
	};
	for (size_t variant: {0U, 1U, 2U}) {
		LogicalGraphFixtureData data = graphData();
		corruptEdge(data.edges.at(10U), variant);
		EXPECT_THROW(static_cast<void>(expectTraversalFailure(std::move(data))), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.edges.at(10U).type.clear();
		EXPECT_NO_THROW(static_cast<void>(expectTraversalFailure(std::move(data))));
	}
	{
		TestLogicalGraphBackend backend(graphData());
		LogicalPlanReadRuntime runtime(backend);
		auto scalarProject = std::make_unique<logical::LogicalProject>(
				std::make_unique<logical::LogicalSingleRow>(),
				std::vector<logical::LogicalProjectItem>{logical::LogicalProjectItem(
						std::make_shared<expressions::LiteralExpression>(int64_t(1)), "n")});
		logical::LogicalTraversal root(std::move(scalarProject), "n", "r", "m", "", "out");
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
	}
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeCoversTraversalFilteringBindingsAndTerminalPages) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds completeBounds = runtimeBounds();
	completeBounds.maximum_node_records = 4U;
	std::vector<uint64_t> nodeIds;
	logical::LogicalNodeScan allNodes("n");
	const LogicalPlanReadRuntimeStats nodeStats =
			runtime.execute(allNodes, completeBounds, [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U, 2U, 3U}));
	EXPECT_EQ(nodeStats, (LogicalPlanReadRuntimeStats{3U, 0U, 0U, 3U}));

	auto deviceTraversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Device"}), "n", "r", "m", "",
			"out");
	const LogicalPlanReadRuntimeStats emptyAdjacency =
			runtime.execute(*deviceTraversal, completeBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(emptyAdjacency, (LogicalPlanReadRuntimeStats{3U, 0U, 0U, 0U}));

	auto typeMismatch = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "m", "MISSING",
			"out");
	const LogicalPlanReadRuntimeStats mismatchedType =
			runtime.execute(*typeMismatch, completeBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(mismatchedType, (LogicalPlanReadRuntimeStats{3U, 2U, 0U, 0U}));

	auto targetMismatch = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "m", "KNOWS",
			"out", std::vector<std::string>{"VIP"});
	const LogicalPlanReadRuntimeStats mismatchedTarget =
			runtime.execute(*targetMismatch, completeBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(mismatchedTarget, (LogicalPlanReadRuntimeStats{3U, 2U, 0U, 0U}));

	auto conflictingBinding = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "n", "m", "KNOWS",
			"out");
	const LogicalPlanReadRuntimeStats conflictStats =
			runtime.execute(*conflictingBinding, completeBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(conflictStats, (LogicalPlanReadRuntimeStats{3U, 2U, 0U, 0U}));

	auto targetBindingConflict = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "", "n", "KNOWS",
			"out");
	const LogicalPlanReadRuntimeStats targetConflictStats =
			runtime.execute(*targetBindingConflict, completeBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(targetConflictStats, (LogicalPlanReadRuntimeStats{3U, 2U, 0U, 0U}));

	auto stoppedTraversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "m", "KNOWS",
			"out");
	const LogicalPlanReadRuntimeStats stoppedStats =
			runtime.execute(*stoppedTraversal, completeBounds, [](const LogicalPlanReadRow &) { return false; });
	EXPECT_EQ(stoppedStats, (LogicalPlanReadRuntimeStats{1U, 1U, 0U, 1U}));

	LogicalGraphFixtureData loopData = graphData();
	loopData.edges.emplace(13U, LogicalGraphEdge{13U, 1U, 1U, "LOOP"});
	TestLogicalGraphBackend loopBackend(std::move(loopData));
	LogicalPlanReadRuntime loopRuntime(loopBackend);
	LogicalPlanReadRuntimeBounds loopBounds = completeBounds;
	loopBounds.maximum_edges_per_node = 5U;
	auto bothTraversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "", "m", "", "both");
	std::vector<uint64_t> bothTargets;
	const LogicalPlanReadRuntimeStats bothStats =
			loopRuntime.execute(*bothTraversal, loopBounds, [&bothTargets](const LogicalPlanReadRow &row) {
				bothTargets.push_back(std::get<LogicalGraphNode>(row.values.at("m")).node_id);
				return true;
			});
	EXPECT_EQ(bothTargets, (std::vector<uint64_t>{2U, 3U, 1U, 2U}));
	EXPECT_EQ(bothStats, (LogicalPlanReadRuntimeStats{3U, 5U, 0U, 4U}));

	auto selfBinding = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "", "n", "LOOP",
			"out");
	const LogicalPlanReadRuntimeStats selfStats =
			loopRuntime.execute(*selfBinding, loopBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(selfStats, (LogicalPlanReadRuntimeStats{3U, 3U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, VerifiedExactProjectionReusesRepeatedFieldsAndReadsOnlyUncoveredFields) {
	for (const bool includeUncovered : {false, true}) {
		SCOPED_TRACE(includeUncovered);
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).labels = {"Account917"};
		data.nodes.at(1U).properties = {{"ranking", PropertyValue(42)}, {"status", PropertyValue("online")}};
		data.enable_native_exact_node_candidates = true;
		data.expected_exact_labels = std::vector<std::string>{"Account917"};
		data.expected_exact_properties = std::vector<std::pair<std::string, PropertyValue>>{{"ranking", PropertyValue(42)}};
		data.native_snapshot_generation = 17U;
		data.exact_candidate_result = IndexCandidateBatch{
				{{1U, false, 0U, 0U, 0U}}, 17U, true, true, {{"ranking", {PropertyValue(42)}}}, true};
		auto pointReads = std::make_shared<size_t>(0U);
		auto columnReads = std::make_shared<size_t>(0U);
		auto batchReads = std::make_shared<size_t>(0U);
		data.node_read_count = pointReads;
		data.node_property_column_read_count = columnReads;
		data.node_batch_read_count = batchReads;
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Account917"},
				std::vector<std::pair<std::string, PropertyValue>>{{"ranking", PropertyValue(42)}});
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "ranking"), "first");
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "ranking"), "second");
		if (includeUncovered)
			items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "status"), "status");
		logical::LogicalProject project(std::move(scan), std::move(items));
		size_t rows = 0U;
		static_cast<void>(runtime.execute(project, runtimeBounds(), [&](const LogicalPlanReadRow &row) {
			++rows;
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("first")), PropertyValue(42));
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("second")), PropertyValue(42));
			if (includeUncovered) EXPECT_EQ(std::get<PropertyValue>(row.values.at("status")), PropertyValue("online"));
			return true;
		}));
		EXPECT_EQ(rows, 1U);
		EXPECT_EQ(*pointReads, 0U);
		EXPECT_EQ(*batchReads, 0U);
		EXPECT_EQ(*columnReads, includeUncovered ? 1U : 0U);
	}
}

namespace {
std::shared_ptr<graph::query::execution::detail::ResourceAccount> sharedRuntimeAccount(size_t maximum) {
	using namespace graph::query::execution::detail;
	auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(maximum));
	auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(maximum));
	return std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, maximum);
}
}

TEST(LogicalGraphReadExecutorTest, QueryResultRetainsSharedAccountAfterExecutionAndBackendDestruction) {
	auto account = sharedRuntimeAccount(1024U * 1024U);
	graph::query::QueryResult result;
	{
		TestLogicalGraphBackend backend(graphData());
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"});
		graph::query::QueryContext context; context.resource_account = account;
		result = runtime.executeToQueryResult(scan, runtimeBounds(), context);
		ASSERT_EQ(result.rowCount(), 2U);
		ASSERT_GT(result.retainedResourceBytes(), 0U);
		EXPECT_EQ(account->used(), result.retainedResourceBytes());
	}
	EXPECT_EQ(account->used(), result.retainedResourceBytes());
	EXPECT_EQ(result.getRows()[0].at("n").asNode().getId(), 1U);
	const size_t retained = account->used();
	{
		graph::query::QueryResult copy(result);
		EXPECT_EQ(account->used(), 2U * retained);
	}
	EXPECT_EQ(account->used(), retained);
	result = graph::query::QueryResult{};
	EXPECT_EQ(account->used(), 0U);
}

TEST(LogicalGraphReadExecutorTest, SharedAccountDenialRefundsColumnsAndBatchWithoutPublishingOutput) {
	using graph::query::execution::detail::ResourceAdmissionDenied;
	auto account = sharedRuntimeAccount(1000U);
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"});
	graph::query::QueryContext context; context.resource_account = account;
	EXPECT_THROW((void)runtime.executeToQueryResult(scan, runtimeBounds(), context), ResourceAdmissionDenied);
	EXPECT_EQ(account->used(), 0U);
}

TEST(LogicalGraphReadExecutorTest, SharedAccountRefundsExecutionStateOnVisitorFailure) {
	auto account = sharedRuntimeAccount(1024U * 1024U);
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"});
	graph::query::QueryContext context; context.resource_account = account;
	EXPECT_THROW((void)runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) -> bool {
		throw std::runtime_error("consumer failure");
	}, context), std::runtime_error);
	EXPECT_EQ(account->used(), 0U);
}

TEST(LogicalGraphReadExecutorTest, LongOutputColumnsRejectBeforeOpeningOrReadingSnapshot) {
    using graph::query::execution::detail::ResourceAdmissionDenied;
    auto account = sharedRuntimeAccount(4096U);
    auto reads = std::make_shared<size_t>(0U);
    auto data = graphData(); data.node_read_count = reads;
    TestLogicalGraphBackend backend(std::move(data));
    LogicalPlanReadRuntime runtime(backend);
    logical::LogicalNodeScan scan(std::string(8192U, 'n'), std::vector<std::string>{"Person"});
    graph::query::QueryContext context; context.resource_account = account;
    EXPECT_THROW((void)runtime.executeToQueryResult(scan, runtimeBounds(), context), ResourceAdmissionDenied);
    EXPECT_EQ(*reads, 0U); EXPECT_EQ(account->used(), 0U);
    EXPECT_EQ(scan.getVariable().size(), 8192U);
}
