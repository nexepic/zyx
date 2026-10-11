/**
 * @file test_LogicalPlanUnwindRuntime.cpp
 * @brief Logical UNWIND execution tests independent of physical graph storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "CypherParserImpl.hpp"
#include "graph/core/TemporalTypes.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalOptionalMatch.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::execution;
using namespace graph::storage;
namespace logical = graph::query::logical;
namespace expressions = graph::query::expressions;

namespace {

	class NoGraphSnapshot final : public LogicalGraphReadSnapshot {
	public:
		[[nodiscard]] LogicalGraphNode readNode(uint64_t) const override { throw std::logic_error("unexpected Node read"); }
		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t) const override { throw std::logic_error("unexpected Edge read"); }
		void visitNodes(size_t, const std::function<bool(const LogicalGraphNode &)> &) const override {
			throw std::logic_error("unexpected Node traversal");
		}
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override {
			throw std::logic_error("unexpected Node page scan");
		}
		[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdges(
				uint64_t, LogicalGraphReadDirection, std::optional<LogicalGraphAdjacencyContinuation>, size_t) const override {
			throw std::logic_error("unexpected adjacency scan");
		}
		void visitEdges(size_t, const std::function<bool(const LogicalGraphEdge &)> &) const override {
			throw std::logic_error("unexpected Edge traversal");
		}
		void visitAdjacentEdges(uint64_t, LogicalGraphReadDirection, size_t,
								const std::function<bool(const LogicalGraphEdge &)> &) const override {
			throw std::logic_error("unexpected adjacency traversal");
		}
	};

	class NoGraphBackend final : public LogicalGraphReadBackend {
	public:
		[[nodiscard]] std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
			return std::make_unique<NoGraphSnapshot>();
		}
	};

	[[nodiscard]] LogicalPlanReadRuntimeBounds bounds() {
		return {
				.maximum_node_records = 16U,
				.maximum_node_page_records = 4U,
				.maximum_metadata_pages_per_node_page = 4U,
				.maximum_edges_per_node = 16U,
				.maximum_adjacency_page_records = 4U,
				.maximum_property_entries = 16U,
				.maximum_rows = 16U,
				.maximum_distinct_rows = 16U,
		};
	}

} // namespace

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

TEST(LogicalPlanReadRuntimeTest, EvaluatesSimpleAndSearchedCaseExpressionsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN CASE 2 WHEN 1 THEN 'one' WHEN 2 THEN 'two' ELSE 'other' END AS simple_case, "
			"CASE WHEN 3 > 4 THEN 'wrong' WHEN 4 > 3 THEN 'right' ELSE 'other' END AS searched_case, "
			"CASE WHEN null THEN 'wrong' END AS null_case");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("simple_case").asPrimitive(), PropertyValue("two"));
	EXPECT_EQ(row.at("searched_case").asPrimitive(), PropertyValue("right"));
	EXPECT_EQ(row.at("null_case").asPrimitive(), PropertyValue());
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesListMembershipSlicesAndComprehensionsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN 2 IN [1, 2, 3] AS member, 4 IN [1, 2, 3] AS non_member, "
			"[1, 2, 3, 4][1..3] AS slice, [value IN [1, 2, 3] WHERE value > 1 | value * 10] AS mapped");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("member").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("non_member").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("slice").asPrimitive().getList(),
			  (std::vector<PropertyValue>{PropertyValue(2), PropertyValue(3)}));
	EXPECT_EQ(row.at("mapped").asPrimitive().getList(),
			  (std::vector<PropertyValue>{PropertyValue(20), PropertyValue(30)}));
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesQuantifierExpressionsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN all(value IN [1, 2, 3] WHERE value > 0) AS all_positive, "
			"any(value IN [1, 2, 3] WHERE value = 2) AS any_two, "
			"none(value IN [1, 2, 3] WHERE value < 0) AS none_negative, "
			"single(value IN [1, 2, 3] WHERE value = 2) AS exactly_one");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("all_positive").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("any_two").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("none_negative").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("exactly_one").asPrimitive(), PropertyValue(true));
}

TEST(LogicalPlanReadRuntimeTest, EvaluatesScalarFunctionCallsAndRejectsInvalidCallsThroughCypher) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN upper('Ada') AS upper_name, substring('graph', 1, 3) AS substring_value, "
			"abs(-4) AS absolute_value, coalesce(null, 'fallback') AS fallback_value");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("upper_name").asPrimitive(), PropertyValue("ADA"));
	EXPECT_EQ(row.at("substring_value").asPrimitive(), PropertyValue("rap"));
	EXPECT_EQ(row.at("absolute_value").asPrimitive(), PropertyValue(4));
	EXPECT_EQ(row.at("fallback_value").asPrimitive(), PropertyValue("fallback"));

	QueryPlan invalidPlan = parser.parseToLogical("RETURN upper(1, 2) AS invalid");
	ASSERT_NE(invalidPlan.root, nullptr);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*invalidPlan.root, bounds())), std::runtime_error);
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
			"RETURN shifted_datetime.month AS datetime_month, shifted_datetime.day AS datetime_day, "
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
