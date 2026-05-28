#include <gtest/gtest.h>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
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

std::vector<LogicalAggItem> makeAggs(std::shared_ptr<expressions::Expression> arg = std::make_shared<expressions::VariableReferenceExpression>("v"),
                                     bool distinct = false) {
	std::vector<LogicalAggItem> aggs;
	aggs.emplace_back("count", std::move(arg), "count", distinct);
	return aggs;
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
