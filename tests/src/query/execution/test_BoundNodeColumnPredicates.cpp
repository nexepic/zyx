#include <gtest/gtest.h>

#include "query/execution/BoundNodeColumnPredicates.hpp"
#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"

namespace graph::query::execution {
namespace {

struct FilterTraceScope {
	bool previous = debug::PerfTrace::areValuesEnabled();
	FilterTraceScope() { debug::PerfTrace::reset(); debug::PerfTrace::setValueCollectionEnabled(true); }
	~FilterTraceScope() { debug::PerfTrace::setValueCollectionEnabled(previous); }
};

TEST(BoundNodeColumnPredicatesTest, BindsReorderedColumnsAndIntersectsRangesParametersAndCompositeKeys) {
	logical::LogicalNodeScan scan("n");
	scan.setRangePredicates({{"x", PropertyValue(2), PropertyValue(4), true, false}});
	scan.setCompositeEquality({{"y"}, {PropertyValue("ok")}});
	const std::vector<std::pair<std::string, PropertyValue>> parameters{{"y", PropertyValue("ok")}};
	const std::vector<std::string> keys{"y", "x"};
	const std::vector<std::vector<PropertyValue>> columns{
		{PropertyValue("ok"), PropertyValue("ok"), PropertyValue("bad"), PropertyValue("ok"), PropertyValue("ok")},
		{PropertyValue(1), PropertyValue(2), PropertyValue(3), PropertyValue(4), PropertyValue{}}};
	MaterializedByteBudget budget(65536U);
	{
		detail::BoundNodeColumnPredicates program(scan, parameters, keys, budget);
		const size_t planBytes = budget.used();
		SelectionVector selection(5U);
		const auto stats = program.filter(columns, selection, budget, [] {});
		EXPECT_EQ(selection.selectedCount(), 1U);
		EXPECT_TRUE(selection.includes(1U));
		EXPECT_EQ(stats.typed_columns, 1U);
		EXPECT_EQ(stats.scalar_columns, 1U);
		EXPECT_EQ(budget.used(), planBytes);
	}
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundNodeColumnPredicatesTest, UnboundedRangesStillRejectMissingAndNullValues) {
	logical::LogicalNodeScan scan("n");
	scan.setRangePredicates({{"x", PropertyValue{}, PropertyValue{}, true, true}});
	const std::vector<std::string> keys{"x"};
	const std::vector<std::vector<PropertyValue>> columns{{PropertyValue("x"), PropertyValue{}, PropertyValue{}}};
	MaterializedByteBudget budget(65536U);
	detail::BoundNodeColumnPredicates program(scan, {}, keys, budget);
	SelectionVector selection(3U);
	static_cast<void>(program.filter(columns, selection, budget, [] {}));
	EXPECT_EQ(selection.selectedCount(), 1U);
	EXPECT_TRUE(selection.includes(0U));
}

TEST(BoundNodeColumnPredicatesTest, MixedNumericTypesUseExistingVariantOrderingWithoutCoercion) {
	logical::LogicalNodeScan scan("n");
	scan.setRangePredicates({{"x", PropertyValue(1), PropertyValue{}, true, true}});
	const std::vector<std::string> keys{"x"};
	const std::vector<std::vector<PropertyValue>> columns{{PropertyValue(0), PropertyValue(1), PropertyValue(0.5), PropertyValue{}}};
	MaterializedByteBudget budget(65536U);
	detail::BoundNodeColumnPredicates program(scan, {}, keys, budget);
	SelectionVector selection(4U);
	const auto stats = program.filter(columns, selection, budget, [] {});
	EXPECT_EQ(stats.scalar_columns, 1U);
	EXPECT_EQ(stats.typed_columns, 0U);
	EXPECT_FALSE(selection.includes(0U));
	EXPECT_TRUE(selection.includes(1U));
	// DOUBLE follows INTEGER in the current variant order, even when 0.5 < 1.
	EXPECT_TRUE(selection.includes(2U));
	EXPECT_FALSE(selection.includes(3U));
}

TEST(BoundNodeColumnPredicatesTest, InvalidBindingsAndAllocationFailureRefundAllMetadata) {
	MaterializedByteBudget budget(65536U);
	const std::vector<std::string> keys{"x"};
	logical::LogicalNodeScan absent("n", {}, {{"missing", PropertyValue(1)}});
	EXPECT_THROW((detail::BoundNodeColumnPredicates(absent, {}, keys, budget)), std::invalid_argument);
	EXPECT_EQ(budget.used(), 0U);
	logical::LogicalNodeScan invalid("n");
	invalid.setCompositeEquality({{"x"}, {}});
	EXPECT_THROW((detail::BoundNodeColumnPredicates(invalid, {}, keys, budget)), std::invalid_argument);
	EXPECT_EQ(budget.used(), 0U);
	logical::LogicalNodeScan scan("n", {}, {{"x", PropertyValue(1)}});
	EXPECT_THROW((detail::BoundNodeColumnPredicates(scan, {}, keys, budget, std::pmr::null_memory_resource())), std::bad_alloc);
	EXPECT_THROW((detail::BoundNodeColumnPredicates(scan, {}, keys, budget, nullptr)), std::invalid_argument);
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundNodeColumnPredicatesTest, CompleteMatrixShapeIsCheckedEvenIfNoRowsRemainSelected) {
	logical::LogicalNodeScan scan("n", {}, {{"x", PropertyValue(1)}});
	const std::vector<std::string> keys{"x", "y"};
	MaterializedByteBudget budget(65536U);
	detail::BoundNodeColumnPredicates program(scan, {}, keys, budget);
	SelectionVector selection(2U);
	selection.rejectAll();
	const std::vector<std::vector<PropertyValue>> wrongWidth{{PropertyValue(1), PropertyValue(1)}};
	const std::vector<std::vector<PropertyValue>> wrongRows{{PropertyValue(1), PropertyValue(1)}, {PropertyValue(2)}};
	EXPECT_THROW(static_cast<void>(program.filter(wrongWidth, selection, budget, [] {})), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(program.filter(wrongRows, selection, budget, [] {})), std::invalid_argument);
}

TEST(BoundNodeColumnPredicatesTest, RuntimeTypedScanPreservesPhysicalOrderProjectionAndOwningResults) {
	using namespace direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	graph.nodes.emplace(4U, LogicalGraphNode{4U, {}, {{"rank", PropertyValue{}}}});
	graph.nodes.emplace(5U, LogicalGraphNode{5U, {}, {}});
	graph.nodes.emplace(6U, LogicalGraphNode{6U, {}, {{"rank", PropertyValue(3)}}});
	graph.node_ref_scan_order = {4U, 1U, 6U, 2U, 5U, 3U};
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n");
	scan->setRangePredicates({{"rank", PropertyValue(1), PropertyValue(3), false, true}});
	std::vector<logical::LogicalProjectItem> items{
		{std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank"},
		{std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "again"}};
	logical::LogicalProject project(std::move(scan), std::move(items));
	FilterTraceScope traceScope;
	const auto result = runtime.executeToQueryResult(project, directBounds());
	ASSERT_EQ(result.rowCount(), 3U);
	std::vector<int64_t> ranks;
	for (const auto &row : result.getRows()) {
		ranks.push_back(std::get<int64_t>(row.at("rank").asPrimitive().getVariant()));
		EXPECT_EQ(row.at("rank").asPrimitive(), row.at("again").asPrimitive());
	}
	EXPECT_EQ(ranks, (std::vector<int64_t>{3, 2, 3}));
	const auto trace = debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(trace.contains("logical_plan.node_column_filter.typed_columns"));
	EXPECT_GT(trace.at("logical_plan.node_column_filter.typed_columns").totalValue, 0);
	ASSERT_TRUE(trace.contains("logical_plan.node_column_filter.rejected_before_rows"));
	EXPECT_EQ(trace.at("logical_plan.node_column_filter.rejected_before_rows").totalValue, 3);
	graph.enable_native_node_ref_scans = false;
	graph.node_ref_scan_order.clear();
	const auto scalar = runtime.executeToQueryResult(project, directBounds());
	std::vector<int64_t> scalarRanks;
	for (const auto &row : scalar.getRows()) scalarRanks.push_back(std::get<int64_t>(row.at("rank").asPrimitive().getVariant()));
	std::ranges::sort(ranks);
	std::ranges::sort(scalarRanks);
	EXPECT_EQ(ranks, scalarRanks);
	graph.nodes.clear();
	EXPECT_EQ(result.rowCount(), 3U);
	EXPECT_EQ(result.getRows().front().at("rank").asPrimitive(), PropertyValue(3));
}

TEST(BoundNodeColumnPredicatesTest, RuntimeTypedScanFeedsExistingAggregatesAndMatchesScalarExecution) {
	using namespace direct_edge_test;
	for (bool native : {false, true}) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_native_node_ref_scans = native;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		scan->setRangePredicates({{"rank", PropertyValue(2), PropertyValue(3), true, true}});
		std::vector<logical::LogicalAggItem> aggregations{{"count", nullptr, "n"},
			{"sum", std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "total"}};
		logical::LogicalAggregate aggregate(std::move(scan), {}, std::move(aggregations));
		const auto result = runtime.executeToQueryResult(aggregate, directBounds());
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("n").asPrimitive(), PropertyValue(2));
		EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(5));
	}
}

TEST(BoundNodeColumnPredicatesTest, ReusedLogicalPlanResolvesParametersForEachQueryAndDoesNotCacheAColumnIdentity) {
	using namespace direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n");
	scan->setParameterPropertyPredicates({{"rank", "needle"}});
	logical::LogicalProject project(std::move(scan),
		{{std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank"}});
	for (int64_t needle : {2, 3}) {
		QueryContext context;
		context.parameters.emplace("needle", PropertyValue(needle));
		const auto result = runtime.executeToQueryResult(project, directBounds(), context);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("rank").asPrimitive(), PropertyValue(needle));
	}
}

TEST(BoundNodeColumnPredicatesTest, MalformedSourceColumnsFailBeforeFilteringAndCanRetryAfterRepair) {
	using namespace direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n");
	scan->setRangePredicates({{"rank", PropertyValue(100), PropertyValue{}, true, true}});
	logical::LogicalProject project(std::move(scan),
		{{std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank"}});
	graph.truncate_node_property_batch = true;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(project, directBounds())), std::runtime_error);
	graph.truncate_node_property_batch = false;
	EXPECT_EQ(runtime.executeToQueryResult(project, directBounds()).rowCount(), 0U);
}

TEST(BoundNodeColumnPredicatesTest, ReusedPlanHandlesNullParametersAndChangingPropertyTypes) {
	using namespace direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n");
	scan->setParameterPropertyPredicates({{"rank", "needle"}});
	logical::LogicalProject project(std::move(scan),
		{{std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank"}});
	QueryContext context;
	context.parameters.emplace("needle", PropertyValue{});
	EXPECT_EQ(runtime.executeToQueryResult(project, directBounds(), context).rowCount(), 0U);
	context.parameters.at("needle") = PropertyValue(2);
	ASSERT_EQ(runtime.executeToQueryResult(project, directBounds(), context).rowCount(), 1U);
	graph.nodes.at(2U).properties = {{"rank", PropertyValue(0.5)}};
	EXPECT_EQ(runtime.executeToQueryResult(project, directBounds(), context).rowCount(), 0U);
	context.parameters.at("needle") = PropertyValue(0.5);
	const auto result = runtime.executeToQueryResult(project, directBounds(), context);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("rank").asPrimitive(), PropertyValue(0.5));
}

TEST(BoundNodeColumnPredicatesTest, RedundantPredicatesDoNotConsumeTheLastAdmissibleRowBudget) {
	using namespace direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	graph.nodes.clear();
	const PropertyValue payload(std::string(1024U, 'x'));
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {}, {{"value", payload}}});
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	const auto makeProject = [&](size_t predicateCount) {
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		scan->setRangePredicates(std::vector<logical::RangePredicate>(predicateCount,
			{"value", payload, payload, true, true}));
		return logical::LogicalProject(std::move(scan),
			{{std::make_shared<expressions::VariableReferenceExpression>("n", "value"), "value"}});
	};
	auto minimal = makeProject(1U);
	auto redundant = makeProject(32U);
	size_t admittedBudgets = 0U;
	for (size_t bytes = 2048U; bytes <= 8192U; bytes += 128U) {
		auto bounds = directBounds();
		bounds.maximum_materialized_bytes = bytes;
		bool admitted = false;
		try {
			const auto reference = runtime.executeToQueryResult(minimal, bounds);
			ASSERT_EQ(reference.rowCount(), 1U);
			ASSERT_EQ(reference.getRows().front().at("value").asPrimitive(), payload);
			admitted = true;
		} catch (const std::runtime_error &error) {
			// Some allowances are below the existing owning-output requirement.
			ASSERT_NE(std::string_view(error.what()).find("byte bound"), std::string_view::npos);
		}
		if (!admitted) continue;
		++admittedBudgets;
		SCOPED_TRACE(bytes);
		EXPECT_NO_THROW({
			const auto result = runtime.executeToQueryResult(redundant, bounds);
			ASSERT_EQ(result.rowCount(), 1U);
			EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), payload);
		});
	}
	EXPECT_GT(admittedBudgets, 0U);
}

} // namespace
} // namespace graph::query::execution
