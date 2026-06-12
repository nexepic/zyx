#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/planner/PhysicalScanLoweringPlanner.hpp"

using namespace graph::query;
using namespace graph::query::logical;
using namespace graph::query::planner;

namespace {

std::shared_ptr<expressions::Expression> property(std::string variable, std::string key) {
	return std::make_shared<expressions::VariableReferenceExpression>(std::move(variable), std::move(key));
}

std::shared_ptr<expressions::Expression> variable(std::string name) {
	return std::make_shared<expressions::VariableReferenceExpression>(std::move(name));
}

} // namespace

TEST(PhysicalScanLoweringPlannerTest, LowersProjectSortLimitNodeScanToTopKScan) {
	auto scan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(property("u", "score"), false);
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));
	auto limit = std::make_unique<LogicalLimit>(std::move(sort), 10);
	std::vector<LogicalProjectItem> projectItems;
	projectItems.emplace_back(property("u", "id"), "id");
	LogicalProject project(std::move(limit), std::move(projectItems));

	auto lowering = tryLowerProjectToScan(project, nullptr);
	auto candidates = collectProjectScanCandidates(project, nullptr);

	ASSERT_TRUE(lowering.has_value());
	ASSERT_EQ(candidates.size(), 1);
	EXPECT_EQ(lowering->kind, PhysicalScanLoweringKind::PSLK_NODE_TOPK_SCAN);
	EXPECT_EQ(lowering->shape, ScanSpecializationShape::SSS_PROJECT);
	EXPECT_EQ(lowering->ruleName, "node_topk_scan");
	EXPECT_EQ(lowering->reason, "project_sort_limit_node_scan");
	EXPECT_GE(lowering->estimatedCost, 0.0);
	const auto &plan = std::get<NodeTopKScanPlan>(lowering->plan);
	EXPECT_EQ(plan.sortProperty, "score");
	EXPECT_EQ(plan.limit, 10);
}

TEST(PhysicalScanLoweringPlannerTest, LowersAggregateToFirstSupportedScanShape) {
	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<LogicalAggItem> aggregations;
	aggregations.emplace_back("count", variable("n"), "rows", false);
	LogicalAggregate aggregate(std::move(scan), {}, std::move(aggregations));

	auto lowering = tryLowerAggregateToScan(aggregate, nullptr);
	auto candidates = collectAggregateScanCandidates(aggregate, nullptr);

	ASSERT_TRUE(lowering.has_value());
	ASSERT_EQ(candidates.size(), 1);
	EXPECT_EQ(lowering->kind, PhysicalScanLoweringKind::PSLK_NODE_COUNT_SCAN);
	EXPECT_EQ(lowering->shape, ScanSpecializationShape::SSS_AGGREGATE);
	EXPECT_EQ(lowering->ruleName, "node_count_scan");
	EXPECT_EQ(lowering->reason, "aggregate_count_node_scan");
	const auto &plan = std::get<NodeCountScanPlan>(lowering->plan);
	EXPECT_EQ(plan.outputAlias, "rows");
}

TEST(PhysicalScanLoweringPlannerTest, ChoosesLowestCostCandidateWithStableTieBreak) {
	NodeCountScanPlan highCostPlan;
	highCostPlan.accessPath.estimatedCost = 10.0;
	NodeCountScanPlan lowCostPlan;
	lowCostPlan.accessPath.estimatedCost = 3.0;

	ScanPlanCandidate highCost;
	highCost.shape = ScanSpecializationShape::SSS_AGGREGATE;
	highCost.kind = PhysicalScanLoweringKind::PSLK_NODE_COUNT_SCAN;
	highCost.plan = highCostPlan;
	highCost.estimatedCost = highCostPlan.accessPath.estimatedCost;
	highCost.ruleName = "z_high";
	ScanPlanCandidate lowCost = highCost;
	lowCost.plan = lowCostPlan;
	lowCost.estimatedCost = lowCostPlan.accessPath.estimatedCost;
	lowCost.ruleName = "m_low";

	auto chosen = chooseBestScanCandidate({highCost, lowCost});

	ASSERT_TRUE(chosen.has_value());
	EXPECT_EQ(chosen->ruleName, "m_low");

	highCost.estimatedCost = lowCost.estimatedCost;
	highCost.ruleName = "a_tie";
	chosen = chooseBestScanCandidate({highCost, lowCost});

	ASSERT_TRUE(chosen.has_value());
	EXPECT_EQ(chosen->ruleName, "a_tie");
}
