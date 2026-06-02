#include <gtest/gtest.h>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>

#include "graph/core/Database.hpp"
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

namespace fs = std::filesystem;

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

TEST(NodeCountFastPathPlannerTest, AcceptsSinglePropertyGroupCountOverNodeScan) {
	auto scan = makeScan("u");
	std::vector<std::shared_ptr<expressions::Expression>> groups;
	groups.push_back(std::make_shared<expressions::VariableReferenceExpression>("u", "country"));
	LogicalAggregate aggregate(std::move(scan), std::move(groups), makeAggs("count", nullptr, false, "rows"), {"country"});

	auto plan = tryBuildNodeGroupCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->config.variable, "u");
	EXPECT_EQ(plan->groupProperty, "country");
	EXPECT_EQ(plan->groupAlias, "country");
	EXPECT_EQ(plan->outputAlias, "rows");
	EXPECT_EQ(plan->requirements.requiredProperties, (std::vector<std::string>{"country"}));
	EXPECT_TRUE(plan->requirements.countOnly);
}

TEST(NodeCountFastPathPlannerTest, RejectsUnsupportedPropertyGroupCountShapes) {
	{
		auto scan = makeScan("u");
		std::vector<std::shared_ptr<expressions::Expression>> groups;
		groups.push_back(std::make_shared<expressions::VariableReferenceExpression>("other", "country"));
		LogicalAggregate aggregate(std::move(scan), std::move(groups), makeAggs("count", nullptr));
		EXPECT_FALSE(tryBuildNodeGroupCountFastPathPlan(aggregate).has_value());
	}
	{
		auto scan = makeScan("u");
		std::vector<std::shared_ptr<expressions::Expression>> groups;
		groups.push_back(std::make_shared<expressions::VariableReferenceExpression>("u", "country"));
		LogicalAggregate aggregate(std::move(scan), std::move(groups), makeAggs("sum", nullptr));
		EXPECT_FALSE(tryBuildNodeGroupCountFastPathPlan(aggregate).has_value());
	}
	{
		auto scan = makeScan("u");
		std::vector<std::shared_ptr<expressions::Expression>> groups;
		groups.push_back(std::make_shared<expressions::VariableReferenceExpression>("u", "country"));
		LogicalAggregate aggregate(
				std::move(scan), std::move(groups),
				makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("other")));
		EXPECT_FALSE(tryBuildNodeGroupCountFastPathPlan(aggregate).has_value());
	}
}

TEST(NodeCountFastPathPlannerTest, RejectsDistinctCount) {
	auto scan = makeScan();
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n"), true));

	EXPECT_FALSE(tryBuildNodeCountFastPathPlan(aggregate).has_value());
}

TEST(NodeCountFastPathPlannerTest, AcceptsDistinctPropertyCountOverNodeScan) {
	auto scan = makeScan("u");
	LogicalAggregate aggregate(
			std::move(scan),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("u", "country"), true, "countries"));

	auto plan = tryBuildNodeDistinctCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->config.variable, "u");
	EXPECT_EQ(plan->distinctProperty, "country");
	EXPECT_EQ(plan->outputAlias, "countries");
	EXPECT_EQ(plan->requirements.materialization, execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES);
	EXPECT_TRUE(plan->requirements.countOnly);
	EXPECT_EQ(plan->requirements.requiredProperties, (std::vector<std::string>{"country"}));
}

TEST(NodeCountFastPathPlannerTest, RejectsDistinctCountWithoutPropertyArgument) {
	auto variableScan = makeScan();
	LogicalAggregate variableAggregate(
			std::move(variableScan),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n"), true));
	EXPECT_FALSE(tryBuildNodeDistinctCountFastPathPlan(variableAggregate).has_value());

	auto literalScan = makeScan();
	LogicalAggregate literalAggregate(
			std::move(literalScan),
			{},
			makeAggs("count", std::make_shared<expressions::LiteralExpression>(int64_t{1}), true));
	EXPECT_FALSE(tryBuildNodeDistinctCountFastPathPlan(literalAggregate).has_value());
}

TEST(NodeCountFastPathPlannerTest, RejectsDistinctPropertyCountUnsupportedShapes) {
	auto groupedScan = makeScan();
	std::vector<std::shared_ptr<expressions::Expression>> groups;
	groups.push_back(std::make_shared<expressions::VariableReferenceExpression>("n", "age"));
	LogicalAggregate grouped(
			std::move(groupedScan),
			std::move(groups),
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "country"), true));
	EXPECT_FALSE(tryBuildNodeDistinctCountFastPathPlan(grouped).has_value());

	auto multiScan = makeScan();
	auto multiAggs = makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "country"), true);
	multiAggs.emplace_back("count", std::make_shared<expressions::VariableReferenceExpression>("n"), "rows", false);
	LogicalAggregate multiple(std::move(multiScan), {}, std::move(multiAggs));
	EXPECT_FALSE(tryBuildNodeDistinctCountFastPathPlan(multiple).has_value());

	auto sumScan = makeScan();
	LogicalAggregate nonCount(
			std::move(sumScan),
			{},
			makeAggs("sum", std::make_shared<expressions::VariableReferenceExpression>("n", "country"), true));
	EXPECT_FALSE(tryBuildNodeDistinctCountFastPathPlan(nonCount).has_value());

	auto nonDistinctScan = makeScan();
	LogicalAggregate nonDistinct(
			std::move(nonDistinctScan),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "country"), false));
	EXPECT_FALSE(tryBuildNodeDistinctCountFastPathPlan(nonDistinct).has_value());

	auto nullArgScan = makeScan();
	LogicalAggregate nullArg(std::move(nullArgScan), {}, makeAggs("count", nullptr, true));
	EXPECT_FALSE(tryBuildNodeDistinctCountFastPathPlan(nullArg).has_value());
}

TEST(NodeCountFastPathPlannerTest, RejectsDistinctPropertyCountInvalidChildOrVariable) {
	auto nestedChild = std::make_unique<LogicalAggregate>(makeScan(), std::vector<std::shared_ptr<expressions::Expression>>{}, makeAggs());
	LogicalAggregate nonNode(
			std::move(nestedChild),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "country"), true));
	EXPECT_FALSE(tryBuildNodeDistinctCountFastPathPlan(nonNode).has_value());

	LogicalAggregate missingChild(
			nullptr,
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "country"), true));
	EXPECT_FALSE(tryBuildNodeDistinctCountFastPathPlan(missingChild).has_value());

	auto wrongVariableScan = makeScan("n");
	LogicalAggregate wrongVariable(
			std::move(wrongVariableScan),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("other", "country"), true));
	EXPECT_FALSE(tryBuildNodeDistinctCountFastPathPlan(wrongVariable).has_value());
}

TEST(NodeCountFastPathPlannerTest, DistinctPropertyCountAddsResidualPredicates) {
	std::vector<std::pair<std::string, PropertyValue>> predicates = {{"country", PropertyValue("CN")}};
	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"}, predicates);
	LogicalAggregate aggregate(
			std::move(scan),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "city"), true));

	auto plan = tryBuildNodeDistinctCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->requirements.requiredProperties, (std::vector<std::string>{"city", "country"}));
	ASSERT_EQ(plan->predicates.size(), 1U);
	EXPECT_EQ(plan->predicates[0].propertyKey, "country");
	EXPECT_EQ(plan->predicates[0].op, execution::VectorPredicateOp::VPO_EQ);
}

TEST(NodeCountFastPathPlannerTest, DistinctPropertyCountUsesAvailableIndexes) {
	auto uuid = boost::uuids::random_generator()();
	auto path = fs::temp_directory_path() / ("test_distinct_count_planner_" + boost::uuids::to_string(uuid) + ".dat");
	auto db = std::make_unique<Database>(path.string());
	db->open();
	auto indexManager = db->getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("idx_country_distinct", "node", "Person", "country"));
	ASSERT_TRUE(indexManager->createIndex("idx_age_distinct", "node", "Person", "age"));
	ASSERT_TRUE(indexManager->createCompositeIndex("idx_country_age_distinct", "node", "Person", {"country", "age"}));
	ASSERT_TRUE(indexManager->createIndex("idx_label_distinct", "node", "Person", ""));

	std::vector<std::pair<std::string, PropertyValue>> propertyPredicates = {{"country", PropertyValue("CN")}};
	auto propertyScan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"}, propertyPredicates);
	LogicalAggregate propertyAggregate(
			std::move(propertyScan),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "city"), true));
	auto propertyPlan = tryBuildNodeDistinctCountFastPathPlan(propertyAggregate, indexManager);
	ASSERT_TRUE(propertyPlan.has_value());
	EXPECT_EQ(propertyPlan->config.type, execution::ScanType::PROPERTY_SCAN);
	EXPECT_TRUE(propertyPlan->predicates.empty());

	auto rangeScan = makeScan();
	RangePredicate range;
	range.key = "age";
	range.minValue = PropertyValue(int64_t{18});
	range.maxValue = PropertyValue(int64_t{65});
	rangeScan->setRangePredicates({range});
	LogicalAggregate rangeAggregate(
			std::move(rangeScan),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "country"), true));
	auto rangePlan = tryBuildNodeDistinctCountFastPathPlan(rangeAggregate, indexManager);
	ASSERT_TRUE(rangePlan.has_value());
	EXPECT_EQ(rangePlan->config.type, execution::ScanType::RANGE_SCAN);
	EXPECT_TRUE(rangePlan->predicates.empty());

	auto compositeScan = makeScan();
	CompositeEqualityPredicate composite;
	composite.keys = {"country", "age"};
	composite.values = {PropertyValue("CN"), PropertyValue(int64_t{42})};
	compositeScan->setCompositeEquality(std::move(composite));
	LogicalAggregate compositeAggregate(
			std::move(compositeScan),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "city"), true));
	auto compositePlan = tryBuildNodeDistinctCountFastPathPlan(compositeAggregate, indexManager);
	ASSERT_TRUE(compositePlan.has_value());
	EXPECT_EQ(compositePlan->config.type, execution::ScanType::COMPOSITE_SCAN);
	EXPECT_TRUE(compositePlan->predicates.empty());

	auto labelScan = makeScan();
	LogicalAggregate labelAggregate(
			std::move(labelScan),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "country"), true));
	auto labelPlan = tryBuildNodeDistinctCountFastPathPlan(labelAggregate, indexManager);
	ASSERT_TRUE(labelPlan.has_value());
	EXPECT_EQ(labelPlan->config.type, execution::ScanType::LABEL_SCAN);

	db->close();
	db.reset();
	std::error_code ec;
	fs::remove(path, ec);
}

TEST(NodeCountFastPathPlannerTest, DistinctPropertyCountHandlesFallbackAndMalformedPredicates) {
	auto openRangeScan = makeScan();
	RangePredicate openRange;
	openRange.key = "age";
	openRange.maxValue = PropertyValue(int64_t{65});
	openRangeScan->setRangePredicates({openRange});
	LogicalAggregate openRangeAggregate(
			std::move(openRangeScan),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "country"), true));
	auto openRangePlan = tryBuildNodeDistinctCountFastPathPlan(openRangeAggregate);
	ASSERT_TRUE(openRangePlan.has_value());
	EXPECT_EQ(openRangePlan->config.type, execution::ScanType::FULL_SCAN);
	ASSERT_EQ(openRangePlan->predicates.size(), 1U);
	EXPECT_EQ(openRangePlan->predicates[0].op, execution::VectorPredicateOp::VPO_LE);

	auto emptyRangeScan = makeScan();
	RangePredicate emptyRange;
	emptyRange.key = "age";
	emptyRangeScan->setRangePredicates({emptyRange});
	LogicalAggregate emptyRangeAggregate(
			std::move(emptyRangeScan),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "country"), true));
	EXPECT_FALSE(tryBuildNodeDistinctCountFastPathPlan(emptyRangeAggregate).has_value());

	auto malformedCompositeScan = makeScan();
	CompositeEqualityPredicate malformed;
	malformed.keys = {"country", "age"};
	malformed.values = {PropertyValue("CN")};
	malformedCompositeScan->setCompositeEquality(std::move(malformed));
	LogicalAggregate malformedCompositeAggregate(
			std::move(malformedCompositeScan),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "city"), true));
	EXPECT_FALSE(tryBuildNodeDistinctCountFastPathPlan(malformedCompositeAggregate).has_value());
}

TEST(NodeCountFastPathPlannerTest, RejectsCountPropertyAccess) {
	auto scan = makeScan();
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "age")));

	EXPECT_FALSE(tryBuildNodeCountFastPathPlan(aggregate).has_value());
}

TEST(NodeCountFastPathPlannerTest, RejectsNonVariableAndMismatchedCountArguments) {
	auto literalScan = makeScan();
	LogicalAggregate literalAggregate(std::move(literalScan), {}, makeAggs("count", std::make_shared<expressions::LiteralExpression>(int64_t{1})));
	EXPECT_FALSE(tryBuildNodeCountFastPathPlan(literalAggregate).has_value());

	auto otherVariableScan = makeScan();
	LogicalAggregate otherVariableAggregate(
			std::move(otherVariableScan), {}, makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("other")));
	EXPECT_FALSE(tryBuildNodeCountFastPathPlan(otherVariableAggregate).has_value());
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

TEST(NodeCountFastPathPlannerTest, RejectsMissingChild) {
	LogicalAggregate aggregate(nullptr, {}, makeAggs());

	EXPECT_FALSE(tryBuildNodeCountFastPathPlan(aggregate).has_value());
}

TEST(NodeCountFastPathPlannerTest, UsesDefaultAliasWhenAggregateAliasIsEmpty) {
	auto scan = makeScan();
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n"), false, ""));

	auto plan = tryBuildNodeCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->outputAlias, "count");
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

TEST(NodeCountFastPathPlannerTest, AbsorbsEqualityPredicateHandledByPropertyCandidateSource) {
	std::vector<std::pair<std::string, PropertyValue>> predicates = {{"age", PropertyValue(int64_t{42})}};
	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"}, predicates);
	scan->setPreferredScanType(execution::ScanType::PROPERTY_SCAN);
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs());

	auto plan = tryBuildNodeCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->config.type, execution::ScanType::PROPERTY_SCAN);
	EXPECT_EQ(plan->config.indexKey, "age");
	EXPECT_EQ(plan->config.indexValue, PropertyValue(int64_t{42}));
	EXPECT_EQ(plan->requirements.materialization, execution::NodeMaterializationMode::NSM_ID_ONLY);
	EXPECT_TRUE(plan->requirements.requiredProperties.empty());
	EXPECT_TRUE(plan->predicates.empty());
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

TEST(NodeCountFastPathPlannerTest, AbsorbsInclusiveRangeHandledByRangeCandidateSource) {
	RangePredicate inclusive;
	inclusive.key = "age";
	inclusive.minValue = PropertyValue(int64_t{18});
	inclusive.maxValue = PropertyValue(int64_t{65});
	inclusive.minInclusive = true;
	inclusive.maxInclusive = true;
	auto scan = makeScan();
	scan->setPreferredScanType(execution::ScanType::RANGE_SCAN);
	scan->setRangePredicates({inclusive});
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs());

	auto plan = tryBuildNodeCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->config.type, execution::ScanType::RANGE_SCAN);
	EXPECT_EQ(plan->config.indexKey, "age");
	EXPECT_EQ(plan->requirements.materialization, execution::NodeMaterializationMode::NSM_ID_ONLY);
	EXPECT_TRUE(plan->requirements.requiredProperties.empty());
	EXPECT_TRUE(plan->predicates.empty());
}

TEST(NodeCountFastPathPlannerTest, AbsorbsExclusiveRangeHandledByRangeCandidateSource) {
	RangePredicate exclusive;
	exclusive.key = "age";
	exclusive.minValue = PropertyValue(int64_t{30});
	exclusive.maxValue = PropertyValue(int64_t{40});
	exclusive.minInclusive = true;
	exclusive.maxInclusive = false;
	auto scan = makeScan();
	scan->setPreferredScanType(execution::ScanType::RANGE_SCAN);
	scan->setRangePredicates({exclusive});
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs());

	auto plan = tryBuildNodeCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->config.type, execution::ScanType::RANGE_SCAN);
	EXPECT_TRUE(plan->config.minInclusive);
	EXPECT_FALSE(plan->config.maxInclusive);
	EXPECT_EQ(plan->requirements.materialization, execution::NodeMaterializationMode::NSM_ID_ONLY);
	EXPECT_TRUE(plan->requirements.requiredProperties.empty());
	EXPECT_TRUE(plan->predicates.empty());
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

TEST(NodeCountFastPathPlannerTest, AbsorbsCompositeEqualityHandledByCompositeCandidateSource) {
	std::vector<std::pair<std::string, PropertyValue>> predicates = {
			{"country", PropertyValue("CN")},
			{"age", PropertyValue(int64_t{42})}};
	auto scan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"}, predicates);
	CompositeEqualityPredicate composite;
	composite.keys = {"country", "age"};
	composite.values = {PropertyValue("CN"), PropertyValue(int64_t{42})};
	scan->setCompositeEquality(std::move(composite));
	scan->setPreferredScanType(execution::ScanType::COMPOSITE_SCAN);
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("u")));

	auto plan = tryBuildNodeCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->config.type, execution::ScanType::COMPOSITE_SCAN);
	EXPECT_EQ(plan->config.compositeKeys, (std::vector<std::string>{"country", "age"}));
	EXPECT_EQ(plan->requirements.materialization, execution::NodeMaterializationMode::NSM_ID_ONLY);
	EXPECT_TRUE(plan->requirements.requiredProperties.empty());
	EXPECT_TRUE(plan->predicates.empty());
}

TEST(NodeCountFastPathPlannerTest, RejectsMalformedCompositeEquality) {
	auto scan = makeScan();
	CompositeEqualityPredicate composite;
	composite.keys = {"country", "age"};
	composite.values = {PropertyValue("CN")};
	scan->setCompositeEquality(std::move(composite));
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs());

	EXPECT_FALSE(tryBuildNodeCountFastPathPlan(aggregate).has_value());
}

TEST(NodeCountFastPathPlannerTest, RejectsEmptyRangePredicate) {
	auto scan = makeScan();
	RangePredicate range;
	range.key = "age";
	scan->setRangePredicates({range});
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs());

	EXPECT_FALSE(tryBuildNodeCountFastPathPlan(aggregate).has_value());
}

TEST(NodeCountFastPathPlannerTest, InvalidPreferredPropertyScanFallsBackToLabelScan) {
	auto scan = makeScan();
	scan->setPreferredScanType(execution::ScanType::PROPERTY_SCAN);
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs());

	auto plan = tryBuildNodeCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->config.type, execution::ScanType::LABEL_SCAN);
	EXPECT_TRUE(plan->config.indexKey.empty());
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

TEST(NodeCountFastPathPlannerTest, PreferredCandidateSourcesWithoutPredicatesFallBackSafely) {
	auto rangeScan = makeScan();
	rangeScan->setPreferredScanType(execution::ScanType::RANGE_SCAN);
	LogicalAggregate rangeAggregate(std::move(rangeScan), {}, makeAggs());
	auto rangePlan = tryBuildNodeCountFastPathPlan(rangeAggregate);
	ASSERT_TRUE(rangePlan.has_value());
	EXPECT_EQ(rangePlan->config.type, execution::ScanType::LABEL_SCAN);

	auto compositeScan = makeScan();
	compositeScan->setPreferredScanType(execution::ScanType::COMPOSITE_SCAN);
	LogicalAggregate compositeAggregate(std::move(compositeScan), {}, makeAggs());
	auto compositePlan = tryBuildNodeCountFastPathPlan(compositeAggregate);
	ASSERT_TRUE(compositePlan.has_value());
	EXPECT_EQ(compositePlan->config.type, execution::ScanType::LABEL_SCAN);

	auto labelScan = makeScan();
	labelScan->setPreferredScanType(execution::ScanType::LABEL_SCAN);
	LogicalAggregate labelAggregate(std::move(labelScan), {}, makeAggs());
	auto labelPlan = tryBuildNodeCountFastPathPlan(labelAggregate);
	ASSERT_TRUE(labelPlan.has_value());
	EXPECT_EQ(labelPlan->config.type, execution::ScanType::LABEL_SCAN);
}

TEST(NodeCountFastPathPlannerTest, ChoosesIndexedPropertyRangeCompositeAndLabelCandidateSources) {
	const auto dbPath = fs::temp_directory_path() /
	                    ("test_node_count_planner_indexes_" + boost::uuids::to_string(boost::uuids::random_generator()()) + ".zyx");
	Database db(dbPath.string());
	db.open();
	auto indexManager = db.getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("idx_person_age", "node", "Person", "age"));
	ASSERT_TRUE(indexManager->createCompositeIndex("idx_person_country_age", "node", "Person", {"country", "age"}));
	ASSERT_TRUE(indexManager->createIndex("idx_person_label", "node", "Person", ""));

	auto propertyScan = std::make_unique<LogicalNodeScan>(
			"n", std::vector<std::string>{"Person"},
			std::vector<std::pair<std::string, PropertyValue>>{{"age", PropertyValue(int64_t{42})}});
	LogicalAggregate propertyAggregate(std::move(propertyScan), {}, makeAggs());
	auto propertyPlan = tryBuildNodeCountFastPathPlan(propertyAggregate, indexManager);
	ASSERT_TRUE(propertyPlan.has_value());
	EXPECT_EQ(propertyPlan->config.type, execution::ScanType::PROPERTY_SCAN);
	EXPECT_TRUE(propertyPlan->predicates.empty());

	auto rangeScan = makeScan();
	rangeScan->setRangePredicates({RangePredicate{"age", PropertyValue(int64_t{18}), PropertyValue(int64_t{65}), true, true}});
	LogicalAggregate rangeAggregate(std::move(rangeScan), {}, makeAggs());
	auto rangePlan = tryBuildNodeCountFastPathPlan(rangeAggregate, indexManager);
	ASSERT_TRUE(rangePlan.has_value());
	EXPECT_EQ(rangePlan->config.type, execution::ScanType::RANGE_SCAN);
	EXPECT_TRUE(rangePlan->predicates.empty());

	auto compositeScan = makeScan();
	compositeScan->setCompositeEquality({{"country", "age"}, {PropertyValue("CN"), PropertyValue(int64_t{30})}});
	LogicalAggregate compositeAggregate(std::move(compositeScan), {}, makeAggs());
	auto compositePlan = tryBuildNodeCountFastPathPlan(compositeAggregate, indexManager);
	ASSERT_TRUE(compositePlan.has_value());
	EXPECT_EQ(compositePlan->config.type, execution::ScanType::COMPOSITE_SCAN);
	EXPECT_TRUE(compositePlan->predicates.empty());

	LogicalAggregate labelAggregate(makeScan(), {}, makeAggs());
	auto labelPlan = tryBuildNodeCountFastPathPlan(labelAggregate, indexManager);
	ASSERT_TRUE(labelPlan.has_value());
	EXPECT_EQ(labelPlan->config.type, execution::ScanType::LABEL_SCAN);

	db.close();
	std::error_code ec;
	fs::remove_all(dbPath, ec);
}

TEST(NodeCountFastPathPlannerTest, OpenRangeWithOnlyUpperBoundFallsBackBeforeApplyingPredicate) {
	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{});
	scan->setPreferredScanType(execution::ScanType::RANGE_SCAN);
	RangePredicate range;
	range.key = "age";
	range.maxValue = PropertyValue(int64_t{65});
	range.maxInclusive = true;
	scan->setRangePredicates({range});
	LogicalAggregate aggregate(std::move(scan), {}, makeAggs());

	auto plan = tryBuildNodeCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->config.type, execution::ScanType::FULL_SCAN);
	ASSERT_EQ(plan->predicates.size(), 1U);
	EXPECT_EQ(plan->predicates[0].op, execution::VectorPredicateOp::VPO_LE);
}

TEST(NodeCountFastPathPlannerTest, DistinctCountFallsBackFromOpenPreferredRangeAndAddsResidualPredicate) {
	auto scan = makeScan("n");
	scan->setPreferredScanType(execution::ScanType::RANGE_SCAN);
	RangePredicate range;
	range.key = "age";
	range.minValue = PropertyValue(int64_t{18});
	range.minInclusive = false;
	scan->setRangePredicates({range});
	LogicalAggregate aggregate(
			std::move(scan),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "country"), true));

	auto plan = tryBuildNodeDistinctCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->config.type, execution::ScanType::LABEL_SCAN);
	ASSERT_EQ(plan->predicates.size(), 1U);
	EXPECT_EQ(plan->predicates[0].op, execution::VectorPredicateOp::VPO_GT);
	EXPECT_EQ(plan->requirements.requiredProperties, (std::vector<std::string>{"country", "age"}));
}

TEST(NodeCountFastPathPlannerTest, DistinctCountAddsCompositeResidualPredicatesWhenNotHandledByIndex) {
	auto scan = makeScan("n");
	CompositeEqualityPredicate composite;
	composite.keys = {"country", "age"};
	composite.values = {PropertyValue("CN"), PropertyValue(int64_t{30})};
	scan->setCompositeEquality(std::move(composite));
	LogicalAggregate aggregate(
			std::move(scan),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("n", "city"), true));

	auto plan = tryBuildNodeDistinctCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->config.type, execution::ScanType::FULL_SCAN);
	EXPECT_EQ(plan->requirements.requiredProperties, (std::vector<std::string>{"city", "country", "age"}));
	ASSERT_EQ(plan->predicates.size(), 2U);
	EXPECT_EQ(plan->predicates[0].propertyKey, "country");
	EXPECT_EQ(plan->predicates[1].propertyKey, "age");
}

TEST(NodeCountFastPathPlannerTest, GroupCountHandlesFallbackRangesAndCompositePredicates) {
	std::vector<std::shared_ptr<expressions::Expression>> groups;
	groups.push_back(std::make_shared<expressions::VariableReferenceExpression>("u", "country"));
	auto scan = std::make_unique<LogicalNodeScan>(
			"u", std::vector<std::string>{"User"},
			std::vector<std::pair<std::string, PropertyValue>>{{"status", PropertyValue("active")}});
	RangePredicate range;
	range.key = "age";
	range.minValue = PropertyValue(int64_t{18});
	range.maxValue = PropertyValue(int64_t{65});
	range.minInclusive = true;
	range.maxInclusive = true;
	scan->setRangePredicates({range});
	CompositeEqualityPredicate composite;
	composite.keys = {"region", "tier"};
	composite.values = {PropertyValue("APAC"), PropertyValue(int64_t{2})};
	scan->setCompositeEquality(std::move(composite));
	LogicalAggregate aggregate(std::move(scan), std::move(groups), makeAggs("count", nullptr, false, "rows"));

	auto plan = tryBuildNodeGroupCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->config.type, execution::ScanType::FULL_SCAN);
	EXPECT_EQ(plan->requirements.requiredProperties,
	          (std::vector<std::string>{"country", "status", "age", "region", "tier"}));
	ASSERT_EQ(plan->predicates.size(), 4U);
	EXPECT_EQ(plan->predicates[1].op, execution::VectorPredicateOp::VPO_RANGE_CLOSED);
}

TEST(NodeCountFastPathPlannerTest, GroupCountRejectsMalformedFilterPredicates) {
	{
		std::vector<std::shared_ptr<expressions::Expression>> groups;
		groups.push_back(std::make_shared<expressions::VariableReferenceExpression>("n", "country"));
		auto scan = makeScan("n");
		RangePredicate range;
		range.key = "age";
		scan->setRangePredicates({range});
		LogicalAggregate aggregate(std::move(scan), std::move(groups), makeAggs("count", nullptr));
		EXPECT_FALSE(tryBuildNodeGroupCountFastPathPlan(aggregate).has_value());
	}
	{
		std::vector<std::shared_ptr<expressions::Expression>> groups;
		groups.push_back(std::make_shared<expressions::VariableReferenceExpression>("n", "country"));
		auto scan = makeScan("n");
		CompositeEqualityPredicate composite;
		composite.keys = {"country", "age"};
		composite.values = {PropertyValue("CN")};
		scan->setCompositeEquality(std::move(composite));
		LogicalAggregate aggregate(std::move(scan), std::move(groups), makeAggs("count", nullptr));
		EXPECT_FALSE(tryBuildNodeGroupCountFastPathPlan(aggregate).has_value());
	}
}

TEST(NodeCountFastPathPlannerTest, GroupCountChoosesAvailableIndexesAndAbsorbsHandledPredicates) {
	const auto dbPath = fs::temp_directory_path() /
	                    ("test_group_count_planner_indexes_" + boost::uuids::to_string(boost::uuids::random_generator()()) + ".zyx");
	Database db(dbPath.string());
	db.open();
	auto indexManager = db.getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("idx_group_status", "node", "User", "status"));
	ASSERT_TRUE(indexManager->createIndex("idx_group_age", "node", "User", "age"));
	ASSERT_TRUE(indexManager->createCompositeIndex("idx_group_status_age", "node", "User", {"status", "age"}));

	std::vector<std::shared_ptr<expressions::Expression>> groups;
	groups.push_back(std::make_shared<expressions::VariableReferenceExpression>("u", "country"));
	auto propertyScan = std::make_unique<LogicalNodeScan>(
			"u", std::vector<std::string>{"User"},
			std::vector<std::pair<std::string, PropertyValue>>{{"status", PropertyValue("active")}});
	LogicalAggregate propertyAggregate(std::move(propertyScan), groups, makeAggs("count", nullptr, false, "rows"));
	auto propertyPlan = tryBuildNodeGroupCountFastPathPlan(propertyAggregate, indexManager);
	ASSERT_TRUE(propertyPlan.has_value());
	EXPECT_EQ(propertyPlan->config.type, execution::ScanType::PROPERTY_SCAN);
	EXPECT_TRUE(propertyPlan->predicates.empty());

	auto rangeScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	rangeScan->setRangePredicates({RangePredicate{"age", PropertyValue(int64_t{18}), PropertyValue(int64_t{65}), true, false}});
	LogicalAggregate rangeAggregate(std::move(rangeScan), groups, makeAggs("count", nullptr, false, "rows"));
	auto rangePlan = tryBuildNodeGroupCountFastPathPlan(rangeAggregate, indexManager);
	ASSERT_TRUE(rangePlan.has_value());
	EXPECT_EQ(rangePlan->config.type, execution::ScanType::RANGE_SCAN);
	EXPECT_TRUE(rangePlan->predicates.empty());

	auto compositeScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	compositeScan->setCompositeEquality({{"status", "age"}, {PropertyValue("active"), PropertyValue(int64_t{30})}});
	LogicalAggregate compositeAggregate(std::move(compositeScan), groups, makeAggs("count", nullptr, false, "rows"));
	auto compositePlan = tryBuildNodeGroupCountFastPathPlan(compositeAggregate, indexManager);
	ASSERT_TRUE(compositePlan.has_value());
	EXPECT_EQ(compositePlan->config.type, execution::ScanType::COMPOSITE_SCAN);
	EXPECT_TRUE(compositePlan->predicates.empty());

	db.close();
	std::error_code ec;
	fs::remove_all(dbPath, ec);
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

TEST(NodeCountFastPathPlannerTest, PhysicalPlanConverterUsesDistinctCountFastPathForPropertyArgument) {
	auto scan = makeScan("u");
	LogicalAggregate aggregate(
			std::move(scan),
			{},
			makeAggs("count", std::make_shared<expressions::VariableReferenceExpression>("u", "country"), true));
	query::PhysicalPlanConverter converter(std::shared_ptr<storage::DataManager>{},
	                                      std::shared_ptr<indexes::IndexManager>{});

	auto physical = converter.convert(&aggregate);

	ASSERT_NE(physical, nullptr);
	EXPECT_EQ(physical->toString(), "NodeDistinctCountFastPath(u.country -> count)");
}
