/**
 * @file test_LogicalPlanUnwindRuntime.cpp
 * @brief Logical UNWIND execution tests independent of physical graph storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <memory>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "CypherParserImpl.hpp"
#include "graph/core/TemporalTypes.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "query/execution/LogicalPlanUnwindRuntimeTestFixture.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/ReduceExpression.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalOptionalMatch.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::execution;
using namespace graph::storage;
namespace logical = graph::query::logical;
namespace expressions = graph::query::expressions;

using namespace graph::query::execution::test;

TEST(LogicalPlanUnwindRuntimeTest, ExecutesParserProducedLiteralAndParameterUnwindPlans) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;

	QueryPlan literalPlan = parser.parseToLogical("UNWIND [1, 2, 3] AS item RETURN item");
	ASSERT_NE(literalPlan.root, nullptr);
	std::vector<PropertyValue> literalValues;
	const LogicalPlanReadRuntimeStats literalStats = runtime.execute(
			*literalPlan.root, bounds(), [&literalValues](const LogicalPlanReadRow &row) {
				literalValues.push_back(std::get<PropertyValue>(row.values.at("item")));
				return true;
			});
	EXPECT_EQ(literalValues, (std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3)}));
	EXPECT_EQ(literalStats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 3U}));

	QueryPlan parameterPlan = parser.parseToLogical("UNWIND $items AS item RETURN item");
	ASSERT_NE(parameterPlan.root, nullptr);
	QueryContext context;
	context.parameters.emplace("items", PropertyValue(std::vector<PropertyValue>{PropertyValue("a"), PropertyValue("b")}));
	const QueryResult parameterResult = runtime.executeToQueryResult(*parameterPlan.root, bounds(), context);
	ASSERT_EQ(parameterResult.rowCount(), 2U);
	EXPECT_EQ(parameterResult.getRows()[0].at("item").asPrimitive(), PropertyValue("a"));
	EXPECT_EQ(parameterResult.getRows()[1].at("item").asPrimitive(), PropertyValue("b"));
}

TEST(LogicalPlanUnwindRuntimeTest, DistinctUsesStructuralKeysForNestedMapAndListValues) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;

	QueryPlan plan = parser.parseToLogical(
			"UNWIND [{alpha: [1, 2], beta: 'same'}, {beta: 'same', alpha: [1, 2]}, "
			"{alpha: [1, 3], beta: 'same'}] AS item RETURN DISTINCT item");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows()[0].at("item").asPrimitive(),
			  PropertyValue(PropertyValue::MapType{{"alpha", PropertyValue(std::vector<PropertyValue>{1, 2})},
																	 {"beta", PropertyValue("same")}}));
	EXPECT_EQ(result.getRows()[1].at("item").asPrimitive(),
			  PropertyValue(PropertyValue::MapType{{"alpha", PropertyValue(std::vector<PropertyValue>{1, 3})},
																	 {"beta", PropertyValue("same")}}));
}

TEST(LogicalPlanUnwindRuntimeTest, DistinctPreservesEveryPublicScalarParameterValueCategory) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;

	QueryPlan plan = parser.parseToLogical("UNWIND $values AS value RETURN DISTINCT value");
	ASSERT_NE(plan.root, nullptr);

	QueryContext context;
	context.parameters.emplace(
			"values", PropertyValue(std::vector<PropertyValue>{
								PropertyValue(), PropertyValue(true), PropertyValue(false), PropertyValue(0.0),
								PropertyValue(-0.0), PropertyValue(1.5), PropertyValue("text"),
								PropertyValue(TemporalDate::fromYMD(2024, 1, 2)),
								PropertyValue(TemporalDateTime::fromComponents(2024, 1, 2, 3, 4, 5)),
								PropertyValue(TemporalDuration{0, 1, 0}),
								PropertyValue(PropertyValue::Bytes{std::byte{0x0A}}),
					}));

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds(), context);
	ASSERT_EQ(result.rowCount(), 10U);
	const std::vector<PropertyType> types = {
			PropertyType::NULL_TYPE, PropertyType::BOOLEAN, PropertyType::BOOLEAN, PropertyType::DOUBLE,
			PropertyType::DOUBLE,    PropertyType::STRING,  PropertyType::DATE,    PropertyType::DATETIME,
			PropertyType::DURATION,  PropertyType::BYTES,
	};
	for (size_t index = 0U; index < types.size(); ++index) {
		EXPECT_EQ(result.getRows()[index].at("value").asPrimitive().getType(), types[index]);
	}
	EXPECT_EQ(result.getRows()[3].at("value").asPrimitive(), PropertyValue(0.0));
	EXPECT_EQ(result.getRows()[4].at("value").asPrimitive(), PropertyValue(1.5));
}

TEST(LogicalPlanUnwindRuntimeTest, EvaluatesBoundedForwardAndReverseRangesFromParserProducedPlans) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;

	QueryPlan forwardPlan = parser.parseToLogical("UNWIND range(1, 3) AS value RETURN value");
	ASSERT_NE(forwardPlan.root, nullptr);
	const QueryResult forward = runtime.executeToQueryResult(*forwardPlan.root, bounds());
	ASSERT_EQ(forward.rowCount(), 3U);
	EXPECT_EQ(forward.getRows()[0].at("value").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(forward.getRows()[1].at("value").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(forward.getRows()[2].at("value").asPrimitive(), PropertyValue(3));

	QueryPlan reversePlan = parser.parseToLogical("UNWIND range(3, 1, -1) AS value RETURN value");
	ASSERT_NE(reversePlan.root, nullptr);
	const QueryResult reverse = runtime.executeToQueryResult(*reversePlan.root, bounds());
	ASSERT_EQ(reverse.rowCount(), 3U);
	EXPECT_EQ(reverse.getRows()[0].at("value").asPrimitive(), PropertyValue(3));
	EXPECT_EQ(reverse.getRows()[1].at("value").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(reverse.getRows()[2].at("value").asPrimitive(), PropertyValue(1));

	QueryPlan zeroStepPlan = parser.parseToLogical("RETURN range(1, 3, 0) AS values");
	ASSERT_NE(zeroStepPlan.root, nullptr);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*zeroStepPlan.root, bounds())), std::runtime_error);

	LogicalPlanReadRuntimeBounds constrained = bounds();
	constrained.maximum_rows = 2U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*forwardPlan.root, constrained)), std::runtime_error);
}

TEST(LogicalPlanUnwindRuntimeTest, PreservesOuterBindingsAndUsesPipelineScalarSemantics) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	const auto expression = std::make_shared<expressions::ParameterExpression>("value");
	auto inner = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "seed", std::vector<PropertyValue>{PropertyValue(7), PropertyValue(8)});
	logical::LogicalUnwind root(std::move(inner), "item", expression);
	QueryContext context;
	context.parameters.emplace("value", PropertyValue("single"));
	std::vector<std::pair<PropertyValue, PropertyValue>> rows;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			root, bounds(), [&rows](const LogicalPlanReadRow &row) {
				rows.emplace_back(std::get<PropertyValue>(row.values.at("seed")),
							  std::get<PropertyValue>(row.values.at("item")));
				return true;
			},
			context);
	EXPECT_EQ(rows, (std::vector<std::pair<PropertyValue, PropertyValue>>{
						 {PropertyValue(7), PropertyValue("single")}, {PropertyValue(8), PropertyValue("single")}}));
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 2U}));
}

TEST(LogicalPlanUnwindRuntimeTest, EvaluatesListLiteralExpressionsWithoutPhysicalExpressionHelpers) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto list = std::make_shared<expressions::ListLiteralExpression>(
			PropertyValue(std::vector<PropertyValue>{PropertyValue("left"), PropertyValue("right")}));
	logical::LogicalUnwind root(std::make_unique<logical::LogicalSingleRow>(), "value", std::move(list));

	std::vector<PropertyValue> values;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			root, bounds(), [&values](const LogicalPlanReadRow &row) {
				values.push_back(std::get<PropertyValue>(row.values.at("value")));
				return true;
			});
	EXPECT_EQ(values, (std::vector<PropertyValue>{PropertyValue("left"), PropertyValue("right")}));
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 2U}));
}

TEST(LogicalPlanReadRuntimeTest, EnforcesBooleanOrNullSearchedCasePredicates) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	const auto evaluate = [&runtime](std::unique_ptr<expressions::Expression> condition,
									 const QueryContext *context = nullptr) {
		auto expression = std::make_unique<expressions::CaseExpression>();
		expression->addBranch(std::move(condition), std::make_unique<expressions::LiteralExpression>(int64_t(1)));
		expression->setElseExpression(std::make_unique<expressions::LiteralExpression>(int64_t(0)));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::shared_ptr<expressions::Expression>(std::move(expression)), "result");
		logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
		int64_t result = -1;
		const auto visitor = [&result](const LogicalPlanReadRow &row) {
			result = std::get<int64_t>(std::get<PropertyValue>(row.values.at("result")).getVariant());
			return true;
		};
		const LogicalPlanReadRuntimeStats stats =
				context == nullptr ? runtime.execute(root, bounds(), visitor) : runtime.execute(root, bounds(), visitor, *context);
		EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));
		return result;
	};

	EXPECT_EQ(evaluate(std::make_unique<expressions::LiteralExpression>(true)), 1);
	EXPECT_EQ(evaluate(std::make_unique<expressions::LiteralExpression>(false)), 0);
	EXPECT_EQ(evaluate(std::make_unique<expressions::LiteralExpression>()), 0);
	EXPECT_THROW(static_cast<void>(evaluate(std::make_unique<expressions::LiteralExpression>(int64_t(7)))),
			 std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate(std::make_unique<expressions::LiteralExpression>(int64_t(0)))),
			 std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate(std::make_unique<expressions::LiteralExpression>(2.5))), std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate(std::make_unique<expressions::LiteralExpression>(0.0))), std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate(std::make_unique<expressions::LiteralExpression>(std::string("value")))),
			 std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate(std::make_unique<expressions::LiteralExpression>(std::string()))),
			 std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate(std::make_unique<expressions::ListLiteralExpression>(
				  PropertyValue(std::vector<PropertyValue>{PropertyValue(1)})))),
			 std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate(std::make_unique<expressions::ListLiteralExpression>(
				  PropertyValue(std::vector<PropertyValue>{})))),
			 std::runtime_error);
	QueryContext context;
	context.parameters.emplace("map", PropertyValue(PropertyValue::MapType{{"key", PropertyValue(1)}}));
	EXPECT_THROW(static_cast<void>(evaluate(std::make_unique<expressions::ParameterExpression>("map"), &context)),
			 std::runtime_error);
}

TEST(LogicalPlanUnwindRuntimeTest, CanSuppressScalarExpansionForForeachLowering) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto expression = std::make_shared<expressions::ParameterExpression>("not_a_list");
	logical::LogicalUnwind root(std::make_unique<logical::LogicalSingleRow>(), "value", std::move(expression), false);
	QueryContext context;
	context.parameters.emplace("not_a_list", PropertyValue(9));

	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			root, bounds(), [](const LogicalPlanReadRow &) { return true; }, context);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{}));
}

TEST(LogicalPlanUnwindRuntimeTest, ExecutesPublicSnapshotAndExpressionEntryPointsWithTheSameBounds) {
	NoGraphBackend backend;
	NoGraphSnapshot snapshot;
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalProject root(
			std::make_unique<logical::LogicalSingleRow>(),
			{{std::make_shared<expressions::LiteralExpression>(int64_t(7)), "value"}});
	QueryContext context;

	std::vector<PropertyValue> values;
	const LogicalPlanReadRuntimeStats directStats = runtime.execute(
			root, bounds(), [&values](const LogicalPlanReadRow &row) {
				values.push_back(std::get<PropertyValue>(row.values.at("value")));
				return true;
			});
	EXPECT_EQ(values, (std::vector<PropertyValue>{PropertyValue(7)}));
	EXPECT_EQ(directStats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));

	values.clear();
	const LogicalPlanReadRuntimeStats snapshotStats = runtime.executeWithSnapshot(
			snapshot, root, bounds(), [&values](const LogicalPlanReadRow &row) {
				values.push_back(std::get<PropertyValue>(row.values.at("value")));
				return true;
			}, context);
	EXPECT_EQ(values, (std::vector<PropertyValue>{PropertyValue(7)}));
	EXPECT_EQ(snapshotStats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));

	LogicalPlanReadRow row;
	const expressions::LiteralExpression literal(PropertyValue("value"));
	EXPECT_EQ(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(snapshot, literal, row, bounds(), context),
			  LogicalPlanReadValue(PropertyValue("value")));
}

TEST(LogicalPlanUnwindRuntimeTest, BuildsDistinctKeysForPublicValuesAndRejectsEmptyColumnSets) {
	LogicalPlanReadRow row;
	row.values.emplace("integer", PropertyValue(7));
	row.values.emplace("bytes", PropertyValue(PropertyValue::Bytes{std::byte{0x01}, std::byte{0x02}}));

	const std::string first = LogicalPlanReadRuntime::makeDistinctRowKey(row, {"integer", "bytes"});
	const std::string second = LogicalPlanReadRuntime::makeDistinctRowKey(row, {"integer", "bytes"});
	EXPECT_FALSE(first.empty());
	EXPECT_EQ(first, second);
	EXPECT_THROW(static_cast<void>(LogicalPlanReadRuntime::makeDistinctRowKey(row, {})), std::invalid_argument);
}

TEST(LogicalPlanReadRuntimeTest, RejectsInvalidBoundsBeforeOpeningAReadSnapshot) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalSingleRow root;

	// Exercise each public resource-bound guard independently. Keeping the other
	// fields valid proves validation does not depend on a particular guard order.
	const auto expectInvalid = [&runtime, &root](auto mutate) {
		auto invalid = bounds();
		mutate(invalid);
		EXPECT_THROW(static_cast<void>(runtime.execute(root, invalid, [](const LogicalPlanReadRow &) { return true; })),
				 std::invalid_argument);
	};
	const auto expectNoVisitor = [&runtime, &root] {
		EXPECT_THROW(static_cast<void>(runtime.execute(root, bounds(), {})), std::invalid_argument);
	};

	expectNoVisitor();
	expectInvalid([](auto &value) { value.maximum_node_records = 0U; });
	expectInvalid([](auto &value) { value.maximum_node_page_records = 0U; });
	expectInvalid([](auto &value) { value.maximum_metadata_pages_per_node_page = 0U; });
	expectInvalid([](auto &value) { value.maximum_edges_per_node = 0U; });
	expectInvalid([](auto &value) { value.maximum_edge_records = 0U; });
	expectInvalid([](auto &value) { value.maximum_adjacency_page_records = 0U; });
	expectInvalid([](auto &value) { value.maximum_property_entries = 0U; });
	expectInvalid([](auto &value) { value.maximum_rows = 0U; });
	expectInvalid([](auto &value) { value.maximum_distinct_rows = 0U; });
	expectInvalid([](auto &value) { value.maximum_node_page_records = value.maximum_node_records + 1U; });
	expectInvalid([](auto &value) { value.maximum_adjacency_page_records = value.maximum_edges_per_node + 1U; });
}

TEST(LogicalPlanReadRuntimeTest, ExecutesParserProducedOrderByWithBoundedMaterialization) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("UNWIND [3, 1, 2] AS value RETURN value ORDER BY value ASC");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 3U);
	EXPECT_EQ(result.getRows()[0].at("value").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(result.getRows()[1].at("value").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(result.getRows()[2].at("value").asPrimitive(), PropertyValue(3));

	// A one-row aggregate has no ordering work to perform. Its ORDER BY input
	// can therefore remain unavailable after aggregation without affecting the result.
	QueryPlan singleRowPlan = parser.parseToLogical("UNWIND [7] AS value RETURN count(*) AS count ORDER BY value");
	ASSERT_NE(singleRowPlan.root, nullptr);
	const QueryResult singleRow = runtime.executeToQueryResult(*singleRowPlan.root, bounds());
	ASSERT_EQ(singleRow.rowCount(), 1U);
	EXPECT_EQ(singleRow.getRows()[0].at("count").asPrimitive(), PropertyValue(1));

	QueryPlan descendingPlan = parser.parseToLogical("UNWIND [3, 1, 2] AS value RETURN value ORDER BY value DESC");
	ASSERT_NE(descendingPlan.root, nullptr);
	const QueryResult descending = runtime.executeToQueryResult(*descendingPlan.root, bounds());
	ASSERT_EQ(descending.rowCount(), 3U);
	EXPECT_EQ(descending.getRows()[0].at("value").asPrimitive(), PropertyValue(3));
	EXPECT_EQ(descending.getRows()[1].at("value").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(descending.getRows()[2].at("value").asPrimitive(), PropertyValue(1));
	uint64_t delivered = 0U;
	const LogicalPlanReadRuntimeStats cancelledStats = runtime.execute(
			*descendingPlan.root, bounds(), [&delivered](const LogicalPlanReadRow &) {
				++delivered;
				return false;
			});
	EXPECT_EQ(delivered, 1U);
	EXPECT_EQ(cancelledStats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));

	QueryPlan tieBreakPlan = parser.parseToLogical(
			"UNWIND [{group: 1, value: 2}, {group: 1, value: 1}] AS item "
			"RETURN item.group AS group, item.value AS value ORDER BY group ASC, value DESC");
	ASSERT_NE(tieBreakPlan.root, nullptr);
	const QueryResult tieBreak = runtime.executeToQueryResult(*tieBreakPlan.root, bounds());
	ASSERT_EQ(tieBreak.rowCount(), 2U);
	EXPECT_EQ(tieBreak.getRows()[0].at("group").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(tieBreak.getRows()[0].at("value").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(tieBreak.getRows()[1].at("group").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(tieBreak.getRows()[1].at("value").asPrimitive(), PropertyValue(1));
}

TEST(LogicalPlanReadRuntimeTest, AppliesDistinctAndAllUnionSemanticsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;

	QueryPlan distinctPlan = parser.parseToLogical(
			"UNWIND [1, 2] AS value RETURN value UNION UNWIND [2, 3] AS value RETURN value");
	ASSERT_NE(distinctPlan.root, nullptr);
	const QueryResult distinct = runtime.executeToQueryResult(*distinctPlan.root, bounds());
	ASSERT_EQ(distinct.rowCount(), 3U);
	EXPECT_EQ(distinct.getRows()[0].at("value").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(distinct.getRows()[1].at("value").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(distinct.getRows()[2].at("value").asPrimitive(), PropertyValue(3));
	LogicalPlanReadRuntimeBounds constrainedDistinct = bounds();
	constrainedDistinct.maximum_distinct_rows = 1U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*distinctPlan.root, constrainedDistinct)), std::runtime_error);

	QueryPlan allPlan = parser.parseToLogical(
			"UNWIND [1, 2] AS value RETURN value UNION ALL UNWIND [2, 3] AS value RETURN value");
	ASSERT_NE(allPlan.root, nullptr);
	const QueryResult all = runtime.executeToQueryResult(*allPlan.root, bounds());
	ASSERT_EQ(all.rowCount(), 4U);
	EXPECT_EQ(all.getRows()[0].at("value").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(all.getRows()[1].at("value").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(all.getRows()[2].at("value").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(all.getRows()[3].at("value").asPrimitive(), PropertyValue(3));

	uint64_t delivered = 0U;
	const LogicalPlanReadRuntimeStats cancelledStats = runtime.execute(
			*allPlan.root, bounds(), [&delivered](const LogicalPlanReadRow &) {
				++delivered;
				return false;
			});
	EXPECT_EQ(delivered, 1U);
	EXPECT_EQ(cancelledStats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));

	delivered = 0U;
	const LogicalPlanReadRuntimeStats rightCancelledStats = runtime.execute(
			*allPlan.root, bounds(), [&delivered](const LogicalPlanReadRow &) {
				++delivered;
				return delivered < 3U;
			});
	EXPECT_EQ(delivered, 3U);
	EXPECT_EQ(rightCancelledStats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 3U}));
}

TEST(LogicalPlanReadRuntimeTest, AppliesParserProducedPaginationWithoutScanningPastLimit) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;

	QueryPlan pagePlan = parser.parseToLogical("UNWIND [1, 2, 3] AS value RETURN value SKIP 1 LIMIT 1");
	ASSERT_NE(pagePlan.root, nullptr);
	const QueryResult page = runtime.executeToQueryResult(*pagePlan.root, bounds());
	ASSERT_EQ(page.rowCount(), 1U);
	EXPECT_EQ(page.getRows().front().at("value").asPrimitive(), PropertyValue(2));

	uint64_t delivered = 0U;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			*pagePlan.root, bounds(), [&delivered](const LogicalPlanReadRow &) {
				++delivered;
				return true;
			});
	EXPECT_EQ(delivered, 1U);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));

	QueryPlan cancellablePlan = parser.parseToLogical("UNWIND [1, 2, 3] AS value RETURN value LIMIT 2");
	ASSERT_NE(cancellablePlan.root, nullptr);
	delivered = 0U;
	const LogicalPlanReadRuntimeStats cancelledStats = runtime.execute(
			*cancellablePlan.root, bounds(), [&delivered](const LogicalPlanReadRow &) {
				++delivered;
				return false;
			});
	EXPECT_EQ(delivered, 1U);
	EXPECT_EQ(cancelledStats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));

	QueryPlan emptyPlan = parser.parseToLogical("UNWIND [1, 2, 3] AS value RETURN value LIMIT 0");
	ASSERT_NE(emptyPlan.root, nullptr);
	EXPECT_EQ(runtime.executeToQueryResult(*emptyPlan.root, bounds()).rowCount(), 0U);
}

TEST(LogicalPlanReadRuntimeTest, PaginatesAcrossMultipleBoundedUnwindBatches) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;

	std::vector<PropertyValue> input;
	input.reserve(1024U);
	for (int64_t value = 0; value < 1024; ++value) {
		input.emplace_back(value);
	}
	QueryContext context;
	context.parameters.emplace("items", PropertyValue(input));
	QueryPlan plan = parser.parseToLogical(
			"UNWIND $items AS value RETURN value SKIP 513 LIMIT 5");
	ASSERT_NE(plan.root, nullptr);
	LogicalPlanReadRuntimeBounds largeBounds = bounds();
	largeBounds.maximum_rows = 2048U;
	largeBounds.maximum_materialized_bytes = 2U * 1024U * 1024U;

	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	std::vector<PropertyValue> output;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			*plan.root, largeBounds, [&output](const LogicalPlanReadRow &row) {
				output.push_back(std::get<PropertyValue>(row.values.at("value")));
				return true;
			}, context);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_EQ(output.size(), 5U);
	for (size_t index = 0U; index < output.size(); ++index) {
		EXPECT_EQ(output[index], PropertyValue(static_cast<int64_t>(513U + index)));
	}
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 5U}));
	ASSERT_TRUE(trace.contains("logical_plan.skip.discarded_rows"));
	EXPECT_EQ(trace.at("logical_plan.skip.discarded_rows").totalValue, 513);
	ASSERT_TRUE(trace.contains("logical_plan.limit.delivered_rows"));
	EXPECT_EQ(trace.at("logical_plan.limit.delivered_rows").totalValue, 5);
	ASSERT_TRUE(trace.contains("logical_plan.limit.early_stop"));
	EXPECT_EQ(trace.at("logical_plan.limit.early_stop").totalValue, 1);
}

TEST(LogicalPlanReadRuntimeTest, ExecutesParserProducedGlobalAndGroupedAggregates) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;

	QueryPlan globalPlan = parser.parseToLogical(
			"UNWIND [1, 2, 2] AS value RETURN count(DISTINCT value) AS count, sum(value) AS total, "
			"collect(DISTINCT value) AS values");
	ASSERT_NE(globalPlan.root, nullptr);
	const QueryResult global = runtime.executeToQueryResult(*globalPlan.root, bounds());
	ASSERT_EQ(global.rowCount(), 1U);
	EXPECT_EQ(global.getRows()[0].at("count").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(global.getRows()[0].at("total").asPrimitive(), PropertyValue(5));
	EXPECT_EQ(global.getRows()[0].at("values").asPrimitive().getList(),
			  (std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)}));

	QueryPlan groupedPlan = parser.parseToLogical(
			"UNWIND [1, 1, 2] AS value RETURN value, count(*) AS amount ORDER BY value ASC");
	ASSERT_NE(groupedPlan.root, nullptr);
	const QueryResult grouped = runtime.executeToQueryResult(*groupedPlan.root, bounds());
	ASSERT_EQ(grouped.rowCount(), 2U);
	EXPECT_EQ(grouped.getRows()[0].at("value").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(grouped.getRows()[0].at("amount").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(grouped.getRows()[1].at("value").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(grouped.getRows()[1].at("amount").asPrimitive(), PropertyValue(1));
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesEveryNativeAggregateFunctionThroughTheBoundedRuntime) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"UNWIND [1, 2, 3] AS value RETURN avg(value) AS mean, min(value) AS lowest, max(value) AS highest, "
			"stdev(value) AS sample_deviation, stdevp(value) AS population_deviation, "
			"percentileDisc(value, 0.5) AS discrete_median");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("mean").asPrimitive(), PropertyValue(2.0));
	EXPECT_EQ(row.at("lowest").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(row.at("highest").asPrimitive(), PropertyValue(3));
	EXPECT_EQ(row.at("sample_deviation").asPrimitive(), PropertyValue(1.0));
	EXPECT_NEAR(std::get<double>(row.at("population_deviation").asPrimitive().getVariant()), std::sqrt(2.0 / 3.0),
				1e-12);
	EXPECT_EQ(row.at("discrete_median").asPrimitive(), PropertyValue(2));
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesArithmeticComparisonAndThreeValuedBooleanExpressionsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN 1 + 2 * 3 AS arithmetic, -4 AS negated, 7 > 3 AS comparison, "
			"true AND null AS and_null, false OR null AS or_null, NOT false AS negated_boolean");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("arithmetic").asPrimitive(), PropertyValue(7));
	EXPECT_EQ(row.at("negated").asPrimitive(), PropertyValue(-4));
	EXPECT_EQ(row.at("comparison").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("and_null").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("or_null").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("negated_boolean").asPrimitive(), PropertyValue(true));
}

TEST(LogicalPlanReadRuntimeTest, RejectsInvalidRegularExpressionsAtTheLogicalRuntimeBoundary) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("RETURN 'value' =~ '[' AS matches");
	ASSERT_NE(plan.root, nullptr);

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, bounds())), std::runtime_error);
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesParserProducedStringComparisonOperatorsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN 'graph' STARTS WITH 'gr' AS starts, 'graph' ENDS WITH 'ph' AS ends, "
			"'graph' CONTAINS 'rap' AS contains, 'graph' =~ 'gr.*' AS matches");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("starts").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("ends").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("contains").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("matches").asPrimitive(), PropertyValue(true));
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesAllDecisiveThreeValuedBooleanOutcomesThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN false AND true AS false_left_and, true AND false AS false_right_and, "
			"null AND false AS false_after_null_and, true OR false AS true_left_or, "
			"false OR true AS true_right_or, null OR true AS true_after_null_or, "
			"false AND null AS false_and_null, true OR null AS true_or_null, "
			"true XOR false AS xor_true, false XOR false AS xor_false, null XOR true AS xor_null");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("false_left_and").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("false_right_and").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("false_after_null_and").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("true_left_or").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("true_right_or").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("true_after_null_or").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("false_and_null").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("true_or_null").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("xor_true").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("xor_false").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("xor_null").asPrimitive(), PropertyValue());
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesSimpleAndSearchedCaseExpressionsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN CASE 2 WHEN 1 THEN 'one' WHEN 2 THEN 'two' ELSE 'other' END AS simple_case, "
			"CASE 9 WHEN null THEN 'null' WHEN 1 THEN 'one' ELSE 'other' END AS simple_else, "
			"CASE WHEN 3 > 4 THEN 'wrong' WHEN 4 > 3 THEN 'right' ELSE 'other' END AS searched_case, "
			"CASE WHEN true THEN 'first' ELSE 'other' END AS searched_first, "
			"CASE WHEN false THEN 'wrong' ELSE 'other' END AS searched_else, "
			"CASE WHEN null THEN 'wrong' END AS null_case");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("simple_case").asPrimitive(), PropertyValue("two"));
	EXPECT_EQ(row.at("simple_else").asPrimitive(), PropertyValue("other"));
	EXPECT_EQ(row.at("searched_case").asPrimitive(), PropertyValue("right"));
	EXPECT_EQ(row.at("searched_first").asPrimitive(), PropertyValue("first"));
	EXPECT_EQ(row.at("searched_else").asPrimitive(), PropertyValue("other"));
	EXPECT_EQ(row.at("null_case").asPrimitive(), PropertyValue());
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesListMembershipSlicesAndComprehensionsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN 2 IN [1, 2, 3] AS member, 4 IN [1, 2, 3] AS non_member, null IN [1] AS null_member, "
			"2 IN [null, 2] AS member_after_null, 4 IN [null, 2] AS indeterminate_member, "
			"[1, 2, 3, 4][1..3] AS slice, [1, 2, 3][-5] AS out_of_range, "
			"[1, 2, 3][1..-5] AS reversed_slice, [value IN [1, 2] | value] AS identity, "
			"[value IN [1, 2, 3] WHERE value > 1 | value * 10] AS mapped");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("member").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("non_member").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("null_member").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("member_after_null").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("indeterminate_member").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("slice").asPrimitive().getList(),
			  (std::vector<PropertyValue>{PropertyValue(2), PropertyValue(3)}));
	EXPECT_EQ(row.at("out_of_range").asPrimitive(), PropertyValue());
	EXPECT_TRUE(row.at("reversed_slice").asPrimitive().getList().empty());
	EXPECT_EQ(row.at("identity").asPrimitive().getList(),
			  (std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)}));
	EXPECT_EQ(row.at("mapped").asPrimitive().getList(),
			  (std::vector<PropertyValue>{PropertyValue(20), PropertyValue(30)}));
}

TEST(LogicalPlanReadRuntimeTest, PreservesOpenEndedAndClampedListSliceSemanticsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN [1, 2, 3][-2..] AS suffix, [1, 2, 3][-9..2] AS clamped_start, "
			"[1, 2, 3][1..9] AS clamped_end");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("suffix").asPrimitive().getList(),
			  (std::vector<PropertyValue>{PropertyValue(2), PropertyValue(3)}));
	EXPECT_EQ(row.at("clamped_start").asPrimitive().getList(),
			  (std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)}));
	EXPECT_EQ(row.at("clamped_end").asPrimitive().getList(),
			  (std::vector<PropertyValue>{PropertyValue(2), PropertyValue(3)}));
}

TEST(LogicalPlanReadRuntimeTest, PropagatesNullListComprehensionsAndRejectsNonBooleanPredicates) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan nullSource = parser.parseToLogical("RETURN [value IN null | value] AS values");
	ASSERT_NE(nullSource.root, nullptr);
	const QueryResult nullResult = runtime.executeToQueryResult(*nullSource.root, bounds());
	ASSERT_EQ(nullResult.rowCount(), 1U);
	EXPECT_EQ(nullResult.getRows().front().at("values").asPrimitive(), PropertyValue());

	QueryPlan invalidPredicate = parser.parseToLogical("RETURN [value IN [1] WHERE 1 | value] AS values");
	ASSERT_NE(invalidPredicate.root, nullptr);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*invalidPredicate.root, bounds())), std::runtime_error);
}

TEST(LogicalPlanReadRuntimeTest, ListComprehensionRetainsSourceAndOutputUnderOneByteBudget) {
	NoGraphSnapshot snapshot;
	LogicalPlanReadRow row;
	QueryContext context;
	const std::string payload(1000U, 'v');
	const PropertyValue expected(std::vector<PropertyValue>{PropertyValue(payload), PropertyValue(payload)});
	context.parameters.emplace("values", expected);
	expressions::ListComprehensionExpression comprehension(
		"item", std::make_unique<expressions::ParameterExpression>("values"), nullptr,
		std::make_unique<expressions::VariableReferenceExpression>("item"),
		expressions::ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
	auto limited = bounds();
	limited.maximum_materialized_bytes = 3500U;
	ASSERT_LT(property_utils::getPropertyValueSize(expected), limited.maximum_materialized_bytes);
	try {
		static_cast<void>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			snapshot, comprehension, row, limited, context));
		FAIL() << "the source and accumulated output must share the byte budget";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "shared materialization exceeded its byte bound");
	}
	limited.maximum_materialized_bytes = 10000U;
	const auto result = std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
		snapshot, comprehension, row, limited, context));
	EXPECT_EQ(result, expected);
}

TEST(LogicalPlanReadRuntimeTest, ReduceRetainsSourceAndBothAccumulatorGenerationsUnderOneByteBudget) {
	NoGraphSnapshot snapshot;
	LogicalPlanReadRow row;
	QueryContext context;
	const std::string payload(1000U, 'v');
	context.parameters.emplace("initial", PropertyValue(payload));
	context.parameters.emplace("values", PropertyValue(std::vector<PropertyValue>{PropertyValue(payload)}));
	expressions::ReduceExpression reduce(
		"total", std::make_unique<expressions::ParameterExpression>("initial"),
		"item", std::make_unique<expressions::ParameterExpression>("values"),
		std::make_unique<expressions::VariableReferenceExpression>("item"));
	auto limited = bounds();
	limited.maximum_materialized_bytes = 2500U;
	try {
		static_cast<void>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			snapshot, reduce, row, limited, context));
		FAIL() << "the prior and next accumulator must coexist within the byte budget";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "shared materialization exceeded its byte bound");
	}
	limited.maximum_materialized_bytes = 8000U;
	const auto result = std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
		snapshot, reduce, row, limited, context));
	EXPECT_EQ(result, PropertyValue(payload));
}

TEST(LogicalPlanReadRuntimeTest, QuantifierRetainsCopiedSourceListWhileEvaluating) {
	NoGraphSnapshot snapshot;
	LogicalPlanReadRow row;
	QueryContext context;
	const std::string payload(1000U, 'v');
	context.parameters.emplace("values", PropertyValue(std::vector<PropertyValue>{
		PropertyValue(payload), PropertyValue(payload)}));
	expressions::QuantifierFunctionExpression quantifier(
		"any", "item", std::make_unique<expressions::ParameterExpression>("values"),
		std::make_unique<expressions::LiteralExpression>(true));
	auto limited = bounds();
	limited.maximum_materialized_bytes = 1500U;
	try {
		static_cast<void>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			snapshot, quantifier, row, limited, context));
		FAIL() << "the copied quantifier source must fit the shared byte budget";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "shared materialization exceeded its byte bound");
	}
	limited.maximum_materialized_bytes = 8000U;
	const auto result = std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
		snapshot, quantifier, row, limited, context));
	EXPECT_EQ(result, PropertyValue(true));
}

TEST(LogicalPlanReadRuntimeTest, ScopedListBindingAdmitsShadowCopyBeforeReplacingOuterValue) {
	NoGraphSnapshot snapshot;
	LogicalPlanReadRow row;
	const PropertyValue outer(std::string(1000U, 'o'));
	row.values.emplace("item", outer);
	QueryContext context;
	context.parameters.emplace("values", PropertyValue(std::vector<PropertyValue>{PropertyValue(1)}));
	expressions::ListComprehensionExpression comprehension(
		"item", std::make_unique<expressions::ParameterExpression>("values"),
		std::make_unique<expressions::LiteralExpression>(false), nullptr,
		expressions::ListComprehensionExpression::ComprehensionType::COMP_FILTER);
	auto limited = bounds();
	limited.maximum_materialized_bytes = 1000U;
	try {
		static_cast<void>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			snapshot, comprehension, row, limited, context));
		FAIL() << "the saved outer binding must be admitted before it is copied";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "shared materialization exceeded its byte bound");
	}
	EXPECT_EQ(std::get<PropertyValue>(row.values.at("item")), outer);
	limited.maximum_materialized_bytes = 5000U;
	const auto result = std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
		snapshot, comprehension, row, limited, context));
	EXPECT_EQ(result, PropertyValue(std::vector<PropertyValue>{}));
	EXPECT_EQ(std::get<PropertyValue>(row.values.at("item")), outer);
}

TEST(LogicalPlanReadRuntimeTest, ListLiteralAndSliceShareSourceAndOutputByteBudget) {
	NoGraphSnapshot snapshot;
	LogicalPlanReadRow row;
	QueryContext context;
	const std::string payload(1000U, 'v');
	context.parameters.emplace("first", PropertyValue(payload));
	context.parameters.emplace("second", PropertyValue(payload));
	const PropertyValue full(std::vector<PropertyValue>{PropertyValue(payload), PropertyValue(payload)});
	context.parameters.emplace("values", full);
	std::vector<std::unique_ptr<expressions::Expression>> elements;
	elements.push_back(std::make_unique<expressions::ParameterExpression>("first"));
	elements.push_back(std::make_unique<expressions::ParameterExpression>("second"));
	expressions::ListLiteralExpression literal(std::move(elements));
	auto limited = bounds();
	limited.maximum_materialized_bytes = 1500U;
	EXPECT_THROW(static_cast<void>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
		snapshot, literal, row, limited, context)), std::runtime_error);
	limited.maximum_materialized_bytes = 5000U;
	EXPECT_EQ(std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
		snapshot, literal, row, limited, context)), full);
	expressions::ListSliceExpression partial(
		std::make_unique<expressions::ParameterExpression>("values"),
		std::make_unique<expressions::LiteralExpression>(int64_t{0}),
		std::make_unique<expressions::LiteralExpression>(int64_t{1}), true);
	limited.maximum_materialized_bytes = 2500U;
	EXPECT_THROW(static_cast<void>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
		snapshot, partial, row, limited, context)), std::runtime_error);
	limited.maximum_materialized_bytes = 6000U;
	EXPECT_EQ(std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
		snapshot, partial, row, limited, context)),
		PropertyValue(std::vector<PropertyValue>{PropertyValue(payload)}));
	expressions::ListSliceExpression complete(
		std::make_unique<expressions::ParameterExpression>("values"), nullptr, nullptr, true);
	limited.maximum_materialized_bytes = 2500U;
	EXPECT_EQ(std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
		snapshot, complete, row, limited, context)), full);
}

TEST(LogicalPlanReadRuntimeTest, PropagatesNullCollectionInputsAndRejectsNonBooleanQuantifiers) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan nullInputs = parser.parseToLogical(
			"RETURN reduce(total = 0, value IN null | total + value) AS reduced, "
			"any(value IN null WHERE value > 0) AS any_value, null[0] AS sliced");
	ASSERT_NE(nullInputs.root, nullptr);
	const QueryResult nullResult = runtime.executeToQueryResult(*nullInputs.root, bounds());
	ASSERT_EQ(nullResult.rowCount(), 1U);
	const auto &row = nullResult.getRows().front();
	EXPECT_EQ(row.at("reduced").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("any_value").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("sliced").asPrimitive(), PropertyValue());

	QueryPlan invalidPredicate = parser.parseToLogical("RETURN any(value IN [1] WHERE 1) AS any_value");
	ASSERT_NE(invalidPredicate.root, nullptr);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*invalidPredicate.root, bounds())), std::runtime_error);
}

TEST(LogicalPlanReadRuntimeTest, MaterializesDynamicMapLiteralsForEachInputRowThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"UNWIND [1, null] AS value RETURN {value: value, doubled: value * 2, fixed: [1, 2]} AS item");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 2U);
	const auto &first = result.getRows()[0].at("item").asPrimitive().getMap();
	EXPECT_EQ(first.at("value"), PropertyValue(1));
	EXPECT_EQ(first.at("doubled"), PropertyValue(2));
	EXPECT_EQ(first.at("fixed"), PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)}));
	const auto &second = result.getRows()[1].at("item").asPrimitive().getMap();
	EXPECT_EQ(second.at("value"), PropertyValue());
	EXPECT_EQ(second.at("doubled"), PropertyValue());
	EXPECT_EQ(second.at("fixed"), PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)}));
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesQuantifierExpressionsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN all(value IN [1, 2, 3] WHERE value > 0) AS all_positive, "
			"all(value IN [1, 0, 3] WHERE value > 0) AS all_stops_on_false, "
			"any(value IN [1, 2, 3] WHERE value = 2) AS any_two, "
			"any(value IN [null, 0] WHERE value > 0) AS any_indeterminate, "
			"none(value IN [1, 2, 3] WHERE value < 0) AS none_negative, "
			"none(value IN [1, 2, 3] WHERE value = 2) AS none_stops_on_true, "
			"single(value IN [1, 2, 3] WHERE value = 2) AS exactly_one, "
			"single(value IN [1, 2, 3] WHERE value > 1) AS more_than_one, "
			"single(value IN [null, 0] WHERE value > 0) AS single_indeterminate");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("all_positive").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("all_stops_on_false").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("any_two").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("any_indeterminate").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("none_negative").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("none_stops_on_true").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("exactly_one").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("more_than_one").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("single_indeterminate").asPrimitive(), PropertyValue());
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesScalarFunctionCallsAndRejectsInvalidCallsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN upper('Ada') AS upper_name, substring('graph', 1, 3) AS substring_value, "
			"substring('graph', 1) AS open_ended_substring, abs(-4) AS absolute_value, "
			"coalesce(null, 'fallback') AS fallback_value");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("upper_name").asPrimitive(), PropertyValue("ADA"));
	EXPECT_EQ(row.at("substring_value").asPrimitive(), PropertyValue("rap"));
	EXPECT_EQ(row.at("open_ended_substring").asPrimitive(), PropertyValue("raph"));
	EXPECT_EQ(row.at("absolute_value").asPrimitive(), PropertyValue(4));
	EXPECT_EQ(row.at("fallback_value").asPrimitive(), PropertyValue("fallback"));

	QueryPlan invalidPlan = parser.parseToLogical("RETURN upper(1, 2) AS invalid");
	ASSERT_NE(invalidPlan.root, nullptr);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*invalidPlan.root, bounds())), std::runtime_error);
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesNativeScalarFunctionFamiliesThroughTheBoundedRuntime) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN lower('Ada') AS lower_name, trim('  graph  ') AS trimmed, ltrim('  graph') AS left_trimmed, "
			"rtrim('graph  ') AS right_trimmed, startsWith('graph', 'gr') AS starts, "
			"endsWith('graph', 'ph') AS ends, contains('graph', 'rap') AS contains, replace('aba', 'a', 'x') AS replaced, "
			"left('graph', 2) AS left_value, right('graph', 2) AS right_value, split('a,b', ',') AS split_value, "
			"reverse([1, 2, 3]) AS reversed, reverse('stressed') AS reversed_string, "
			"size([1, 2, 3]) AS list_size, size('graph') AS string_size, head([1, 2]) AS first, "
			"tail([1, 2]) AS rest, last([1, 2]) AS final, toInteger('42') AS integer_value, "
			"toFloat('4.5') AS float_value, pow(2, 3) AS powered, atan2(1, 1) AS atan_two, "
			"sqrt(4) AS square_root, sign(-2) AS sign_value, log(1) AS natural_log, log10(10) AS common_log, "
			"exp(0) AS exponential, sin(0) AS sine, cos(0) AS cosine, tan(0) AS tangent, "
			"asin(0) AS arc_sine, acos(1) AS arc_cosine, atan(0) AS arc_tangent, pi() AS pi_value, e() AS e_value, "
			"bytes('0x0a0b') AS bytes_value");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("lower_name").asPrimitive(), PropertyValue("ada"));
	EXPECT_EQ(row.at("trimmed").asPrimitive(), PropertyValue("graph"));
	EXPECT_EQ(row.at("list_size").asPrimitive(), PropertyValue(3));
	EXPECT_EQ(row.at("reversed_string").asPrimitive(), PropertyValue("desserts"));
	EXPECT_EQ(row.at("string_size").asPrimitive(), PropertyValue(5));
	EXPECT_EQ(row.at("first").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(row.at("final").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(row.at("integer_value").asPrimitive(), PropertyValue(42));
	EXPECT_EQ(row.at("float_value").asPrimitive(), PropertyValue(4.5));
	EXPECT_EQ(row.at("bytes_value").asPrimitive().getBytes().size(), 2U);
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesListLengthAndOrdinaryListSizeThroughTheirDistinctPaths) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN length([1, 2, 3]) AS list_length, length('graph') AS string_length, "
			"size([1, 2, 3]) AS list_size");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("list_length").asPrimitive(), PropertyValue(3));
	EXPECT_EQ(result.getRows().front().at("string_length").asPrimitive(), PropertyValue(5));
	EXPECT_EQ(result.getRows().front().at("list_size").asPrimitive(), PropertyValue(3));
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesContinuousPercentileAggregateThroughTheBoundedRuntime) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;

	// Keep this as a tiny deterministic aggregate so the runtime's continuous
	// percentile path is exercised without introducing a scale workload.
	QueryPlan plan = parser.parseToLogical(
			"UNWIND [10, 20, 30] AS value RETURN percentileCont(value, 0.5) AS median");
	ASSERT_NE(plan.root, nullptr);
	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("median").asPrimitive(), PropertyValue(20));

	QueryPlan fractionalPlan = parser.parseToLogical(
			"UNWIND [10, 20, 30] AS value RETURN percentileCont(value, 0.25) AS quartile");
	ASSERT_NE(fractionalPlan.root, nullptr);
	const QueryResult fractional = runtime.executeToQueryResult(*fractionalPlan.root, bounds());
	ASSERT_EQ(fractional.rowCount(), 1U);
	EXPECT_EQ(fractional.getRows().front().at("quartile").asPrimitive(), PropertyValue(15.0));

	QueryPlan invalidPlan = parser.parseToLogical(
			"UNWIND [10, 20, 30] AS value RETURN percentileCont(value, 2.0) AS invalid");
	ASSERT_NE(invalidPlan.root, nullptr);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*invalidPlan.root, bounds())), std::invalid_argument);
}

TEST(LogicalPlanReadRuntimeTest, BoundsRetainedAggregatePayloadByMaterializedBytes) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("UNWIND $items AS item RETURN collect(item) AS values");
	ASSERT_NE(plan.root, nullptr);
	QueryContext context;
	context.parameters.emplace("items", PropertyValue(std::vector<PropertyValue>{
			PropertyValue(std::string(1000U, 'a')), PropertyValue(std::string(1000U, 'b'))}));
	LogicalPlanReadRuntimeBounds limited = bounds();
	limited.maximum_materialized_bytes = 2048U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, limited, context)), std::runtime_error);

	QueryPlan countPlan = parser.parseToLogical("UNWIND $items AS item RETURN count(item) AS total");
	ASSERT_NE(countPlan.root, nullptr);
	EXPECT_NO_THROW(static_cast<void>(runtime.executeToQueryResult(*countPlan.root, limited, context)));
}

TEST(LogicalPlanUnwindRuntimeTest, BorrowedListsPreserveLargeNestedValuesAndRepeatedExecution) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	const PropertyValue nested(PropertyValue::MapType{{"payload", PropertyValue(std::string(8192U, 'x'))}});
	const std::vector<PropertyValue> items{nested, PropertyValue(), nested};
	QueryContext context;
	context.parameters.emplace("items", PropertyValue(items));
	for (const bool literal: {false, true}) {
		std::unique_ptr<logical::LogicalOperator> root;
		if (literal) {
			root = std::make_unique<logical::LogicalUnwind>(std::make_unique<logical::LogicalSingleRow>(), "item", items);
		} else {
			root = parser.parseToLogical("UNWIND $items AS item RETURN item").root;
		}
		ASSERT_NE(root, nullptr);
		for (const bool stopEarly: {true, false, false}) {
			SCOPED_TRACE(literal);
			SCOPED_TRACE(stopEarly);
			std::vector<PropertyValue> delivered;
			static_cast<void>(runtime.execute(*root, bounds(), [&](const LogicalPlanReadRow &row) {
				delivered.push_back(std::get<PropertyValue>(row.values.at("item")));
				return !stopEarly;
			}, context));
			EXPECT_EQ(delivered, stopEarly ? std::vector<PropertyValue>{nested} : items);
			EXPECT_EQ(context.parameters.at("items"), PropertyValue(items));
		}
	}
}

TEST(LogicalPlanUnwindRuntimeTest, RejectsOversizedTransientRows) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("UNWIND $items AS item RETURN item");
	ASSERT_NE(plan.root, nullptr);
	QueryContext context;
	context.parameters.emplace("items", PropertyValue(std::vector<PropertyValue>{
			PropertyValue(std::string(256U, 'x'))}));
	LogicalPlanReadRuntimeBounds limited = bounds();
	limited.maximum_materialized_bytes = 64U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, limited, context)), std::runtime_error);
}

TEST(LogicalPlanUnwindRuntimeTest, BoundsScalarProjectionResultsBeforePublicMaterialization) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("RETURN $value AS value");
	ASSERT_NE(plan.root, nullptr);
	QueryContext context;
	context.parameters.emplace("value", PropertyValue(std::string(256U, 'x')));
	LogicalPlanReadRuntimeBounds limited = bounds();
	limited.maximum_materialized_bytes = 64U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, limited, context)), std::runtime_error);
}

TEST(LogicalPlanUnwindRuntimeTest, RejectsInputListsAboveRowBound) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("UNWIND $items AS item RETURN item");
	ASSERT_NE(plan.root, nullptr);
	QueryContext context;
	context.parameters.emplace("items", PropertyValue(std::vector<PropertyValue>{
			PropertyValue(1), PropertyValue(2), PropertyValue(3)}));
	LogicalPlanReadRuntimeBounds limited = bounds();
	limited.maximum_rows = 2U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, limited, context)), std::runtime_error);
}

TEST(LogicalPlanReadRuntimeTest, NativeConversionContractAcceptsOnlyExplicitToBooleanInputs) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan acceptedPlan = parser.parseToLogical(
			"RETURN toBoolean(null) AS null_value, toBoolean(true) AS boolean_value, toBoolean(0) AS zero, "
			"toBoolean(-2.5) AS number, toBoolean('TRUE') AS true_text, toBoolean('false') AS false_text");
	ASSERT_NE(acceptedPlan.root, nullptr);

	const QueryResult accepted = runtime.executeToQueryResult(*acceptedPlan.root, bounds());
	ASSERT_EQ(accepted.rowCount(), 1U);
	const auto &row = accepted.getRows().front();
	EXPECT_EQ(row.at("null_value").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("boolean_value").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("zero").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("number").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("true_text").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("false_text").asPrimitive(), PropertyValue(false));

	for (const std::string_view query: {"RETURN toBoolean('yes') AS invalid", "RETURN toBoolean([]) AS invalid"}) {
		QueryPlan invalidPlan = parser.parseToLogical(std::string(query));
		ASSERT_NE(invalidPlan.root, nullptr);
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*invalidPlan.root, bounds())), std::runtime_error)
				<< query;
	}
}

TEST(LogicalPlanReadRuntimeTest, RejectsNativeScalarFunctionTypeMismatchesAtTheRuntimeBoundary) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	const std::vector<std::string> invalidQueries = {
			"RETURN abs('x') AS value",       "RETURN ceil('x') AS value",
			"RETURN floor('x') AS value",     "RETURN sqrt('x') AS value",
			"RETURN pow('x', 2) AS value",    "RETURN atan2(1, 'x') AS value",
			"RETURN upper(1) AS value",       "RETURN lower(1) AS value",
			"RETURN trim(1) AS value",        "RETURN substring(1, 0) AS value",
			"RETURN substring('x', '0') AS value", "RETURN startsWith(1, 'x') AS value",
			"RETURN endsWith('x', 1) AS value", "RETURN contains(1, 'x') AS value",
			"RETURN split('x', 1) AS value",  "RETURN replace('x', 1, 'y') AS value",
			"RETURN left(1, 1) AS value",      "RETURN right('x', '1') AS value",
			"RETURN length(1) AS value",       "RETURN head('x') AS value",
			"RETURN tail('x') AS value",       "RETURN last('x') AS value",
			"RETURN nodes('x') AS value",      "RETURN relationships('x') AS value",
	};
	for (const std::string &query: invalidQueries) {
		QueryPlan plan = parser.parseToLogical(query);
		ASSERT_NE(plan.root, nullptr) << query;
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, bounds())), std::runtime_error) << query;
	}
}

TEST(LogicalPlanReadRuntimeTest, NativeRangeContractPreservesNullAndRejectsNonFiniteInputs) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan rangePlan = parser.parseToLogical("RETURN range($start, $end, $step) AS values");
	ASSERT_NE(rangePlan.root, nullptr);

	QueryContext fractional;
	fractional.parameters.emplace("start", PropertyValue(1.9));
	fractional.parameters.emplace("end", PropertyValue(3.9));
	fractional.parameters.emplace("step", PropertyValue(1.0));
	const QueryResult fractionalResult = runtime.executeToQueryResult(*rangePlan.root, bounds(), fractional);
	ASSERT_EQ(fractionalResult.rowCount(), 1U);
	EXPECT_EQ(fractionalResult.getRows().front().at("values").asPrimitive().getList(),
			  (std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3)}));

	QueryContext nullArgument;
	nullArgument.parameters.emplace("start", PropertyValue());
	nullArgument.parameters.emplace("end", PropertyValue(3));
	nullArgument.parameters.emplace("step", PropertyValue(1));
	const QueryResult nullResult = runtime.executeToQueryResult(*rangePlan.root, bounds(), nullArgument);
	ASSERT_EQ(nullResult.rowCount(), 1U);
	EXPECT_EQ(nullResult.getRows().front().at("values").asPrimitive(), PropertyValue());

	QueryContext nonFinite;
	nonFinite.parameters.emplace("start", PropertyValue(std::numeric_limits<double>::infinity()));
	nonFinite.parameters.emplace("end", PropertyValue(3));
	nonFinite.parameters.emplace("step", PropertyValue(1));
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*rangePlan.root, bounds(), nonFinite)), std::runtime_error);
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesTemporalArithmeticThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN date('2024-01-30') + duration('P2D') AS shifted_date, "
			"duration('P1D') + duration('PT2H') AS combined_duration, "
			"date('2024-01-03') - date('2024-01-01') AS date_difference");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(std::get<TemporalDate>(row.at("shifted_date").asPrimitive().getVariant()).toISO(), "2024-02-01");
	EXPECT_EQ(std::get<TemporalDuration>(row.at("combined_duration").asPrimitive().getVariant()),
			  (TemporalDuration{0, 1, 2LL * 60LL * 60LL * 1000000000LL}));
	EXPECT_EQ(std::get<TemporalDuration>(row.at("date_difference").asPrimitive().getVariant()),
			  (TemporalDuration{0, 2, 0}));
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesEverySupportedTemporalArithmeticOperandOrderThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN duration('P2D') + date('2024-01-30') AS date_on_right, "
			"datetime('2024-01-30T12:00:00') + duration('P2D') AS datetime_on_left, "
			"duration('P1D') + datetime('2024-01-30T12:00:00') AS datetime_on_right, "
			"duration('P3D') - duration('P1D') AS duration_difference, "
			"date('2024-01-03') - duration('P1D') AS date_minus_duration, "
			"datetime('2024-01-03T12:00:00') - duration('P1D') AS datetime_minus_duration, "
			"datetime('2024-01-03T12:00:00') - datetime('2024-01-01T00:00:00') AS datetime_difference");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(std::get<TemporalDate>(row.at("date_on_right").asPrimitive().getVariant()).toISO(), "2024-02-01");
	EXPECT_EQ(std::get<TemporalDateTime>(row.at("datetime_on_left").asPrimitive().getVariant()).toISO(),
			  "2024-02-01T12:00:00");
	EXPECT_EQ(std::get<TemporalDateTime>(row.at("datetime_on_right").asPrimitive().getVariant()).toISO(),
			  "2024-01-31T12:00:00");
	EXPECT_EQ(std::get<TemporalDuration>(row.at("duration_difference").asPrimitive().getVariant()),
			  (TemporalDuration{0, 2, 0}));
	EXPECT_EQ(std::get<TemporalDate>(row.at("date_minus_duration").asPrimitive().getVariant()).toISO(), "2024-01-02");
	EXPECT_EQ(std::get<TemporalDateTime>(row.at("datetime_minus_duration").asPrimitive().getVariant()).toISO(),
			  "2024-01-02T12:00:00");
	EXPECT_EQ(std::get<TemporalDuration>(row.at("datetime_difference").asPrimitive().getVariant()),
			  (TemporalDuration{0, 2, 12LL * 60LL * 60LL * 1'000'000'000LL}));
}

TEST(LogicalPlanReadRuntimeTest, PreservesArithmeticPrecedenceAssociativityAndFailureSemanticsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN 8 - 3 - 1 AS subtraction, 20 / 5 AS division, 17 % 5 AS remainder, "
			"2 ^ 3 AS power, 'node-' + 7 AS concatenated");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("subtraction").asPrimitive(), PropertyValue(4));
	EXPECT_EQ(row.at("division").asPrimitive(), PropertyValue(4.0));
	EXPECT_EQ(row.at("remainder").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(row.at("power").asPrimitive(), PropertyValue(8.0));
	EXPECT_EQ(row.at("concatenated").asPrimitive(), PropertyValue("node-7"));

	QueryPlan divideByZero = parser.parseToLogical("RETURN 1 / 0 AS invalid");
	ASSERT_NE(divideByZero.root, nullptr);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*divideByZero.root, bounds())), std::runtime_error);
}

TEST(LogicalPlanReadRuntimeTest, RejectsIntegerArithmeticOverflowAtTheExpressionBoundary) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	const auto expectOverflow = [&](std::string expression) {
		QueryPlan plan = parser.parseToLogical(std::move(expression));
		ASSERT_NE(plan.root, nullptr);
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, bounds())), std::overflow_error);
	};

	// Exercise both operand orders for each signed overflow guard. These are
	// scalar boundary checks, not a bulk workload.
	expectOverflow("RETURN 9223372036854775807 + 1 AS overflow");
	expectOverflow("RETURN -9223372036854775807 - 2 AS overflow");
	expectOverflow("RETURN -9223372036854775807 + -2 AS overflow");
	expectOverflow("RETURN 9223372036854775807 - -1 AS overflow");
	expectOverflow("RETURN 3037000500 * 3037000500 AS overflow");
	expectOverflow("RETURN -3037000500 * 3037000500 AS overflow");
	expectOverflow("RETURN 3037000500 * -3037000500 AS overflow");
	expectOverflow("RETURN -3037000500 * -3037000500 AS overflow");

	QueryContext boundaryValues;
	boundaryValues.parameters.emplace("minimum", PropertyValue(std::numeric_limits<int64_t>::min()));
	boundaryValues.parameters.emplace("maximum_double", PropertyValue(std::numeric_limits<double>::max()));
	QueryPlan moduloOverflow = parser.parseToLogical("RETURN $minimum % -1 AS overflow");
	ASSERT_NE(moduloOverflow.root, nullptr);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*moduloOverflow.root, bounds(), boundaryValues)),
			 std::overflow_error);

	QueryPlan negationOverflow = parser.parseToLogical("RETURN -$minimum AS overflow");
	ASSERT_NE(negationOverflow.root, nullptr);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*negationOverflow.root, bounds(), boundaryValues)),
			 std::overflow_error);

	QueryPlan floatingPointOverflow = parser.parseToLogical("RETURN $maximum_double ^ 2 AS overflow");
	ASSERT_NE(floatingPointOverflow.root, nullptr);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*floatingPointOverflow.root, bounds(), boundaryValues)),
			 std::overflow_error);
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesReduceDynamicMembershipAndListBoundarySemanticsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"UNWIND $value_lists AS values "
			"RETURN reduce(total = 0, value IN values | total + value) AS reduced, "
			"2 IN values AS dynamic_member, values[-1] AS last, values[99] AS missing, values[..2] AS prefix");
	ASSERT_NE(plan.root, nullptr);
	QueryContext context;
	context.parameters.emplace(
			"value_lists", PropertyValue(std::vector<PropertyValue>{
																				PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3)})}));

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds(), context);
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("reduced").asPrimitive(), PropertyValue(6));
	EXPECT_EQ(row.at("dynamic_member").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("last").asPrimitive(), PropertyValue(3));
	EXPECT_EQ(row.at("missing").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("prefix").asPrimitive().getList(), (std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)}));
}

TEST(LogicalPlanReadRuntimeTest, PreservesDynamicInThreeValuedSemanticsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"UNWIND $candidate_lists AS candidates "
			"RETURN 2 IN candidates AS matches, 4 IN candidates AS missing, null IN candidates AS null_value");
	ASSERT_NE(plan.root, nullptr);

	QueryContext context;
	context.parameters.emplace(
			"candidate_lists", PropertyValue(std::vector<PropertyValue>{
											PropertyValue(std::vector<PropertyValue>{PropertyValue(2)}),
											PropertyValue(std::vector<PropertyValue>{PropertyValue(), PropertyValue(1)}),
											PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(3)}),
										}));

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds(), context);
	ASSERT_EQ(result.rowCount(), 3U);
	EXPECT_EQ(result.getRows()[0].at("matches").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(result.getRows()[0].at("missing").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(result.getRows()[1].at("matches").asPrimitive(), PropertyValue());
	EXPECT_EQ(result.getRows()[1].at("missing").asPrimitive(), PropertyValue());
	EXPECT_EQ(result.getRows()[2].at("matches").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(result.getRows()[2].at("missing").asPrimitive(), PropertyValue(false));
	for (const QueryResult::Row &row: result.getRows()) {
		EXPECT_EQ(row.at("null_value").asPrimitive(), PropertyValue());
	}
}

TEST(LogicalPlanReadRuntimeTest, PropagatesANullDynamicListOperandForInThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("UNWIND $candidate_lists AS candidates RETURN 1 IN candidates AS member");
	ASSERT_NE(plan.root, nullptr);

	QueryContext context;
	context.parameters.emplace(
			"candidate_lists", PropertyValue(std::vector<PropertyValue>{
											PropertyValue(),
											PropertyValue(std::vector<PropertyValue>{}),
											PropertyValue(std::vector<PropertyValue>{PropertyValue(1)}),
										}));
	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds(), context);
	ASSERT_EQ(result.rowCount(), 3U);
	EXPECT_EQ(result.getRows()[0].at("member").asPrimitive(), PropertyValue());
	EXPECT_EQ(result.getRows()[1].at("member").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(result.getRows()[2].at("member").asPrimitive(), PropertyValue(true));
}

TEST(LogicalPlanReadRuntimeTest, PreservesQuantifierEmptyAndNullSemanticsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN all(value IN [] WHERE value > 0) AS all_empty, "
			"any(value IN [] WHERE value > 0) AS any_empty, "
			"none(value IN [] WHERE value > 0) AS none_empty, "
			"single(value IN [] WHERE value > 0) AS single_empty, "
			"all(value IN [null, 1] WHERE value > 0) AS all_null");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("all_empty").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("any_empty").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("none_empty").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("single_empty").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("all_null").asPrimitive().getType(), PropertyType::NULL_TYPE);
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesTemporalComponentsAndDateTimeDurationArithmeticThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"UNWIND $shifted_datetimes AS shifted_datetime "
			"UNWIND $date_values AS date_value "
			"UNWIND $duration_values AS duration_value "
			"UNWIND $scalar_values AS scalar_value "
			"RETURN shifted_datetime.year AS datetime_year, shifted_datetime.month AS datetime_month, "
			"shifted_datetime.day AS datetime_day, "
			"shifted_datetime.hour AS shifted_hour, shifted_datetime.minute AS datetime_minute, "
			"shifted_datetime.second AS datetime_second, shifted_datetime.epochMillis AS datetime_epoch_millis, "
			"date_value.year AS date_year, date_value.month AS date_month, date_value.day AS date_day, "
			"date_value.epochDays AS date_epoch_days, duration_value.months AS duration_months, "
			"duration_value.days AS duration_days, duration_value.seconds AS duration_seconds, "
			"duration_value.nanoseconds AS duration_nanoseconds, date_value.unknown AS unknown_component, "
			"scalar_value.year AS non_temporal_component");
	ASSERT_NE(plan.root, nullptr);
	QueryContext context;
	context.parameters.emplace("shifted_datetimes", PropertyValue(std::vector<PropertyValue>{
			PropertyValue(TemporalDateTime::fromComponents(2024, 1, 2, 1, 0, 0))}));
	context.parameters.emplace("date_values", PropertyValue(std::vector<PropertyValue>{
			PropertyValue(TemporalDate::fromYMD(2024, 3, 1))}));
	context.parameters.emplace("duration_values", PropertyValue(std::vector<PropertyValue>{
			PropertyValue(TemporalDuration{2, 3, 4LL * 60LL * 60LL * 1000000000LL})}));
	context.parameters.emplace("scalar_values", PropertyValue(std::vector<PropertyValue>{PropertyValue(42)}));

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds(), context);
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("datetime_year").asPrimitive(), PropertyValue(2024));
	EXPECT_EQ(row.at("datetime_month").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(row.at("datetime_day").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(row.at("shifted_hour").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(row.at("datetime_minute").asPrimitive(), PropertyValue(0));
	EXPECT_EQ(row.at("datetime_second").asPrimitive(), PropertyValue(0));
	EXPECT_EQ(row.at("datetime_epoch_millis").asPrimitive(),
			  PropertyValue(std::get<TemporalDateTime>(context.parameters.at("shifted_datetimes").getList().front().getVariant()).epochMillis));
	EXPECT_EQ(row.at("date_year").asPrimitive(), PropertyValue(2024));
	EXPECT_EQ(row.at("date_month").asPrimitive(), PropertyValue(3));
	EXPECT_EQ(row.at("date_day").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(row.at("date_epoch_days").asPrimitive(),
			  PropertyValue(static_cast<int64_t>(std::get<TemporalDate>(context.parameters.at("date_values").getList().front().getVariant()).epochDays)));
	EXPECT_EQ(row.at("duration_months").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(row.at("duration_days").asPrimitive(), PropertyValue(3));
	EXPECT_EQ(row.at("duration_seconds").asPrimitive(), PropertyValue(14'400));
	EXPECT_EQ(row.at("duration_nanoseconds").asPrimitive(), PropertyValue(14'400'000'000'000LL));
	EXPECT_EQ(row.at("unknown_component").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("non_temporal_component").asPrimitive(), PropertyValue());
}

TEST(LogicalPlanReadRuntimeTest, PreservesNegativeMonthRolloverAcrossTheTemporalEraThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"UNWIND $dates AS date_value UNWIND $datetimes AS datetime_value "
			"UNWIND $durations AS duration_value "
			"RETURN date_value + duration_value AS shifted_date, "
			"datetime_value + duration_value AS shifted_datetime");
	ASSERT_NE(plan.root, nullptr);

	QueryContext context;
	context.parameters.emplace("dates", PropertyValue(std::vector<PropertyValue>{
			PropertyValue(TemporalDate::fromYMD(0, 1, 10))}));
	context.parameters.emplace("datetimes", PropertyValue(std::vector<PropertyValue>{
			PropertyValue(TemporalDateTime::fromComponents(0, 1, 10, 8, 15, 20))}));
	context.parameters.emplace("durations", PropertyValue(std::vector<PropertyValue>{
			PropertyValue(TemporalDuration{-1, 0, 0})}));

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds(), context);
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(std::get<TemporalDate>(row.at("shifted_date").asPrimitive().getVariant()).month(), 12);
	EXPECT_EQ(std::get<TemporalDateTime>(row.at("shifted_datetime").asPrimitive().getVariant()).month(), 12);
}

TEST(LogicalPlanReadRuntimeTest, RejectsTemporalArithmeticOverflowAtTheExpressionBoundary) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryContext context;
	context.parameters.emplace("date", PropertyValue(TemporalDate{0}));
	context.parameters.emplace("datetime", PropertyValue(TemporalDateTime{0}));

	const auto expectOverflow = [&](const std::string &query) {
		QueryPlan plan = parser.parseToLogical(query);
		ASSERT_NE(plan.root, nullptr);
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, bounds(), context)),
				 std::overflow_error);
	};

	context.parameters.emplace("months", PropertyValue(TemporalDuration{std::numeric_limits<int64_t>::max(), 0, 0}));
	expectOverflow("RETURN $date + $months AS overflow");

	context.parameters.emplace("days", PropertyValue(TemporalDuration{0, std::numeric_limits<int64_t>::max(), 0}));
	expectOverflow("RETURN $datetime + $days AS overflow");
}

TEST(LogicalPlanReadRuntimeTest, RejectsTemporalYearAndDateRangeOverflowBeforeReturningAValue) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"UNWIND $dates AS date_value UNWIND $durations AS duration_value "
			"RETURN date_value + duration_value AS shifted");
	ASSERT_NE(plan.root, nullptr);

	QueryContext yearOverflow;
	yearOverflow.parameters.emplace("dates", PropertyValue(std::vector<PropertyValue>{
			PropertyValue(TemporalDate::fromYMD(2024, 1, 1))}));
	yearOverflow.parameters.emplace("durations", PropertyValue(std::vector<PropertyValue>{
			PropertyValue(TemporalDuration{std::numeric_limits<int64_t>::max(), 0, 0})}));
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, bounds(), yearOverflow)), std::overflow_error);

	QueryContext dateOverflow;
	dateOverflow.parameters.emplace("dates", PropertyValue(std::vector<PropertyValue>{
			PropertyValue(TemporalDate::fromYMD(2024, 1, 1))}));
	dateOverflow.parameters.emplace("durations", PropertyValue(std::vector<PropertyValue>{
			PropertyValue(TemporalDuration{0, std::numeric_limits<int64_t>::max(), 0})}));
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, bounds(), dateOverflow)), std::overflow_error);
}

TEST(LogicalPlanReadRuntimeTest, ExecutesBoundedCartesianJoinWithoutPhysicalStorageRecords) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto left = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "left",
			std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)});
	auto right = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "right",
			std::vector<PropertyValue>{PropertyValue("a"), PropertyValue("b")});
	logical::LogicalJoin join(std::move(left), std::move(right));

	const QueryResult result = runtime.executeToQueryResult(join, bounds());
	ASSERT_EQ(result.rowCount(), 4U);
	EXPECT_EQ(result.getRows()[0].at("left").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(result.getRows()[0].at("right").asPrimitive(), PropertyValue("a"));
	EXPECT_EQ(result.getRows()[1].at("left").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(result.getRows()[1].at("right").asPrimitive(), PropertyValue("b"));
	EXPECT_EQ(result.getRows()[2].at("left").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(result.getRows()[2].at("right").asPrimitive(), PropertyValue("a"));
	EXPECT_EQ(result.getRows()[3].at("left").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(result.getRows()[3].at("right").asPrimitive(), PropertyValue("b"));
}

TEST(LogicalPlanReadRuntimeTest, JoinsOnlyCompatibleBindingsAndRejectsAnUnboundedMaterializedSide) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto left = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "shared", std::vector<PropertyValue>{PropertyValue(1)});
	auto right = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "shared", std::vector<PropertyValue>{PropertyValue(2)});
	logical::LogicalJoin incompatible(std::move(left), std::move(right));
	EXPECT_EQ(runtime.executeToQueryResult(incompatible, bounds()).rowCount(), 0U);

	auto boundedLeft = std::make_unique<logical::LogicalSingleRow>();
	auto oversizedRight = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "right",
			std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)});
	logical::LogicalJoin oversized(std::move(boundedLeft), std::move(oversizedRight));
	LogicalPlanReadRuntimeBounds constrained = bounds();
	constrained.maximum_rows = 1U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(oversized, constrained)), std::runtime_error);
}

TEST(LogicalPlanReadRuntimeTest, HashJoinPreservesDuplicateCandidateOrder) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto left = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "shared",
			std::vector<PropertyValue>{PropertyValue(2), PropertyValue(1)});
	auto right = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "shared",
			std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(1)});
	// The right side has a duplicate binding. Candidate indexing must retain both
	// rows and their original order instead of turning the join into DISTINCT.
	auto rightProjection = std::make_unique<logical::LogicalProject>(std::move(right), std::vector<logical::LogicalProjectItem>{
			{std::make_shared<expressions::VariableReferenceExpression>("shared"), "shared"}});
	logical::LogicalJoin join(std::move(left), std::move(rightProjection));

		const QueryResult result = runtime.executeToQueryResult(join, bounds());
	ASSERT_EQ(result.rowCount(), 3U);
	EXPECT_EQ(result.getRows()[0].at("shared").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(result.getRows()[1].at("shared").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(result.getRows()[2].at("shared").asPrimitive(), PropertyValue(1));
}

TEST(LogicalPlanReadRuntimeTest, HashJoinChargesBindingIndexMetadataAgainstByteBound) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	const std::string longBinding(512U, 'x');
	auto left = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "shared",
			std::vector<PropertyValue>{PropertyValue(longBinding)});
	auto right = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "shared",
			std::vector<PropertyValue>{PropertyValue(longBinding)});
	logical::LogicalJoin join(std::move(left), std::move(right));
	LogicalPlanReadRuntimeBounds constrained = bounds();
	constrained.maximum_materialized_bytes = 512U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(join, constrained)), std::runtime_error);
}

TEST(LogicalPlanReadRuntimeTest, CorrelatesOptionalMatchAndNullExtendsOnlyNewVariables) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto input = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "a",
			std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)});
	auto patternInput = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "a", std::vector<PropertyValue>{PropertyValue(1)});
	auto pattern = std::make_unique<logical::LogicalUnwind>(
			std::move(patternInput), "b", std::vector<PropertyValue>{PropertyValue("matched")});
	logical::LogicalOptionalMatch optional(std::move(input), std::move(pattern), {"b"});

	const QueryResult result = runtime.executeToQueryResult(optional, bounds());
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows()[0].at("a").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(result.getRows()[0].at("b").asPrimitive(), PropertyValue("matched"));
	EXPECT_EQ(result.getRows()[1].at("a").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(result.getRows()[1].at("b").asPrimitive(), PropertyValue());
}

TEST(LogicalPlanReadRuntimeTest, OptionalMatchPreservesNullBindingsAndDuplicatePatternRows) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto input = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "shared",
			std::vector<PropertyValue>{PropertyValue(), PropertyValue(7)});
	auto patternInput = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "shared",
			std::vector<PropertyValue>{PropertyValue(), PropertyValue(), PropertyValue(7)});
	auto pattern = std::make_unique<logical::LogicalUnwind>(
			std::move(patternInput), "matched",
			std::vector<PropertyValue>{PropertyValue("a"), PropertyValue("b")});
	logical::LogicalOptionalMatch optional(std::move(input), std::move(pattern), {"matched"});

	const QueryResult result = runtime.executeToQueryResult(optional, bounds());
	// NULL is a real binding value for correlation, and both pattern rows must
	// survive the hash index rather than being collapsed into DISTINCT.
	ASSERT_EQ(result.rowCount(), 6U);
	EXPECT_EQ(result.getRows()[0].at("shared").asPrimitive(), PropertyValue());
	EXPECT_EQ(result.getRows()[0].at("matched").asPrimitive(), PropertyValue("a"));
	EXPECT_EQ(result.getRows()[1].at("shared").asPrimitive(), PropertyValue());
	EXPECT_EQ(result.getRows()[1].at("matched").asPrimitive(), PropertyValue("b"));
	EXPECT_EQ(result.getRows()[2].at("shared").asPrimitive(), PropertyValue());
	EXPECT_EQ(result.getRows()[2].at("matched").asPrimitive(), PropertyValue("a"));
	EXPECT_EQ(result.getRows()[3].at("shared").asPrimitive(), PropertyValue());
	EXPECT_EQ(result.getRows()[3].at("matched").asPrimitive(), PropertyValue("b"));
	EXPECT_EQ(result.getRows()[4].at("shared").asPrimitive(), PropertyValue(7));
	EXPECT_EQ(result.getRows()[4].at("matched").asPrimitive(), PropertyValue("a"));
	EXPECT_EQ(result.getRows()[5].at("shared").asPrimitive(), PropertyValue(7));
	EXPECT_EQ(result.getRows()[5].at("matched").asPrimitive(), PropertyValue("b"));
}

TEST(LogicalPlanReadRuntimeTest, OptionalMatchStopsDownstreamWithoutEmittingRowsAfterCancellation) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto input = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "shared",
			std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3)});
	auto patternInput = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "shared",
			std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3)});
	auto pattern = std::make_unique<logical::LogicalUnwind>(
			std::move(patternInput), "matched", std::vector<PropertyValue>{PropertyValue("ok")});
	logical::LogicalOptionalMatch optional(std::move(input), std::move(pattern), {"matched"});
	uint64_t delivered = 0U;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			optional, bounds(), [&delivered](const LogicalPlanReadRow &) {
				++delivered;
				return false;
			});
	EXPECT_EQ(delivered, 1U);
	EXPECT_EQ(stats.rows_emitted, 1U);
}

TEST(LogicalPlanReadRuntimeTest, RejectsOptionalMatchPatternRowsBeyondTheMaterializationBound) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto input = std::make_unique<logical::LogicalSingleRow>();
	auto pattern = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "optional_value",
			std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)});
	logical::LogicalOptionalMatch optional(std::move(input), std::move(pattern), {"optional_value"});
	LogicalPlanReadRuntimeBounds constrained = bounds();
	constrained.maximum_rows = 1U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(optional, constrained)), std::runtime_error);
}

TEST(LogicalPlanReadRuntimeTest, AccountsOptionalMatchBindingIndexMetadataInByteBound) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	std::vector<PropertyValue> bindings;
	bindings.reserve(100U);
	for (int64_t value = 0; value < 100; ++value) {
		bindings.emplace_back(value);
	}
	auto patternInput = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "shared", std::move(bindings));
	auto pattern = std::make_unique<logical::LogicalUnwind>(
			std::move(patternInput), "optional_value", std::vector<PropertyValue>{PropertyValue("matched")});
	auto input = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "shared",
			std::vector<PropertyValue>{PropertyValue(int64_t{999})});
	logical::LogicalOptionalMatch optional(std::move(input), std::move(pattern), {"shared"});
	LogicalPlanReadRuntimeBounds constrained = bounds();
	constrained.maximum_rows = 128U;
	constrained.maximum_materialized_bytes = 10000U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(optional, constrained)), std::runtime_error);
}

TEST(LogicalPlanReadRuntimeTest, ExecutesCorrelatedReadSubqueriesThroughAnExplicitImportBoundary) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto input = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "outer",
			std::vector<PropertyValue>{PropertyValue(4), PropertyValue(5)});
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("outer"), "value");
	auto body = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
	logical::LogicalCallSubquery call(std::move(input), std::move(body), std::vector<std::string>{"outer"},
										 std::vector<std::string>{"value"});

	const QueryResult result = runtime.executeToQueryResult(call, bounds());
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows()[0].at("outer").asPrimitive(), PropertyValue(4));
	EXPECT_EQ(result.getRows()[0].at("value").asPrimitive(), PropertyValue(4));
	EXPECT_EQ(result.getRows()[1].at("outer").asPrimitive(), PropertyValue(5));
	EXPECT_EQ(result.getRows()[1].at("value").asPrimitive(), PropertyValue(5));

	auto passthroughInput = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "outer", std::vector<PropertyValue>{PropertyValue(7)});
	auto noRowsBody = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalSingleRow>(), std::make_shared<expressions::LiteralExpression>(false));
	logical::LogicalCallSubquery writeOnly(std::move(passthroughInput), std::move(noRowsBody),
										  std::vector<std::string>{}, std::vector<std::string>{});
	const QueryResult passthrough = runtime.executeToQueryResult(writeOnly, bounds());
	ASSERT_EQ(passthrough.rowCount(), 1U);
	EXPECT_EQ(passthrough.getRows()[0].at("outer").asPrimitive(), PropertyValue(7));
}

TEST(LogicalPlanReadRuntimeTest, RejectsMalformedCorrelatedSubqueryShapesBeforeExecution) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);

	auto input = std::make_unique<logical::LogicalSingleRow>();
	auto body = std::make_unique<logical::LogicalSingleRow>();
	logical::LogicalCallSubquery inTransactions(
			std::move(input), std::move(body), std::vector<std::string>{}, std::vector<std::string>{}, true, 1);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(inTransactions, bounds())), std::invalid_argument);

	auto missingImportInput = std::make_unique<logical::LogicalSingleRow>();
	auto missingImportBody = std::make_unique<logical::LogicalSingleRow>();
	logical::LogicalCallSubquery missingImport(
			std::move(missingImportInput), std::move(missingImportBody), std::vector<std::string>{"missing"},
			std::vector<std::string>{});
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(missingImport, bounds())), std::runtime_error);

	auto duplicateReturnInput = std::make_unique<logical::LogicalSingleRow>();
	auto duplicateReturnBody = std::make_unique<logical::LogicalSingleRow>();
	logical::LogicalCallSubquery duplicateReturn(
			std::move(duplicateReturnInput), std::move(duplicateReturnBody), std::vector<std::string>{},
			std::vector<std::string>{"value", "value"});
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(duplicateReturn, bounds())), std::invalid_argument);

	auto rebindInput = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "outer", std::vector<PropertyValue>{PropertyValue(7)});
	std::vector<logical::LogicalProjectItem> rebindItems;
	rebindItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("outer"), "outer");
	auto rebindBody = std::make_unique<logical::LogicalProject>(
			std::make_unique<logical::LogicalSingleRow>(), std::move(rebindItems));
	logical::LogicalCallSubquery rebindOuter(
			std::move(rebindInput), std::move(rebindBody), std::vector<std::string>{"outer"},
			std::vector<std::string>{"outer"});
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(rebindOuter, bounds())), std::runtime_error);
}

TEST(LogicalPlanUnwindRuntimeTest, RejectsEmptyAliasesAndUnboundExpressions) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalUnwind emptyAlias(std::make_unique<logical::LogicalSingleRow>(), "",
									 std::vector<PropertyValue>{PropertyValue(1)});
	EXPECT_THROW(static_cast<void>(runtime.execute(emptyAlias, bounds(), [](const LogicalPlanReadRow &) { return true; })),
				 std::invalid_argument);

	logical::LogicalUnwind missingExpression(std::make_unique<logical::LogicalSingleRow>(), "item",
										  std::shared_ptr<expressions::Expression>{});
	const LogicalPlanReadRuntimeStats emptyStats =
			runtime.execute(missingExpression, bounds(), [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(emptyStats, (LogicalPlanReadRuntimeStats{}));

	auto parameter = std::make_shared<expressions::ParameterExpression>("items");
	logical::LogicalUnwind unboundParameter(std::make_unique<logical::LogicalSingleRow>(), "item", std::move(parameter));
	QueryContext context;
	EXPECT_THROW(static_cast<void>(runtime.execute(unboundParameter, bounds(),
															 [](const LogicalPlanReadRow &) { return true; }, context)),
				 std::runtime_error);
}

TEST(LogicalPlanReadRuntimeTest, PaginationFlushesUnderBytePressureWithoutChangingTheWindow) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("UNWIND [1,2,3,4,5,6,7,8] AS value RETURN value SKIP 6 LIMIT 2");
	ASSERT_NE(plan.root, nullptr);
	auto constrained = bounds();
	constrained.maximum_materialized_bytes = 1000U;
	// The two retained QueryResult rows now share the cap with both pagination
	// buffers and the output batch; the former 1000-byte allowance omits them.
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, constrained)), std::runtime_error);
	constrained.maximum_materialized_bytes = 1500U;
	const auto result = runtime.executeToQueryResult(*plan.root, constrained);
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows()[0].at("value").asPrimitive(), PropertyValue(7));
	EXPECT_EQ(result.getRows()[1].at("value").asPrimitive(), PropertyValue(8));
}

TEST(LogicalPlanReadRuntimeTest, SkipStreamsAnExactBudgetRowWithoutSelectionStorage) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	const PropertyValue payload(std::string(1000U, 'x'));
	auto source = std::make_unique<logical::LogicalUnwind>(
		std::make_unique<logical::LogicalSingleRow>(), "value", std::vector<PropertyValue>{payload});
	logical::LogicalSkip skip(std::move(source), 0);
	auto constrained = bounds();
	const size_t rowBytes = sizeof(LogicalPlanReadRow) + std::string("value").size() +
		sizeof(LogicalPlanReadValue) + property_utils::getPropertyValueSize(payload);
	constrained.maximum_materialized_bytes = rowBytes + sizeof(LogicalPlanReadRow);
	size_t delivered = 0U;
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(skip, constrained, [&](const LogicalPlanReadRow &row) {
		++delivered;
		EXPECT_EQ(std::get<PropertyValue>(row.values.at("value")), payload);
		return true;
	})));
	EXPECT_EQ(delivered, 1U);
}

TEST(LogicalPlanReadRuntimeTest, ScalarResultPublicationChecksTheSharedBudgetBeforeCopying) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	const std::string payload(500U, 'v');
	QueryPlan plan = parser.parseToLogical("UNWIND ['" + payload + "'] AS value RETURN value");
	ASSERT_NE(plan.root, nullptr);
	auto constrained = bounds();
	constrained.maximum_materialized_bytes = 1000U + sizeof(LogicalPlanReadRow);
	size_t streamed = 0U;
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, constrained,
		[&](const LogicalPlanReadRow &) { ++streamed; return true; })));
	EXPECT_EQ(streamed, 1U);
	try {
		static_cast<void>(runtime.executeToQueryResult(*plan.root, constrained));
		FAIL() << "the retained input and its public result copy must share one allowance";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "shared materialization exceeded its byte bound");
	}
	constrained.maximum_materialized_bytes = 3000U;
	const QueryResult result = runtime.executeToQueryResult(*plan.root, constrained);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(payload));
}

TEST(LogicalPlanReadRuntimeTest, SortPaginationAndSortTransferRowsWithinTheSharedBudget) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"UNWIND ['d','b','a','c'] AS value WITH value ORDER BY value SKIP 1 LIMIT 2 "
			"RETURN value ORDER BY value DESC");
	ASSERT_NE(plan.root, nullptr);
	auto constrained = bounds();
	// Four retained rows, one output slot and both Sort key-slot sets.
	// Fixed slack covers scalar bindings and result-vector growth, not payloads.
	constrained.maximum_materialized_bytes =
			5U * sizeof(LogicalPlanReadRow) + 8U * sizeof(PropertyValue) + 616U;
	EXPECT_LT(constrained.maximum_materialized_bytes,
			8U * (sizeof(LogicalPlanReadRow) + sizeof(LogicalPlanReadValue) + 6U));
	const auto result = runtime.executeToQueryResult(*plan.root, constrained);
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows()[0].at("value").asPrimitive(), PropertyValue("c"));
	EXPECT_EQ(result.getRows()[1].at("value").asPrimitive(), PropertyValue("b"));
	size_t delivered = 0U;
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, constrained,
			[&](const LogicalPlanReadRow &row) {
				++delivered;
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("value")), PropertyValue("c"));
				return false;
			})));
	EXPECT_EQ(delivered, 1U);
}

TEST(LogicalPlanReadRuntimeTest, AppliesWithOrderingAndPaginationForAliasStarAndAggregateBodies) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	for (const std::string &query: {
		std::string("UNWIND [3,1,2] AS value WITH value AS renamed ORDER BY renamed SKIP 1 LIMIT 1 RETURN renamed AS value"),
		std::string("UNWIND [3,1,2] AS value WITH * ORDER BY value SKIP 1 LIMIT 1 RETURN value"),
		std::string("UNWIND [3,1,2] AS value RETURN * ORDER BY value SKIP 1 LIMIT 1"),
		std::string("UNWIND [3,1,3,2,2] AS value WITH value, count(*) AS count ORDER BY count DESC, value SKIP 1 LIMIT 1 RETURN value"),
		std::string("UNWIND [3,1,2,2] AS value WITH DISTINCT value ORDER BY value SKIP 1 LIMIT 1 RETURN value")
	}) {
		SCOPED_TRACE(query);
		QueryPlan plan = parser.parseToLogical(query);
		ASSERT_NE(plan.root, nullptr);
		const auto result = runtime.executeToQueryResult(*plan.root, bounds());
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows()[0].at("value").asPrimitive(), PropertyValue(query.find("count(*)") != std::string::npos ? 3 : 2));
	}
}

TEST(LogicalPlanReadRuntimeTest, LimitExhaustsItsLogicalWindowForSortAndAggregateIncludingZero) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	for (const int limit: {0, 1, 2}) {
		SCOPED_TRACE(limit);
		const std::string input = "UNWIND [3,1,2] AS value WITH value LIMIT " + std::to_string(limit);
		QueryPlan sorted = parser.parseToLogical(input + " RETURN value ORDER BY value");
		const auto rows = runtime.executeToQueryResult(*sorted.root, bounds());
		ASSERT_EQ(rows.rowCount(), static_cast<size_t>(limit));
		if (limit == 1) EXPECT_EQ(rows.getRows()[0].at("value").asPrimitive(), PropertyValue(3));
		if (limit == 2) {
			EXPECT_EQ(rows.getRows()[0].at("value").asPrimitive(), PropertyValue(1));
			EXPECT_EQ(rows.getRows()[1].at("value").asPrimitive(), PropertyValue(3));
		}
		QueryPlan counted = parser.parseToLogical(input + " RETURN count(*) AS count");
		const auto result = runtime.executeToQueryResult(*counted.root, bounds());
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows()[0].at("count").asPrimitive(), PropertyValue(limit));
	}
}


TEST(LogicalPlanReadRuntimeTest, HashJoinCanonicalNestedMapsPreserveBagAndSignedZero) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	PropertyValue::MapType first;
	first.emplace("wide", PropertyValue(std::string(4096U, 'x')));
	first.emplace("nested", PropertyValue(std::vector<PropertyValue>{PropertyValue(2), PropertyValue(-0.0)}));
	PropertyValue::MapType second;
	second.emplace("nested", PropertyValue(std::vector<PropertyValue>{PropertyValue(2), PropertyValue(0.0)}));
	second.emplace("wide", PropertyValue(std::string(4096U, 'x')));
	auto left = std::make_unique<logical::LogicalUnwind>(
		std::make_unique<logical::LogicalSingleRow>(), "shared", std::vector<PropertyValue>{PropertyValue(first)});
	auto right = std::make_unique<logical::LogicalUnwind>(
		std::make_unique<logical::LogicalSingleRow>(), "shared",
		std::vector<PropertyValue>{PropertyValue(second), PropertyValue(second)});
	logical::LogicalJoin join(std::move(left), std::move(right));
	const QueryResult result = runtime.executeToQueryResult(join, bounds());
	first.clear();
	second.clear();
	ASSERT_EQ(result.rowCount(), 2U);
	for (const auto &row : result.getRows()) {
		const auto &map = row.at("shared").asPrimitive().getMap();
		EXPECT_EQ(map.at("wide"), PropertyValue(std::string(4096U, 'x')));
		EXPECT_EQ(map.at("nested"), PropertyValue(std::vector<PropertyValue>{PropertyValue(2), PropertyValue(0.0)}));
	}
}
