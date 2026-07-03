#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/planner/PhysicalScanLoweringPlanner.hpp"

using namespace graph::query;
using namespace graph::query::logical;
using namespace graph::query::planner;
using graph::PropertyValue;

namespace {

std::shared_ptr<expressions::Expression> property(std::string variable, std::string key) {
	return std::make_shared<expressions::VariableReferenceExpression>(std::move(variable), std::move(key));
}

std::shared_ptr<expressions::Expression> variable(std::string name) {
	return std::make_shared<expressions::VariableReferenceExpression>(std::move(name));
}

class ProjectWithExtraChildren final : public LogicalProject {
public:
	ProjectWithExtraChildren(std::vector<LogicalProjectItem> items) :
		LogicalProject(nullptr, std::move(items), false) {}

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {nullptr, nullptr}; }
};

class LimitWithExtraChildren final : public LogicalLimit {
public:
	explicit LimitWithExtraChildren(int64_t limit) : LogicalLimit(nullptr, limit) {}

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {nullptr, nullptr}; }
};

class FilterWithExtraChildren final : public LogicalFilter {
public:
	explicit FilterWithExtraChildren(std::shared_ptr<expressions::Expression> predicate) :
		LogicalFilter(nullptr, std::move(predicate)) {}

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {nullptr, nullptr}; }
};

class TraversalWithExtraChildren final : public LogicalTraversal {
public:
	TraversalWithExtraChildren() : LogicalTraversal(nullptr, "", "r", "v", "FOLLOWS", "out") {}

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {nullptr, nullptr}; }
};

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

TEST(PhysicalScanLoweringPlannerTest, LowersProjectLimitNodeScanToProjectionScan) {
	auto scan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	auto limit = std::make_unique<LogicalLimit>(std::move(scan), 100);
	std::vector<LogicalProjectItem> projectItems;
	projectItems.emplace_back(property("u", "id"), "id");
	projectItems.emplace_back(property("u", "score"), "score");
	LogicalProject project(std::move(limit), std::move(projectItems));

	auto lowering = tryLowerProjectToScan(project, nullptr);
	auto candidates = collectProjectScanCandidates(project, nullptr);

	ASSERT_TRUE(lowering.has_value());
	ASSERT_EQ(candidates.size(), 1);
	EXPECT_EQ(lowering->kind, PhysicalScanLoweringKind::PSLK_NODE_PROJECTION_SCAN);
	EXPECT_EQ(lowering->ruleName, "node_projection_scan");
	const auto &plan = std::get<NodeProjectionScanPlan>(lowering->plan);
	ASSERT_TRUE(plan.limit.has_value());
	EXPECT_EQ(*plan.limit, 100U);
	ASSERT_EQ(plan.projections.size(), 2U);
	EXPECT_EQ(plan.projections[0].property, "id");
	EXPECT_EQ(plan.projections[1].property, "score");
}

TEST(PhysicalScanLoweringPlannerTest, NodeProjectionPlannerRejectsUnsupportedShapesAndKeepsSafeFallbacks) {
	std::vector<LogicalProjectItem> items;
	items.emplace_back(property("u", "id"), "id");

	auto validScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	LogicalProject validProject(std::move(validScan), items);
	auto directPlan = tryBuildNodeProjectionScanPlan(validProject);
	ASSERT_TRUE(directPlan.has_value());
	EXPECT_FALSE(directPlan->limit.has_value());

	LogicalProject distinctProject(std::make_unique<LogicalNodeScan>("u"), items, true);
	EXPECT_FALSE(tryBuildNodeProjectionScanPlan(distinctProject, nullptr).has_value());

	LogicalProject emptyProject(std::make_unique<LogicalNodeScan>("u"), {});
	EXPECT_FALSE(tryBuildNodeProjectionScanPlan(emptyProject, nullptr).has_value());

	LogicalProject nullChildProject(nullptr, items);
	EXPECT_FALSE(tryBuildNodeProjectionScanPlan(nullChildProject, nullptr).has_value());

	ProjectWithExtraChildren extraProjectChildren(items);
	EXPECT_FALSE(tryBuildNodeProjectionScanPlan(extraProjectChildren, nullptr).has_value());

	auto nullLimit = std::make_unique<LogicalLimit>(nullptr, 1);
	LogicalProject nullLimitedProject(std::move(nullLimit), items);
	EXPECT_FALSE(tryBuildNodeProjectionScanPlan(nullLimitedProject, nullptr).has_value());

	auto extraLimitChildren = std::make_unique<LimitWithExtraChildren>(1);
	LogicalProject malformedLimitProject(std::move(extraLimitChildren), items);
	EXPECT_FALSE(tryBuildNodeProjectionScanPlan(malformedLimitProject, nullptr).has_value());

	std::vector<LogicalProjectItem> badProjectionItems;
	badProjectionItems.emplace_back(variable("u"), "u");
	LogicalProject badProjection(std::make_unique<LogicalNodeScan>("u"), std::move(badProjectionItems));
	EXPECT_FALSE(tryBuildNodeProjectionScanPlan(badProjection, nullptr).has_value());

	auto negativeLimit = std::make_unique<LogicalLimit>(std::make_unique<LogicalNodeScan>("u"), -1);
	LogicalProject negativeLimitProject(std::move(negativeLimit), items);
	auto negativeLimitPlan = tryBuildNodeProjectionScanPlan(negativeLimitProject, nullptr);
	ASSERT_TRUE(negativeLimitPlan.has_value());
	ASSERT_TRUE(negativeLimitPlan->limit.has_value());
	EXPECT_EQ(*negativeLimitPlan->limit, 0U);

	auto preferredIndexScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	preferredIndexScan->setPreferredScanType(execution::ScanType::PROPERTY_SCAN);
	LogicalProject preferredIndexProject(std::move(preferredIndexScan), items);
	auto fallbackPlan = tryBuildNodeProjectionScanPlan(preferredIndexProject, nullptr);
	ASSERT_TRUE(fallbackPlan.has_value());
	EXPECT_EQ(fallbackPlan->config.type, execution::ScanType::LABEL_SCAN);

	auto residualScan = std::make_unique<LogicalNodeScan>(
		"u", std::vector<std::string>{"User"},
		std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue("u1")}});
	residualScan->setPreferredScanType(execution::ScanType::PROPERTY_SCAN);
	LogicalProject residualProject(std::move(residualScan), items);
	auto residualPlan = tryBuildNodeProjectionScanPlan(residualProject, nullptr);
	ASSERT_TRUE(residualPlan.has_value());
	EXPECT_EQ(residualPlan->config.type, execution::ScanType::LABEL_SCAN);
	ASSERT_EQ(residualPlan->predicates.size(), 1U);
	EXPECT_EQ(residualPlan->predicates[0].propertyKey, "id");

	auto malformedComposite = std::make_unique<LogicalNodeScan>("u");
	malformedComposite->setCompositeEquality({{"country", "age"}, {PropertyValue("CN")}});
	LogicalProject malformedCompositeProject(std::move(malformedComposite), items);
	EXPECT_FALSE(tryBuildNodeProjectionScanPlan(malformedCompositeProject, nullptr).has_value());
}

TEST(PhysicalScanLoweringPlannerTest, LowersRelationshipProjectionWithLimitAndPredicate) {
	auto scan = std::make_unique<LogicalNodeScan>("");
	auto traversal = std::make_unique<LogicalTraversal>(
		std::move(scan), "", "r", "v", "FOLLOWS", "out",
		std::vector<std::string>{"User"});
	auto predicate = std::make_shared<expressions::BinaryOpExpression>(
		std::make_unique<expressions::VariableReferenceExpression>("r", "weight"),
		expressions::BinaryOperatorType::BOP_EQUAL,
		std::make_unique<expressions::LiteralExpression>(int64_t{1}));
	auto filter = std::make_unique<LogicalFilter>(std::move(traversal), predicate);
	auto limit = std::make_unique<LogicalLimit>(std::move(filter), 100);
	std::vector<LogicalProjectItem> projectItems;
	projectItems.emplace_back(property("r", "weight"), "weight");
	projectItems.emplace_back(property("v", "id"), "id");
	LogicalProject project(std::move(limit), std::move(projectItems));

	auto lowering = tryLowerProjectToScan(project, nullptr);
	auto candidates = collectProjectScanCandidates(project, nullptr);

	ASSERT_TRUE(lowering.has_value());
	ASSERT_EQ(candidates.size(), 1);
	EXPECT_EQ(lowering->kind, PhysicalScanLoweringKind::PSLK_RELATIONSHIP_PROJECTION_SCAN);
	EXPECT_EQ(lowering->ruleName, "relationship_projection_scan");
	const auto &plan = std::get<RelationshipProjectionScanPlan>(lowering->plan);
	ASSERT_TRUE(plan.limit.has_value());
	EXPECT_EQ(*plan.limit, 100U);
	EXPECT_EQ(plan.config.edgeType, "FOLLOWS");
	ASSERT_EQ(plan.config.edgePredicates.size(), 1U);
	EXPECT_EQ(plan.config.edgePredicates[0].propertyKey, "weight");
	ASSERT_EQ(plan.projections.size(), 2U);
	EXPECT_EQ(plan.projections[0].source, execution::operators::RelationshipProjectionSource::RPS_EDGE);
	EXPECT_EQ(plan.projections[1].source, execution::operators::RelationshipProjectionSource::RPS_TARGET_NODE);
}

TEST(PhysicalScanLoweringPlannerTest, RelationshipProjectionPlannerRejectsUnsupportedShapes) {
	std::vector<LogicalProjectItem> items;
	items.emplace_back(property("r", "weight"), "weight");

	auto validTraversal = std::make_unique<LogicalTraversal>(
		std::make_unique<LogicalNodeScan>(""), "", "r", "v", "FOLLOWS", "out");
	LogicalProject validProject(std::move(validTraversal), items);
	auto directPlan = tryBuildRelationshipProjectionScanPlan(validProject);
	ASSERT_TRUE(directPlan.has_value());
	EXPECT_FALSE(directPlan->limit.has_value());

	auto negativeLimitTraversal = std::make_unique<LogicalTraversal>(
		std::make_unique<LogicalNodeScan>(""), "", "r", "v", "FOLLOWS", "out");
	auto negativeLimit = std::make_unique<LogicalLimit>(std::move(negativeLimitTraversal), -1);
	LogicalProject negativeLimitProject(std::move(negativeLimit), items);
	auto negativeLimitPlan = tryBuildRelationshipProjectionScanPlan(negativeLimitProject, nullptr);
	ASSERT_TRUE(negativeLimitPlan.has_value());
	ASSERT_TRUE(negativeLimitPlan->limit.has_value());
	EXPECT_EQ(*negativeLimitPlan->limit, 0U);

	auto anonymousSeedTraversal = std::make_unique<LogicalTraversal>(
		std::make_unique<LogicalNodeScan>("__anon_seed"), "", "r", "v", "FOLLOWS", "out");
	LogicalProject anonymousSeedProject(std::move(anonymousSeedTraversal), items);
	EXPECT_TRUE(tryBuildRelationshipProjectionScanPlan(anonymousSeedProject, nullptr).has_value());

	LogicalProject distinctProject(
		std::make_unique<LogicalTraversal>(std::make_unique<LogicalNodeScan>(""), "", "r", "v", "FOLLOWS", "out"),
		items,
		true);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(distinctProject, nullptr).has_value());

	LogicalProject emptyProject(
		std::make_unique<LogicalTraversal>(std::make_unique<LogicalNodeScan>(""), "", "r", "v", "FOLLOWS", "out"),
		{});
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(emptyProject, nullptr).has_value());

	LogicalProject nullChildProject(nullptr, items);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(nullChildProject, nullptr).has_value());

	ProjectWithExtraChildren extraProjectChildren(items);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(extraProjectChildren, nullptr).has_value());

	LogicalProject nonTraversalProject(std::make_unique<LogicalSingleRow>(), items);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(nonTraversalProject, nullptr).has_value());

	auto nullLimit = std::make_unique<LogicalLimit>(nullptr, 1);
	LogicalProject nullLimitedProject(std::move(nullLimit), items);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(nullLimitedProject, nullptr).has_value());

	auto extraLimitChildren = std::make_unique<LimitWithExtraChildren>(1);
	LogicalProject extraLimitProject(std::move(extraLimitChildren), items);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(extraLimitProject, nullptr).has_value());

	auto extraFilterChildren = std::make_unique<FilterWithExtraChildren>(
		std::make_shared<expressions::LiteralExpression>(true));
	LogicalProject extraFilterProject(std::move(extraFilterChildren), items);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(extraFilterProject, nullptr).has_value());

	auto nullTraversalChild = std::make_unique<LogicalTraversal>(nullptr, "", "r", "v", "FOLLOWS", "out");
	LogicalProject nullTraversalChildProject(std::move(nullTraversalChild), items);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(nullTraversalChildProject, nullptr).has_value());

	auto extraTraversalChildren = std::make_unique<TraversalWithExtraChildren>();
	LogicalProject extraTraversalChildProject(std::move(extraTraversalChildren), items);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(extraTraversalChildProject, nullptr).has_value());

	auto inwardTraversal = std::make_unique<LogicalTraversal>(
		std::make_unique<LogicalNodeScan>(""), "", "r", "v", "FOLLOWS", "in");
	LogicalProject inwardProject(std::move(inwardTraversal), items);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(inwardProject, nullptr).has_value());

	auto targetPropertyTraversal = std::make_unique<LogicalTraversal>(
		std::make_unique<LogicalNodeScan>(""), "", "r", "v", "FOLLOWS", "out",
		std::vector<std::string>{},
		std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue("v1")}});
	LogicalProject targetPropertyProject(std::move(targetPropertyTraversal), items);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(targetPropertyProject, nullptr).has_value());

	auto anchoredTraversal = std::make_unique<LogicalTraversal>(
		std::make_unique<LogicalNodeScan>("u"), "u", "r", "v", "FOLLOWS", "out");
	LogicalProject anchoredProject(std::move(anchoredTraversal), items);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(anchoredProject, nullptr).has_value());

	auto propertySeedTraversal = std::make_unique<LogicalTraversal>(
		std::make_unique<LogicalNodeScan>(
			"", std::vector<std::string>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue("u1")}}),
		"", "r", "v", "FOLLOWS", "out");
	LogicalProject propertySeedProject(std::move(propertySeedTraversal), items);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(propertySeedProject, nullptr).has_value());

	auto rangeSeed = std::make_unique<LogicalNodeScan>("");
	RangePredicate range;
	range.key = "age";
	range.minValue = PropertyValue(int64_t{18});
	rangeSeed->setRangePredicates({range});
	auto rangeSeedTraversal = std::make_unique<LogicalTraversal>(
		std::move(rangeSeed), "", "r", "v", "FOLLOWS", "out");
	LogicalProject rangeSeedProject(std::move(rangeSeedTraversal), items);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(rangeSeedProject, nullptr).has_value());

	auto compositeSeed = std::make_unique<LogicalNodeScan>("");
	compositeSeed->setCompositeEquality({{"country", "age"}, {PropertyValue("CN"), PropertyValue(int64_t{30})}});
	auto compositeSeedTraversal = std::make_unique<LogicalTraversal>(
		std::move(compositeSeed), "", "r", "v", "FOLLOWS", "out");
	LogicalProject compositeSeedProject(std::move(compositeSeedTraversal), items);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(compositeSeedProject, nullptr).has_value());

	std::vector<LogicalProjectItem> badProjectionItems;
	badProjectionItems.emplace_back(property("x", "weight"), "weight");
	auto badProjectionTraversal = std::make_unique<LogicalTraversal>(
		std::make_unique<LogicalNodeScan>(""), "", "r", "v", "FOLLOWS", "out");
	LogicalProject badProjection(std::move(badProjectionTraversal), std::move(badProjectionItems));
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(badProjection, nullptr).has_value());

	auto unsupportedPredicate = std::make_shared<expressions::BinaryOpExpression>(
		std::make_unique<expressions::VariableReferenceExpression>("v", "id"),
		expressions::BinaryOperatorType::BOP_EQUAL,
		std::make_unique<expressions::LiteralExpression>("v1"));
	auto filteredTraversal = std::make_unique<LogicalTraversal>(
		std::make_unique<LogicalNodeScan>(""), "", "r", "v", "FOLLOWS", "out");
	auto filter = std::make_unique<LogicalFilter>(std::move(filteredTraversal), unsupportedPredicate);
	LogicalProject unsupportedFilterProject(std::move(filter), items);
	EXPECT_FALSE(tryBuildRelationshipProjectionScanPlan(unsupportedFilterProject, nullptr).has_value());
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
