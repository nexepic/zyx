/**
 * @file test_PatternBuilder_Parsing.cpp
 * @brief Branch coverage tests for PatternBuilder.cpp
 *
 * Tests call PatternBuilder static methods directly with ANTLR parse contexts
 * since PatternBuilder is not used by the active query pipeline.
 */

#include <gtest/gtest.h>
#include <string>
#include "CypherLexer.h"
#include "CypherParser.h"
#include "antlr4-runtime.h"
#include "helpers/PatternBuilder.hpp"
#include "helpers/ExpressionBuilder.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/execution/operators/SetOperator.hpp"

using namespace graph::parser::cypher::helpers;
using namespace graph::query::logical;
using namespace graph::query::execution::operators;

class PatternBuilderParsingTest : public ::testing::Test {
protected:
	struct ParseResult {
		std::unique_ptr<antlr4::ANTLRInputStream> input;
		std::unique_ptr<CypherLexer> lexer;
		std::unique_ptr<antlr4::CommonTokenStream> tokens;
		std::unique_ptr<CypherParser> parser;
		CypherParser::CypherContext* tree = nullptr;
	};

	std::vector<std::unique_ptr<ParseResult>> parseResults;

	CypherParser::CypherContext* parse(const std::string& query) {
		auto result = std::make_unique<ParseResult>();
		result->input = std::make_unique<antlr4::ANTLRInputStream>(query);
		result->lexer = std::make_unique<CypherLexer>(result->input.get());
		result->lexer->removeErrorListeners();
		result->tokens = std::make_unique<antlr4::CommonTokenStream>(result->lexer.get());
		result->parser = std::make_unique<CypherParser>(result->tokens.get());
		result->parser->removeErrorListeners();
		result->tree = result->parser->cypher();
		auto* tree = result->tree;
		parseResults.push_back(std::move(result));
		return tree;
	}

	CypherParser::SingleQueryContext* getSingleQuery(const std::string& query) {
		auto* tree = parse(query);
		auto* stmt = dynamic_cast<CypherParser::RegularStatementContext*>(tree->statement());
		if (!stmt || !stmt->query() || !stmt->query()->regularQuery()) return nullptr;
		return stmt->query()->regularQuery()->singleQuery(0);
	}

	CypherParser::PatternContext* getMatchPattern(const std::string& query) {
		auto* sq = getSingleQuery(query);
		if (!sq) return nullptr;
		for (auto* rc : sq->readingClause()) {
			if (rc->matchStatement()) {
				return rc->matchStatement()->pattern();
			}
		}
		return nullptr;
	}

	CypherParser::WhereContext* getMatchWhere(const std::string& query) {
		auto* sq = getSingleQuery(query);
		if (!sq) return nullptr;
		for (auto* rc : sq->readingClause()) {
			if (rc->matchStatement() && rc->matchStatement()->where()) {
				return rc->matchStatement()->where();
			}
		}
		return nullptr;
	}

	CypherParser::PatternContext* getCreatePattern(const std::string& query) {
		auto* sq = getSingleQuery(query);
		if (!sq) return nullptr;
		for (auto* uc : sq->updatingClause()) {
			if (uc->createStatement()) {
				return uc->createStatement()->pattern();
			}
		}
		return nullptr;
	}

	CypherParser::PatternPartContext* getMergePatternPart(const std::string& query) {
		auto* sq = getSingleQuery(query);
		if (!sq) return nullptr;
		for (auto* uc : sq->updatingClause()) {
			if (uc->mergeStatement()) {
				return uc->mergeStatement()->patternPart();
			}
		}
		return nullptr;
	}

	CypherParser::SetStatementContext* getSetStatement(const std::string& query) {
		auto* sq = getSingleQuery(query);
		if (!sq) return nullptr;
		for (auto* uc : sq->updatingClause()) {
			if (uc->setStatement()) {
				return uc->setStatement();
			}
		}
		return nullptr;
	}

	// Parse a RETURN expression to use as SetItem expression
	CypherParser::ExpressionContext* getReturnExpr(const std::string& query) {
		auto* sq = getSingleQuery(query);
		if (!sq || !sq->returnStatement()) return nullptr;
		auto* body = sq->returnStatement()->projectionBody();
		if (!body || !body->projectionItems()) return nullptr;
		auto items = body->projectionItems()->projectionItem();
		if (items.empty()) return nullptr;
		return items[0]->expression();
	}

	std::shared_ptr<graph::query::QueryPlanner> nullPlanner() {
		return nullptr;
	}

	std::unique_ptr<LogicalOperator> makeRootOp() {
		auto* pat = getMatchPattern("MATCH (x) RETURN x");
		return PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	}
};

// ============== buildMatchPattern: null / empty ==============

TEST_F(PatternBuilderParsingTest, NullPattern) {
	auto result = PatternBuilder::buildMatchPattern(nullptr, nullptr, nullPlanner());
	EXPECT_EQ(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, NullPatternWithRoot) {
	auto root = makeRootOp();
	auto result = PatternBuilder::buildMatchPattern(nullptr, std::move(root), nullPlanner());
	EXPECT_NE(result, nullptr);
}

// ============== Single node patterns ==============

TEST_F(PatternBuilderParsingTest, SingleNode) {
	auto* pat = getMatchPattern("MATCH (n) RETURN n");
	ASSERT_NE(pat, nullptr);
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, SingleNodeWithLabel) {
	auto* pat = getMatchPattern("MATCH (n:Person) RETURN n");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, SingleNodeWithProps) {
	auto* pat = getMatchPattern("MATCH (n {name: 'Alice'}) RETURN n");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

// ============== Traversal directions ==============

TEST_F(PatternBuilderParsingTest, OutboundTraversal) {
	auto* pat = getMatchPattern("MATCH (a)-[r:KNOWS]->(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, InboundTraversal) {
	auto* pat = getMatchPattern("MATCH (a)<-[r:KNOWS]-(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, BidirectionalTraversal) {
	auto* pat = getMatchPattern("MATCH (a)-[r:KNOWS]-(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

// ============== Variable-length paths ==============

TEST_F(PatternBuilderParsingTest, VarLengthDefault) {
	auto* pat = getMatchPattern("MATCH (a)-[r:KNOWS*]->(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, VarLengthExact) {
	auto* pat = getMatchPattern("MATCH (a)-[r:KNOWS*3]->(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, VarLengthMinMax) {
	auto* pat = getMatchPattern("MATCH (a)-[r:KNOWS*1..5]->(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, VarLengthMinOnly) {
	auto* pat = getMatchPattern("MATCH (a)-[r:KNOWS*2..]->(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, VarLengthMaxOnly) {
	auto* pat = getMatchPattern("MATCH (a)-[r:KNOWS*..5]->(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

// ============== No relDetail (bare relationship) ==============

TEST_F(PatternBuilderParsingTest, BareRelationship) {
	auto* pat = getMatchPattern("MATCH (a)-->(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

// ============== Multiple pattern parts ==============

TEST_F(PatternBuilderParsingTest, MultiplePatternPartsNoRoot) {
	auto* pat = getMatchPattern("MATCH (a), (b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, MultiplePatternPartsWithRoot) {
	auto root = makeRootOp();
	auto* pat = getMatchPattern("MATCH (a), (b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(pat, std::move(root), nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, SinglePatternWithRoot) {
	auto root = makeRootOp();
	auto* pat = getMatchPattern("MATCH (a)-[r]->(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(pat, std::move(root), nullPlanner());
	EXPECT_NE(result, nullptr);
}

// ============== Named paths ==============

TEST_F(PatternBuilderParsingTest, NamedPathWithEdge) {
	auto* pat = getMatchPattern("MATCH p = (a)-[r:KNOWS]->(b) RETURN p");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, NamedPathNoEdgeVar) {
	auto* pat = getMatchPattern("MATCH p = (a)-->(b) RETURN p");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, NamedPathAnonymousNodes) {
	auto* pat = getMatchPattern("MATCH p = ()-[r:KNOWS]->() RETURN p");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

// ============== WHERE clause ==============

TEST_F(PatternBuilderParsingTest, MatchWithWhere) {
	auto* pat = getMatchPattern("MATCH (n:Person) WHERE n.age > 21 RETURN n");
	auto* where = getMatchWhere("MATCH (n:Person) WHERE n.age > 21 RETURN n");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner(), where);
	EXPECT_NE(result, nullptr);
}

// ============== OPTIONAL MATCH ==============

TEST_F(PatternBuilderParsingTest, OptionalMatchWithRoot) {
	auto root = makeRootOp();
	auto* pat = getMatchPattern("OPTIONAL MATCH (a)-[r:KNOWS]->(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(
		pat, std::move(root), nullPlanner(), nullptr, true);
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, OptionalMatchWithWhereAndRoot) {
	auto root = makeRootOp();
	auto* pat = getMatchPattern("OPTIONAL MATCH (a:Person) WHERE a.age > 21 RETURN a");
	auto* where = getMatchWhere("OPTIONAL MATCH (a:Person) WHERE a.age > 21 RETURN a");
	auto result = PatternBuilder::buildMatchPattern(
		pat, std::move(root), nullPlanner(), where, true);
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, OptionalMatchNoRoot) {
	auto* pat = getMatchPattern("OPTIONAL MATCH (a)-[r]->(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(
		pat, nullptr, nullPlanner(), nullptr, true);
	EXPECT_NE(result, nullptr);
}

// ============== Edge and target properties ==============

TEST_F(PatternBuilderParsingTest, EdgeWithProps) {
	auto* pat = getMatchPattern("MATCH (a)-[r:KNOWS {since: 2020}]->(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, TargetNodeWithProps) {
	auto* pat = getMatchPattern("MATCH (a)-[r:KNOWS]->(b:Person {name: 'Bob'}) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, TargetNodeWithLabel) {
	auto* pat = getMatchPattern("MATCH (a)-[r]->(b:Person) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

// ============== Multi-hop traversals ==============

TEST_F(PatternBuilderParsingTest, MultiHop) {
	auto* pat = getMatchPattern("MATCH (a)-[r1:KNOWS]->(b)-[r2:LIKES]->(c) RETURN a, c");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

// ============== CREATE patterns ==============

TEST_F(PatternBuilderParsingTest, CreateNull) {
	auto result = PatternBuilder::buildCreatePattern(nullptr, nullptr, nullPlanner());
	EXPECT_EQ(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, CreateSingleNode) {
	auto* pat = getCreatePattern("CREATE (n:Person)");
	auto result = PatternBuilder::buildCreatePattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, CreateNodeWithProps) {
	auto* pat = getCreatePattern("CREATE (n:Person {name: 'Alice', age: 30})");
	auto result = PatternBuilder::buildCreatePattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, CreateWithEdge) {
	auto* pat = getCreatePattern("CREATE (a:Person)-[r:KNOWS]->(b:Person)");
	auto result = PatternBuilder::buildCreatePattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, CreateWithRootOp) {
	auto root = makeRootOp();
	auto* pat = getCreatePattern("CREATE (n:Person {name: 'Bob'})");
	auto result = PatternBuilder::buildCreatePattern(pat, std::move(root), nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, CreateNodeNoProps) {
	auto* pat = getCreatePattern("CREATE (n)");
	auto result = PatternBuilder::buildCreatePattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, CreateMultiplePatterns) {
	auto* pat = getCreatePattern("CREATE (a:X), (b:Y)");
	auto result = PatternBuilder::buildCreatePattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

// CREATE with expression-based property (non-literal: triggers line 322 else branch)
TEST_F(PatternBuilderParsingTest, CreateWithExprProps) {
	// Property value m.name is not a literal, triggers the expression branch
	auto* pat = getCreatePattern("MATCH (m:Person) CREATE (n:Copy {name: m.name})");
	if (pat) {
		auto result = PatternBuilder::buildCreatePattern(pat, nullptr, nullPlanner());
		EXPECT_NE(result, nullptr);
	}
}

// CREATE with null literal (triggers line 322: null type check with "null" text)
TEST_F(PatternBuilderParsingTest, CreateWithNullProp) {
	auto* pat = getCreatePattern("CREATE (n:Person {name: null})");
	auto result = PatternBuilder::buildCreatePattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

// ============== MERGE patterns ==============

TEST_F(PatternBuilderParsingTest, MergeNull) {
	std::vector<SetItem> empty;
	auto result = PatternBuilder::buildMergePattern(nullptr, empty, empty, nullPlanner());
	EXPECT_EQ(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, MergeSingleNode) {
	auto* pp = getMergePatternPart("MERGE (n:Person {name: 'Alice'})");
	ASSERT_NE(pp, nullptr);
	std::vector<SetItem> empty;
	auto result = PatternBuilder::buildMergePattern(pp, empty, empty, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, MergeEdgeOut) {
	auto* pp = getMergePatternPart("MERGE (a:Person)-[r:KNOWS]->(b:Person)");
	ASSERT_NE(pp, nullptr);
	std::vector<SetItem> empty;
	auto result = PatternBuilder::buildMergePattern(pp, empty, empty, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, MergeEdgeIn) {
	auto* pp = getMergePatternPart("MERGE (a:Person)<-[r:KNOWS]-(b:Person)");
	ASSERT_NE(pp, nullptr);
	std::vector<SetItem> empty;
	auto result = PatternBuilder::buildMergePattern(pp, empty, empty, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, MergeEdgeBoth) {
	auto* pp = getMergePatternPart("MERGE (a:Person)-[r:KNOWS]-(b:Person)");
	ASSERT_NE(pp, nullptr);
	std::vector<SetItem> empty;
	auto result = PatternBuilder::buildMergePattern(pp, empty, empty, nullPlanner());
	EXPECT_NE(result, nullptr);
}

TEST_F(PatternBuilderParsingTest, MergeEdgeWithProps) {
	auto* pp = getMergePatternPart("MERGE (a:Person)-[r:KNOWS {since: 2020}]->(b:Person)");
	ASSERT_NE(pp, nullptr);
	std::vector<SetItem> empty;
	auto result = PatternBuilder::buildMergePattern(pp, empty, empty, nullPlanner());
	EXPECT_NE(result, nullptr);
}

// MERGE with ON CREATE SET items (triggers line 398 loop body)
TEST_F(PatternBuilderParsingTest, MergeWithOnCreateSetItems) {
	auto* pp = getMergePatternPart("MERGE (n:Person {name: 'Alice'})");
	ASSERT_NE(pp, nullptr);

	// Build expression for the SetItem
	auto* exprCtx = getReturnExpr("RETURN true");
	ASSERT_NE(exprCtx, nullptr);
	auto expr = ExpressionBuilder::buildExpression(exprCtx);
	auto exprShared = std::shared_ptr<graph::query::expressions::Expression>(expr.release());

	std::vector<SetItem> onCreateItems;
	onCreateItems.emplace_back(SetActionType::PROPERTY, "n", "created", exprShared);

	std::vector<SetItem> onMatchItems;
	onMatchItems.emplace_back(SetActionType::PROPERTY, "n", "lastSeen", exprShared);

	auto result = PatternBuilder::buildMergePattern(pp, onCreateItems, onMatchItems, nullPlanner());
	EXPECT_NE(result, nullptr);
}

// MERGE edge with ON CREATE/MATCH SET items
TEST_F(PatternBuilderParsingTest, MergeEdgeWithSetItems) {
	auto* pp = getMergePatternPart("MERGE (a:Person)-[r:KNOWS]->(b:Person)");
	ASSERT_NE(pp, nullptr);

	auto* exprCtx = getReturnExpr("RETURN 42");
	ASSERT_NE(exprCtx, nullptr);
	auto expr = ExpressionBuilder::buildExpression(exprCtx);
	auto exprShared = std::shared_ptr<graph::query::expressions::Expression>(expr.release());

	std::vector<SetItem> onCreate;
	onCreate.emplace_back(SetActionType::PROPERTY, "r", "weight", exprShared);
	std::vector<SetItem> onMatch;
	onMatch.emplace_back(SetActionType::PROPERTY, "r", "count", exprShared);

	auto result = PatternBuilder::buildMergePattern(pp, onCreate, onMatch, nullPlanner());
	EXPECT_NE(result, nullptr);
}

// ============== extractSetItems ==============

TEST_F(PatternBuilderParsingTest, ExtractSetItems_Null) {
	auto items = PatternBuilder::extractSetItems(nullptr);
	EXPECT_TRUE(items.empty());
}

TEST_F(PatternBuilderParsingTest, ExtractSetItems_Property) {
	auto* ctx = getSetStatement("MATCH (n) SET n.name = 'Alice' RETURN n");
	ASSERT_NE(ctx, nullptr);
	auto items = PatternBuilder::extractSetItems(ctx);
	EXPECT_GE(items.size(), 1u);
	EXPECT_EQ(items[0].type, SetActionType::PROPERTY);
	EXPECT_EQ(items[0].variable, "n");
	EXPECT_EQ(items[0].key, "name");
}

TEST_F(PatternBuilderParsingTest, ExtractSetItems_MapMerge) {
	auto* ctx = getSetStatement("MATCH (n) SET n += {name: 'Bob', age: 25} RETURN n");
	ASSERT_NE(ctx, nullptr);
	auto items = PatternBuilder::extractSetItems(ctx);
	EXPECT_GE(items.size(), 2u);
	EXPECT_EQ(items[0].type, SetActionType::PROPERTY);
}

TEST_F(PatternBuilderParsingTest, ExtractSetItems_MapMergeNonMap) {
	auto* ctx = getSetStatement("MATCH (n), (m) SET n += m RETURN n");
	ASSERT_NE(ctx, nullptr);
	auto items = PatternBuilder::extractSetItems(ctx);
	EXPECT_GE(items.size(), 1u);
	EXPECT_EQ(items[0].type, SetActionType::MAP_MERGE);
}

// OPTIONAL MATCH with anonymous nodes/edges to hit empty variable branches
TEST_F(PatternBuilderParsingTest, OptionalMatchAnonymousNodesAndEdge) {
	auto root = makeRootOp();
	auto* pat = getMatchPattern("OPTIONAL MATCH ()-->() RETURN 1");
	auto result = PatternBuilder::buildMatchPattern(
		pat, std::move(root), nullPlanner(), nullptr, true);
	EXPECT_NE(result, nullptr);
}

// OPTIONAL MATCH with anonymous edge but named nodes
TEST_F(PatternBuilderParsingTest, OptionalMatchAnonymousEdge) {
	auto root = makeRootOp();
	auto* pat = getMatchPattern("OPTIONAL MATCH (a)-->(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(
		pat, std::move(root), nullPlanner(), nullptr, true);
	EXPECT_NE(result, nullptr);
}

// OPTIONAL MATCH with anonymous edge having relDetail (line 608 False)
TEST_F(PatternBuilderParsingTest, OptionalMatchAnonymousRelDetail) {
	auto root = makeRootOp();
	auto* pat = getMatchPattern("OPTIONAL MATCH (a)-[:KNOWS]->(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(
		pat, std::move(root), nullPlanner(), nullptr, true);
	EXPECT_NE(result, nullptr);
}

// OPTIONAL MATCH with anonymous edge variable but with relDetail
TEST_F(PatternBuilderParsingTest, OptionalMatchEmptyEdgeBrackets) {
	auto root = makeRootOp();
	auto* pat = getMatchPattern("OPTIONAL MATCH (a)-[]->(b) RETURN a, b");
	if (pat) {
		auto result = PatternBuilder::buildMatchPattern(
			pat, std::move(root), nullPlanner(), nullptr, true);
		EXPECT_NE(result, nullptr);
	}
}

// Var-length with neither min nor max (just *..)
TEST_F(PatternBuilderParsingTest, VarLengthOnlyDoubleDot) {
	auto* pat = getMatchPattern("MATCH (a)-[r:KNOWS*..]->(b) RETURN a, b");
	if (pat) {
		auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
		EXPECT_NE(result, nullptr);
	}
}

// OPTIONAL MATCH with duplicate variables across pattern parts
// Triggers duplicate detection in collectVariablesFromPatternElement (lines 594, 609, 618 False)
TEST_F(PatternBuilderParsingTest, OptionalMatchDuplicateVars) {
	auto root = makeRootOp();
	auto* pat = getMatchPattern("OPTIONAL MATCH (a)-[r:KNOWS]->(b), (a)-[r:LIKES]->(b) RETURN a, b");
	auto result = PatternBuilder::buildMatchPattern(
		pat, std::move(root), nullPlanner(), nullptr, true);
	EXPECT_NE(result, nullptr);
}

// Named path with anonymous head and target (line 114 False, 123 False)
TEST_F(PatternBuilderParsingTest, NamedPathAllAnonymous) {
	auto* pat = getMatchPattern("MATCH p = ()-->() RETURN p");
	auto result = PatternBuilder::buildMatchPattern(pat, nullptr, nullPlanner());
	EXPECT_NE(result, nullptr);
}

// CREATE with bare edge (no relDetail) — triggers line 358 False
TEST_F(PatternBuilderParsingTest, CreateBareEdge) {
	auto* pat = getCreatePattern("CREATE (a)-->(b)");
	if (pat) {
		auto result = PatternBuilder::buildCreatePattern(pat, nullptr, nullPlanner());
		EXPECT_NE(result, nullptr);
	}
}

// MERGE multi-hop edge to trigger isLastChain=false (line 474/475 False)
TEST_F(PatternBuilderParsingTest, MergeMultiHop) {
	auto* pp = getMergePatternPart("MERGE (a:X)-[r1:R]->(b:Y)-[r2:S]->(c:Z)");
	if (pp) {
		std::vector<SetItem> empty;
		auto result = PatternBuilder::buildMergePattern(pp, empty, empty, nullPlanner());
		EXPECT_NE(result, nullptr);
	}
}

// MERGE multi-hop with set items to trigger isLastChain branching
TEST_F(PatternBuilderParsingTest, MergeMultiHopWithSetItems) {
	auto* pp = getMergePatternPart("MERGE (a:X)-[r1:R]->(b:Y)-[r2:S]->(c:Z)");
	if (pp) {
		auto* exprCtx = getReturnExpr("RETURN 1");
		ASSERT_NE(exprCtx, nullptr);
		auto expr = ExpressionBuilder::buildExpression(exprCtx);
		auto exprShared = std::shared_ptr<graph::query::expressions::Expression>(expr.release());

		std::vector<SetItem> onCreate;
		onCreate.emplace_back(SetActionType::PROPERTY, "r2", "w", exprShared);
		std::vector<SetItem> onMatch;
		onMatch.emplace_back(SetActionType::PROPERTY, "r2", "c", exprShared);

		auto result = PatternBuilder::buildMergePattern(pp, onCreate, onMatch, nullPlanner());
		EXPECT_NE(result, nullptr);
	}
}

TEST_F(PatternBuilderParsingTest, ExtractSetItems_Label) {
	auto* ctx = getSetStatement("MATCH (n) SET n:Person RETURN n");
	ASSERT_NE(ctx, nullptr);
	auto items = PatternBuilder::extractSetItems(ctx);
	EXPECT_GE(items.size(), 1u);
	EXPECT_EQ(items[0].type, SetActionType::LABEL);
}
