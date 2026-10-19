#include <gtest/gtest.h>

#include "graph/query/QueryGuard.hpp"
#include "graph/query/execution/LogicalAggregateAccumulator.hpp"
#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"

namespace graph::query::execution {
namespace {
using namespace direct_edge_test;

struct ColumnAggregateTraceScope {
	bool previous = debug::PerfTrace::areValuesEnabled();
	ColumnAggregateTraceScope() { debug::PerfTrace::reset(); debug::PerfTrace::setValueCollectionEnabled(true); }
	~ColumnAggregateTraceScope() { debug::PerfTrace::setValueCollectionEnabled(previous); }
};

std::shared_ptr<expressions::Expression> rankReference() {
	return std::make_shared<expressions::VariableReferenceExpression>("n", "rank");
}

std::vector<logical::LogicalAggItem> numericItems() {
	return {{"count", nullptr, "rows"}, {"count", rankReference(), "present"},
		{"sum", rankReference(), "total"}, {"avg", rankReference(), "mean"}};
}

TEST(LogicalPlanNodeColumnAggregateTest, FiltersAndAggregatesOneColumnWithoutMaterializingInputRows) {
	DirectEdgeGraph graph = directGraph();
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n");
	scan->setRangePredicates({{"rank", PropertyValue(1), PropertyValue(3), false, true}});
	logical::LogicalAggregate aggregate(std::move(scan), {}, numericItems());
	ColumnAggregateTraceScope tracing;
	const auto result = runtime.executeToQueryResult(aggregate, directBounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("rows").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(row.at("present").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(row.at("total").asPrimitive(), PropertyValue(5));
	EXPECT_EQ(row.at("mean").asPrimitive(), PropertyValue(2.5));
	const auto trace = debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(trace.contains("logical_plan.aggregate.column_input_rows"));
	EXPECT_EQ(trace.at("logical_plan.aggregate.column_input_rows").totalValue, 2);
	ASSERT_TRUE(trace.contains("logical_plan.aggregate.typed_columns"));
	EXPECT_EQ(trace.at("logical_plan.aggregate.typed_columns").totalValue, 1);
	EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
	ASSERT_EQ(graph.node_property_column_batch_keys.size(), 1U);
	EXPECT_EQ(graph.node_property_column_batch_keys.front(), (std::vector<std::string>{"rank"}));
	EXPECT_TRUE(graph.node_read_ids.empty());
}

TEST(LogicalPlanNodeColumnAggregateTest, MixedNumericNullAndMissingInputsMatchTheScalarPath) {
	for (bool native : {false, true}) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_native_node_ref_scans = native;
		graph.nodes.at(2U).properties = {{"rank", PropertyValue(0.5)}};
		graph.nodes.emplace(4U, LogicalGraphNode{4U, {}, {{"rank", PropertyValue{}}}});
		graph.nodes.emplace(5U, LogicalGraphNode{5U, {}, {}});
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {}, numericItems());
		const auto result = runtime.executeToQueryResult(aggregate, directBounds());
		ASSERT_EQ(result.rowCount(), 1U);
		const auto &row = result.getRows().front();
		EXPECT_EQ(row.at("rows").asPrimitive(), PropertyValue(5));
		EXPECT_EQ(row.at("present").asPrimitive(), PropertyValue(3));
		EXPECT_EQ(row.at("total").asPrimitive(), PropertyValue(4.5));
		EXPECT_EQ(row.at("mean").asPrimitive(), PropertyValue(1.5));
	}
}

TEST(LogicalPlanNodeColumnAggregateTest, FloatingAccumulationUsesPhysicalScanOrderRatherThanSortedLookupOrder) {
	DirectEdgeGraph graph = directGraph();
	graph.nodes.at(1U).properties = {{"rank", PropertyValue(1e16)}};
	graph.nodes.at(2U).properties = {{"rank", PropertyValue(-1e16)}};
	graph.nodes.at(3U).properties = {{"rank", PropertyValue(1.0)}};
	graph.node_ref_scan_order = {3U, 1U, 2U};
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
		{{"sum", rankReference(), "total"}});
	const auto result = runtime.executeToQueryResult(aggregate, directBounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(0.0));
}

TEST(LogicalPlanNodeColumnAggregateTest, EmptyAndNullParameterInputsStillEmitOneAggregateRow) {
	DirectEdgeGraph graph = directGraph();
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n");
	scan->setParameterPropertyPredicates({{"rank", "needle"}});
	logical::LogicalAggregate aggregate(std::move(scan), {}, numericItems());
	for (const PropertyValue &needle : {PropertyValue{}, PropertyValue(100)}) {
		QueryContext context;
		context.parameters.emplace("needle", needle);
		const auto result = runtime.executeToQueryResult(aggregate, directBounds(), context);
		ASSERT_EQ(result.rowCount(), 1U);
		const auto &row = result.getRows().front();
		EXPECT_EQ(row.at("rows").asPrimitive(), PropertyValue(0));
		EXPECT_EQ(row.at("present").asPrimitive(), PropertyValue(0));
		EXPECT_EQ(row.at("total").asPrimitive().getType(), PropertyType::NULL_TYPE);
		EXPECT_EQ(row.at("mean").asPrimitive().getType(), PropertyType::NULL_TYPE);
	}
}

TEST(LogicalPlanNodeColumnAggregateTest, NullParameterCountFastPathEmitsZeroInsteadOfDroppingTheResultRow) {
	DirectEdgeGraph graph = directGraph();
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n");
	scan->setParameterPropertyPredicates({{"rank", "needle"}});
	logical::LogicalAggregate aggregate(std::move(scan), {}, {{"count", nullptr, "rows"}});
	QueryContext context;
	context.parameters.emplace("needle", PropertyValue{});
	const auto result = runtime.executeToQueryResult(aggregate, directBounds(), context);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("rows").asPrimitive(), PropertyValue(0));
}

TEST(LogicalPlanNodeColumnAggregateTest, InvalidNumericValuesAndMalformedColumnsFailWithoutPublishingResults) {
	DirectEdgeGraph graph = directGraph();
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {}, numericItems());
	graph.nodes.at(2U).properties = {{"rank", PropertyValue("invalid")}};
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(aggregate, directBounds())), std::invalid_argument);
	graph.nodes.at(2U).properties = {{"rank", PropertyValue(2)}};
	graph.truncate_node_property_batch = true;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(aggregate, directBounds())), std::runtime_error);
	graph.truncate_node_property_batch = false;
	EXPECT_EQ(runtime.executeToQueryResult(aggregate, directBounds()).getRows().front().at("total").asPrimitive(), PropertyValue(6));
}

TEST(LogicalPlanNodeColumnAggregateTest, DistinctAggregatesKeepTheExistingConsumer) {
	DirectEdgeGraph graph = directGraph();
	graph.nodes.at(3U).properties = {{"rank", PropertyValue(2)}};
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
		{{"sum", rankReference(), "total", true}});
	ColumnAggregateTraceScope tracing;
	const auto result = runtime.executeToQueryResult(aggregate, directBounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(3));
	EXPECT_FALSE(debug::PerfTrace::snapshotAndReset().contains("logical_plan.aggregate.column_input_rows"));
}

TEST(LogicalPlanNodeColumnAggregateTest, RowBoundsFailBeforeReturningAPartialAggregateAndPermitRetry) {
	DirectEdgeGraph graph = directGraph();
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {}, numericItems());
	auto bounds = directBounds();
	bounds.maximum_rows = 2U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(aggregate, bounds)), std::runtime_error);
	EXPECT_EQ(runtime.executeToQueryResult(aggregate, directBounds()).getRows().front().at("total").asPrimitive(), PropertyValue(6));
}

TEST(LogicalPlanNodeColumnAggregateTest, DistinctPredicateAndAggregateColumnsStayBoundAcrossMultipleBatches) {
	DirectEdgeGraph graph;
	for (uint64_t id = 1U; id <= 600U; ++id)
		graph.nodes.emplace(id, LogicalGraphNode{id, {}, {{"selected", PropertyValue(true)}, {"rank", PropertyValue(1)}}});
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{},
		std::vector<std::pair<std::string, PropertyValue>>{{"selected", PropertyValue(true)}});
	logical::LogicalAggregate aggregate(std::move(scan), {}, numericItems());
	auto bounds = directBounds();
	bounds.maximum_node_records = 600U;
	bounds.maximum_rows = 600U;
	ColumnAggregateTraceScope tracing;
	const auto result = runtime.executeToQueryResult(aggregate, bounds);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(600));
	EXPECT_EQ(result.getRows().front().at("present").asPrimitive(), PropertyValue(600));
	const auto trace = debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(trace.contains("logical_plan.node_column_consumer.batches"));
	EXPECT_GT(trace.at("logical_plan.node_column_consumer.batches").totalValue, 1);
	EXPECT_EQ(trace.at("logical_plan.aggregate.column_input_rows").totalValue, 600);
	for (const auto &keys : graph.node_property_column_batch_keys)
		EXPECT_EQ(keys, (std::vector<std::string>{"selected", "rank"}));
	EXPECT_TRUE(graph.node_read_ids.empty());
}

TEST(LogicalPlanNodeColumnAggregateTest, IntegerOverflowFailsBothColumnAndScalarConsumersAndAllowsQueryRetry) {
	for (bool native : {false, true}) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_native_node_ref_scans = native;
		graph.nodes.at(1U).properties = {{"rank", PropertyValue(std::numeric_limits<int64_t>::max())}};
		graph.nodes.at(2U).properties = {{"rank", PropertyValue(1)}};
		graph.nodes.at(3U).properties = {{"rank", PropertyValue{}}};
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {}, numericItems());
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(aggregate, directBounds())), std::overflow_error);
		graph.nodes.at(2U).properties = {{"rank", PropertyValue(-1)}};
		const auto result = runtime.executeToQueryResult(aggregate, directBounds());
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(std::numeric_limits<int64_t>::max() - 1));
	}
}

TEST(LogicalPlanNodeColumnAggregateTest, NumericTypeInferenceIsLocalToEachSourceBatch) {
	DirectEdgeGraph graph;
	for (uint64_t id = 1U; id <= 600U; ++id)
		graph.nodes.emplace(id, LogicalGraphNode{id, {}, {{"rank", id <= 512U ? PropertyValue(1) : PropertyValue(0.5)}}});
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {}, numericItems());
	auto bounds = directBounds();
	bounds.maximum_node_records = 600U;
	bounds.maximum_rows = 600U;
	ColumnAggregateTraceScope tracing;
	const auto result = runtime.executeToQueryResult(aggregate, bounds);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("rows").asPrimitive(), PropertyValue(600));
	EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(556.0));
	EXPECT_EQ(result.getRows().front().at("mean").asPrimitive(), PropertyValue(556.0 / 600.0));
	const auto trace = debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(trace.contains("logical_plan.aggregate.typed_columns"));
	EXPECT_EQ(trace.at("logical_plan.aggregate.typed_columns").totalValue, 2);
}

TEST(LogicalPlanNodeColumnAggregateTest, ExtremaAndSharedCountConsumeFilteredColumnsAndOwnStringResults) {
	for (bool native : {false, true}) {
		DirectEdgeGraph graph = directGraph();
		const PropertyValue small(std::string(512U, 'a'));
		const PropertyValue large(std::string(4096U, 'z'));
		graph.enable_native_node_ref_scans = native;
		graph.nodes.at(1U).properties = {{"rank", small}};
		graph.nodes.at(2U).properties = {{"rank", large}};
		graph.nodes.at(3U).properties = {{"rank", PropertyValue{}}};
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		scan->setRangePredicates({{"rank", PropertyValue{}, PropertyValue{}, true, true}});
		logical::LogicalAggregate aggregate(std::move(scan), {},
			{{"count", rankReference(), "present"}, {"min", rankReference(), "low"}, {"max", rankReference(), "high"}});
		ColumnAggregateTraceScope tracing;
		const auto result = runtime.executeToQueryResult(aggregate, directBounds());
		graph.nodes.clear();
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("present").asPrimitive(), PropertyValue(2));
		EXPECT_EQ(result.getRows().front().at("low").asPrimitive(), small);
		EXPECT_EQ(result.getRows().front().at("high").asPrimitive(), large);
		const auto trace = debug::PerfTrace::snapshotAndReset();
		EXPECT_EQ(trace.contains("logical_plan.aggregate.column_input_rows"), native);
	}
}

TEST(LogicalPlanNodeColumnAggregateTest, TypedExtremaPreserveFirstNanAndSignedZeroFromPhysicalScanOrder) {
	DirectEdgeGraph graph = directGraph();
	const double nan = std::numeric_limits<double>::quiet_NaN();
	graph.nodes.at(1U).properties = {{"rank", PropertyValue(nan)}};
	graph.nodes.at(2U).properties = {{"rank", PropertyValue(-0.0)}};
	graph.nodes.at(3U).properties = {{"rank", PropertyValue(0.0)}};
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
		{{"min", rankReference(), "low"}, {"max", rankReference(), "high"}});
	const auto firstNan = runtime.executeToQueryResult(aggregate, directBounds());
	EXPECT_TRUE(std::isnan(std::get<double>(firstNan.getRows().front().at("low").asPrimitive().getVariant())));
	EXPECT_TRUE(std::isnan(std::get<double>(firstNan.getRows().front().at("high").asPrimitive().getVariant())));
	graph.node_ref_scan_order = {2U, 3U, 1U};
	const auto firstNegativeZero = runtime.executeToQueryResult(aggregate, directBounds());
	EXPECT_TRUE(std::signbit(std::get<double>(firstNegativeZero.getRows().front().at("low").asPrimitive().getVariant())));
	EXPECT_TRUE(std::signbit(std::get<double>(firstNegativeZero.getRows().front().at("high").asPrimitive().getVariant())));
}

TEST(LogicalPlanNodeColumnAggregateTest, WideExtremaRespectSharedBudgetAndRecoverForAnAdmissibleQuery) {
	DirectEdgeGraph graph = directGraph();
	for (auto &[id, node] : graph.nodes) node.properties = {{"rank", PropertyValue(std::string(4096U, static_cast<char>('a' + id)))}};
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
		{{"min", rankReference(), "low"}, {"max", rankReference(), "high"}});
	auto bounds = directBounds();
	bounds.maximum_materialized_bytes = 2048U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(aggregate, bounds)), std::runtime_error);
	const auto result = runtime.executeToQueryResult(aggregate, directBounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("low").asPrimitive(), PropertyValue(std::string(4096U, 'b')));
	EXPECT_EQ(result.getRows().front().at("high").asPrimitive(), PropertyValue(std::string(4096U, 'd')));
}

TEST(LogicalPlanNodeColumnAggregateTest, ReplacingAWideExtremumWithAShortValuePreservesOwningResults) {
	DirectEdgeGraph graph = directGraph();
	const PropertyValue initial(std::string(4096U, 'm'));
	graph.nodes.at(1U).properties = {{"rank", initial}};
	graph.nodes.at(2U).properties = {{"rank", PropertyValue("z")}};
	graph.nodes.at(3U).properties = {{"rank", PropertyValue("a")}};
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
		{{"min", rankReference(), "low"}, {"max", rankReference(), "high"}});
	const auto result = runtime.executeToQueryResult(aggregate, directBounds());
	graph.nodes.clear();
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("low").asPrimitive(), PropertyValue("a"));
	EXPECT_EQ(result.getRows().front().at("high").asPrimitive(), PropertyValue("z"));
}

TEST(LogicalPlanNodeColumnAggregateTest, EmptyNullAndMissingExtremaEmitOneNullResultRow) {
	for (bool empty : {false, true}) {
		DirectEdgeGraph graph = directGraph();
		if (empty) graph.nodes.clear();
		else {
			graph.nodes.at(1U).properties = {{"rank", PropertyValue{}}};
			graph.nodes.at(2U).properties.clear();
			graph.nodes.at(3U).properties.clear();
		}
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
			{{"count", rankReference(), "present"}, {"min", rankReference(), "low"}, {"max", rankReference(), "high"}});
		const auto result = runtime.executeToQueryResult(aggregate, directBounds());
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("present").asPrimitive(), PropertyValue(0));
		EXPECT_EQ(result.getRows().front().at("low").asPrimitive().getType(), PropertyType::NULL_TYPE);
		EXPECT_EQ(result.getRows().front().at("high").asPrimitive().getType(), PropertyType::NULL_TYPE);
	}
}

TEST(LogicalPlanNodeColumnAggregateTest, OptionalNumericBuffersYieldToAnOwningExtremumWithoutReplayingOtherAggregates) {
	DirectEdgeGraph graph;
	const PropertyValue text(std::string(4096U, 'm'));
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {}, {{"rank", PropertyValue(7)}, {"text", text}}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {}, {{"rank", PropertyValue(0)}, {"text", PropertyValue("a")}}});
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
		{{"sum", rankReference(), "total"}, {"min", std::make_shared<expressions::VariableReferenceExpression>("n", "text"), "low"}});
	ColumnAggregateTraceScope tracing;
	size_t successfulFallbacks = 0U;
	for (size_t bytes = 4096U; bytes <= 16384U; bytes += 64U) {
		auto bounds = directBounds();
		bounds.maximum_node_records = 2U;
		bounds.maximum_node_page_records = 2U;
		bounds.maximum_rows = 2U;
		bounds.maximum_distinct_rows = 2U;
		bounds.maximum_materialized_bytes = bytes;
		debug::PerfTrace::reset();
		try {
			const auto result = runtime.executeToQueryResult(aggregate, bounds);
			ASSERT_EQ(result.rowCount(), 1U);
			EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(7));
			EXPECT_EQ(result.getRows().front().at("low").asPrimitive(), PropertyValue("a"));
			const auto trace = debug::PerfTrace::snapshotAndReset();
			if (trace.contains("logical_plan.aggregate.optional_buffers_released")) ++successfulFallbacks;
		} catch (const std::runtime_error &error) {
			ASSERT_NE(std::string_view(error.what()).find("byte bound"), std::string_view::npos);
		}
	}
	EXPECT_GT(successfulFallbacks, 0U);
}

TEST(LogicalPlanNodeColumnAggregateTest, AGuardBudgetExceptionDuringExtremumCopyIsNotRetriedAsBufferPressure) {
	DirectEdgeGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {}, {{"rank", PropertyValue(7)}, {"text", PropertyValue(std::string(4096U, 'm'))}}});
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
		{{"sum", rankReference(), "total"}, {"min", std::make_shared<expressions::VariableReferenceExpression>("n", "text"), "low"}});
	ColumnAggregateTraceScope tracing;
	bool cancelled = false;
	QueryGuard guard(0, 1, [&] {
		// The first immediate guard after typed preparation belongs to the
		// extremum copy. Its exception has the same type as byte admission.
		const auto trace = debug::PerfTrace::snapshotAndReset();
		if (trace.contains("logical_plan.aggregate.typed_columns")) {
			cancelled = true;
			throw MaterializedByteLimitExceeded();
		}
		return size_t{0U};
	});
	QueryContext context;
	context.guard = &guard;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(aggregate, directBounds(), context)), MaterializedByteLimitExceeded);
	EXPECT_TRUE(cancelled);
	EXPECT_FALSE(debug::PerfTrace::snapshotAndReset().contains("logical_plan.aggregate.optional_buffers_released"));
	EXPECT_EQ(runtime.executeToQueryResult(aggregate, directBounds()).getRows().front().at("total").asPrimitive(), PropertyValue(7));
}

} // namespace
TEST(LogicalPlanNodeColumnAggregateTest, CollectionsAndStatisticsConsumeColumnsInPhysicalOrder) {
	for (bool native : {false, true}) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_native_node_ref_scans = native;
		graph.node_ref_scan_order = {3U, 1U, 2U};
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
			{{"collect", rankReference(), "values"}, {"count", rankReference(), "present"},
			 {"sum", rankReference(), "total"}, {"stdev", rankReference(), "sample"},
			 {"stdevp", rankReference(), "population"}});
		ColumnAggregateTraceScope tracing;
		const auto result = runtime.executeToQueryResult(aggregate, directBounds());
		const auto &row = result.getRows().front();
		EXPECT_EQ(row.at("values").asPrimitive(), PropertyValue(std::vector<PropertyValue>{
			PropertyValue(native ? 3 : 1), PropertyValue(native ? 1 : 2), PropertyValue(native ? 2 : 3)}));
		EXPECT_EQ(row.at("present").asPrimitive(), PropertyValue(3));
		EXPECT_EQ(row.at("total").asPrimitive(), PropertyValue(6));
		EXPECT_EQ(row.at("sample").asPrimitive(), PropertyValue(1.0));
		EXPECT_EQ(row.at("population").asPrimitive(), PropertyValue(std::sqrt(2.0 / 3.0)));
		const auto trace = debug::PerfTrace::snapshotAndReset();
		EXPECT_EQ(trace.contains("logical_plan.aggregate.column_input_rows"), native);
		if (native) EXPECT_EQ(trace.at("logical_plan.aggregate.typed_columns").totalValue, 0);
	}
}

TEST(LogicalPlanNodeColumnAggregateTest, PercentileColumnsUseValidatedParametersAndScalarSemantics) {
	for (bool native : {false, true}) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_native_node_ref_scans = native;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto percentile = std::make_shared<expressions::LiteralExpression>(PropertyValue(0.25));
		logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
			{{"percentileDisc", rankReference(), "discrete", false, percentile},
			 {"percentileCont", rankReference(), "continuous", false, percentile}});
		ColumnAggregateTraceScope tracing;
		const auto result = runtime.executeToQueryResult(aggregate, directBounds());
		EXPECT_EQ(result.getRows().front().at("discrete").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(result.getRows().front().at("continuous").asPrimitive(), PropertyValue(1.5));
		EXPECT_EQ(debug::PerfTrace::snapshotAndReset().contains("logical_plan.aggregate.column_input_rows"), native);
	}
}

TEST(LogicalPlanNodeColumnAggregateTest, ExtendedAggregatesSkipMissingValuesAndOwnCollectedPayloads) {
	DirectEdgeGraph graph = directGraph();
	graph.nodes.at(1U).properties = {{"rank", PropertyValue("owned")}};
	graph.nodes.at(2U).properties = {{"rank", PropertyValue{}}};
	graph.nodes.at(3U).properties.clear();
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
		{{"collect", rankReference(), "values"}, {"count", rankReference(), "present"}});
	const auto result = runtime.executeToQueryResult(aggregate, directBounds());
	graph.nodes.clear();
	EXPECT_EQ(result.getRows().front().at("values").asPrimitive(),
		PropertyValue(std::vector<PropertyValue>{PropertyValue("owned")}));
	EXPECT_EQ(result.getRows().front().at("present").asPrimitive(), PropertyValue(1));
}

TEST(LogicalPlanNodeColumnAggregateTest, ExtendedAggregateFailuresPermitFreshExecution) {
	for (const std::string function : {"collect", "percentileDisc", "percentileCont", "stdev", "stdevp"}) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		const bool percentileFunction = function.starts_with("percentile");
		auto parameter = percentileFunction ? std::make_shared<expressions::LiteralExpression>(PropertyValue(0.5)) : nullptr;
		logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
			{{function, rankReference(), "result", false, parameter}});
		auto bounds = directBounds();
		bounds.maximum_materialized_bytes = 1U;
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(aggregate, bounds)), std::runtime_error);
		EXPECT_EQ(runtime.executeToQueryResult(aggregate, directBounds()).rowCount(), 1U);
		if (function != "collect") {
			graph.nodes.at(2U).properties = {{"rank", PropertyValue("invalid")}};
			EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(aggregate, directBounds())), std::invalid_argument);
			graph.nodes.at(2U).properties = {{"rank", PropertyValue(2)}};
			EXPECT_EQ(runtime.executeToQueryResult(aggregate, directBounds()).rowCount(), 1U);
		}
		graph.nodes.clear();
		const auto empty = runtime.executeToQueryResult(aggregate, directBounds());
		ASSERT_EQ(empty.rowCount(), 1U);
		EXPECT_EQ(empty.getRows().front().at("result").asPrimitive(), function == "collect" ?
			PropertyValue(std::vector<PropertyValue>{}) : PropertyValue{});
	}
}

TEST(LogicalPlanNodeColumnAggregateTest, StandardDeviationUsesOneSharedTypedColumn) {
	DirectEdgeGraph graph = directGraph();
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
		{{"stdev", rankReference(), "sample"}, {"stdevp", rankReference(), "population"}});
	ColumnAggregateTraceScope tracing;
	const auto result = runtime.executeToQueryResult(aggregate, directBounds());
	EXPECT_EQ(result.getRows().front().at("sample").asPrimitive(), PropertyValue(1.0));
	EXPECT_EQ(result.getRows().front().at("population").asPrimitive(), PropertyValue(std::sqrt(2.0 / 3.0)));
	const auto trace = debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(trace.contains("logical_plan.aggregate.column_input_rows"));
	EXPECT_EQ(trace.at("logical_plan.aggregate.column_input_rows").totalValue, 3);
	ASSERT_TRUE(trace.contains("logical_plan.aggregate.typed_columns"));
	EXPECT_EQ(trace.at("logical_plan.aggregate.typed_columns").totalValue, 1);
}

TEST(LogicalPlanNodeColumnAggregateTest, DeviationStateSpansIntegerAndDoubleSourceBatches) {
	DirectEdgeGraph graph;
	StDevAccumulator expectedSample(false), expectedPopulation(true);
	for (uint64_t id = 1U; id <= 600U; ++id) {
		const PropertyValue value = id <= 512U ? PropertyValue(static_cast<int64_t>(id)) : PropertyValue(0.5);
		graph.nodes.emplace(id, LogicalGraphNode{id, {}, {{"rank", value}}});
		expectedSample.update(value);
		expectedPopulation.update(value);
	}
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
		{{"stdev", rankReference(), "sample"}, {"stdevp", rankReference(), "population"}});
	auto bounds = directBounds();
	bounds.maximum_node_records = 600U;
	bounds.maximum_rows = 600U;
	ColumnAggregateTraceScope tracing;
	const auto result = runtime.executeToQueryResult(aggregate, bounds);
	EXPECT_EQ(result.getRows().front().at("sample").asPrimitive(), expectedSample.getResult());
	EXPECT_EQ(result.getRows().front().at("population").asPrimitive(), expectedPopulation.getResult());
	EXPECT_EQ(debug::PerfTrace::snapshotAndReset().at("logical_plan.aggregate.typed_columns").totalValue, 2);
}

TEST(LogicalPlanNodeColumnAggregateTest, NestedExecutionKeepsQueryBudgetsAndTaskStatisticsIndependent) {
	DirectEdgeGraph graph = directGraph();
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan outer("n");
	logical::LogicalAggregate inner(std::make_unique<logical::LogicalNodeScan>("n"), {}, numericItems());
	size_t delivered = 0U;
	const auto stats = runtime.execute(outer, directBounds(), [&](const LogicalPlanReadRow &row) {
		EXPECT_TRUE(row.values.contains("n"));
		++delivered;
		auto limited = directBounds();
		limited.maximum_materialized_bytes = 1U;
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(inner, limited)), std::runtime_error);
		const auto nested = runtime.executeToQueryResult(inner, directBounds());
		EXPECT_EQ(nested.getRows().front().at("total").asPrimitive(), PropertyValue(6));
		EXPECT_TRUE(row.values.contains("n"));
		return true;
	});
	EXPECT_EQ(delivered, 3U);
	EXPECT_EQ(stats.nodes_scanned, 3U);
	EXPECT_EQ(stats.rows_emitted, 3U);
}

TEST(LogicalPlanNodeColumnAggregateTest, NestedInjectedRowsRestoreOuterBindingsAfterAnInnerFailure) {
	DirectEdgeGraph graph = directGraph();
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	logical::LogicalSingleRow plan;
	LogicalPlanReadRow outer, inner;
	outer.values.emplace("value", PropertyValue(7));
	inner.values.emplace("value", PropertyValue(9));
	const auto stats = runtime.executeWithSnapshotAndInputRow(*snapshot, plan, directBounds(), outer,
		[&](const LogicalPlanReadRow &row) {
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("value")), PropertyValue(7));
			EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, plan, directBounds(), inner,
				[](const LogicalPlanReadRow &) -> bool { throw std::runtime_error("injected failure"); }, QueryContext{})), std::runtime_error);
			const auto nestedStats = runtime.executeWithSnapshotAndInputRow(*snapshot, plan, directBounds(), inner,
				[](const LogicalPlanReadRow &nested) {
					EXPECT_EQ(std::get<PropertyValue>(nested.values.at("value")), PropertyValue(9));
					return true;
				}, QueryContext{});
			EXPECT_EQ(nestedStats.rows_emitted, 1U);
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("value")), PropertyValue(7));
			return true;
		}, QueryContext{});
	EXPECT_EQ(stats.rows_emitted, 1U);
	EXPECT_EQ(stats.nodes_scanned, 0U);
}


namespace {
std::shared_ptr<expressions::Expression> directField(const char *name) {
    return std::make_shared<expressions::VariableReferenceExpression>("n", name);
}
std::vector<std::vector<PropertyValue>> observedCells(const QueryResult &result) {
    std::vector<std::vector<PropertyValue>> rows;
    for (const auto &row : result.getRows()) {
        rows.emplace_back();
        for (const auto &name : result.getColumns()) rows.back().push_back(row.at(name).asPrimitive());
    }
    return rows;
}
DirectEdgeGraph groupingGraph() {
    DirectEdgeGraph graph;
    graph.nodes.emplace(1, LogicalGraphNode{1, {}, {{"country", PropertyValue("CN")}, {"active", PropertyValue(true)}, {"rank", PropertyValue(1)}}});
    graph.nodes.emplace(2, LogicalGraphNode{2, {}, {{"country", PropertyValue("CN")}, {"active", PropertyValue(false)}, {"rank", PropertyValue(2)}}});
    graph.nodes.emplace(3, LogicalGraphNode{3, {}, {{"country", PropertyValue("CN")}, {"active", PropertyValue(true)}, {"rank", PropertyValue(1)}}});
    graph.nodes.emplace(4, LogicalGraphNode{4, {}, {{"country", PropertyValue("US")}, {"active", PropertyValue(true)}, {"rank", PropertyValue(4)}}});
    graph.nodes.emplace(5, LogicalGraphNode{5, {}, {{"active", PropertyValue(false)}, {"rank", PropertyValue(8)}}});
    graph.nodes.emplace(6, LogicalGraphNode{6, {}, {{"country", PropertyValue()}, {"active", PropertyValue(false)}, {"rank", PropertyValue()}}});
    return graph;
}
}

TEST(LogicalPlanDirectColumnAggregateTest, CompleteTupleGroupingDistinctAndNumericStateMatchLegacy) {
    std::vector<std::vector<PropertyValue>> expected;
    for (const bool direct : {false, true}) {
        auto graph = groupingGraph(); graph.enable_direct_property_cells = direct;
        DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
        logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"),
            {directField("country"), directField("active")},
            {{"count", nullptr, "rows"}, {"count", directField("rank"), "distinct", true},
             {"sum", directField("rank"), "sum"}}, {"country", "active"});
        ColumnAggregateTraceScope tracing;
        const auto result = runtime.executeToQueryResult(aggregate, directBounds());
        ASSERT_EQ(result.rowCount(), 4U);
        if (!direct) expected = observedCells(result);
        else {
            EXPECT_EQ(observedCells(result), expected);
            EXPECT_GT(graph.direct_property_batches, 0U);
            EXPECT_EQ(graph.node_property_column_batch_reads, 0U);
            const auto trace = debug::PerfTrace::snapshotAndReset();
            ASSERT_TRUE(trace.contains("logical_plan.aggregate.direct_input_rows"));
            EXPECT_EQ(trace.at("logical_plan.aggregate.direct_input_rows").totalValue, 6);
            EXPECT_FALSE(trace.contains("logical_plan.batch.rows"));
        }
    }
}

TEST(LogicalPlanDirectColumnAggregateTest, StringAndNumericPredicatesFeedGroupedCountsWithoutRows) {
    for (const bool direct : {false, true}) {
        auto graph = groupingGraph(); graph.enable_direct_property_cells = direct;
        DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
        auto scan = std::make_unique<logical::LogicalNodeScan>("n");
        scan->setPropertyPredicates({{"country", PropertyValue("CN")}});
        scan->setRangePredicates({{"rank", PropertyValue(1), PropertyValue(2), true, true}});
        logical::LogicalAggregate aggregate(std::move(scan), {directField("active")}, {{"count", nullptr, "rows"}}, {"active"});
        const auto result = runtime.executeToQueryResult(aggregate, directBounds());
        ASSERT_EQ(result.rowCount(), 2U);
        EXPECT_EQ(result.getRows()[0].at("rows").asPrimitive(), PropertyValue(2));
        EXPECT_EQ(result.getRows()[1].at("rows").asPrimitive(), PropertyValue(1));
        if (direct) EXPECT_GT(graph.direct_property_batches, 0U);
    }
}

TEST(LogicalPlanDirectColumnAggregateTest, DistinctPreservesTypesNullAndEmbeddedStringBytes) {
    for (const bool direct : {false, true}) {
        DirectEdgeGraph graph; graph.enable_direct_property_cells = direct;
        const std::vector<PropertyValue> values{PropertyValue(2), PropertyValue(2.0), PropertyValue("2"),
            PropertyValue(true), PropertyValue(), PropertyValue(std::string("a\0b", 3)), PropertyValue(std::string("a\0b", 3))};
        for (size_t i = 0; i < values.size(); ++i) graph.nodes.emplace(i + 1, LogicalGraphNode{i + 1, {}, {{"rank", values[i]}}});
        graph.nodes.emplace(8, LogicalGraphNode{8, {}, {}});
        DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
        logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
            {{"count", directField("rank"), "distinct", true}});
        const auto result = runtime.executeToQueryResult(aggregate, directBounds());
        ASSERT_EQ(result.rowCount(), 1U);
        EXPECT_EQ(result.getRows()[0].at("distinct").asPrimitive(), PropertyValue(5));
        if (direct) EXPECT_GT(graph.direct_property_batches, 0U);
    }
}

TEST(LogicalPlanDirectColumnAggregateTest, EmptyInputAndEmptySelectionRetainGlobalAndGroupedSemantics) {
    for (bool empty : {false, true}) for (bool grouped : {false, true}) {
        auto graph = groupingGraph(); graph.enable_direct_property_cells = true;
        if (empty) graph.nodes.clear();
        DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
        auto scan = std::make_unique<logical::LogicalNodeScan>("n");
        scan->setPropertyPredicates({{"country", PropertyValue("absent")}});
        logical::LogicalAggregate aggregate(std::move(scan), grouped ?
            std::vector<std::shared_ptr<expressions::Expression>>{directField("country")} : std::vector<std::shared_ptr<expressions::Expression>>{},
            {{"count", directField("rank"), "count", true}, {"sum", directField("rank"), "sum"}});
        const auto result = runtime.executeToQueryResult(aggregate, directBounds());
        EXPECT_EQ(result.rowCount(), grouped ? 0U : 1U);
        if (!grouped) {
            EXPECT_EQ(result.getRows()[0].at("count").asPrimitive(), PropertyValue(0));
            EXPECT_EQ(result.getRows()[0].at("sum").asPrimitive(), PropertyValue());
        }
    }
}

TEST(LogicalPlanDirectColumnAggregateTest, AccumulationPreservesPhysicalScanOrder) {
    auto graph = directGraph(); graph.enable_direct_property_cells = true;
    graph.nodes.at(1).properties = {{"rank", PropertyValue(1e16)}};
    graph.nodes.at(2).properties = {{"rank", PropertyValue(-1e16)}};
    graph.nodes.at(3).properties = {{"rank", PropertyValue(1.0)}};
    graph.node_ref_scan_order = {3, 1, 2};
    DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
    logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {}, {{"sum", directField("rank"), "sum"}});
    const auto result = runtime.executeToQueryResult(aggregate, directBounds());
    EXPECT_EQ(result.getRows()[0].at("sum").asPrimitive(), PropertyValue(0.0));
    EXPECT_GT(graph.direct_property_batches, 0U);
}

TEST(LogicalPlanDirectColumnAggregateTest, SourceAndConsumerErrorsDoNotReplayAndFollowingQueryWorks) {
    auto graph = directGraph(); graph.enable_direct_property_cells = true; graph.fail_direct_property_cells = true;
    DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
    logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {}, {{"sum", directField("rank"), "sum"}});
    EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(aggregate, directBounds())), std::runtime_error);
    EXPECT_EQ(graph.direct_property_batches, 1U);
    graph.fail_direct_property_cells = false; graph.nodes.at(1).properties = {{"rank", PropertyValue("bad numeric")}};
    EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(aggregate, directBounds())), std::invalid_argument);
    EXPECT_EQ(graph.direct_property_batches, 2U);
    graph.nodes.at(1).properties = {{"rank", PropertyValue(1)}};
    EXPECT_EQ(runtime.executeToQueryResult(aggregate, directBounds()).getRows()[0].at("sum").asPrimitive(), PropertyValue(6));
}

TEST(LogicalPlanDirectColumnAggregateTest, PrivatePressureSplitsWithoutDuplicateCountsOrDanglingOutput) {
    auto graph = directGraph(); graph.enable_direct_property_cells = true;
    for (auto &[id, node] : graph.nodes) node.properties = {{"country", PropertyValue(std::string(8000, 'x'))}};
    DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
    logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {directField("country")},
        {{"count", nullptr, "rows"}}, {"country"});
    auto bounds = directBounds(); bounds.maximum_materialized_bytes = 48U * 1024U;
    ColumnAggregateTraceScope tracing;
    const auto result = runtime.executeToQueryResult(aggregate, bounds);
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows()[0].at("rows").asPrimitive(), PropertyValue(3));
    EXPECT_EQ(result.getRows()[0].at("country").asPrimitive(), PropertyValue(std::string(8000, 'x')));
    const auto trace = debug::PerfTrace::snapshotAndReset();
    EXPECT_EQ(trace.at("logical_plan.aggregate.direct_input_rows").totalValue, 3);
    EXPECT_GT(graph.direct_property_batches, 1U);
}


TEST(LogicalPlanDirectColumnAggregateTest, ResidualLiteralComparisonsMatchScalarTypeAndNullSemantics) {
    using Op = expressions::BinaryOperatorType;
    for (const auto operation : {Op::BOP_EQUAL, Op::BOP_NOT_EQUAL, Op::BOP_LESS, Op::BOP_LESS_EQUAL, Op::BOP_GREATER, Op::BOP_GREATER_EQUAL}) {
        for (const auto &needle : {PropertyValue("CN"), PropertyValue(1), PropertyValue(1.5), PropertyValue()}) {
            std::vector<std::vector<PropertyValue>> expected;
            for (const bool direct : {false, true}) {
                auto graph = groupingGraph(); graph.enable_direct_property_cells = direct;
                graph.nodes.at(1).properties = {{"rank", PropertyValue("CN")}};
                graph.nodes.at(2).properties = {{"rank", PropertyValue(1)}};
                graph.nodes.at(3).properties = {{"rank", PropertyValue(1.5)}};
                graph.nodes.at(4).properties = {{"rank", PropertyValue(false)}};
                auto predicate = std::make_shared<expressions::BinaryOpExpression>(
                    std::make_unique<expressions::VariableReferenceExpression>("n", "rank"), operation,
                    std::make_unique<expressions::LiteralExpression>(needle));
                auto filter = std::make_unique<logical::LogicalFilter>(std::make_unique<logical::LogicalNodeScan>("n"), predicate);
                logical::LogicalAggregate aggregate(std::move(filter), {}, {{"count", directField("rank"), "count", true}});
                DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
                const auto result = runtime.executeToQueryResult(aggregate, directBounds());
                if (!direct) expected = observedCells(result);
                else { EXPECT_EQ(observedCells(result), expected); EXPECT_GT(graph.direct_property_batches, 0U); }
            }
        }
    }
}

TEST(LogicalPlanDirectColumnAggregateTest, GuardAdmissionTypedExceptionDuringProductionIsNeverRetried) {
    auto graph = directGraph(); graph.enable_direct_property_cells = true;
    DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
    logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {}, {{"sum", directField("rank"), "sum"}});
    bool fired = false;
    QueryGuard guard(0, 1, [&] {
        if (!fired && graph.direct_property_batches != 0U) { fired = true; throw MaterializedByteLimitExceeded(); }
        return size_t{0U};
    });
    QueryContext context; context.guard = &guard;
    EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(aggregate, directBounds(), context)), MaterializedByteLimitExceeded);
    EXPECT_TRUE(fired); EXPECT_EQ(graph.direct_property_batches, 1U);
    EXPECT_EQ(runtime.executeToQueryResult(aggregate, directBounds()).getRows()[0].at("sum").asPrimitive(), PropertyValue(6));
}
} // namespace graph::query::execution
