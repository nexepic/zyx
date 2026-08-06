/**
 * @file test_LogicalPlanUnwindRuntime.cpp
 * @brief Logical UNWIND execution tests independent of physical graph storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "CypherParserImpl.hpp"
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

TEST(LogicalPlanReadRuntimeTest, EvaluatesSearchedCaseTruthinessAcrossSupportedScalarAndListValues) {
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
	EXPECT_EQ(evaluate(std::make_unique<expressions::LiteralExpression>(int64_t(7))), 1);
	EXPECT_EQ(evaluate(std::make_unique<expressions::LiteralExpression>(int64_t(0))), 0);
	EXPECT_EQ(evaluate(std::make_unique<expressions::LiteralExpression>(2.5)), 1);
	EXPECT_EQ(evaluate(std::make_unique<expressions::LiteralExpression>(0.0)), 0);
	EXPECT_EQ(evaluate(std::make_unique<expressions::LiteralExpression>(std::string("value"))), 1);
	EXPECT_EQ(evaluate(std::make_unique<expressions::LiteralExpression>(std::string())), 0);
	EXPECT_EQ(evaluate(std::make_unique<expressions::ListLiteralExpression>(
				  PropertyValue(std::vector<PropertyValue>{PropertyValue(1)}))),
			  1);
	EXPECT_EQ(evaluate(std::make_unique<expressions::ListLiteralExpression>(
				  PropertyValue(std::vector<PropertyValue>{}))),
			  0);
	QueryContext context;
	context.parameters.emplace("map", PropertyValue(PropertyValue::MapType{{"key", PropertyValue(1)}}));
	EXPECT_EQ(evaluate(std::make_unique<expressions::ParameterExpression>("map"), &context), 0);
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
