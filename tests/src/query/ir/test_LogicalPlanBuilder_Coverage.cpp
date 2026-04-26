/**
 * @file test_LogicalPlanBuilder_Coverage.cpp
 * @brief Branch coverage tests for LogicalPlanBuilder covering:
 *        - RETURN * with skip/limit
 *        - Aggregate projection with nested aggregates
 *        - WITH with aggregates and WHERE
 *        - MATCH with multiple patterns, optional match, named paths
 *        - CREATE with chains and anonymous variables
 *        - MERGE with edge chains
 *        - SET with MAP_MERGE and default type
 *        - REMOVE with label type
 *        - buildCall with YIELD items and procedure registry
 *        - buildCreateIndex with multiple properties
 */

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "graph/query/ir/LogicalPlanBuilder.hpp"
#include "graph/query/ir/QueryAST.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNamedPath.hpp"
#include "graph/query/logical/operators/LogicalOptionalMatch.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalMergeNode.hpp"
#include "graph/query/logical/operators/LogicalMergeEdge.hpp"
#include "graph/query/logical/operators/LogicalCreateNode.hpp"
#include "graph/query/logical/operators/LogicalCreateEdge.hpp"
#include "graph/query/logical/operators/LogicalSet.hpp"
#include "graph/query/logical/operators/LogicalRemove.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"
#include "graph/query/planner/ProcedureRegistry.hpp"

using namespace graph::query::ir;
using namespace graph::query::expressions;
using namespace graph::query::logical;

namespace {

std::shared_ptr<Expression> varRef(const std::string &name) {
	return std::make_shared<VariableReferenceExpression>(name);
}

std::shared_ptr<Expression> litInt(int64_t v) {
	return std::make_shared<LiteralExpression>(v);
}

// Helper to create a FunctionCallExpression for aggregate tests
std::shared_ptr<Expression> makeCountExpr(std::shared_ptr<Expression> arg, bool distinct = false) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(arg->clone());
	return std::make_shared<FunctionCallExpression>("count", std::move(args), distinct);
}

} // namespace

class LogicalPlanBuilderCoverageTest : public ::testing::Test {
protected:
	LogicalPlanBuilder builder;
};

// ============================================================================
// RETURN * with skip and limit
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, ReturnStarWithSkipAndLimit) {
	ProjectionBody body;
	body.returnStar = true;
	body.skip = 5;
	body.limit = 10;

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildReturn(body, std::move(input));
	ASSERT_NE(result, nullptr);

	// Verify structure: should have Limit(Skip(SingleRow))
	auto *limit = dynamic_cast<LogicalLimit *>(result.get());
	ASSERT_NE(limit, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, ReturnStarWithSkipOnly) {
	ProjectionBody body;
	body.returnStar = true;
	body.skip = 3;

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildReturn(body, std::move(input));
	ASSERT_NE(result, nullptr);
	auto *skip = dynamic_cast<LogicalSkip *>(result.get());
	ASSERT_NE(skip, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, ReturnStarWithLimitOnly) {
	ProjectionBody body;
	body.returnStar = true;
	body.limit = 5;

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildReturn(body, std::move(input));
	ASSERT_NE(result, nullptr);
	auto *limit = dynamic_cast<LogicalLimit *>(result.get());
	ASSERT_NE(limit, nullptr);
}

// ============================================================================
// RETURN with aggregates (covers buildAggregateProjection + appendSortSkipLimit)
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, ReturnWithAggregateAndSortSkipLimit) {
	ProjectionBody body;
	body.hasAggregates = true;

	// count(n) AS cnt
	ProjectionItem countItem;
	countItem.expression = makeCountExpr(varRef("n"));
	countItem.alias = "cnt";
	countItem.isTopLevelAggregate = true;
	countItem.containsAggregate = true;
	body.items.push_back(countItem);

	// Non-aggregate group-by key: city
	ProjectionItem cityItem;
	cityItem.expression = varRef("city");
	cityItem.alias = "city";
	cityItem.containsAggregate = false;
	cityItem.isTopLevelAggregate = false;
	body.items.push_back(cityItem);

	body.orderBy.push_back({varRef("cnt"), false}); // ORDER BY cnt DESC
	body.skip = 2;
	body.limit = 10;

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildReturn(body, std::move(input));
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, ReturnWithNestedAggregate) {
	ProjectionBody body;
	body.hasAggregates = true;

	// count(n) + 1 — containsAggregate but NOT isTopLevelAggregate
	// This tests the "Case 2" branch in buildAggregateProjection
	ProjectionItem nestedItem;
	auto countExpr = makeCountExpr(varRef("n"));
	auto oneExpr = litInt(1);
	auto plusExpr = std::make_unique<BinaryOpExpression>(
		countExpr->clone(), BinaryOperatorType::BOP_ADD, oneExpr->clone());
	nestedItem.expression = std::shared_ptr<Expression>(plusExpr.release());
	nestedItem.alias = "total";
	nestedItem.containsAggregate = true;
	nestedItem.isTopLevelAggregate = false;
	body.items.push_back(nestedItem);

	// Group-by key
	ProjectionItem groupItem;
	groupItem.expression = varRef("city");
	groupItem.alias = "city";
	body.items.push_back(groupItem);

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildReturn(body, std::move(input));
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, ReturnWithDistinctAggregate) {
	ProjectionBody body;
	body.hasAggregates = true;

	ProjectionItem countItem;
	countItem.expression = makeCountExpr(varRef("n"), true);
	countItem.alias = "unique_count";
	countItem.isTopLevelAggregate = true;
	countItem.containsAggregate = true;
	body.items.push_back(countItem);

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildReturn(body, std::move(input));
	ASSERT_NE(result, nullptr);
}

// ============================================================================
// RETURN with non-aggregate projection + ORDER BY + skip + limit
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, ReturnSimpleWithOrderBySkipLimit) {
	ProjectionBody body;
	ProjectionItem item;
	item.expression = varRef("n");
	item.alias = "n";
	body.items.push_back(item);
	body.orderBy.push_back({varRef("n"), true});
	body.skip = 1;
	body.limit = 5;

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildReturn(body, std::move(input));
	ASSERT_NE(result, nullptr);
}

// ============================================================================
// RETURN with null input (standalone RETURN, injects SingleRow)
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, ReturnWithNullInput) {
	ProjectionBody body;
	ProjectionItem item;
	item.expression = litInt(42);
	item.alias = "value";
	body.items.push_back(item);

	auto result = builder.buildReturn(body, nullptr);
	ASSERT_NE(result, nullptr);
}

// ============================================================================
// WITH clause
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, WithAggregates) {
	WithClause clause;
	clause.body.hasAggregates = true;

	ProjectionItem countItem;
	countItem.expression = makeCountExpr(varRef("n"));
	countItem.alias = "cnt";
	countItem.isTopLevelAggregate = true;
	countItem.containsAggregate = true;
	clause.body.items.push_back(countItem);

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildWith(clause, std::move(input));
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, WithReturnStar) {
	WithClause clause;
	clause.body.returnStar = true;

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildWith(clause, std::move(input));
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, WithWhereFilter) {
	WithClause clause;
	ProjectionItem item;
	item.expression = varRef("n");
	item.alias = "n";
	clause.body.items.push_back(item);
	clause.where = std::make_shared<LiteralExpression>(true);

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildWith(clause, std::move(input));
	ASSERT_NE(result, nullptr);
	// Should have Filter as root
	auto *filter = dynamic_cast<LogicalFilter *>(result.get());
	EXPECT_NE(filter, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, WithNullInputThrows) {
	WithClause clause;
	EXPECT_THROW(builder.buildWith(clause, nullptr), std::runtime_error);
}

TEST_F(LogicalPlanBuilderCoverageTest, WithDistinct) {
	WithClause clause;
	clause.body.distinct = true;
	ProjectionItem item;
	item.expression = varRef("n");
	item.alias = "n";
	clause.body.items.push_back(item);

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildWith(clause, std::move(input));
	ASSERT_NE(result, nullptr);
}

// ============================================================================
// MATCH clause
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, MatchEmptyPatterns) {
	MatchClause clause;
	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildMatch(clause, std::move(input));
	// Returns input unchanged
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, MatchOptionalWithInput) {
	MatchClause clause;
	clause.optional = true;

	PatternPartAST part;
	part.element.headNode.variable = "m";
	part.element.headNode.labels = {"City"};
	clause.patterns.push_back(part);
	clause.where = std::make_shared<LiteralExpression>(true);

	auto input = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto result = builder.buildMatch(clause, std::move(input));
	ASSERT_NE(result, nullptr);
	auto *opt = dynamic_cast<LogicalOptionalMatch *>(result.get());
	EXPECT_NE(opt, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, MatchMultiplePatternsWithInput) {
	MatchClause clause;

	PatternPartAST part1;
	part1.element.headNode.variable = "a";
	part1.element.headNode.labels = {"Person"};
	clause.patterns.push_back(part1);

	PatternPartAST part2;
	part2.element.headNode.variable = "b";
	part2.element.headNode.labels = {"City"};
	clause.patterns.push_back(part2);

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildMatch(clause, std::move(input));
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, MatchSinglePatternWithInput) {
	// Tests the "else if (clause.patterns.size() == 1)" branch
	MatchClause clause;

	PatternPartAST part;
	part.element.headNode.variable = "m";
	part.element.headNode.labels = {"City"};
	clause.patterns.push_back(part);

	auto input = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto result = builder.buildMatch(clause, std::move(input));
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, MatchWithNamedPath) {
	MatchClause clause;

	PatternPartAST part;
	part.pathVariable = "p";
	part.element.headNode.variable = "a";
	part.element.headNode.labels = {"Person"};

	PatternChainAST chain;
	chain.relationship.variable = "r";
	chain.relationship.type = "KNOWS";
	chain.relationship.direction = "out";
	chain.targetNode.variable = "b";
	chain.targetNode.labels = {"Person"};
	part.element.chain.push_back(chain);

	clause.patterns.push_back(part);

	auto result = builder.buildMatch(clause, nullptr);
	ASSERT_NE(result, nullptr);
	// Should be a LogicalNamedPath
	auto *namedPath = dynamic_cast<LogicalNamedPath *>(result.get());
	EXPECT_NE(namedPath, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, MatchWithVarLengthTraversal) {
	MatchClause clause;

	PatternPartAST part;
	part.element.headNode.variable = "a";
	part.element.headNode.labels = {"Person"};

	PatternChainAST chain;
	chain.relationship.variable = "r";
	chain.relationship.type = "KNOWS";
	chain.relationship.direction = "out";
	chain.relationship.isVarLength = true;
	chain.relationship.minHops = 1;
	chain.relationship.maxHops = 3;
	chain.targetNode.variable = "b";
	chain.targetNode.labels = {"Person"};
	chain.targetNode.properties.emplace_back("age", graph::PropertyValue(static_cast<int64_t>(30)));
	part.element.chain.push_back(chain);

	clause.patterns.push_back(part);

	auto result = builder.buildMatch(clause, nullptr);
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, MatchWithWhere) {
	MatchClause clause;

	PatternPartAST part;
	part.element.headNode.variable = "n";
	part.element.headNode.labels = {"Person"};
	clause.patterns.push_back(part);
	clause.where = std::make_shared<LiteralExpression>(true);

	auto result = builder.buildMatch(clause, nullptr);
	ASSERT_NE(result, nullptr);
}

// ============================================================================
// CREATE clause
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, CreateWithAnonymousNodes) {
	CreateClause clause;

	PatternPartAST part;
	part.element.headNode.variable = ""; // anonymous
	part.element.headNode.labels = {"A"};
	clause.patterns.push_back(part);

	auto result = builder.buildCreate(clause, nullptr);
	ASSERT_NE(result, nullptr);
	EXPECT_TRUE(builder.mutatesData());
}

TEST_F(LogicalPlanBuilderCoverageTest, CreateWithChainAndAnonymousVars) {
	CreateClause clause;

	PatternPartAST part;
	part.element.headNode.variable = "a";
	part.element.headNode.labels = {"Person"};

	PatternChainAST chain;
	chain.relationship.variable = ""; // anonymous edge
	chain.relationship.type = "KNOWS";
	chain.relationship.direction = "out";
	chain.targetNode.variable = ""; // anonymous target
	chain.targetNode.labels = {"Person"};
	chain.targetNode.properties.emplace_back("name", graph::PropertyValue("Bob"));
	part.element.chain.push_back(chain);

	clause.patterns.push_back(part);

	auto result = builder.buildCreate(clause, nullptr);
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, CreateWithExistingInput) {
	CreateClause clause;

	PatternPartAST part;
	part.element.headNode.variable = "m";
	part.element.headNode.labels = {"City"};
	clause.patterns.push_back(part);

	auto input = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto result = builder.buildCreate(clause, std::move(input));
	ASSERT_NE(result, nullptr);
}

// ============================================================================
// SET clause
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, SetWithLabelType) {
	SetClause clause;
	clause.items.push_back({SetItemType::SIT_LABEL, "n", "Admin", nullptr});

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildSet(clause, std::move(input));
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, SetWithMapMergeType) {
	SetClause clause;
	clause.items.push_back({SetItemType::SIT_MAP_MERGE, "n", "", varRef("props")});

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildSet(clause, std::move(input));
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, SetWithDefaultType) {
	SetClause clause;
	clause.items.push_back({static_cast<SetItemType>(99), "n", "x", litInt(1)});

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildSet(clause, std::move(input));
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, SetNullInputThrows) {
	SetClause clause;
	clause.items.push_back({SetItemType::SIT_PROPERTY, "n", "x", litInt(1)});
	EXPECT_THROW(builder.buildSet(clause, nullptr), std::runtime_error);
}

// ============================================================================
// DELETE clause
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, DeleteNullInputThrows) {
	DeleteClause clause;
	clause.variables = {"n"};
	EXPECT_THROW(builder.buildDelete(clause, nullptr), std::runtime_error);
}

// ============================================================================
// REMOVE clause
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, RemoveLabelType) {
	RemoveClause clause;
	clause.items.push_back({RemoveItemType::RIT_LABEL, "n", "Admin"});

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildRemove(clause, std::move(input));
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, RemovePropertyType) {
	RemoveClause clause;
	clause.items.push_back({RemoveItemType::RIT_PROPERTY, "n", "age"});

	auto input = std::make_unique<LogicalSingleRow>();
	auto result = builder.buildRemove(clause, std::move(input));
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, RemoveNullInputThrows) {
	RemoveClause clause;
	clause.items.push_back({RemoveItemType::RIT_PROPERTY, "n", "x"});
	EXPECT_THROW(builder.buildRemove(clause, nullptr), std::runtime_error);
}

// ============================================================================
// MERGE clause
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, MergeSingleNode) {
	MergeClause clause;
	clause.pattern.element.headNode.variable = "n";
	clause.pattern.element.headNode.labels = {"Person"};
	clause.pattern.element.headNode.properties.emplace_back("name", graph::PropertyValue("Alice"));

	clause.onCreateItems.push_back({SetItemType::SIT_PROPERTY, "n", "created", litInt(1)});
	clause.onMatchItems.push_back({SetItemType::SIT_PROPERTY, "n", "matched", litInt(1)});

	auto result = builder.buildMerge(clause, nullptr);
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, MergeEdgeChain) {
	MergeClause clause;
	clause.pattern.element.headNode.variable = "a";
	clause.pattern.element.headNode.labels = {"Person"};

	PatternChainAST chain;
	chain.relationship.variable = "r";
	chain.relationship.type = "KNOWS";
	chain.relationship.direction = "out";
	chain.targetNode.variable = "b";
	chain.targetNode.labels = {"Person"};
	chain.targetNode.properties.emplace_back("name", graph::PropertyValue("Bob"));
	clause.pattern.element.chain.push_back(chain);

	clause.onCreateItems.push_back({SetItemType::SIT_PROPERTY, "r", "since", litInt(2024)});
	clause.onMatchItems.push_back({SetItemType::SIT_PROPERTY, "r", "updated", litInt(1)});

	auto result = builder.buildMerge(clause, nullptr);
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, MergeEdgeMultiChain) {
	// Two-hop merge chain: a-[r1]->b-[r2]->c
	MergeClause clause;
	clause.pattern.element.headNode.variable = "a";
	clause.pattern.element.headNode.labels = {"Person"};

	PatternChainAST chain1;
	chain1.relationship.variable = "r1";
	chain1.relationship.type = "KNOWS";
	chain1.relationship.direction = "out";
	chain1.targetNode.variable = "b";
	chain1.targetNode.labels = {"Person"};
	clause.pattern.element.chain.push_back(chain1);

	PatternChainAST chain2;
	chain2.relationship.variable = "r2";
	chain2.relationship.type = "LIKES";
	chain2.relationship.direction = "out";
	chain2.targetNode.variable = "c";
	chain2.targetNode.labels = {"City"};
	clause.pattern.element.chain.push_back(chain2);

	auto result = builder.buildMerge(clause, nullptr);
	ASSERT_NE(result, nullptr);
}

// ============================================================================
// UNWIND clause
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, UnwindWithLiteralList) {
	UnwindClause clause;
	clause.alias = "x";
	clause.literalList = {graph::PropertyValue(static_cast<int64_t>(1)),
	                      graph::PropertyValue(static_cast<int64_t>(2))};

	auto result = builder.buildUnwind(clause, nullptr);
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, UnwindWithExpression) {
	UnwindClause clause;
	clause.alias = "x";
	clause.expression = varRef("myList");

	auto result = builder.buildUnwind(clause, nullptr);
	ASSERT_NE(result, nullptr);
}

// ============================================================================
// CALL procedure with YIELD
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, CallWithYield) {
	CallClause clause;
	clause.procedureName = "db.indexes";
	clause.standalone = true;
	clause.yieldItems.push_back({"name", "indexName"});
	clause.yieldItems.push_back({"type", "indexType"});

	auto result = builder.buildCall(clause, nullptr);
	ASSERT_NE(result, nullptr);
}

TEST_F(LogicalPlanBuilderCoverageTest, CallWithoutYield) {
	CallClause clause;
	clause.procedureName = "db.indexes";
	clause.standalone = true;

	auto result = builder.buildCall(clause, nullptr);
	ASSERT_NE(result, nullptr);
}

// ============================================================================
// Admin: CreateIndex with multiple properties (composite)
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, CreateIndexComposite) {
	CreateIndexClause clause;
	clause.name = "idx_comp";
	clause.label = "Person";
	clause.properties = {"first_name", "last_name"};

	auto result = builder.buildCreateIndex(clause);
	ASSERT_NE(result, nullptr);
	EXPECT_TRUE(builder.mutatesSchema());
}

TEST_F(LogicalPlanBuilderCoverageTest, CreateIndexSingle) {
	CreateIndexClause clause;
	clause.name = "idx_single";
	clause.label = "Person";
	clause.properties = {"name"};

	auto result = builder.buildCreateIndex(clause);
	ASSERT_NE(result, nullptr);
}

// ============================================================================
// MATCH with named path and no relationship variable
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, MatchNamedPathNoRelVar) {
	MatchClause clause;

	PatternPartAST part;
	part.pathVariable = "p";
	part.element.headNode.variable = "a";
	part.element.headNode.labels = {"Person"};

	PatternChainAST chain;
	chain.relationship.variable = ""; // empty edge variable
	chain.relationship.type = "KNOWS";
	chain.relationship.direction = "out";
	chain.targetNode.variable = "b";
	clause.patterns.push_back(part);
	part.element.chain.push_back(chain);
	clause.patterns.clear();
	clause.patterns.push_back(part);

	auto result = builder.buildMatch(clause, nullptr);
	ASSERT_NE(result, nullptr);
}

// ============================================================================
// collectPatternVariables dedup paths
// ============================================================================

TEST_F(LogicalPlanBuilderCoverageTest, MatchOptionalWithDuplicateVars) {
	MatchClause clause;
	clause.optional = true;

	PatternPartAST part1;
	part1.element.headNode.variable = "n";
	part1.element.headNode.labels = {"Person"};

	PatternChainAST chain;
	chain.relationship.variable = "r";
	chain.relationship.type = "KNOWS";
	chain.relationship.direction = "out";
	chain.targetNode.variable = "n"; // same as head — dedup branch
	part1.element.chain.push_back(chain);

	clause.patterns.push_back(part1);

	auto input = std::make_unique<LogicalNodeScan>("x", std::vector<std::string>{"X"});
	auto result = builder.buildMatch(clause, std::move(input));
	ASSERT_NE(result, nullptr);
}
