/**
 * @file test_CypherASTBuilder_ASTExtraction.cpp
 * @brief Direct parser-to-AST tests for Cypher clause extraction.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "CypherLexer.h"
#include "CypherParser.h"
#include "antlr4-runtime.h"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/ir/CypherASTBuilder.hpp"

using graph::query::ir::CypherASTBuilder;

namespace {

class CypherASTBuilderExtractionTest : public ::testing::Test {
protected:
	struct ParseResult {
		std::unique_ptr<antlr4::ANTLRInputStream> input;
		std::unique_ptr<CypherLexer> lexer;
		std::unique_ptr<antlr4::CommonTokenStream> tokens;
		std::unique_ptr<CypherParser> parser;
		CypherParser::CypherContext* tree = nullptr;
	};

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

	CypherParser::SingleQueryContext* singleQuery(const std::string& query) {
		auto* tree = parse(query);
		auto* regular = dynamic_cast<CypherParser::RegularStatementContext*>(tree->statement());
		if (!regular || !regular->query() || !regular->query()->regularQuery()) {
			return nullptr;
		}
		return regular->query()->regularQuery()->singleQuery(0);
	}

	CypherParser::MatchStatementContext* matchStatement(const std::string& query) {
		auto* sq = singleQuery(query);
		if (!sq) return nullptr;
		for (auto* clause : sq->readingClause()) {
			if (clause->matchStatement()) return clause->matchStatement();
		}
		return nullptr;
	}

	CypherParser::CreateStatementContext* createStatement(const std::string& query) {
		auto* sq = singleQuery(query);
		if (!sq) return nullptr;
		for (auto* clause : sq->updatingClause()) {
			if (clause->createStatement()) return clause->createStatement();
		}
		return nullptr;
	}

	CypherParser::SetStatementContext* setStatement(const std::string& query) {
		auto* sq = singleQuery(query);
		if (!sq) return nullptr;
		for (auto* clause : sq->updatingClause()) {
			if (clause->setStatement()) return clause->setStatement();
		}
		return nullptr;
	}

	CypherParser::CreateNodeConstraintContext* nodeConstraint(const std::string& query) {
		auto* tree = parse(query);
		auto* admin = dynamic_cast<CypherParser::AdminStatementContext*>(tree->statement());
		if (!admin) return nullptr;
		auto* create = admin->administrationStatement()->createConstraintStatement();
		return dynamic_cast<CypherParser::CreateNodeConstraintContext*>(create);
	}

	std::vector<std::unique_ptr<ParseResult>> parseResults;
};

} // namespace

TEST_F(CypherASTBuilderExtractionTest, MatchExtractsRelationshipDirectionsAndBarePatterns) {
	auto* ctx = matchStatement(
		"MATCH (a)<-[inRel:LIKES]-(b), (c)-[outRel]->(d), "
		"(e)-[bothRel]-(f), (g)--(h) RETURN a");
	ASSERT_NE(ctx, nullptr);

	auto clause = CypherASTBuilder::buildMatchClause(ctx);
	ASSERT_EQ(clause.patterns.size(), 4u);

	EXPECT_EQ(clause.patterns[0].element.chain[0].relationship.direction, "in");
	EXPECT_EQ(clause.patterns[1].element.chain[0].relationship.direction, "out");
	EXPECT_EQ(clause.patterns[2].element.chain[0].relationship.direction, "both");
	EXPECT_EQ(clause.patterns[3].element.chain[0].relationship.direction, "both");
	EXPECT_TRUE(clause.patterns[3].element.chain[0].relationship.type.empty());
	EXPECT_TRUE(clause.patterns[3].element.chain[0].relationship.properties.empty());
	EXPECT_TRUE(clause.patterns[3].element.headNode.properties.empty());
}

TEST_F(CypherASTBuilderExtractionTest, MatchSeparatesLiteralNullAndParameterizedProperties) {
	auto* ctx = matchStatement(
		"MATCH (n:User {id: 7, nickname: null, runtime: $value})"
		"-[r:KNOWS {since: 2020, active: null, score: $score}]->"
		"(m {name: 'Bob'}) RETURN n");
	ASSERT_NE(ctx, nullptr);

	auto clause = CypherASTBuilder::buildMatchClause(ctx);
	ASSERT_EQ(clause.patterns.size(), 1u);
	const auto& head = clause.patterns[0].element.headNode;
	ASSERT_EQ(head.properties.size(), 2u);
	EXPECT_EQ(head.properties[0].first, "id");
	EXPECT_EQ(head.properties[1].first, "nickname");
	EXPECT_EQ(head.propertyExpressions.size(), 1u);
	EXPECT_NE(head.propertyExpressions.at("runtime"), nullptr);

	const auto& rel = clause.patterns[0].element.chain[0].relationship;
	EXPECT_EQ(rel.properties.size(), 2u);
	EXPECT_TRUE(rel.properties.contains("since"));
	EXPECT_TRUE(rel.properties.contains("active"));
	ASSERT_EQ(rel.propertyExpressions.size(), 1u);
	EXPECT_NE(rel.propertyExpressions.at("score"), nullptr);
}

TEST_F(CypherASTBuilderExtractionTest, CreateExtractsExpressionPropertiesAndBareHeadNode) {
	auto* expressionCtx = createStatement("MATCH (m) CREATE (n:Copy {name: m.name}) RETURN n");
	ASSERT_NE(expressionCtx, nullptr);
	auto expressionClause = CypherASTBuilder::buildCreateClause(expressionCtx);
	ASSERT_EQ(expressionClause.patterns.size(), 1u);
	const auto& expressionNode = expressionClause.patterns[0].element.headNode;
	EXPECT_TRUE(expressionNode.properties.empty());
	ASSERT_EQ(expressionNode.propertyExpressions.size(), 1u);
	EXPECT_NE(expressionNode.propertyExpressions.at("name"), nullptr);

	auto* bareCtx = createStatement("CREATE (a)-[r:REL]->(b)");
	ASSERT_NE(bareCtx, nullptr);
	auto bareClause = CypherASTBuilder::buildCreateClause(bareCtx);
	ASSERT_EQ(bareClause.patterns.size(), 1u);
	EXPECT_TRUE(bareClause.patterns[0].element.headNode.properties.empty());
	EXPECT_EQ(bareClause.patterns[0].element.chain[0].relationship.direction, "out");
}

TEST_F(CypherASTBuilderExtractionTest, SetExtractsParenthesizedPropertyMapMergeAndLabel) {
	auto* ctx = setStatement("MATCH (n), (m) SET (n).name = 'Alice', n += m, n += {age: 30}, n:Person RETURN n");
	ASSERT_NE(ctx, nullptr);

	auto clause = CypherASTBuilder::buildSetClause(ctx);
	ASSERT_EQ(clause.items.size(), 4u);

	EXPECT_EQ(clause.items[0].type, graph::query::ir::SetItemType::SIT_PROPERTY);
	EXPECT_TRUE(clause.items[0].variable.empty());
	EXPECT_EQ(clause.items[0].key, "name");
	EXPECT_NE(clause.items[0].expression, nullptr);

	EXPECT_EQ(clause.items[1].type, graph::query::ir::SetItemType::SIT_MAP_MERGE);
	EXPECT_EQ(clause.items[1].variable, "n");
	EXPECT_NE(clause.items[1].expression, nullptr);

	EXPECT_EQ(clause.items[2].type, graph::query::ir::SetItemType::SIT_PROPERTY);
	EXPECT_EQ(clause.items[2].key, "age");

	EXPECT_EQ(clause.items[3].type, graph::query::ir::SetItemType::SIT_LABEL);
	EXPECT_EQ(clause.items[3].key, "Person");
}

TEST_F(CypherASTBuilderExtractionTest, NodeConstraintBodiesExtractUniqueTypeAndNodeKey) {
	auto* uniqueCtx = nodeConstraint("CREATE CONSTRAINT c_unique FOR (n:Person) REQUIRE n.email IS UNIQUE");
	ASSERT_NE(uniqueCtx, nullptr);
	auto unique = CypherASTBuilder::buildCreateNodeConstraintClause(uniqueCtx);
	EXPECT_EQ(unique.constraintType, "unique");
	ASSERT_EQ(unique.properties.size(), 1u);
	EXPECT_EQ(unique.properties[0], "email");

	auto* typeCtx = nodeConstraint("CREATE CONSTRAINT c_type FOR (n:Person) REQUIRE n.age IS ::INTEGER");
	ASSERT_NE(typeCtx, nullptr);
	auto type = CypherASTBuilder::buildCreateNodeConstraintClause(typeCtx);
	EXPECT_EQ(type.constraintType, "property_type");
	EXPECT_EQ(type.typeName, "INTEGER");

	auto* keyCtx = nodeConstraint("CREATE CONSTRAINT c_key FOR (n:Person) REQUIRE (n.id, n.email) IS NODE KEY");
	ASSERT_NE(keyCtx, nullptr);
	auto key = CypherASTBuilder::buildCreateNodeConstraintClause(keyCtx);
	EXPECT_EQ(key.constraintType, "node_key");
	ASSERT_EQ(key.properties.size(), 2u);
	EXPECT_EQ(key.properties[0], "id");
	EXPECT_EQ(key.properties[1], "email");
}
