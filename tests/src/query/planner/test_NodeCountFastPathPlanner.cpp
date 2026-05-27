#include <gtest/gtest.h>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/planner/NodeCountFastPathPlanner.hpp"
#include "graph/query/planner/PhysicalPlanConverter.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::logical;
using namespace graph::query::planner;

namespace {

std::unique_ptr<LogicalNodeScan> makeScan(std::string variable = "n") {
	return std::make_unique<LogicalNodeScan>(std::move(variable), std::vector<std::string>{"Person"});
}

std::vector<LogicalAggItem> makeAggs(
		std::string functionName = "count",
		std::shared_ptr<expressions::Expression> argument = std::make_shared<expressions::VariableReferenceExpression>("n"),
		bool distinct = false,
		std::string alias = "count") {
	std::vector<LogicalAggItem> aggs;
	aggs.emplace_back(std::move(functionName), std::move(argument), std::move(alias), distinct);
	return aggs;
}

} // namespace

TEST(NodeCountFastPathPlannerTest, AcceptsSimpleCountOverNodeScan) {
	auto scan = makeScan();
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs());

	auto plan = tryBuildNodeCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->config.variable, "n");
	EXPECT_EQ(plan->config.labels, (std::vector<std::string>{"Person"}));
	EXPECT_EQ(plan->config.type, execution::ScanType::FULL_SCAN);
	EXPECT_EQ(plan->requirements.materialization, execution::NodeMaterializationMode::NSM_ID_ONLY);
	EXPECT_TRUE(plan->requirements.countOnly);
	EXPECT_TRUE(plan->requirements.requiredProperties.empty());
	EXPECT_TRUE(plan->predicates.empty());
	EXPECT_EQ(plan->outputAlias, "count");
}

TEST(NodeCountFastPathPlannerTest, AcceptsCountStarNullArgument) {
	auto scan = makeScan();
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs("count", nullptr));

	auto plan = tryBuildNodeCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->requirements.materialization, execution::NodeMaterializationMode::NSM_ID_ONLY);
	EXPECT_TRUE(plan->requirements.countOnly);
}

TEST(NodeCountFastPathPlannerTest, RejectsGroupedAggregate) {
	auto scan = makeScan();
	std::vector<std::shared_ptr<expressions::Expression>> groups;
	groups.push_back(std::make_shared<expressions::VariableReferenceExpression>("n", "age"));
	LogicalAggregate aggregate(std::move(scan), std::move(groups), makeAggs());

	EXPECT_FALSE(tryBuildNodeCountFastPathPlan(aggregate).has_value());
}

TEST(NodeCountFastPathPlannerTest, RejectsDistinctCount) {
	auto scan = makeScan();
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n"), true));

	EXPECT_FALSE(tryBuildNodeCountFastPathPlan(aggregate).has_value());
}

TEST(NodeCountFastPathPlannerTest, RejectsCountPropertyAccess) {
	auto scan = makeScan();
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "age")));

	EXPECT_FALSE(tryBuildNodeCountFastPathPlan(aggregate).has_value());
}

TEST(NodeCountFastPathPlannerTest, RejectsNonCountAggregate) {
	auto scan = makeScan();
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs("sum"));

	EXPECT_FALSE(tryBuildNodeCountFastPathPlan(aggregate).has_value());
}

TEST(NodeCountFastPathPlannerTest, RejectsNonNodeScanChild) {
	auto child = std::make_unique<LogicalAggregate>(makeScan(), std::vector<std::shared_ptr<expressions::Expression>>{}, makeAggs());
	LogicalAggregate aggregate(std::move(child), {}, makeAggs());

	EXPECT_FALSE(tryBuildNodeCountFastPathPlan(aggregate).has_value());
}

TEST(NodeCountFastPathPlannerTest, AddsRequiredPropertiesAndEqPredicateForPushedEqualityPredicate) {
	std::vector<std::pair<std::string, PropertyValue>> predicates = {{"age", PropertyValue(int64_t{42})}};
	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"}, predicates);
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs());

	auto plan = tryBuildNodeCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->requirements.materialization, execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES);
	EXPECT_EQ(plan->requirements.requiredProperties, (std::vector<std::string>{"age"}));
	ASSERT_EQ(plan->predicates.size(), 1U);
	EXPECT_EQ(plan->predicates[0].variable, "n");
	EXPECT_EQ(plan->predicates[0].propertyKey, "age");
	EXPECT_EQ(plan->predicates[0].op, execution::VectorPredicateOp::VPO_EQ);
	EXPECT_EQ(plan->predicates[0].value, PropertyValue(int64_t{42}));
}

TEST(NodeCountFastPathPlannerTest, AddsClosedRangePredicateAndRejectsExclusiveRange) {
	RangePredicate inclusive;
	inclusive.key = "age";
	inclusive.minValue = PropertyValue(int64_t{18});
	inclusive.maxValue = PropertyValue(int64_t{65});
	inclusive.minInclusive = true;
	inclusive.maxInclusive = true;
	auto acceptedScan = makeScan();
	acceptedScan->setRangePredicates({inclusive});
	LogicalAggregate accepted(std::move(acceptedScan), {}, makeAggs());

	auto plan = tryBuildNodeCountFastPathPlan(accepted);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->requirements.materialization, execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES);
	EXPECT_EQ(plan->requirements.requiredProperties, (std::vector<std::string>{"age"}));
	ASSERT_EQ(plan->predicates.size(), 1U);
	EXPECT_EQ(plan->predicates[0].op, execution::VectorPredicateOp::VPO_RANGE_CLOSED);
	EXPECT_EQ(plan->predicates[0].value, PropertyValue(int64_t{18}));
	ASSERT_TRUE(plan->predicates[0].upperValue.has_value());
	EXPECT_EQ(plan->predicates[0].upperValue.value(), PropertyValue(int64_t{65}));

	RangePredicate exclusive = inclusive;
	exclusive.minInclusive = false;
	auto rejectedScan = makeScan();
	rejectedScan->setRangePredicates({exclusive});
	LogicalAggregate rejected(std::move(rejectedScan), {}, makeAggs());

	EXPECT_FALSE(tryBuildNodeCountFastPathPlan(rejected).has_value());
}

TEST(NodeCountFastPathPlannerTest, DeduplicatesRequiredPropertiesAcrossPredicateSources) {
	std::vector<std::pair<std::string, PropertyValue>> predicates = {{"age", PropertyValue(int64_t{42})}};
	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"}, predicates);
	RangePredicate range;
	range.key = "age";
	range.minValue = PropertyValue(int64_t{40});
	range.maxValue = PropertyValue(int64_t{50});
	scan->setRangePredicates({range});
	CompositeEqualityPredicate composite;
	composite.keys = {"age", "name"};
	composite.values = {PropertyValue(int64_t{42}), PropertyValue("Alice")};
	scan->setCompositeEquality(std::move(composite));
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs());

	auto plan = tryBuildNodeCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->requirements.requiredProperties, (std::vector<std::string>{"age", "name"}));
	EXPECT_EQ(plan->predicates.size(), 4U);
}

TEST(NodeCountFastPathPlannerTest, PhysicalPlanConverterUsesFastPathForRecognizedAggregate) {
	auto scan = makeScan();
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs());
	query::PhysicalPlanConverter converter(std::shared_ptr<storage::DataManager>{},
	                                      std::shared_ptr<indexes::IndexManager>{});

	auto physical = converter.convert(&aggregate);

	ASSERT_NE(physical, nullptr);
	EXPECT_EQ(physical->toString(), "NodeCountFastPath(n -> count)");
}
