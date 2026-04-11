/**
 * @file test_LimitPushdownSortEliminationRules.cpp
 * @brief Unit tests for LimitPushdownRule and SortEliminationRule.
 *
 * Licensed under the Apache License, Version 2.0
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/optimizer/Statistics.hpp"
#include "graph/query/optimizer/rules/LimitPushdownRule.hpp"
#include "graph/query/optimizer/rules/SortEliminationRule.hpp"

using namespace graph::query::expressions;
using namespace graph::query::logical;
using namespace graph::query::optimizer;

namespace {

std::shared_ptr<Expression> propRef(const std::string &var, const std::string &prop) {
	return std::shared_ptr<Expression>(
		std::make_unique<VariableReferenceExpression>(var, prop).release());
}

std::shared_ptr<Expression> varRef(const std::string &name) {
	return std::shared_ptr<Expression>(
		std::make_unique<VariableReferenceExpression>(name).release());
}

} // namespace

// ============================================================================
// LimitPushdownRule
// ============================================================================

TEST(LimitPushdownRuleTest, GetName) {
	rules::LimitPushdownRule rule;
	EXPECT_EQ(rule.getName(), "LimitPushdownRule");
}

TEST(LimitPushdownRuleTest, NullPlan) {
	rules::LimitPushdownRule rule;
	Statistics stats;
	auto result = rule.apply(nullptr, stats);
	EXPECT_EQ(result, nullptr);
}

TEST(LimitPushdownRuleTest, NonLimitPassthrough) {
	// A plan with no Limit should be returned unchanged
	rules::LimitPushdownRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});

	std::vector<LogicalProjectItem> items;
	items.emplace_back(varRef("n"), "n");
	auto project = std::make_unique<LogicalProject>(std::move(scan), std::move(items));

	auto result = rule.apply(std::move(project), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_PROJECT);
}

TEST(LimitPushdownRuleTest, LimitOverNonProject) {
	// Limit over NodeScan (not a Project) → no swap
	rules::LimitPushdownRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto limit = std::make_unique<LogicalLimit>(std::move(scan), 5);

	auto result = rule.apply(std::move(limit), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_LIMIT);
}

TEST(LimitPushdownRuleTest, SwapLimitOverNonDistinctProject) {
	// Limit(10) → Project → Scan  ⟹  Project → Limit(10) → Scan
	rules::LimitPushdownRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<LogicalProjectItem> items;
	items.emplace_back(propRef("n", "name"), "name");
	auto project = std::make_unique<LogicalProject>(std::move(scan), std::move(items), false);
	auto limit = std::make_unique<LogicalLimit>(std::move(project), 10);

	auto result = rule.apply(std::move(limit), stats);
	ASSERT_NE(result, nullptr);

	// Top should now be Project
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_PROJECT);

	// Project's child should be Limit
	auto children = result->getChildren();
	ASSERT_EQ(children.size(), 1u);
	ASSERT_NE(children[0], nullptr);
	EXPECT_EQ(children[0]->getType(), LogicalOpType::LOP_LIMIT);

	// Limit's child should be NodeScan
	auto limitChildren = children[0]->getChildren();
	ASSERT_EQ(limitChildren.size(), 1u);
	EXPECT_EQ(limitChildren[0]->getType(), LogicalOpType::LOP_NODE_SCAN);
}

TEST(LimitPushdownRuleTest, NoSwapOnDistinctProject) {
	// Limit over DISTINCT Project → no swap
	rules::LimitPushdownRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<LogicalProjectItem> items;
	items.emplace_back(propRef("n", "name"), "name");
	auto project = std::make_unique<LogicalProject>(std::move(scan), std::move(items), true);
	auto limit = std::make_unique<LogicalLimit>(std::move(project), 5);

	auto result = rule.apply(std::move(limit), stats);
	ASSERT_NE(result, nullptr);
	// Should remain Limit on top
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_LIMIT);
}

TEST(LimitPushdownRuleTest, LimitWithNoChildren) {
	// Limit with no child → early return
	rules::LimitPushdownRule rule;
	Statistics stats;

	auto limit = std::make_unique<LogicalLimit>(nullptr, 5);
	// Null child means apply just returns the node unchanged after recursion
	auto result = rule.apply(std::move(limit), stats);
	// The null child case - the limit node itself is still returned
	ASSERT_NE(result, nullptr);
}

TEST(LimitPushdownRuleTest, RecursiveApplication) {
	// Nested: Limit → Project → Limit → Project → Scan
	// Both Limit→Project pairs should be swapped
	rules::LimitPushdownRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});

	std::vector<LogicalProjectItem> items1;
	items1.emplace_back(varRef("n"), "n");
	auto innerProject = std::make_unique<LogicalProject>(std::move(scan), std::move(items1), false);
	auto innerLimit = std::make_unique<LogicalLimit>(std::move(innerProject), 20);

	std::vector<LogicalProjectItem> items2;
	items2.emplace_back(propRef("n", "name"), "name");
	auto outerProject = std::make_unique<LogicalProject>(std::move(innerLimit), std::move(items2), false);
	auto outerLimit = std::make_unique<LogicalLimit>(std::move(outerProject), 5);

	auto result = rule.apply(std::move(outerLimit), stats);
	ASSERT_NE(result, nullptr);
	// Outer: Project should be on top after swap
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_PROJECT);
}

// ============================================================================
// SortEliminationRule
// ============================================================================

TEST(SortEliminationRuleTest, GetName) {
	rules::SortEliminationRule rule;
	EXPECT_EQ(rule.getName(), "SortEliminationRule");
}

TEST(SortEliminationRuleTest, NullPlan) {
	rules::SortEliminationRule rule;
	Statistics stats;
	auto result = rule.apply(nullptr, stats);
	EXPECT_EQ(result, nullptr);
}

TEST(SortEliminationRuleTest, NonSortPassthrough) {
	// A plan without Sort should pass through
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto result = rule.apply(std::move(scan), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
}

TEST(SortEliminationRuleTest, MultiKeySortNotEliminated) {
	// Sort with multiple keys → not eliminated
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>(
		"n", std::vector<std::string>{"Person"},
		std::vector<std::pair<std::string, graph::PropertyValue>>{{"age", graph::PropertyValue(int64_t(30))}});
	scan->setPreferredScanType(graph::query::execution::ScanType::PROPERTY_SCAN);

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(propRef("n", "age"), true);
	sortItems.emplace_back(propRef("n", "name"), true);
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));

	auto result = rule.apply(std::move(sort), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_SORT);
}

TEST(SortEliminationRuleTest, DescSortNotEliminated) {
	// Sort DESC → not eliminated (only ASC is handled)
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>(
		"n", std::vector<std::string>{"Person"},
		std::vector<std::pair<std::string, graph::PropertyValue>>{{"age", graph::PropertyValue(int64_t(30))}});
	scan->setPreferredScanType(graph::query::execution::ScanType::PROPERTY_SCAN);

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(propRef("n", "age"), false);  // descending
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));

	auto result = rule.apply(std::move(sort), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_SORT);
}

TEST(SortEliminationRuleTest, NullExpressionNotEliminated) {
	// Sort with null expression → not eliminated
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(nullptr, true);
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));

	auto result = rule.apply(std::move(sort), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_SORT);
}

TEST(SortEliminationRuleTest, NonPropertyAccessNotEliminated) {
	// Sort on a non-property expression (e.g., variable ref) → not eliminated
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	scan->setPreferredScanType(graph::query::execution::ScanType::PROPERTY_SCAN);

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(varRef("n"), true);  // not a property access
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));

	auto result = rule.apply(std::move(sort), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_SORT);
}

TEST(SortEliminationRuleTest, SortOverNonNodeScanNotEliminated) {
	// Sort over a Project (not a NodeScan) → not eliminated
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<LogicalProjectItem> items;
	items.emplace_back(propRef("n", "age"), "age");
	auto project = std::make_unique<LogicalProject>(std::move(scan), std::move(items));

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(propRef("n", "age"), true);
	auto sort = std::make_unique<LogicalSort>(std::move(project), std::move(sortItems));

	auto result = rule.apply(std::move(sort), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_SORT);
}

TEST(SortEliminationRuleTest, VariableMismatchNotEliminated) {
	// Sort on "m.age" but scan is on variable "n" → not eliminated
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>(
		"n", std::vector<std::string>{"Person"},
		std::vector<std::pair<std::string, graph::PropertyValue>>{{"age", graph::PropertyValue(int64_t(30))}});
	scan->setPreferredScanType(graph::query::execution::ScanType::PROPERTY_SCAN);

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(propRef("m", "age"), true);  // wrong variable
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));

	auto result = rule.apply(std::move(sort), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_SORT);
}

TEST(SortEliminationRuleTest, FullScanNotEliminated) {
	// Sort over NodeScan with FULL_SCAN → not eliminated
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>(
		"n", std::vector<std::string>{"Person"},
		std::vector<std::pair<std::string, graph::PropertyValue>>{{"age", graph::PropertyValue(int64_t(30))}});
	// Default scan type is FULL_SCAN

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(propRef("n", "age"), true);
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));

	auto result = rule.apply(std::move(sort), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_SORT);
}

TEST(SortEliminationRuleTest, LabelScanNotEliminated) {
	// Sort over NodeScan with LABEL_SCAN → not eliminated
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>(
		"n", std::vector<std::string>{"Person"},
		std::vector<std::pair<std::string, graph::PropertyValue>>{{"age", graph::PropertyValue(int64_t(30))}});
	scan->setPreferredScanType(graph::query::execution::ScanType::LABEL_SCAN);

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(propRef("n", "age"), true);
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));

	auto result = rule.apply(std::move(sort), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_SORT);
}

TEST(SortEliminationRuleTest, PropertyScanMatchEliminated) {
	// Sort ASC on n.age, NodeScan with PROPERTY_SCAN on "age" → eliminated
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>(
		"n", std::vector<std::string>{"Person"},
		std::vector<std::pair<std::string, graph::PropertyValue>>{{"age", graph::PropertyValue(int64_t(30))}});
	scan->setPreferredScanType(graph::query::execution::ScanType::PROPERTY_SCAN);

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(propRef("n", "age"), true);
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));

	auto result = rule.apply(std::move(sort), stats);
	ASSERT_NE(result, nullptr);
	// Sort should be eliminated, leaving just the NodeScan
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
}

TEST(SortEliminationRuleTest, RangeScanMatchEliminated) {
	// Sort ASC on n.score, NodeScan with RANGE_SCAN on "score" → eliminated
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	scan->setPreferredScanType(graph::query::execution::ScanType::RANGE_SCAN);
	scan->setRangePredicates({
		RangePredicate{"score", graph::PropertyValue(int64_t(1)), graph::PropertyValue(int64_t(100)), true, true}
	});

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(propRef("n", "score"), true);
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));

	auto result = rule.apply(std::move(sort), stats);
	ASSERT_NE(result, nullptr);
	// Sort should be eliminated
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
}

TEST(SortEliminationRuleTest, PropertyMismatchNotEliminated) {
	// Sort on n.name but PROPERTY_SCAN is on "age" → not eliminated
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>(
		"n", std::vector<std::string>{"Person"},
		std::vector<std::pair<std::string, graph::PropertyValue>>{{"age", graph::PropertyValue(int64_t(30))}});
	scan->setPreferredScanType(graph::query::execution::ScanType::PROPERTY_SCAN);

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(propRef("n", "name"), true);  // sort on "name", not "age"
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));

	auto result = rule.apply(std::move(sort), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_SORT);
}

TEST(SortEliminationRuleTest, RangeMismatchNotEliminated) {
	// Sort on n.name but RANGE_SCAN is on "score" → not eliminated
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	scan->setPreferredScanType(graph::query::execution::ScanType::RANGE_SCAN);
	scan->setRangePredicates({
		RangePredicate{"score", graph::PropertyValue(int64_t(1)), graph::PropertyValue(int64_t(100)), true, true}
	});

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(propRef("n", "name"), true);  // sort on "name", not "score"
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));

	auto result = rule.apply(std::move(sort), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_SORT);
}

TEST(SortEliminationRuleTest, PropertyScanNoPredicatesNotEliminated) {
	// PROPERTY_SCAN with no property predicates and no range predicates → not eliminated
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	scan->setPreferredScanType(graph::query::execution::ScanType::PROPERTY_SCAN);
	// No property predicates set, no range predicates

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(propRef("n", "age"), true);
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));

	auto result = rule.apply(std::move(sort), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_SORT);
}

TEST(SortEliminationRuleTest, RecursiveElimination) {
	// Sort nested inside another operator tree
	// Project → Sort → NodeScan(PROPERTY_SCAN on age)
	// The Sort should be recursively found and eliminated
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>(
		"n", std::vector<std::string>{"Person"},
		std::vector<std::pair<std::string, graph::PropertyValue>>{{"age", graph::PropertyValue(int64_t(30))}});
	scan->setPreferredScanType(graph::query::execution::ScanType::PROPERTY_SCAN);

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(propRef("n", "age"), true);
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));

	std::vector<LogicalProjectItem> projItems;
	projItems.emplace_back(propRef("n", "age"), "age");
	auto project = std::make_unique<LogicalProject>(std::move(sort), std::move(projItems));

	auto result = rule.apply(std::move(project), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_PROJECT);

	// Project's child should now be NodeScan (Sort eliminated)
	auto children = result->getChildren();
	ASSERT_EQ(children.size(), 1u);
	EXPECT_EQ(children[0]->getType(), LogicalOpType::LOP_NODE_SCAN);
}

TEST(SortEliminationRuleTest, RangeScanFallbackToRangePredicates) {
	// RANGE_SCAN with no property predicates, but range predicates match → eliminated
	// This tests the fallback path (line 100-107)
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	scan->setPreferredScanType(graph::query::execution::ScanType::RANGE_SCAN);
	// No property predicates, only range predicates
	scan->setRangePredicates({
		RangePredicate{"age", graph::PropertyValue(int64_t(10)), graph::PropertyValue(int64_t(50)), true, true}
	});

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(propRef("n", "age"), true);
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));

	auto result = rule.apply(std::move(sort), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
}

TEST(SortEliminationRuleTest, PropertyPredicateMatchSkipsRangeCheck) {
	// PROPERTY_SCAN with matching property predicate → eliminated without checking range
	// This tests that the first loop short-circuits before the range loop
	rules::SortEliminationRule rule;
	Statistics stats;

	auto scan = std::make_unique<LogicalNodeScan>(
		"n", std::vector<std::string>{"Person"},
		std::vector<std::pair<std::string, graph::PropertyValue>>{
			{"name", graph::PropertyValue(std::string("Alice"))},
			{"age", graph::PropertyValue(int64_t(30))}
		});
	scan->setPreferredScanType(graph::query::execution::ScanType::PROPERTY_SCAN);

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(propRef("n", "age"), true);
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));

	auto result = rule.apply(std::move(sort), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
}
