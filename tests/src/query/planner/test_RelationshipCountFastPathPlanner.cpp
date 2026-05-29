#include <gtest/gtest.h>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <filesystem>

#include "graph/core/Database.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/planner/PhysicalPlanConverter.hpp"
#include "graph/query/planner/RelationshipCountFastPathPlanner.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::logical;
using namespace graph::query::planner;

namespace fs = std::filesystem;

namespace {
std::unique_ptr<LogicalNodeScan> makeSeedScan() {
	std::vector<std::pair<std::string, PropertyValue>> predicates = {{"id", PropertyValue("u1")}};
	auto scan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"}, predicates);
	scan->setPreferredScanType(execution::ScanType::PROPERTY_SCAN);
	return scan;
}

std::unique_ptr<LogicalNodeScan> makeUnindexedSeedScan() {
	std::vector<std::pair<std::string, PropertyValue>> predicates = {{"id", PropertyValue("u1")}};
	return std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"}, predicates);
}

std::unique_ptr<LogicalNodeScan> makeUnanchoredSeedScan() {
	return std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
}

std::unique_ptr<LogicalTraversal> makeOneHop() {
	return std::make_unique<LogicalTraversal>(makeSeedScan(), "u", "r", "v", "FOLLOWS", "out", std::vector<std::string>{"User"});
}

std::unique_ptr<LogicalTraversal> makeOneHopFrom(std::unique_ptr<LogicalNodeScan> seed) {
	return std::make_unique<LogicalTraversal>(std::move(seed), "u", "r", "v", "FOLLOWS", "out", std::vector<std::string>{"User"});
}

std::vector<LogicalAggItem> makeAggs(std::shared_ptr<expressions::Expression> arg = std::make_shared<expressions::VariableReferenceExpression>("v"),
                                     bool distinct = false) {
	std::vector<LogicalAggItem> aggs;
	aggs.emplace_back("count", std::move(arg), "count", distinct);
	return aggs;
}

std::unique_ptr<expressions::BinaryOpExpression> makeEdgeEquality(std::unique_ptr<expressions::Expression> left,
                                                                  std::unique_ptr<expressions::Expression> right) {
	return std::make_unique<expressions::BinaryOpExpression>(
			std::move(left), expressions::BinaryOperatorType::BOP_EQUAL, std::move(right));
}
} // namespace

TEST(RelationshipCountFastPathPlannerTest, AcceptsOneHopCountTraversal) {
	LogicalAggregate aggregate(makeOneHop(), {}, makeAggs());

	auto plan = tryBuildRelationshipCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->seedConfig.type, execution::ScanType::PROPERTY_SCAN);
	EXPECT_EQ(plan->seedConfig.indexKey, "id");
	EXPECT_EQ(plan->seedConfig.indexValue, PropertyValue("u1"));
	EXPECT_EQ(plan->seedConfig.variable, "u");
	EXPECT_EQ(plan->seedConfig.labels, (std::vector<std::string>{"User"}));
	EXPECT_EQ(plan->seedRequirements.materialization, execution::NodeMaterializationMode::NSM_ID_ONLY);
	EXPECT_TRUE(plan->seedRequirements.requiredProperties.empty());
	EXPECT_TRUE(plan->seedPredicates.empty());
	ASSERT_EQ(plan->hops.size(), 1U);
	EXPECT_EQ(plan->hops[0].sourceVar, "u");
	EXPECT_EQ(plan->hops[0].edgeVar, "r");
	EXPECT_EQ(plan->hops[0].targetVar, "v");
	EXPECT_EQ(plan->hops[0].edgeType, "FOLLOWS");
	EXPECT_EQ(plan->hops[0].direction, "out");
	EXPECT_EQ(plan->hops[0].targetLabels, (std::vector<std::string>{"User"}));
	EXPECT_EQ(plan->outputAlias, "count");
}

TEST(RelationshipCountFastPathPlannerTest, AcceptsTwoHopCountTraversal) {
	auto firstHop = makeOneHop();
	auto secondHop = std::make_unique<LogicalTraversal>(std::move(firstHop), "v", "r2", "w", "FOLLOWS", "out", std::vector<std::string>{"User"});
	LogicalAggregate aggregate(std::move(secondHop), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("w")));

	auto plan = tryBuildRelationshipCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	ASSERT_EQ(plan->hops.size(), 2U);
	EXPECT_EQ(plan->hops[0].targetVar, "v");
	EXPECT_EQ(plan->hops[1].sourceVar, "v");
	EXPECT_EQ(plan->hops[1].targetVar, "w");
}

TEST(RelationshipCountFastPathPlannerTest, AcceptsCountStarAndCountEdgeVariable) {
	LogicalAggregate countStar(makeOneHop(), {}, makeAggs(nullptr));
	EXPECT_TRUE(tryBuildRelationshipCountFastPathPlan(countStar).has_value());

	LogicalAggregate countEdge(makeOneHop(), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("r")));
	EXPECT_TRUE(tryBuildRelationshipCountFastPathPlan(countEdge).has_value());
}

TEST(RelationshipCountFastPathPlannerTest, RejectsAnchoredSeedWithoutIndexCandidateSource) {
	auto hop = std::make_unique<LogicalTraversal>(makeUnindexedSeedScan(), "u", "r", "v", "FOLLOWS", "out",
	                                             std::vector<std::string>{"User"});
	LogicalAggregate aggregate(std::move(hop), {}, makeAggs());

	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(aggregate).has_value());
}

TEST(RelationshipCountFastPathPlannerTest, PreservesUnanchoredRelationshipTypeScanFastPath) {
	auto hop = std::make_unique<LogicalTraversal>(makeUnanchoredSeedScan(), "u", "r", "v", "FOLLOWS", "out",
	                                             std::vector<std::string>{"User"});
	LogicalAggregate aggregate(std::move(hop), {}, makeAggs());

	auto plan = tryBuildRelationshipCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->seedConfig.type, execution::ScanType::FULL_SCAN);
	EXPECT_TRUE(plan->seedPredicates.empty());
}

TEST(RelationshipCountFastPathPlannerTest, UsesDirectRelationshipCountForUnanchoredEdgeCountWithoutProperties) {
	auto seed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{});
	auto hop = std::make_unique<LogicalTraversal>(std::move(seed), "u", "r", "v", "FOLLOWS", "out");
	LogicalAggregate aggregate(std::move(hop), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("r")));

	auto plan = tryBuildRelationshipCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_TRUE(plan->directCount.enabled);
	EXPECT_TRUE(plan->directCount.edgeProperties.empty());
	EXPECT_EQ(plan->directCount.edgeType, "FOLLOWS");
	ASSERT_EQ(plan->hops.size(), 1U);
	EXPECT_EQ(plan->hops[0].edgeType, "FOLLOWS");
}

TEST(RelationshipCountFastPathPlannerTest, UsesDirectRelationshipCountForUnanchoredEdgePropertyCount) {
	auto seed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{});
	auto hop = std::make_unique<LogicalTraversal>(std::move(seed), "u", "r", "v", "FOLLOWS", "out",
	                                             std::vector<std::string>{},
	                                             std::vector<std::pair<std::string, PropertyValue>>{},
	                                             std::unordered_map<std::string, PropertyValue>{{"weight", PropertyValue(int64_t{1})}});
	LogicalAggregate aggregate(std::move(hop), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("r")));

	auto plan = tryBuildRelationshipCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_TRUE(plan->directCount.enabled);
	EXPECT_EQ(plan->directCount.edgeProperties.at("weight"), PropertyValue(int64_t{1}));
}

TEST(RelationshipCountFastPathPlannerTest, UsesDirectRelationshipCountForUnanchoredEdgeFilterCount) {
	auto seed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{});
	auto hop = std::make_unique<LogicalTraversal>(std::move(seed), "u", "r", "v", "FOLLOWS", "out");
	auto predicate = std::make_unique<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("r", "weight"),
			expressions::BinaryOperatorType::BOP_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t{1}));
	std::shared_ptr<expressions::Expression> sharedPredicate(std::move(predicate));
	auto filter = std::make_unique<LogicalFilter>(std::move(hop), std::move(sharedPredicate));
	LogicalAggregate aggregate(std::move(filter), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("r")));

	auto plan = tryBuildRelationshipCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_TRUE(plan->directCount.enabled);
	EXPECT_EQ(plan->directCount.edgeProperties.at("weight"), PropertyValue(int64_t{1}));
}

TEST(RelationshipCountFastPathPlannerTest, AllowsDirectRelationshipCountWithEmptyFilterPredicate) {
	auto seed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{});
	auto hop = std::make_unique<LogicalTraversal>(std::move(seed), "u", "r", "v", "FOLLOWS", "out");
	auto filter = std::make_unique<LogicalFilter>(
			std::move(hop), std::shared_ptr<expressions::Expression>{});
	LogicalAggregate aggregate(std::move(filter), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("r")));

	auto plan = tryBuildRelationshipCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_TRUE(plan->directCount.enabled);
	EXPECT_TRUE(plan->directCount.edgeProperties.empty());
}

TEST(RelationshipCountFastPathPlannerTest, RejectsDirectCountArgumentsThatAreNotEdgeVariables) {
	auto targetVariableSeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{});
	auto targetVariableHop = std::make_unique<LogicalTraversal>(std::move(targetVariableSeed), "u", "r", "v", "FOLLOWS", "out");
	LogicalAggregate targetVariableAggregate(std::move(targetVariableHop), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("v")));
	auto targetVariablePlan = tryBuildRelationshipCountFastPathPlan(targetVariableAggregate);
	ASSERT_TRUE(targetVariablePlan.has_value());
	EXPECT_FALSE(targetVariablePlan->directCount.enabled);

	auto edgePropertySeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{});
	auto edgePropertyHop = std::make_unique<LogicalTraversal>(std::move(edgePropertySeed), "u", "r", "v", "FOLLOWS", "out");
	LogicalAggregate edgePropertyAggregate(std::move(edgePropertyHop), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("r", "weight")));
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(edgePropertyAggregate).has_value());

	auto literalSeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{});
	auto literalHop = std::make_unique<LogicalTraversal>(std::move(literalSeed), "u", "r", "v", "FOLLOWS", "out");
	LogicalAggregate literalAggregate(std::move(literalHop), {}, makeAggs(std::make_shared<expressions::LiteralExpression>(int64_t{1})));
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(literalAggregate).has_value());
}

TEST(RelationshipCountFastPathPlannerTest, ExtractsDirectEdgeFilterLiteralVariants) {
	auto seed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{});
	auto hop = std::make_unique<LogicalTraversal>(std::move(seed), "u", "r", "v", "FOLLOWS", "out");
	auto left = makeEdgeEquality(
			std::make_unique<expressions::VariableReferenceExpression>("r", "active"),
			std::make_unique<expressions::LiteralExpression>(true));
	auto right = makeEdgeEquality(
			std::make_unique<expressions::LiteralExpression>(std::string("gold")),
			std::make_unique<expressions::VariableReferenceExpression>("r.tier"));
	auto predicate = std::make_unique<expressions::BinaryOpExpression>(
			std::move(left), expressions::BinaryOperatorType::BOP_AND, std::move(right));
	std::shared_ptr<expressions::Expression> sharedPredicate(std::move(predicate));
	auto filter = std::make_unique<LogicalFilter>(std::move(hop), std::move(sharedPredicate));
	LogicalAggregate aggregate(std::move(filter), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("r")));

	auto plan = tryBuildRelationshipCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_TRUE(plan->directCount.enabled);
	EXPECT_EQ(plan->directCount.edgeProperties.at("active"), PropertyValue(true));
	EXPECT_EQ(plan->directCount.edgeProperties.at("tier"), PropertyValue("gold"));
}

TEST(RelationshipCountFastPathPlannerTest, ExtractsDirectEdgeFilterNullAndDoubleLiterals) {
	auto seed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{});
	auto hop = std::make_unique<LogicalTraversal>(std::move(seed), "u", "r", "v", "FOLLOWS", "out");
	auto left = makeEdgeEquality(
			std::make_unique<expressions::VariableReferenceExpression>("r", "score"),
			std::make_unique<expressions::LiteralExpression>(1.5));
	auto right = makeEdgeEquality(
			std::make_unique<expressions::VariableReferenceExpression>("r", "missing"),
			std::make_unique<expressions::LiteralExpression>());
	auto predicate = std::make_unique<expressions::BinaryOpExpression>(
			std::move(left), expressions::BinaryOperatorType::BOP_AND, std::move(right));
	std::shared_ptr<expressions::Expression> sharedPredicate(std::move(predicate));
	auto filter = std::make_unique<LogicalFilter>(std::move(hop), std::move(sharedPredicate));
	LogicalAggregate aggregate(std::move(filter), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("r")));

	auto plan = tryBuildRelationshipCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_TRUE(plan->directCount.enabled);
	EXPECT_EQ(plan->directCount.edgeProperties.at("score"), PropertyValue(1.5));
	EXPECT_EQ(plan->directCount.edgeProperties.at("missing"), PropertyValue{});
}

TEST(RelationshipCountFastPathPlannerTest, RejectsUnsupportedDirectEdgeFilters) {
	auto makeAggregate = [](std::shared_ptr<expressions::Expression> predicate) {
		auto seed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{});
		auto hop = std::make_unique<LogicalTraversal>(std::move(seed), "u", "r", "v", "FOLLOWS", "out");
		auto filter = std::make_unique<LogicalFilter>(std::move(hop), std::move(predicate));
		return LogicalAggregate(std::move(filter), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("r")));
	};

	auto nonEquality = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("r", "weight"),
			expressions::BinaryOperatorType::BOP_GREATER,
			std::make_unique<expressions::LiteralExpression>(int64_t{1}));
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(makeAggregate(nonEquality)).has_value());

	auto nonLiteral = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("r", "weight"),
			expressions::BinaryOperatorType::BOP_EQUAL,
			std::make_unique<expressions::VariableReferenceExpression>("other"));
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(makeAggregate(nonLiteral)).has_value());

	auto plainExpression = std::make_shared<expressions::VariableReferenceExpression>("r", "weight");
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(makeAggregate(plainExpression)).has_value());
}

TEST(RelationshipCountFastPathPlannerTest, AddsSeedRangePredicatesNotCoveredByCandidateSource) {
	auto seed = makeSeedScan();
	seed->setRangePredicates({RangePredicate{"age", PropertyValue(int64_t{18}), PropertyValue(int64_t{65}), true, true}});
	LogicalAggregate aggregate(makeOneHopFrom(std::move(seed)), {}, makeAggs());

	auto plan = tryBuildRelationshipCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	ASSERT_EQ(plan->seedPredicates.size(), 1U);
	EXPECT_EQ(plan->seedPredicates[0].propertyKey, "age");
	EXPECT_EQ(plan->seedPredicates[0].op, execution::VectorPredicateOp::VPO_RANGE_CLOSED);
	EXPECT_EQ(plan->seedPredicates[0].upperValue, PropertyValue(int64_t{65}));
	EXPECT_EQ(plan->seedRequirements.materialization, execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES);
}

TEST(RelationshipCountFastPathPlannerTest, AddsOpenSeedRangeBoundsWhenNotCoveredByCandidateSource) {
	auto minOnlySeed = makeSeedScan();
	minOnlySeed->setRangePredicates({RangePredicate{"age", PropertyValue(int64_t{18}), PropertyValue{}, false, true}});
	LogicalAggregate minOnlyAggregate(makeOneHopFrom(std::move(minOnlySeed)), {}, makeAggs());
	auto minOnlyPlan = tryBuildRelationshipCountFastPathPlan(minOnlyAggregate);
	ASSERT_TRUE(minOnlyPlan.has_value());
	ASSERT_EQ(minOnlyPlan->seedPredicates.size(), 1U);
	EXPECT_EQ(minOnlyPlan->seedPredicates[0].op, execution::VectorPredicateOp::VPO_GT);

	auto maxOnlySeed = makeSeedScan();
	maxOnlySeed->setRangePredicates({RangePredicate{"age", PropertyValue{}, PropertyValue(int64_t{65}), true, false}});
	LogicalAggregate maxOnlyAggregate(makeOneHopFrom(std::move(maxOnlySeed)), {}, makeAggs());
	auto maxOnlyPlan = tryBuildRelationshipCountFastPathPlan(maxOnlyAggregate);
	ASSERT_TRUE(maxOnlyPlan.has_value());
	ASSERT_EQ(maxOnlyPlan->seedPredicates.size(), 1U);
	EXPECT_EQ(maxOnlyPlan->seedPredicates[0].op, execution::VectorPredicateOp::VPO_LT);

	auto inclusiveMinSeed = makeSeedScan();
	inclusiveMinSeed->setRangePredicates({RangePredicate{"age", PropertyValue(int64_t{18}), PropertyValue{}, true, true}});
	LogicalAggregate inclusiveMinAggregate(makeOneHopFrom(std::move(inclusiveMinSeed)), {}, makeAggs());
	auto inclusiveMinPlan = tryBuildRelationshipCountFastPathPlan(inclusiveMinAggregate);
	ASSERT_TRUE(inclusiveMinPlan.has_value());
	ASSERT_EQ(inclusiveMinPlan->seedPredicates.size(), 1U);
	EXPECT_EQ(inclusiveMinPlan->seedPredicates[0].op, execution::VectorPredicateOp::VPO_GE);

	auto inclusiveMaxSeed = makeSeedScan();
	inclusiveMaxSeed->setRangePredicates({RangePredicate{"age", PropertyValue{}, PropertyValue(int64_t{65}), true, true}});
	LogicalAggregate inclusiveMaxAggregate(makeOneHopFrom(std::move(inclusiveMaxSeed)), {}, makeAggs());
	auto inclusiveMaxPlan = tryBuildRelationshipCountFastPathPlan(inclusiveMaxAggregate);
	ASSERT_TRUE(inclusiveMaxPlan.has_value());
	ASSERT_EQ(inclusiveMaxPlan->seedPredicates.size(), 1U);
	EXPECT_EQ(inclusiveMaxPlan->seedPredicates[0].op, execution::VectorPredicateOp::VPO_LE);
}

TEST(RelationshipCountFastPathPlannerTest, AddsExclusiveRangePairAndKeepsRequiredPropertiesUnique) {
	auto seed = makeSeedScan();
	seed->setRangePredicates({
			RangePredicate{"age", PropertyValue(int64_t{18}), PropertyValue(int64_t{65}), false, false},
			RangePredicate{"age", PropertyValue(int64_t{21}), PropertyValue(int64_t{60}), true, true},
	});
	LogicalAggregate aggregate(makeOneHopFrom(std::move(seed)), {}, makeAggs());

	auto plan = tryBuildRelationshipCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	ASSERT_EQ(plan->seedPredicates.size(), 3U);
	EXPECT_EQ(plan->seedPredicates[0].op, execution::VectorPredicateOp::VPO_GT);
	EXPECT_EQ(plan->seedPredicates[1].op, execution::VectorPredicateOp::VPO_LT);
	EXPECT_EQ(plan->seedPredicates[2].op, execution::VectorPredicateOp::VPO_RANGE_CLOSED);
	EXPECT_EQ(std::count(plan->seedRequirements.requiredProperties.begin(),
	                     plan->seedRequirements.requiredProperties.end(),
	                     "age"), 1);
}

TEST(RelationshipCountFastPathPlannerTest, RejectsSeedRangeWithoutUsableBounds) {
	auto seed = makeSeedScan();
	seed->setRangePredicates({RangePredicate{"age", PropertyValue{}, PropertyValue{}, true, true}});
	LogicalAggregate aggregate(makeOneHopFrom(std::move(seed)), {}, makeAggs());

	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(aggregate).has_value());
}

TEST(RelationshipCountFastPathPlannerTest, UsesDefaultAliasWhenAggregateAliasIsEmpty) {
	std::vector<LogicalAggItem> aggs;
	aggs.emplace_back("count", std::make_shared<expressions::VariableReferenceExpression>("v"), "");
	LogicalAggregate aggregate(makeOneHop(), {}, std::move(aggs));

	auto plan = tryBuildRelationshipCountFastPathPlan(aggregate);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->outputAlias, "count");
}

TEST(RelationshipCountFastPathPlannerTest, UsesPreferredRangeAndCompositeCandidateSources) {
	auto rangeSeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	rangeSeed->setPreferredScanType(execution::ScanType::RANGE_SCAN);
	rangeSeed->setRangePredicates({RangePredicate{"age", PropertyValue(int64_t{18}), PropertyValue(int64_t{65}), true, false}});
	LogicalAggregate rangeAggregate(makeOneHopFrom(std::move(rangeSeed)), {}, makeAggs());
	auto rangePlan = tryBuildRelationshipCountFastPathPlan(rangeAggregate);
	ASSERT_TRUE(rangePlan.has_value());
	EXPECT_EQ(rangePlan->seedConfig.type, execution::ScanType::RANGE_SCAN);
	EXPECT_TRUE(rangePlan->seedPredicates.empty());

	auto compositeSeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	compositeSeed->setPreferredScanType(execution::ScanType::COMPOSITE_SCAN);
	compositeSeed->setCompositeEquality({{"country", "age"}, {PropertyValue("CN"), PropertyValue(int64_t{30})}});
	LogicalAggregate compositeAggregate(makeOneHopFrom(std::move(compositeSeed)), {}, makeAggs());
	auto compositePlan = tryBuildRelationshipCountFastPathPlan(compositeAggregate);
	ASSERT_TRUE(compositePlan.has_value());
	EXPECT_EQ(compositePlan->seedConfig.type, execution::ScanType::COMPOSITE_SCAN);
	EXPECT_TRUE(compositePlan->seedPredicates.empty());
}

TEST(RelationshipCountFastPathPlannerTest, AddsPredicateForPreferredOpenRangeCandidateSource) {
	auto minSeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	minSeed->setPreferredScanType(execution::ScanType::RANGE_SCAN);
	minSeed->setRangePredicates({RangePredicate{"age", PropertyValue(int64_t{18}), PropertyValue{}, true, true}});
	LogicalAggregate minAggregate(makeOneHopFrom(std::move(minSeed)), {}, makeAggs());
	auto minPlan = tryBuildRelationshipCountFastPathPlan(minAggregate);
	ASSERT_TRUE(minPlan.has_value());
	EXPECT_EQ(minPlan->seedConfig.type, execution::ScanType::RANGE_SCAN);
	ASSERT_EQ(minPlan->seedPredicates.size(), 1U);
	EXPECT_EQ(minPlan->seedPredicates[0].op, execution::VectorPredicateOp::VPO_GE);
	EXPECT_EQ(minPlan->seedRequirements.materialization, execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES);

	auto maxSeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	maxSeed->setPreferredScanType(execution::ScanType::RANGE_SCAN);
	maxSeed->setRangePredicates({RangePredicate{"age", PropertyValue{}, PropertyValue(int64_t{65}), true, false}});
	LogicalAggregate maxAggregate(makeOneHopFrom(std::move(maxSeed)), {}, makeAggs());
	auto maxPlan = tryBuildRelationshipCountFastPathPlan(maxAggregate);
	ASSERT_TRUE(maxPlan.has_value());
	ASSERT_EQ(maxPlan->seedPredicates.size(), 1U);
	EXPECT_EQ(maxPlan->seedPredicates[0].op, execution::VectorPredicateOp::VPO_LT);
}

TEST(RelationshipCountFastPathPlannerTest, RejectsInvalidCompositeSeedCandidate) {
	auto seed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	seed->setPreferredScanType(execution::ScanType::COMPOSITE_SCAN);
	seed->setCompositeEquality({{"country", "age"}, {PropertyValue("CN")}});
	LogicalAggregate aggregate(makeOneHopFrom(std::move(seed)), {}, makeAggs());

	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(aggregate).has_value());
}

TEST(RelationshipCountFastPathPlannerTest, AllowsSeedAndSourceCountVariablesButRejectsInvalidCountArguments) {
	LogicalAggregate seedCount(makeOneHop(), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("u")));
	EXPECT_TRUE(tryBuildRelationshipCountFastPathPlan(seedCount).has_value());

	auto shiftedSourceHop = std::make_unique<LogicalTraversal>(makeSeedScan(), "sourceAlias", "r", "v", "FOLLOWS", "out",
	                                                           std::vector<std::string>{"User"});
	LogicalAggregate sourceCount(std::move(shiftedSourceHop), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("sourceAlias")));
	EXPECT_TRUE(tryBuildRelationshipCountFastPathPlan(sourceCount).has_value());

	LogicalAggregate propertyCount(makeOneHop(), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("v", "id")));
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(propertyCount).has_value());

	LogicalAggregate unknownCount(makeOneHop(), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("missing")));
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(unknownCount).has_value());

	LogicalAggregate literalCount(makeOneHop(), {}, makeAggs(std::make_shared<expressions::LiteralExpression>(int64_t{1})));
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(literalCount).has_value());
}

TEST(RelationshipCountFastPathPlannerTest, RejectsDirectCountShapeWhenOnlyTraversalPathCanHandleIt) {
	auto inwardSeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{});
	auto inwardHop = std::make_unique<LogicalTraversal>(std::move(inwardSeed), "u", "r", "v", "FOLLOWS", "in");
	LogicalAggregate inwardAggregate(std::move(inwardHop), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("r")));
	auto inwardPlan = tryBuildRelationshipCountFastPathPlan(inwardAggregate);
	ASSERT_TRUE(inwardPlan.has_value());
	EXPECT_FALSE(inwardPlan->directCount.enabled);

	auto labeledTargetSeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{});
	auto labeledTargetHop = std::make_unique<LogicalTraversal>(
			std::move(labeledTargetSeed), "u", "r", "v", "FOLLOWS", "out", std::vector<std::string>{"User"});
	LogicalAggregate labeledTargetAggregate(std::move(labeledTargetHop), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("r")));
	auto labeledTargetPlan = tryBuildRelationshipCountFastPathPlan(labeledTargetAggregate);
	ASSERT_TRUE(labeledTargetPlan.has_value());
	EXPECT_FALSE(labeledTargetPlan->directCount.enabled);

	auto targetPropertySeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{});
	auto targetPropertyHop = std::make_unique<LogicalTraversal>(
			std::move(targetPropertySeed), "u", "r", "v", "FOLLOWS", "out", std::vector<std::string>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue("v1")}});
	LogicalAggregate targetPropertyAggregate(std::move(targetPropertyHop), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("r")));
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(targetPropertyAggregate).has_value());
}

TEST(RelationshipCountFastPathPlannerTest, RejectsSelectiveSeedWhenPreferredSourceIsNotIndexBacked) {
	auto unanchoredPropertySeed = std::make_unique<LogicalNodeScan>(
			"u",
			std::vector<std::string>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue("u1")}});
	auto unanchoredPropertyHop = std::make_unique<LogicalTraversal>(std::move(unanchoredPropertySeed), "u", "r", "v", "FOLLOWS", "out");
	LogicalAggregate unanchoredPropertyAggregate(std::move(unanchoredPropertyHop), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("r")));
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(unanchoredPropertyAggregate).has_value());

	auto labelSeed = makeUnindexedSeedScan();
	labelSeed->setPreferredScanType(execution::ScanType::LABEL_SCAN);
	LogicalAggregate labelAggregate(makeOneHopFrom(std::move(labelSeed)), {}, makeAggs());
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(labelAggregate).has_value());

	auto compositeSeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	compositeSeed->setCompositeEquality({{"country", "age"}, {PropertyValue("CN"), PropertyValue(int64_t{30})}});
	LogicalAggregate compositeAggregate(makeOneHopFrom(std::move(compositeSeed)), {}, makeAggs());
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(compositeAggregate).has_value());
}

TEST(RelationshipCountFastPathPlannerTest, HandlesPreferredCandidateSourcesWithoutPredicates) {
	auto propertySeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	propertySeed->setPreferredScanType(execution::ScanType::PROPERTY_SCAN);
	LogicalAggregate propertyAggregate(makeOneHopFrom(std::move(propertySeed)), {}, makeAggs());
	auto propertyPlan = tryBuildRelationshipCountFastPathPlan(propertyAggregate);
	ASSERT_TRUE(propertyPlan.has_value());
	EXPECT_EQ(propertyPlan->seedConfig.type, execution::ScanType::PROPERTY_SCAN);
	EXPECT_TRUE(propertyPlan->seedConfig.indexKey.empty());

	auto rangeSeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	rangeSeed->setPreferredScanType(execution::ScanType::RANGE_SCAN);
	LogicalAggregate rangeAggregate(makeOneHopFrom(std::move(rangeSeed)), {}, makeAggs());
	auto rangePlan = tryBuildRelationshipCountFastPathPlan(rangeAggregate);
	ASSERT_TRUE(rangePlan.has_value());
	EXPECT_EQ(rangePlan->seedConfig.type, execution::ScanType::RANGE_SCAN);
	EXPECT_TRUE(rangePlan->seedConfig.indexKey.empty());

	auto compositeSeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	compositeSeed->setPreferredScanType(execution::ScanType::COMPOSITE_SCAN);
	LogicalAggregate compositeAggregate(makeOneHopFrom(std::move(compositeSeed)), {}, makeAggs());
	auto compositePlan = tryBuildRelationshipCountFastPathPlan(compositeAggregate);
	ASSERT_TRUE(compositePlan.has_value());
	EXPECT_EQ(compositePlan->seedConfig.type, execution::ScanType::COMPOSITE_SCAN);
	EXPECT_TRUE(compositePlan->seedConfig.compositeKeys.empty());
}

TEST(RelationshipCountFastPathPlannerTest, RejectsMalformedTraversalShapes) {
	LogicalAggregate missingChild(nullptr, {}, makeAggs());
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(missingChild).has_value());

	auto predicate = std::make_shared<expressions::LiteralExpression>(true);
	auto filter = std::make_unique<LogicalFilter>(nullptr, predicate);
	LogicalAggregate missingFilterChild(std::move(filter), {}, makeAggs());
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(missingFilterChild).has_value());

	auto traversalWithMissingSeed = std::make_unique<LogicalTraversal>(nullptr, "u", "r", "v", "FOLLOWS", "out");
	LogicalAggregate missingTraversalChild(std::move(traversalWithMissingSeed), {}, makeAggs());
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(missingTraversalChild).has_value());

	auto firstHop = makeOneHop();
	auto secondHop = std::make_unique<LogicalTraversal>(std::move(firstHop), "v", "r2", "w", "FOLLOWS", "out");
	auto thirdHop = std::make_unique<LogicalTraversal>(std::move(secondHop), "w", "r3", "x", "FOLLOWS", "out");
	LogicalAggregate threeHopAggregate(std::move(thirdHop), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("x")));
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(threeHopAggregate).has_value());
}

TEST(RelationshipCountFastPathPlannerTest, ChoosesIndexedRangeCompositeAndLabelCandidateSources) {
	const auto dbPath = fs::temp_directory_path() /
	                    ("test_relationship_count_planner_indexes_" + boost::uuids::to_string(boost::uuids::random_generator()()) + ".zyx");
	Database db(dbPath.string());
	db.open();
	auto indexManager = db.getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("idx_user_age", "node", "User", "age"));
	ASSERT_TRUE(indexManager->createCompositeIndex("idx_user_country_age", "node", "User", {"country", "age"}));
	ASSERT_TRUE(indexManager->createIndex("idx_user_label", "node", "User", ""));

	auto rangeSeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	rangeSeed->setRangePredicates({RangePredicate{"age", PropertyValue(int64_t{18}), PropertyValue(int64_t{65}), true, false}});
	LogicalAggregate rangeAggregate(makeOneHopFrom(std::move(rangeSeed)), {}, makeAggs());
	auto rangePlan = tryBuildRelationshipCountFastPathPlan(rangeAggregate, indexManager);
	ASSERT_TRUE(rangePlan.has_value());
	EXPECT_EQ(rangePlan->seedConfig.type, execution::ScanType::RANGE_SCAN);
	EXPECT_TRUE(rangePlan->seedPredicates.empty());

	auto compositeSeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	compositeSeed->setCompositeEquality({{"country", "age"}, {PropertyValue("CN"), PropertyValue(int64_t{30})}});
	LogicalAggregate compositeAggregate(makeOneHopFrom(std::move(compositeSeed)), {}, makeAggs());
	auto compositePlan = tryBuildRelationshipCountFastPathPlan(compositeAggregate, indexManager);
	ASSERT_TRUE(compositePlan.has_value());
	EXPECT_EQ(compositePlan->seedConfig.type, execution::ScanType::COMPOSITE_SCAN);

	LogicalAggregate labelAggregate(makeOneHopFrom(std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"})), {}, makeAggs());
	auto labelPlan = tryBuildRelationshipCountFastPathPlan(labelAggregate, indexManager);
	ASSERT_TRUE(labelPlan.has_value());
	EXPECT_EQ(labelPlan->seedConfig.type, execution::ScanType::LABEL_SCAN);

	db.close();
	std::error_code ec;
	fs::remove_all(dbPath, ec);
}

TEST(RelationshipCountFastPathPlannerTest, RejectsSelectiveSeedWhenIndexManagerHasNoMatchingIndex) {
	const auto dbPath = fs::temp_directory_path() /
	                    ("test_relationship_count_planner_no_indexes_" + boost::uuids::to_string(boost::uuids::random_generator()()) + ".zyx");
	Database db(dbPath.string());
	db.open();
	auto indexManager = db.getQueryEngine()->getIndexManager();

	auto rangeSeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{});
	rangeSeed->setRangePredicates({RangePredicate{"age", PropertyValue(int64_t{18}), PropertyValue(int64_t{65}), true, true}});
	LogicalAggregate rangeAggregate(makeOneHopFrom(std::move(rangeSeed)), {}, makeAggs());
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(rangeAggregate, indexManager).has_value());

	auto labelSeed = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	LogicalAggregate labelAggregate(makeOneHopFrom(std::move(labelSeed)), {}, makeAggs());
	auto labelPlan = tryBuildRelationshipCountFastPathPlan(labelAggregate, indexManager);
	ASSERT_TRUE(labelPlan.has_value());
	EXPECT_EQ(labelPlan->seedConfig.type, execution::ScanType::FULL_SCAN);

	db.close();
	std::error_code ec;
	fs::remove_all(dbPath, ec);
}

TEST(RelationshipCountFastPathPlannerTest, RejectsDistinctGroupedAndPropertyFilters) {
	LogicalAggregate distinct(makeOneHop(), {}, makeAggs(std::make_shared<expressions::VariableReferenceExpression>("v"), true));
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(distinct).has_value());

	std::vector<std::shared_ptr<expressions::Expression>> groups;
	groups.push_back(std::make_shared<expressions::VariableReferenceExpression>("v"));
	LogicalAggregate grouped(makeOneHop(), std::move(groups), makeAggs());
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(grouped).has_value());

	auto edgePropsHop = std::make_unique<LogicalTraversal>(makeSeedScan(), "u", "r", "v", "FOLLOWS", "out", std::vector<std::string>{"User"},
	                                                    std::vector<std::pair<std::string, PropertyValue>>{},
	                                                    std::unordered_map<std::string, PropertyValue>{{"weight", PropertyValue(int64_t{1})}});
	LogicalAggregate edgeProps(std::move(edgePropsHop), {}, makeAggs());
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(edgeProps).has_value());

	auto targetPropsHop = std::make_unique<LogicalTraversal>(makeSeedScan(), "u", "r", "v", "FOLLOWS", "out", std::vector<std::string>{"User"},
	                                                      std::vector<std::pair<std::string, PropertyValue>>{{"country", PropertyValue("CN")}});
	LogicalAggregate targetProps(std::move(targetPropsHop), {}, makeAggs());
	EXPECT_FALSE(tryBuildRelationshipCountFastPathPlan(targetProps).has_value());
}

TEST(RelationshipCountFastPathPlannerTest, PhysicalPlanConverterUsesFastPathForRecognizedTraversalAggregate) {
	LogicalAggregate aggregate(makeOneHop(), {}, makeAggs());
	PhysicalPlanConverter converter(std::shared_ptr<storage::DataManager>{}, std::shared_ptr<indexes::IndexManager>{});

	auto physical = converter.convert(&aggregate);

	ASSERT_NE(physical, nullptr);
	EXPECT_EQ(physical->toString(), "RelationshipCountFastPath(count)");
}
