/**
 * @file test_LogicalPlanUnwindRuntime_Budget.cpp
 * @brief Evaluated UNWIND list ownership and shared-budget regression tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "CypherParserImpl.hpp"
#include "graph/query/QueryGuard.hpp"
#include "query/execution/LogicalPlanUnwindRuntimeTestFixture.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::execution;
using namespace graph::query::execution::test;
using namespace graph::storage;
namespace logical = graph::query::logical;
namespace expressions = graph::query::expressions;

namespace {
	// range(1, 100) grows to 128 slots. Allow its integer payload and scalar
	// bindings, while leaving less than another 100-element List's storage.
	constexpr size_t singleHundredElementRangeAllowance =
			128U * sizeof(PropertyValue) + 100U * sizeof(int64_t) + 1024U;
}

TEST(LogicalPlanUnwindRuntimeTest, HoldsEvaluatedListBudgetAcrossLimitOutput) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryContext context;
	context.parameters.emplace("a", PropertyValue(std::string(3072U, 'x')));
	context.parameters.emplace("b", PropertyValue(std::string(3072U, 'y')));
	context.parameters.emplace("c", PropertyValue(std::string(3072U, 'z')));
	std::vector<std::unique_ptr<expressions::Expression>> elements;
	for (const char *name: {"a", "b", "c"}) {
		elements.push_back(std::make_unique<expressions::ParameterExpression>(name));
	}
	logical::LogicalLimit limit(std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "item",
			std::make_shared<expressions::ListLiteralExpression>(std::move(elements))), 1);
	auto limited = bounds();
	limited.maximum_materialized_bytes = 10000U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(limit, limited, context)), std::runtime_error);
	auto ample = bounds();
	ample.maximum_materialized_bytes = 16384U;
	const QueryResult result = runtime.executeToQueryResult(limit, ample, context);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("item").asPrimitive(), PropertyValue(std::string(3072U, 'x')));
}

TEST(LogicalPlanUnwindRuntimeTest, HoldsUnchargedInputBindingsDuringExpansion) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	const PropertyValue seed(std::string(3072U, 's'));
	const PropertyValue item(std::string(3072U, 'i'));
	auto inner = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "seed", std::vector<PropertyValue>{seed});
	logical::LogicalUnwind outer(std::move(inner), "item", std::vector<PropertyValue>{item});
	auto limited = bounds();
	limited.maximum_materialized_bytes = 7500U;
	EXPECT_THROW(static_cast<void>(runtime.execute(outer, limited,
			[](const LogicalPlanReadRow &) { return true; })), std::runtime_error);
	auto ample = bounds();
	ample.maximum_materialized_bytes = 20000U;
	size_t delivered = 0U;
	static_cast<void>(runtime.execute(outer, ample, [&](const LogicalPlanReadRow &row) {
		++delivered;
		EXPECT_EQ(std::get<PropertyValue>(row.values.at("seed")), seed);
		EXPECT_EQ(std::get<PropertyValue>(row.values.at("item")), item);
		return true;
	}));
	EXPECT_EQ(delivered, 1U);
}

TEST(LogicalPlanUnwindRuntimeTest, RangeAdmitsListGrowthBeforeAllocation) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("RETURN range(1, 100) AS values");
	ASSERT_NE(plan.root, nullptr);
	auto limited = bounds();
	limited.maximum_rows = 128U;
	bool rangeGrowthRejected = false;
	for (size_t budget = 1000U; budget < 20000U; budget += 16U) {
		limited.maximum_materialized_bytes = budget;
		try {
			static_cast<void>(runtime.executeToQueryResult(*plan.root, limited));
			break;
		} catch (const std::runtime_error &error) {
			if (std::string_view(error.what()) == "logical-plan range() exceeded its shared byte bound") {
				rangeGrowthRejected = true;
				break;
			}
			if (std::string_view(error.what()) != "shared materialization exceeded its byte bound") {
				ADD_FAILURE() << "unexpected bounded range failure: " << error.what();
				break;
			}
		}
	}
	EXPECT_TRUE(rangeGrowthRejected) << "could not reach range() growth admission after output reservations";
	auto ample = limited;
	ample.maximum_materialized_bytes = 20000U;
	const QueryResult result = runtime.executeToQueryResult(*plan.root, ample);
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &values = result.getRows().front().at("values").asPrimitive().getList();
	ASSERT_EQ(values.size(), 100U);
	EXPECT_EQ(values.front(), PropertyValue(1));
	EXPECT_EQ(values.back(), PropertyValue(100));
	QueryPlan singleton = parser.parseToLogical("RETURN range(1, 1) AS values");
	ASSERT_NE(singleton.root, nullptr);
	auto small = limited;
	small.maximum_materialized_bytes = 1500U;
	const QueryResult single = runtime.executeToQueryResult(*singleton.root, small);
	ASSERT_EQ(single.rowCount(), 1U);
	EXPECT_EQ(single.getRows().front().at("values").asPrimitive().getList(),
			(std::vector<PropertyValue>{PropertyValue(1)}));
	QueryPlan empty = parser.parseToLogical("RETURN range(2, 1) AS values");
	ASSERT_NE(empty.root, nullptr);
	const QueryResult none = runtime.executeToQueryResult(*empty.root, small);
	ASSERT_EQ(none.rowCount(), 1U);
	EXPECT_TRUE(none.getRows().front().at("values").asPrimitive().getList().empty());
}

TEST(LogicalPlanUnwindRuntimeTest, ProjectRetainsEarlierListWhileEvaluatingLaterRange) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("RETURN range(1, 100) AS first, range(1, 100) AS second");
	ASSERT_NE(plan.root, nullptr);
	auto limited = bounds();
	limited.maximum_rows = 128U;
	limited.maximum_materialized_bytes = singleHundredElementRangeAllowance;
	QueryPlan single = parser.parseToLogical("RETURN range(1, 100) AS values");
	ASSERT_NE(single.root, nullptr);
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(*single.root, limited,
			[](const LogicalPlanReadRow &) { return true; })));
	try {
		static_cast<void>(runtime.execute(*plan.root, limited, [](const LogicalPlanReadRow &) { return true; }));
		FAIL() << "the second range() ignored the earlier projected List";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical-plan range() exceeded its shared byte bound");
	}
	auto ample = limited;
	ample.maximum_materialized_bytes = 20000U;
	size_t delivered = 0U;
	static_cast<void>(runtime.execute(*plan.root, ample, [&](const LogicalPlanReadRow &row) {
		++delivered;
		EXPECT_EQ(std::get<PropertyValue>(row.values.at("first")).getList().size(), 100U);
		EXPECT_EQ(std::get<PropertyValue>(row.values.at("second")).getList().size(), 100U);
		return true;
	}));
	EXPECT_EQ(delivered, 1U);
}

TEST(LogicalPlanUnwindRuntimeTest, RangeChecksGuardDuringListConstruction) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("RETURN range(1, 4096) AS values");
	ASSERT_NE(plan.root, nullptr);
	size_t samples = 0U;
	QueryGuard guard(0, 1, [&] { return ++samples == 1U ? 0U : 2U * 1024U * 1024U; });
	QueryContext context;
	context.guard = &guard;
	auto limited = bounds();
	limited.maximum_rows = 5000U;
	limited.maximum_materialized_bytes = 2U * 1024U * 1024U;
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.execute(*plan.root, limited,
			[&](const LogicalPlanReadRow &) { ++delivered; return true; }, context)), QueryMemoryExceededException);
	EXPECT_EQ(delivered, 0U);
	EXPECT_GE(samples, 2U);
}

TEST(LogicalPlanUnwindRuntimeTest, FunctionRetainsEarlierArgumentDuringLaterRange) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("RETURN coalesce(range(1, 100), range(1, 100)) AS values");
	ASSERT_NE(plan.root, nullptr);
	auto limited = bounds();
	limited.maximum_rows = 128U;
	limited.maximum_materialized_bytes = singleHundredElementRangeAllowance;
	QueryPlan single = parser.parseToLogical("RETURN range(1, 100) AS values");
	ASSERT_NE(single.root, nullptr);
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(*single.root, limited,
			[](const LogicalPlanReadRow &) { return true; })));
	try {
		static_cast<void>(runtime.execute(*plan.root, limited, [](const LogicalPlanReadRow &) { return true; }));
		FAIL() << "the second function argument ignored the first List";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical-plan range() exceeded its shared byte bound");
	}
	auto ample = limited;
	ample.maximum_materialized_bytes = 20000U;
	size_t delivered = 0U;
	static_cast<void>(runtime.execute(*plan.root, ample, [&](const LogicalPlanReadRow &row) {
		++delivered;
		EXPECT_EQ(std::get<PropertyValue>(row.values.at("values")).getList().size(), 100U);
		return true;
	}));
	EXPECT_EQ(delivered, 1U);
}

TEST(LogicalPlanUnwindRuntimeTest, BinaryExpressionRetainsLeftValueDuringRightEvaluation) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("RETURN range(1, 100) = range(1, 100) AS same");
	ASSERT_NE(plan.root, nullptr);
	auto limited = bounds();
	limited.maximum_rows = 128U;
	limited.maximum_materialized_bytes = singleHundredElementRangeAllowance;
	QueryPlan single = parser.parseToLogical("RETURN range(1, 100) AS values");
	ASSERT_NE(single.root, nullptr);
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(*single.root, limited,
			[](const LogicalPlanReadRow &) { return true; })));
	try {
		static_cast<void>(runtime.execute(*plan.root, limited, [](const LogicalPlanReadRow &) { return true; }));
		FAIL() << "the right operand ignored the retained left List";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical-plan range() exceeded its shared byte bound");
	}
	auto ample = limited;
	ample.maximum_materialized_bytes = 20000U;
	size_t delivered = 0U;
	static_cast<void>(runtime.execute(*plan.root, ample, [&](const LogicalPlanReadRow &row) {
		++delivered;
		EXPECT_EQ(std::get<PropertyValue>(row.values.at("same")), PropertyValue(true));
		return true;
	}));
	EXPECT_EQ(delivered, 1U);
}

TEST(LogicalPlanUnwindRuntimeTest, InExpressionRetainsActualValueDuringCandidateEvaluation) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("RETURN range(1, 100) IN [range(1, 100)] AS found");
	ASSERT_NE(plan.root, nullptr);
	auto limited = bounds();
	limited.maximum_rows = 128U;
	limited.maximum_materialized_bytes = singleHundredElementRangeAllowance;
	QueryPlan single = parser.parseToLogical("RETURN range(1, 100) AS values");
	ASSERT_NE(single.root, nullptr);
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(*single.root, limited,
			[](const LogicalPlanReadRow &) { return true; })));
	try {
		static_cast<void>(runtime.execute(*plan.root, limited, [](const LogicalPlanReadRow &) { return true; }));
		FAIL() << "the IN candidate ignored the retained actual List";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical-plan range() exceeded its shared byte bound");
	}
	auto ample = limited;
	ample.maximum_materialized_bytes = 20000U;
	size_t delivered = 0U;
	static_cast<void>(runtime.execute(*plan.root, ample, [&](const LogicalPlanReadRow &row) {
		++delivered;
		EXPECT_EQ(std::get<PropertyValue>(row.values.at("found")), PropertyValue(true));
		return true;
	}));
	EXPECT_EQ(delivered, 1U);
}

TEST(LogicalPlanUnwindRuntimeTest, SimpleCaseRetainsComparisonValueDuringWhenEvaluation) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN CASE range(1, 100) WHEN range(1, 100) THEN 'same' ELSE 'different' END AS result");
	ASSERT_NE(plan.root, nullptr);
	auto limited = bounds();
	limited.maximum_rows = 128U;
	limited.maximum_materialized_bytes = singleHundredElementRangeAllowance;
	QueryPlan single = parser.parseToLogical("RETURN range(1, 100) AS values");
	ASSERT_NE(single.root, nullptr);
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(*single.root, limited,
			[](const LogicalPlanReadRow &) { return true; })));
	try {
		static_cast<void>(runtime.execute(*plan.root, limited, [](const LogicalPlanReadRow &) { return true; }));
		FAIL() << "the CASE WHEN value ignored the retained comparison List";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical-plan range() exceeded its shared byte bound");
	}
	auto ample = limited;
	ample.maximum_materialized_bytes = 20000U;
	size_t delivered = 0U;
	static_cast<void>(runtime.execute(*plan.root, ample, [&](const LogicalPlanReadRow &row) {
		++delivered;
		EXPECT_EQ(std::get<PropertyValue>(row.values.at("result")), PropertyValue(std::string("same")));
		return true;
	}));
	EXPECT_EQ(delivered, 1U);
}
