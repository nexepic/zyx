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

TEST(NodeCountFastPathPlannerTest, AddsClosedRangePredicateForInclusiveBounds) {
	RangePredicate inclusive;
	inclusive.key = "age";
	inclusive.minValue = PropertyValue(int64_t{18});
	inclusive.maxValue = PropertyValue(int64_t{65});
	inclusive.minInclusive = true;
	inclusive.maxInclusive = true;
	auto scan = makeScan();
	scan->setRangePredicates({inclusive});
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs());

	auto plan = tryBuildNodeCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->requirements.materialization, execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES);
	EXPECT_EQ(plan->requirements.requiredProperties, (std::vector<std::string>{"age"}));
	ASSERT_EQ(plan->predicates.size(), 1U);
	EXPECT_EQ(plan->predicates[0].op, execution::VectorPredicateOp::VPO_RANGE_CLOSED);
	EXPECT_EQ(plan->predicates[0].value, PropertyValue(int64_t{18}));
	ASSERT_TRUE(plan->predicates[0].upperValue.has_value());
	EXPECT_EQ(plan->predicates[0].upperValue.value(), PropertyValue(int64_t{65}));
}

TEST(NodeCountFastPathPlannerTest, AddsLowerBoundRangePredicateForInclusiveAndExclusiveBounds) {
	RangePredicate inclusive;
	inclusive.key = "age";
	inclusive.minValue = PropertyValue(int64_t{30});
	inclusive.minInclusive = true;
	auto inclusiveScan = makeScan();
	inclusiveScan->setRangePredicates({inclusive});
	LogicalAggregate inclusiveAggregate(std::move(inclusiveScan), {}, makeAggs());

	auto inclusivePlan = tryBuildNodeCountFastPathPlan(inclusiveAggregate);

	ASSERT_TRUE(inclusivePlan.has_value());
	ASSERT_EQ(inclusivePlan->predicates.size(), 1U);
	EXPECT_EQ(inclusivePlan->predicates[0].propertyKey, "age");
	EXPECT_EQ(inclusivePlan->predicates[0].op, execution::VectorPredicateOp::VPO_GE);
	EXPECT_EQ(inclusivePlan->predicates[0].value, PropertyValue(int64_t{30}));

	RangePredicate exclusive = inclusive;
	exclusive.minInclusive = false;
	auto exclusiveScan = makeScan();
	exclusiveScan->setRangePredicates({exclusive});
	LogicalAggregate exclusiveAggregate(std::move(exclusiveScan), {}, makeAggs());

	auto exclusivePlan = tryBuildNodeCountFastPathPlan(exclusiveAggregate);

	ASSERT_TRUE(exclusivePlan.has_value());
	ASSERT_EQ(exclusivePlan->predicates.size(), 1U);
	EXPECT_EQ(exclusivePlan->predicates[0].op, execution::VectorPredicateOp::VPO_GT);
	EXPECT_EQ(exclusivePlan->predicates[0].value, PropertyValue(int64_t{30}));
}

TEST(NodeCountFastPathPlannerTest, AddsUpperBoundRangePredicateForInclusiveAndExclusiveBounds) {
	RangePredicate inclusive;
	inclusive.key = "age";
	inclusive.maxValue = PropertyValue(int64_t{65});
	inclusive.maxInclusive = true;
	auto inclusiveScan = makeScan();
	inclusiveScan->setRangePredicates({inclusive});
	LogicalAggregate inclusiveAggregate(std::move(inclusiveScan), {}, makeAggs());

	auto inclusivePlan = tryBuildNodeCountFastPathPlan(inclusiveAggregate);

	ASSERT_TRUE(inclusivePlan.has_value());
	ASSERT_EQ(inclusivePlan->predicates.size(), 1U);
	EXPECT_EQ(inclusivePlan->predicates[0].propertyKey, "age");
	EXPECT_EQ(inclusivePlan->predicates[0].op, execution::VectorPredicateOp::VPO_LE);
	EXPECT_EQ(inclusivePlan->predicates[0].value, PropertyValue(int64_t{65}));

	RangePredicate exclusive = inclusive;
	exclusive.maxInclusive = false;
	auto exclusiveScan = makeScan();
	exclusiveScan->setRangePredicates({exclusive});
	LogicalAggregate exclusiveAggregate(std::move(exclusiveScan), {}, makeAggs());

	auto exclusivePlan = tryBuildNodeCountFastPathPlan(exclusiveAggregate);

	ASSERT_TRUE(exclusivePlan.has_value());
	ASSERT_EQ(exclusivePlan->predicates.size(), 1U);
	EXPECT_EQ(exclusivePlan->predicates[0].op, execution::VectorPredicateOp::VPO_LT);
	EXPECT_EQ(exclusivePlan->predicates[0].value, PropertyValue(int64_t{65}));
}

TEST(NodeCountFastPathPlannerTest, AddsEqualityAndSingleSidedRangePredicatesTogether) {
	std::vector<std::pair<std::string, PropertyValue>> predicates = {{"country", PropertyValue("CN")}};
	auto scan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"}, predicates);
	RangePredicate range;
	range.key = "age";
	range.minValue = PropertyValue(int64_t{30});
	range.minInclusive = true;
	scan->setRangePredicates({range});
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("u")));

	auto plan = tryBuildNodeCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->config.variable, "u");
	EXPECT_EQ(plan->requirements.materialization, execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES);
	EXPECT_EQ(plan->requirements.requiredProperties, (std::vector<std::string>{"country", "age"}));
	ASSERT_EQ(plan->predicates.size(), 2U);
	EXPECT_EQ(plan->predicates[0].propertyKey, "country");
	EXPECT_EQ(plan->predicates[0].op, execution::VectorPredicateOp::VPO_EQ);
	EXPECT_EQ(plan->predicates[1].propertyKey, "age");
	EXPECT_EQ(plan->predicates[1].op, execution::VectorPredicateOp::VPO_GE);
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

TEST(NodeCountFastPathPlannerTest, OpenRangeScanFallsBackToLabelOrFullCandidateDiscovery) {
	auto labelScan = makeScan();
	labelScan->setPreferredScanType(execution::ScanType::RANGE_SCAN);
	RangePredicate range;
	range.key = "age";
	range.minValue = PropertyValue(int64_t{30});
	labelScan->setRangePredicates({range});
	LogicalAggregate labelAggregate(std::move(labelScan), {}, makeAggs());

	auto labelPlan = tryBuildNodeCountFastPathPlan(labelAggregate);

	ASSERT_TRUE(labelPlan.has_value());
	EXPECT_EQ(labelPlan->config.type, execution::ScanType::LABEL_SCAN);

	auto fullScan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{});
	fullScan->setPreferredScanType(execution::ScanType::RANGE_SCAN);
	fullScan->setRangePredicates({range});
	LogicalAggregate fullAggregate(std::move(fullScan), {}, makeAggs());

	auto fullPlan = tryBuildNodeCountFastPathPlan(fullAggregate);

	ASSERT_TRUE(fullPlan.has_value());
	EXPECT_EQ(fullPlan->config.type, execution::ScanType::FULL_SCAN);
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
