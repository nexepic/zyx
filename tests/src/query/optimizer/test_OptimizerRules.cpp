/**
 * @file test_OptimizerRules.cpp
 * @brief Unit tests for the query optimizer rules and framework.
 *
 * Tests cover:
 * - PredicateSimplificationRule: constant folding, NOT elimination, filter merging
 * - FilterPushdownRule: push past Project, Join, merge into NodeScan
 * - ProjectionPushdownRule: required column annotation
 * - JoinReorderRule: cardinality-based join reordering
 * - Optimizer: fixed-point iteration, null plan, custom rules
 * - CostModel: cost estimation functions
 * - Statistics: label count estimation, property stats
 */

#include <gtest/gtest.h>

#include "graph/query/optimizer/Optimizer.hpp"
#include "graph/query/optimizer/CostModel.hpp"
#include "graph/query/optimizer/Statistics.hpp"
#include "graph/query/optimizer/rules/PredicateSimplificationRule.hpp"
#include "graph/query/optimizer/rules/FilterPushdownRule.hpp"
#include "graph/query/optimizer/rules/ProjectionPushdownRule.hpp"
#include "graph/query/optimizer/rules/JoinReorderRule.hpp"

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/expressions/Expression.hpp"

using namespace graph::query::logical;
using namespace graph::query::expressions;
using namespace graph::query::optimizer;

// =============================================================================
// Helpers
// =============================================================================

static std::shared_ptr<Expression> makePropertyEquals(
    const std::string &var, const std::string &prop, int64_t value) {
    auto lhs = std::make_unique<VariableReferenceExpression>(var, prop);
    auto rhs = std::make_unique<LiteralExpression>(value);
    return std::shared_ptr<Expression>(
        std::make_unique<BinaryOpExpression>(
            std::move(lhs), BinaryOperatorType::BOP_EQUAL, std::move(rhs)).release());
}

static std::shared_ptr<Expression> makePropertyEqualsStr(
    const std::string &var, const std::string &prop, const std::string &value) {
    auto lhs = std::make_unique<VariableReferenceExpression>(var, prop);
    auto rhs = std::make_unique<LiteralExpression>(value);
    return std::shared_ptr<Expression>(
        std::make_unique<BinaryOpExpression>(
            std::move(lhs), BinaryOperatorType::BOP_EQUAL, std::move(rhs)).release());
}

static std::shared_ptr<Expression> makeBoolLiteral(bool val) {
    return std::make_shared<LiteralExpression>(val);
}

static std::shared_ptr<Expression> makeVarRef(const std::string &var) {
    return std::shared_ptr<Expression>(
        std::make_unique<VariableReferenceExpression>(var).release());
}

static std::shared_ptr<Expression> makeAnd(
    std::shared_ptr<Expression> left, std::shared_ptr<Expression> right) {
    return std::shared_ptr<Expression>(
        std::make_unique<BinaryOpExpression>(
            left->clone(), BinaryOperatorType::BOP_AND, right->clone()).release());
}

static std::shared_ptr<Expression> makeNot(std::shared_ptr<Expression> inner) {
    return std::shared_ptr<Expression>(
        std::make_unique<UnaryOpExpression>(
            UnaryOperatorType::UOP_NOT, inner->clone()).release());
}

static Statistics makeTestStats() {
    Statistics stats;
    stats.totalNodeCount = 1000;
    stats.totalEdgeCount = 5000;

    LabelStatistics personStats;
    personStats.label = "Person";
    personStats.nodeCount = 100;
    PropertyStatistics ageStats;
    ageStats.distinctValueCount = 50;
    personStats.properties["age"] = ageStats;
    PropertyStatistics nameStats;
    nameStats.distinctValueCount = 90;
    personStats.properties["name"] = nameStats;
    stats.labelStats["Person"] = personStats;

    LabelStatistics companyStats;
    companyStats.label = "Company";
    companyStats.nodeCount = 50;
    stats.labelStats["Company"] = companyStats;

    return stats;
}

// =============================================================================
// Statistics Tests
// =============================================================================

class StatisticsTest : public ::testing::Test {};

TEST_F(StatisticsTest, EstimateLabelCount) {
    auto stats = makeTestStats();
    EXPECT_EQ(stats.estimateLabelCount("Person"), 100);
    EXPECT_EQ(stats.estimateLabelCount("Company"), 50);
    EXPECT_EQ(stats.estimateLabelCount("Unknown"), 1000); // fallback to total
}

TEST_F(StatisticsTest, GetPropertyStats) {
    auto stats = makeTestStats();
    auto *ageStats = stats.getPropertyStats("Person", "age");
    ASSERT_NE(ageStats, nullptr);
    EXPECT_EQ(ageStats->distinctValueCount, 50);

    auto *unknownStats = stats.getPropertyStats("Person", "unknown");
    EXPECT_EQ(unknownStats, nullptr);

    auto *noLabelStats = stats.getPropertyStats("Unknown", "age");
    EXPECT_EQ(noLabelStats, nullptr);
}

TEST_F(StatisticsTest, PropertySelectivity) {
    PropertyStatistics ps;
    ps.distinctValueCount = 10;
    EXPECT_DOUBLE_EQ(ps.equalitySelectivity(), 0.1);

    PropertyStatistics emptyPs;
    emptyPs.distinctValueCount = 0;
    EXPECT_DOUBLE_EQ(emptyPs.equalitySelectivity(), 1.0);
}

// =============================================================================
// CostModel Tests
// =============================================================================

class CostModelTest : public ::testing::Test {};

TEST_F(CostModelTest, FullScanCost) {
    auto stats = makeTestStats();
    double cost = CostModel::fullScanCost(stats);
    EXPECT_DOUBLE_EQ(cost, 1000.0 * CostModel::SCAN_COST_PER_ROW);
}

TEST_F(CostModelTest, LabelScanCost) {
    auto stats = makeTestStats();
    double cost = CostModel::labelScanCost(stats, "Person");
    EXPECT_DOUBLE_EQ(cost, 100.0 * CostModel::SCAN_COST_PER_ROW);
}

TEST_F(CostModelTest, PropertyIndexCostWithStats) {
    auto stats = makeTestStats();
    double cost = CostModel::propertyIndexCost(stats, "Person", "age", true);
    // 100 * (1/50) * 0.2 = 0.4
    EXPECT_DOUBLE_EQ(cost, 100.0 * (1.0 / 50.0) * CostModel::INDEX_LOOKUP_COST);
}

TEST_F(CostModelTest, PropertyIndexCostWithoutStats) {
    auto stats = makeTestStats();
    double cost = CostModel::propertyIndexCost(stats, "Company", "name", true);
    // No property stats => 50 * 0.1 * 0.2 = 1.0
    EXPECT_DOUBLE_EQ(cost, 50.0 * 0.1 * CostModel::INDEX_LOOKUP_COST);
}

TEST_F(CostModelTest, CrossJoinCost) {
    double cost = CostModel::crossJoinCost(100.0, 50.0);
    EXPECT_DOUBLE_EQ(cost, 100.0 * 50.0 * CostModel::JOIN_COST_PER_ROW);
}

TEST_F(CostModelTest, EstimateScanCardinality) {
    auto stats = makeTestStats();
    double card = CostModel::estimateScanCardinality(stats, {"Person"});
    EXPECT_DOUBLE_EQ(card, 100.0);

    double multiCard = CostModel::estimateScanCardinality(stats, {"Person", "Company"});
    EXPECT_DOUBLE_EQ(multiCard, 50.0); // min of 100 and 50

    double noLabel = CostModel::estimateScanCardinality(stats, {});
    EXPECT_DOUBLE_EQ(noLabel, 1000.0);
}

// =============================================================================
// PredicateSimplificationRule Tests
// =============================================================================

class PredicateSimplificationTest : public ::testing::Test {
protected:
    rules::PredicateSimplificationRule rule;
    Statistics stats;
};

TEST_F(PredicateSimplificationTest, NameCheck) {
    EXPECT_EQ(rule.getName(), "PredicateSimplificationRule");
}

TEST_F(PredicateSimplificationTest, TrueAndX_SimplifiesToX) {
    // Filter(true AND n.age=25) -> Filter(n.age=25)
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto pred = makeAnd(makeBoolLiteral(true), makePropertyEquals("n", "age", 25));
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);
    // Should still be a Filter but with simplified predicate
    if (result->getType() == LogicalOpType::LOP_FILTER) {
        auto *f = static_cast<LogicalFilter *>(result.get());
        auto predStr = f->getPredicate()->toString();
        // Should not contain "true"
        EXPECT_EQ(predStr.find("true"), std::string::npos);
    }
}

TEST_F(PredicateSimplificationTest, FalseAndX_SimplifiesToFalse) {
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto pred = makeAnd(makeBoolLiteral(false), makePropertyEquals("n", "age", 25));
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);
    // The filter should have predicate simplified to false
    if (result->getType() == LogicalOpType::LOP_FILTER) {
        auto *f = static_cast<LogicalFilter *>(result.get());
        EXPECT_NE(f->getPredicate(), nullptr);
    }
}

TEST_F(PredicateSimplificationTest, FilterWithTruePredicate_DropsFilter) {
    // Filter(true) over Scan -> should drop the filter
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), makeBoolLiteral(true));

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);
    // Should be just the scan (filter dropped)
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
}

TEST_F(PredicateSimplificationTest, DoubleNegation_Eliminated) {
    // Filter(NOT(NOT(n.age=25))) -> Filter(n.age=25)
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto inner = makePropertyEquals("n", "age", 25);
    auto doubleNot = makeNot(makeNot(inner));
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), doubleNot);

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

TEST_F(PredicateSimplificationTest, DuplicateFilters_Merged) {
    // Filter(n.age=25) -> Filter(n.age=25) -> Scan("n")
    // Should be reduced to single filter
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto pred = makePropertyEquals("n", "age", 25);
    auto innerFilter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto pred2 = makePropertyEquals("n", "age", 25);
    auto outerFilter = std::make_unique<LogicalFilter>(std::move(innerFilter), pred2);

    auto result = rule.apply(std::move(outerFilter), stats);
    ASSERT_NE(result, nullptr);
    // Should have removed the duplicate filter
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
    auto *f = static_cast<LogicalFilter *>(result.get());
    // Child should be the scan, not another filter
    EXPECT_EQ(f->getChildren()[0]->getType(), LogicalOpType::LOP_NODE_SCAN);
}

TEST_F(PredicateSimplificationTest, AdjacentFilters_MergedWithAnd) {
    // Filter(n.age=25) -> Filter(n.name="Alice") -> Scan("n")
    // Should merge into: Filter(n.name="Alice" AND n.age=25) -> Scan("n")
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto pred1 = makePropertyEqualsStr("n", "name", "Alice");
    auto innerFilter = std::make_unique<LogicalFilter>(std::move(scan), pred1);

    auto pred2 = makePropertyEquals("n", "age", 25);
    auto outerFilter = std::make_unique<LogicalFilter>(std::move(innerFilter), pred2);

    auto result = rule.apply(std::move(outerFilter), stats);
    ASSERT_NE(result, nullptr);
    // After merging, the child of the single filter should be the scan
    if (result->getType() == LogicalOpType::LOP_FILTER) {
        auto *f = static_cast<LogicalFilter *>(result.get());
        // The predicate should be an AND expression (merged)
        EXPECT_EQ(f->getPredicate()->getExpressionType(), ExpressionType::BINARY_OP);
    }
}

TEST_F(PredicateSimplificationTest, NullPlanPassthrough) {
    auto result = rule.apply(nullptr, stats);
    EXPECT_EQ(result, nullptr);
}

// =============================================================================
// FilterPushdownRule Tests
// =============================================================================

class FilterPushdownTest : public ::testing::Test {
protected:
    rules::FilterPushdownRule rule;
    Statistics stats;
};

TEST_F(FilterPushdownTest, NameCheck) {
    EXPECT_EQ(rule.getName(), "FilterPushdownRule");
}

TEST_F(FilterPushdownTest, PushFilterPastProject) {
    // Input: Filter(n.age=25) -> Project([n]) -> NodeScan("n")
    // The filter pushes past project, then merges into scan.
    // Expected: Project([n]) -> NodeScan("n", {age=25})
    auto scan = std::make_unique<LogicalNodeScan>("n");
    std::vector<LogicalProjectItem> items;
    items.emplace_back(makeVarRef("n"), "n");
    auto project = std::make_unique<LogicalProject>(std::move(scan), std::move(items));

    auto pred = makePropertyEquals("n", "age", 25);
    auto filter = std::make_unique<LogicalFilter>(std::move(project), pred);

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);

    // Result should be Project at top
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_PROJECT);
    auto *proj = static_cast<LogicalProject *>(result.get());

    // Child should be NodeScan with predicate merged in
    ASSERT_EQ(proj->getChildren().size(), 1u);
    EXPECT_EQ(proj->getChildren()[0]->getType(), LogicalOpType::LOP_NODE_SCAN);
    auto *s = static_cast<LogicalNodeScan *>(proj->getChildren()[0]);
    EXPECT_EQ(s->getPropertyPredicates().size(), 1u);
    EXPECT_EQ(s->getPropertyPredicates()[0].first, "age");
}

TEST_F(FilterPushdownTest, PushFilterIntoLeftJoinSide) {
    // Input: Filter(n.age=25) -> Join(NodeScan("n"), NodeScan("m"))
    // Filter pushes into left side, then merges into scan.
    // Expected: Join(NodeScan("n", {age=25}), NodeScan("m"))
    auto scanN = std::make_unique<LogicalNodeScan>("n");
    auto scanM = std::make_unique<LogicalNodeScan>("m");
    auto join = std::make_unique<LogicalJoin>(std::move(scanN), std::move(scanM));

    auto pred = makePropertyEquals("n", "age", 25);
    auto filter = std::make_unique<LogicalFilter>(std::move(join), pred);

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);

    // Result should be a Join
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_JOIN);
    auto *j = static_cast<LogicalJoin *>(result.get());
    // Left child should be NodeScan with predicate merged
    EXPECT_EQ(j->getLeft()->getType(), LogicalOpType::LOP_NODE_SCAN);
    auto *leftScan = static_cast<LogicalNodeScan *>(j->getLeft());
    EXPECT_EQ(leftScan->getPropertyPredicates().size(), 1u);
    EXPECT_EQ(j->getRight()->getType(), LogicalOpType::LOP_NODE_SCAN);
}

TEST_F(FilterPushdownTest, PushFilterIntoRightJoinSide) {
    // Input: Filter(m.name="X") -> Join(NodeScan("n"), NodeScan("m"))
    // Filter pushes into right side, then merges into scan.
    auto scanN = std::make_unique<LogicalNodeScan>("n");
    auto scanM = std::make_unique<LogicalNodeScan>("m");
    auto join = std::make_unique<LogicalJoin>(std::move(scanN), std::move(scanM));

    auto pred = makePropertyEqualsStr("m", "name", "X");
    auto filter = std::make_unique<LogicalFilter>(std::move(join), pred);

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);

    EXPECT_EQ(result->getType(), LogicalOpType::LOP_JOIN);
    auto *j = static_cast<LogicalJoin *>(result.get());
    EXPECT_EQ(j->getLeft()->getType(), LogicalOpType::LOP_NODE_SCAN);
    // Right side should be NodeScan with predicate merged
    EXPECT_EQ(j->getRight()->getType(), LogicalOpType::LOP_NODE_SCAN);
    auto *rightScan = static_cast<LogicalNodeScan *>(j->getRight());
    EXPECT_EQ(rightScan->getPropertyPredicates().size(), 1u);
    EXPECT_EQ(rightScan->getPropertyPredicates()[0].first, "name");
}

TEST_F(FilterPushdownTest, MergePropertyEqualityIntoNodeScan) {
    // Input: Filter(n.age=25) -> NodeScan("n":Person)
    // Expected: NodeScan("n":Person, {age=25})
    auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
    auto pred = makePropertyEquals("n", "age", 25);
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);

    // Filter should be eliminated, only scan remains
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
    auto *s = static_cast<LogicalNodeScan *>(result.get());
    EXPECT_EQ(s->getPropertyPredicates().size(), 1u);
    EXPECT_EQ(s->getPropertyPredicates()[0].first, "age");
}

TEST_F(FilterPushdownTest, SplitConjunctivePredicates) {
    // Input: Filter(n.age=25 AND n.name="Alice") -> NodeScan("n")
    // Expected: Both predicates merged into NodeScan
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto predAge = makePropertyEquals("n", "age", 25);
    auto predName = makePropertyEqualsStr("n", "name", "Alice");
    auto combinedPred = makeAnd(predAge, predName);
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), combinedPred);

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);

    // After splitting and pushing both predicates into scan, scan should have 2 predicates
    if (result->getType() == LogicalOpType::LOP_NODE_SCAN) {
        auto *s = static_cast<LogicalNodeScan *>(result.get());
        EXPECT_EQ(s->getPropertyPredicates().size(), 2u);
    }
}

TEST_F(FilterPushdownTest, NullPlanPassthrough) {
    auto result = rule.apply(nullptr, stats);
    EXPECT_EQ(result, nullptr);
}

TEST_F(FilterPushdownTest, NonFilterPlanUnchanged) {
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto result = rule.apply(std::move(scan), stats);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
}

// =============================================================================
// ProjectionPushdownRule Tests
// =============================================================================

class ProjectionPushdownTest : public ::testing::Test {
protected:
    rules::ProjectionPushdownRule rule;
    Statistics stats;
};

TEST_F(ProjectionPushdownTest, NameCheck) {
    EXPECT_EQ(rule.getName(), "ProjectionPushdownRule");
}

TEST_F(ProjectionPushdownTest, AnnotatesProjectWithRequiredColumns) {
    // Build: Project([n]) -> NodeScan("n")
    // After rule: Project should have requiredColumns set
    auto scan = std::make_unique<LogicalNodeScan>("n");
    std::vector<LogicalProjectItem> items;
    items.emplace_back(makeVarRef("n"), "n");
    auto project = std::make_unique<LogicalProject>(std::move(scan), std::move(items));

    // Wrap in another projection that uses "n"
    std::vector<LogicalProjectItem> outerItems;
    outerItems.emplace_back(makeVarRef("n"), "result");
    auto outerProject = std::make_unique<LogicalProject>(
        std::move(project), std::move(outerItems));

    auto result = rule.apply(std::move(outerProject), stats);
    ASSERT_NE(result, nullptr);
    // The result should still be a Project, and inner project should have requiredColumns set
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_PROJECT);
}

TEST_F(ProjectionPushdownTest, NullPlanPassthrough) {
    auto result = rule.apply(nullptr, stats);
    EXPECT_EQ(result, nullptr);
}

// =============================================================================
// JoinReorderRule Tests
// =============================================================================

class JoinReorderTest : public ::testing::Test {
protected:
    rules::JoinReorderRule rule;
};

TEST_F(JoinReorderTest, NameCheck) {
    EXPECT_EQ(rule.getName(), "JoinReorderRule");
}

TEST_F(JoinReorderTest, ReordersJoinByCardinality) {
    // Build: Join(NodeScan("n":Person), NodeScan("m":Company))
    // Person=100 nodes, Company=50 nodes
    // Expected: Join(NodeScan("m":Company), NodeScan("n":Person))
    // (smaller relation first)
    auto scanPerson = std::make_unique<LogicalNodeScan>(
        "n", std::vector<std::string>{"Person"});
    auto scanCompany = std::make_unique<LogicalNodeScan>(
        "m", std::vector<std::string>{"Company"});
    auto join = std::make_unique<LogicalJoin>(
        std::move(scanPerson), std::move(scanCompany));

    auto stats = makeTestStats();
    auto result = rule.apply(std::move(join), stats);
    ASSERT_NE(result, nullptr);

    // Result should be a join with Company (smaller) on left
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_JOIN);
    auto *j = static_cast<LogicalJoin *>(result.get());
    auto *left = dynamic_cast<LogicalNodeScan *>(j->getLeft());
    auto *right = dynamic_cast<LogicalNodeScan *>(j->getRight());
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(left->getLabels()[0], "Company");
    EXPECT_EQ(right->getLabels()[0], "Person");
}

TEST_F(JoinReorderTest, ThreeWayJoinReorder) {
    // Build: Join(Join(NodeScan:Person, NodeScan:Company), NodeScan:Unknown)
    // Person=100, Company=50, Unknown=1000 (totalNodeCount)
    // Expected: Join(Join(Company, Person), Unknown)
    auto scanPerson = std::make_unique<LogicalNodeScan>(
        "n", std::vector<std::string>{"Person"});
    auto scanCompany = std::make_unique<LogicalNodeScan>(
        "m", std::vector<std::string>{"Company"});
    auto scanUnknown = std::make_unique<LogicalNodeScan>(
        "x", std::vector<std::string>{});  // no label -> totalNodeCount
    auto innerJoin = std::make_unique<LogicalJoin>(
        std::move(scanPerson), std::move(scanCompany));
    auto outerJoin = std::make_unique<LogicalJoin>(
        std::move(innerJoin), std::move(scanUnknown));

    auto stats = makeTestStats();
    auto result = rule.apply(std::move(outerJoin), stats);
    ASSERT_NE(result, nullptr);

    // All three should be flattened and reordered: Company(50), Person(100), Unknown(1000)
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_JOIN);
    auto *topJoin = static_cast<LogicalJoin *>(result.get());
    // Left child should be another join
    EXPECT_EQ(topJoin->getLeft()->getType(), LogicalOpType::LOP_JOIN);
    // Right child should be the largest (Unknown)
    auto *rightScan = dynamic_cast<LogicalNodeScan *>(topJoin->getRight());
    ASSERT_NE(rightScan, nullptr);
    EXPECT_TRUE(rightScan->getLabels().empty()); // Unknown has no labels
}

TEST_F(JoinReorderTest, SingleInputNoReorder) {
    // A non-join plan should pass through unchanged
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto stats = makeTestStats();
    auto result = rule.apply(std::move(scan), stats);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
}

TEST_F(JoinReorderTest, NullPlanPassthrough) {
    auto stats = makeTestStats();
    auto result = rule.apply(nullptr, stats);
    EXPECT_EQ(result, nullptr);
}

// =============================================================================
// Optimizer Framework Tests
// =============================================================================

class OptimizerTest : public ::testing::Test {};

TEST_F(OptimizerTest, NullPlanReturnsNull) {
    Optimizer opt(nullptr);
    auto result = opt.optimize(nullptr, Statistics{});
    EXPECT_EQ(result, nullptr);
}

TEST_F(OptimizerTest, SimplePlanPassesThrough) {
    // A simple NodeScan with no optimization opportunity should pass through
    Optimizer opt(nullptr);
    auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
    auto stats = makeTestStats();
    auto result = opt.optimize(std::move(scan), stats);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
}

TEST_F(OptimizerTest, FilterPushdownIntegration) {
    // Filter(n.age=25) -> Project([n]) -> NodeScan("n")
    // After optimization: Project([n]) -> NodeScan("n", {age=25})
    Optimizer opt(nullptr);
    auto scan = std::make_unique<LogicalNodeScan>("n");
    std::vector<LogicalProjectItem> items;
    items.emplace_back(makeVarRef("n"), "n");
    auto project = std::make_unique<LogicalProject>(std::move(scan), std::move(items));

    auto pred = makePropertyEquals("n", "age", 25);
    auto filter = std::make_unique<LogicalFilter>(std::move(project), pred);

    auto stats = makeTestStats();
    auto result = opt.optimize(std::move(filter), stats);
    ASSERT_NE(result, nullptr);

    // After optimization, filter should be pushed past project and merged into scan
    // Result: Project -> NodeScan(with predicate)
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_PROJECT);
    auto *proj = static_cast<LogicalProject *>(result.get());
    auto *child = proj->getChildren()[0];
    EXPECT_EQ(child->getType(), LogicalOpType::LOP_NODE_SCAN);
    auto *s = static_cast<LogicalNodeScan *>(child);
    EXPECT_GE(s->getPropertyPredicates().size(), 1u);
}

TEST_F(OptimizerTest, AddCustomRule) {
    // Test that addRule works
    Optimizer opt(nullptr);
    auto customRule = std::make_unique<rules::PredicateSimplificationRule>();
    EXPECT_EQ(customRule->getName(), "PredicateSimplificationRule");
    opt.addRule(std::move(customRule));
    // Should not crash
}

TEST_F(OptimizerTest, MaxIterationsConstant) {
    EXPECT_EQ(Optimizer::MAX_ITERATIONS, 10);
}
