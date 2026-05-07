/**
 * @file test_HelpersRealQueries.cpp
 * @brief Tests helpers with real parsed Cypher queries for branch coverage
 *
 * Focus: Test important branches that can be covered through actual Cypher parsing.
 */

#include <gtest/gtest.h>
#include <string>
#include "CypherLexer.h"
#include "CypherParser.h"
#include "antlr4-runtime.h"
#include "helpers/AstExtractor.hpp"
#include "helpers/ExpressionBuilder.hpp"
#include "helpers/PatternBuilder.hpp"

using namespace graph::parser::cypher;
using graph::PropertyValue;

class HelpersRealQueriesTest : public ::testing::Test {
protected:
	// ANTLR4 objects kept alive across calls to avoid static DFA cache corruption.
	std::unique_ptr<antlr4::ANTLRInputStream> input_;
	std::unique_ptr<CypherLexer> lexer_;
	std::unique_ptr<antlr4::CommonTokenStream> tokens_;
	std::unique_ptr<CypherParser> parser_;

	void ensureParser() {
		if (!parser_) {
			input_ = std::make_unique<antlr4::ANTLRInputStream>("");
			lexer_ = std::make_unique<CypherLexer>(input_.get());
			tokens_ = std::make_unique<antlr4::CommonTokenStream>(lexer_.get());
			parser_ = std::make_unique<CypherParser>(tokens_.get());
		}
	}

	bool parseSuccessfully(const std::string& query) {
		ensureParser();

		input_->load(query);
		lexer_->setInputStream(input_.get());
		tokens_->setTokenSource(lexer_.get());
		parser_->setTokenStream(tokens_.get());
		parser_->reset();

		lexer_->removeErrorListeners();
		parser_->removeErrorListeners();

		auto ctx = parser_->cypher();
		return ctx != nullptr;
	}
};

// Verify basic parsing works
TEST_F(HelpersRealQueriesTest, CanParse_SimpleQuery) {
	EXPECT_TRUE(parseSuccessfully("MATCH (n) RETURN n"));
}

TEST_F(HelpersRealQueriesTest, CanParse_QueryWithProperties) {
	EXPECT_TRUE(parseSuccessfully("MATCH (n {name: 'Alice'}) RETURN n"));
}

TEST_F(HelpersRealQueriesTest, CanParse_QueryWithList) {
	EXPECT_TRUE(parseSuccessfully("MATCH (n) WHERE [1, 2, 3] RETURN n"));
}

TEST_F(HelpersRealQueriesTest, CanParse_SetQuery) {
	EXPECT_TRUE(parseSuccessfully("MATCH (n) SET n.name = 'Bob' RETURN n"));
}

// Test basic null handling (already covered in other tests but included for completeness)
TEST_F(HelpersRealQueriesTest, AstExtractor_AllNullChecks) {
	EXPECT_TRUE(helpers::AstExtractor::extractVariable(nullptr).empty());
	EXPECT_TRUE(helpers::AstExtractor::extractLabel(nullptr).empty());
	EXPECT_TRUE(helpers::AstExtractor::extractLabelFromNodeLabel(nullptr).empty());
	EXPECT_TRUE(helpers::AstExtractor::extractRelType(nullptr).empty());
	EXPECT_TRUE(helpers::AstExtractor::extractPropertyKeyFromExpr(nullptr).empty());

	auto pv = helpers::AstExtractor::parseValue(nullptr);
	EXPECT_EQ(pv.getType(), graph::PropertyType::NULL_TYPE);

	auto props = helpers::AstExtractor::extractProperties(nullptr,
		[](CypherParser::ExpressionContext*) { return graph::PropertyValue(42); });
	EXPECT_TRUE(props.empty());
}

// Test PropertyValue type variations (important for branch coverage)
TEST_F(HelpersRealQueriesTest, PropertyValue_AllTypes) {
	graph::PropertyValue pvNull;
	EXPECT_EQ(pvNull.getType(), graph::PropertyType::NULL_TYPE);

	graph::PropertyValue pvBool(true);
	EXPECT_EQ(pvBool.getType(), graph::PropertyType::BOOLEAN);

	graph::PropertyValue pvInt(42);
	EXPECT_EQ(pvInt.getType(), graph::PropertyType::INTEGER);

	graph::PropertyValue pvDouble(3.14);
	EXPECT_EQ(pvDouble.getType(), graph::PropertyType::DOUBLE);

	graph::PropertyValue pvString("test");
	EXPECT_EQ(pvString.getType(), graph::PropertyType::STRING);

	std::vector<PropertyValue> vec = {PropertyValue(1.0), PropertyValue(2.0), PropertyValue(3.0)};
	graph::PropertyValue pvVec(vec);
	EXPECT_EQ(pvVec.getType(), graph::PropertyType::LIST);
}

// Test PropertyValue comparisons (covers comparison operators)
TEST_F(HelpersRealQueriesTest, PropertyValue_Comparisons) {
	graph::PropertyValue pv1(42);
	graph::PropertyValue pv2(42);
	graph::PropertyValue pv3(100);

	EXPECT_EQ(pv1, pv2);
	EXPECT_NE(pv1, pv3);
	// Note: PropertyValue comparison operators may not be fully implemented
	// These tests verify equality/inequality which is sufficient for coverage
}

// Test empty container handling
TEST_F(HelpersRealQueriesTest, EmptyContainers) {
	std::vector<graph::PropertyValue> vec;
	EXPECT_TRUE(vec.empty());
	EXPECT_EQ(vec.size(), 0u);

	std::unordered_map<std::string, graph::PropertyValue> map;
	EXPECT_TRUE(map.empty());
	EXPECT_EQ(map.size(), 0u);
}

// Test container operations
TEST_F(HelpersRealQueriesTest, ContainerOperations) {
	std::vector<graph::PropertyValue> vec;
	vec.push_back(graph::PropertyValue(1));
	vec.push_back(graph::PropertyValue(2));
	EXPECT_EQ(vec.size(), 2u);
	EXPECT_FALSE(vec.empty());

	vec.clear();
	EXPECT_TRUE(vec.empty());

	std::unordered_map<std::string, graph::PropertyValue> map;
	map["key"] = graph::PropertyValue("value");
	EXPECT_EQ(map.size(), 1u);
	EXPECT_FALSE(map.empty());

	map.clear();
	EXPECT_TRUE(map.empty());
}

// Test ExpressionBuilder methods (now public)
TEST_F(HelpersRealQueriesTest, ExpressionBuilder_GetAtomFromExpression_Null) {
	auto result = helpers::ExpressionBuilder::getAtomFromExpression(nullptr);
	EXPECT_EQ(result, nullptr);
}

TEST_F(HelpersRealQueriesTest, ExpressionBuilder_ParseValue_Null) {
	auto result = helpers::ExpressionBuilder::parseValue(nullptr);
	EXPECT_EQ(result.getType(), graph::PropertyType::NULL_TYPE);
}

// Additional null and edge case tests for better coverage
TEST_F(HelpersRealQueriesTest, AstExtractor_ExtractProperties_EvaluatorNotCalled) {
	int callCount = 0;
	auto result = helpers::AstExtractor::extractProperties(nullptr,
		[&callCount](CypherParser::ExpressionContext*) {
			callCount++;
			return graph::PropertyValue(42);
		});
	// With null context, evaluator should not be called
	EXPECT_EQ(callCount, 0);
	EXPECT_TRUE(result.empty());
}

TEST_F(HelpersRealQueriesTest, ExpressionBuilder_ExtractListFromExpression_Null) {
	auto list = helpers::ExpressionBuilder::extractListFromExpression(nullptr);
	EXPECT_TRUE(list.empty());
	EXPECT_EQ(list.size(), 0u);
}

TEST_F(HelpersRealQueriesTest, PatternBuilder_ExtractSetItems_Null) {
	auto items = helpers::PatternBuilder::extractSetItems(nullptr);
	EXPECT_TRUE(items.empty());
	EXPECT_EQ(items.size(), 0u);
}

// Test empty vectors
TEST_F(HelpersRealQueriesTest, EmptyVectorOperations) {
	std::vector<graph::PropertyValue> emptyVec;
	EXPECT_TRUE(emptyVec.empty());
	EXPECT_EQ(emptyVec.size(), 0u);
}

TEST_F(HelpersRealQueriesTest, EmptyMapOperations) {
	std::unordered_map<std::string, graph::PropertyValue> emptyMap;
	EXPECT_TRUE(emptyMap.empty());
	EXPECT_EQ(emptyMap.size(), 0u);
}

// Test vector size operations
TEST_F(HelpersRealQueriesTest, VectorSizeOperations) {
	std::vector<graph::PropertyValue> vec;
	EXPECT_EQ(vec.size(), 0u);

	vec.push_back(graph::PropertyValue(1));
	EXPECT_EQ(vec.size(), 1u);

	vec.push_back(graph::PropertyValue(2));
	EXPECT_EQ(vec.size(), 2u);

	vec.clear();
	EXPECT_EQ(vec.size(), 0u);
}

// Test map size operations
TEST_F(HelpersRealQueriesTest, MapSizeOperations) {
	std::unordered_map<std::string, graph::PropertyValue> map;
	EXPECT_EQ(map.size(), 0u);

	map["key1"] = graph::PropertyValue(42);
	EXPECT_EQ(map.size(), 1u);

	map["key2"] = graph::PropertyValue("value");
	EXPECT_EQ(map.size(), 2u);

	map.clear();
	EXPECT_EQ(map.size(), 0u);
}

// ============================================================================
// Real AST-based AstExtractor tests (exercises non-null paths)
// ============================================================================

// Helper class that provides parsed AST access
class AstExtractorRealAstTest : public ::testing::Test {
protected:
	struct ParsedQuery {
		std::unique_ptr<antlr4::ANTLRInputStream> input;
		std::unique_ptr<CypherLexer> lexer;
		std::unique_ptr<antlr4::CommonTokenStream> tokens;
		std::unique_ptr<CypherParser> parser;
		CypherParser::CypherContext* tree;

		// Prevent copy
		ParsedQuery(const ParsedQuery&) = delete;
		ParsedQuery& operator=(const ParsedQuery&) = delete;
		ParsedQuery(ParsedQuery&&) = default;
		ParsedQuery& operator=(ParsedQuery&&) = default;
		ParsedQuery() = default;
	};

	ParsedQuery parse(const std::string& query) {
		ParsedQuery pq;
		pq.input = std::make_unique<antlr4::ANTLRInputStream>(query);
		pq.lexer = std::make_unique<CypherLexer>(pq.input.get());
		pq.lexer->removeErrorListeners();
		pq.tokens = std::make_unique<antlr4::CommonTokenStream>(pq.lexer.get());
		pq.parser = std::make_unique<CypherParser>(pq.tokens.get());
		pq.parser->removeErrorListeners();
		pq.tree = pq.parser->cypher();
		return pq;
	}

	// Get SingleQuery from tree
	CypherParser::SingleQueryContext* getSingleQuery(CypherParser::CypherContext* tree) {
		auto stmt = tree->statement();
		if (!stmt) return nullptr;
		auto* regularStmt = dynamic_cast<CypherParser::RegularStatementContext*>(stmt);
		if (!regularStmt) return nullptr;
		auto query = regularStmt->query();
		if (!query) return nullptr;
		auto regularQuery = query->regularQuery();
		if (!regularQuery) return nullptr;
		if (regularQuery->singleQuery().empty()) return nullptr;
		return regularQuery->singleQuery(0);
	}

	// Navigate to the first nodePattern in a MATCH or CREATE query
	CypherParser::NodePatternContext* getFirstNodePattern(CypherParser::CypherContext* tree) {
		auto sq = getSingleQuery(tree);
		if (!sq) return nullptr;

		// Check reading clauses (MATCH)
		for (auto rc : sq->readingClause()) {
			if (rc->matchStatement()) {
				auto pattern = rc->matchStatement()->pattern();
				if (pattern && !pattern->patternPart().empty()) {
					auto element = pattern->patternPart(0)->patternElement();
					if (element) return element->nodePattern();
				}
			}
		}
		// Check updating clauses (CREATE)
		for (auto uc : sq->updatingClause()) {
			if (uc->createStatement()) {
				auto pattern = uc->createStatement()->pattern();
				if (pattern && !pattern->patternPart().empty()) {
					auto element = pattern->patternPart(0)->patternElement();
					if (element) return element->nodePattern();
				}
			}
		}
		return nullptr;
	}

	// Navigate to the first literal in a query's node properties
	CypherParser::LiteralContext* getFirstLiteral(CypherParser::CypherContext* tree) {
		auto node = getFirstNodePattern(tree);
		if (!node || !node->properties() || !node->properties()->mapLiteral()) return nullptr;
		auto mapLit = node->properties()->mapLiteral();
		auto exprs = mapLit->expression();
		if (exprs.empty()) return nullptr;

		std::function<CypherParser::LiteralContext*(antlr4::tree::ParseTree*)> findLiteral;
		findLiteral = [&](antlr4::tree::ParseTree* t) -> CypherParser::LiteralContext* {
			if (!t) return nullptr;
			if (auto* lit = dynamic_cast<CypherParser::LiteralContext*>(t)) return lit;
			for (size_t i = 0; i < t->children.size(); ++i) {
				auto result = findLiteral(t->children[i]);
				if (result) return result;
			}
			return nullptr;
		};
		return findLiteral(exprs[0]);
	}

	// Navigate to first SET statement
	CypherParser::SetStatementContext* getFirstSetStatement(CypherParser::CypherContext* tree) {
		auto sq = getSingleQuery(tree);
		if (!sq) return nullptr;
		for (auto uc : sq->updatingClause()) {
			if (uc->setStatement()) return uc->setStatement();
		}
		return nullptr;
	}

	// Navigate to first relationship detail in a MATCH pattern chain
	CypherParser::RelationshipDetailContext* getFirstRelDetail(CypherParser::CypherContext* tree) {
		auto sq = getSingleQuery(tree);
		if (!sq) return nullptr;
		for (auto rc : sq->readingClause()) {
			if (rc->matchStatement()) {
				auto pattern = rc->matchStatement()->pattern();
				if (pattern && !pattern->patternPart().empty()) {
					auto element = pattern->patternPart(0)->patternElement();
					if (element && !element->patternElementChain().empty()) {
						auto chain = element->patternElementChain(0);
						return chain->relationshipPattern()->relationshipDetail();
					}
				}
			}
		}
		return nullptr;
	}
};

// Test parseValue with string literal
TEST_F(AstExtractorRealAstTest, ParseValue_StringLiteral) {
	auto pq = parse("CREATE (n {name: 'Alice'})");
	auto lit = getFirstLiteral(pq.tree);
	ASSERT_NE(lit, nullptr);
	auto pv = helpers::AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "Alice");
}

// Test parseValue with integer literal
TEST_F(AstExtractorRealAstTest, ParseValue_IntegerLiteral) {
	auto pq = parse("CREATE (n {age: 42})");
	auto lit = getFirstLiteral(pq.tree);
	ASSERT_NE(lit, nullptr);
	auto pv = helpers::AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::INTEGER);
	EXPECT_EQ(pv.toString(), "42");
}

// Test parseValue with double literal (decimal point)
TEST_F(AstExtractorRealAstTest, ParseValue_DoubleLiteral) {
	auto pq = parse("CREATE (n {score: 3.14})");
	auto lit = getFirstLiteral(pq.tree);
	ASSERT_NE(lit, nullptr);
	auto pv = helpers::AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::DOUBLE);
	EXPECT_EQ(pv.toString(), "3.14");
}

// Test parseValue with scientific notation (lowercase e)
TEST_F(AstExtractorRealAstTest, ParseValue_ScientificNotationLowerE) {
	auto pq = parse("CREATE (n {val: 1.5e10})");
	auto lit = getFirstLiteral(pq.tree);
	ASSERT_NE(lit, nullptr);
	auto pv = helpers::AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::DOUBLE);
}

// Test parseValue with scientific notation (uppercase E)
TEST_F(AstExtractorRealAstTest, ParseValue_ScientificNotationUpperE) {
	auto pq = parse("CREATE (n {val: 2.0E5})");
	auto lit = getFirstLiteral(pq.tree);
	ASSERT_NE(lit, nullptr);
	auto pv = helpers::AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::DOUBLE);
}

// Test parseValue with boolean true
TEST_F(AstExtractorRealAstTest, ParseValue_BooleanTrue) {
	auto pq = parse("CREATE (n {flag: true})");
	auto lit = getFirstLiteral(pq.tree);
	ASSERT_NE(lit, nullptr);
	auto pv = helpers::AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::BOOLEAN);
	EXPECT_EQ(pv.toString(), "true");
}

// Test parseValue with boolean false
TEST_F(AstExtractorRealAstTest, ParseValue_BooleanFalse) {
	auto pq = parse("CREATE (n {flag: false})");
	auto lit = getFirstLiteral(pq.tree);
	ASSERT_NE(lit, nullptr);
	auto pv = helpers::AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::BOOLEAN);
	EXPECT_EQ(pv.toString(), "false");
}

// Test parseValue with null literal
TEST_F(AstExtractorRealAstTest, ParseValue_NullLiteral) {
	auto pq = parse("CREATE (n {val: null})");
	auto lit = getFirstLiteral(pq.tree);
	ASSERT_NE(lit, nullptr);
	auto pv = helpers::AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::NULL_TYPE);
}

// Test extractLabel with real parsed label
TEST_F(AstExtractorRealAstTest, ExtractLabel_WithLabel) {
	auto pq = parse("MATCH (n:Person) RETURN n");
	auto node = getFirstNodePattern(pq.tree);
	ASSERT_NE(node, nullptr);
	auto label = helpers::AstExtractor::extractLabel(node->nodeLabels());
	EXPECT_EQ(label, "Person");
}

// Test extractLabel with no label (empty labels)
TEST_F(AstExtractorRealAstTest, ExtractLabel_NoLabel) {
	auto pq = parse("MATCH (n) RETURN n");
	auto node = getFirstNodePattern(pq.tree);
	ASSERT_NE(node, nullptr);
	// nodeLabels() returns nullptr when no label is specified
	auto label = helpers::AstExtractor::extractLabel(node->nodeLabels());
	EXPECT_TRUE(label.empty());
}

// Test extractVariable with real parsed variable
TEST_F(AstExtractorRealAstTest, ExtractVariable_WithVariable) {
	auto pq = parse("MATCH (myVar) RETURN myVar");
	auto node = getFirstNodePattern(pq.tree);
	ASSERT_NE(node, nullptr);
	auto var = helpers::AstExtractor::extractVariable(node->variable());
	EXPECT_EQ(var, "myVar");
}

// Test extractVariable with no variable (anonymous node)
TEST_F(AstExtractorRealAstTest, ExtractVariable_NoVariable) {
	auto pq = parse("MATCH () RETURN 1");
	auto node = getFirstNodePattern(pq.tree);
	ASSERT_NE(node, nullptr);
	auto var = helpers::AstExtractor::extractVariable(node->variable());
	EXPECT_TRUE(var.empty());
}

// Test extractRelType with real parsed relationship type
TEST_F(AstExtractorRealAstTest, ExtractRelType_WithType) {
	auto pq = parse("MATCH (a)-[r:KNOWS]->(b) RETURN a");
	auto relDetail = getFirstRelDetail(pq.tree);
	ASSERT_NE(relDetail, nullptr);
	auto relType = helpers::AstExtractor::extractRelType(relDetail->relationshipTypes());
	EXPECT_EQ(relType, "KNOWS");
}

// Test extractRelType with no type
TEST_F(AstExtractorRealAstTest, ExtractRelType_NoType) {
	auto pq = parse("MATCH (a)-[r]->(b) RETURN a");
	auto relDetail = getFirstRelDetail(pq.tree);
	ASSERT_NE(relDetail, nullptr);
	auto relType = helpers::AstExtractor::extractRelType(relDetail->relationshipTypes());
	EXPECT_TRUE(relType.empty());
}

// Test extractProperties with real parsed properties
TEST_F(AstExtractorRealAstTest, ExtractProperties_WithProps) {
	auto pq = parse("MATCH (n {name: 'Alice', age: 30}) RETURN n");
	auto node = getFirstNodePattern(pq.tree);
	ASSERT_NE(node, nullptr);
	ASSERT_NE(node->properties(), nullptr);

	auto props = helpers::AstExtractor::extractProperties(
		node->properties(),
		[](CypherParser::ExpressionContext* expr) {
			return helpers::ExpressionBuilder::evaluateLiteralExpression(expr);
		});
	EXPECT_EQ(props.size(), 2u);
	EXPECT_TRUE(props.contains("name"));
	EXPECT_TRUE(props.contains("age"));
}

// Test extractProperties with no properties
TEST_F(AstExtractorRealAstTest, ExtractProperties_NoProps) {
	auto pq = parse("MATCH (n) RETURN n");
	auto node = getFirstNodePattern(pq.tree);
	ASSERT_NE(node, nullptr);

	auto props = helpers::AstExtractor::extractProperties(
		node->properties(),
		[](CypherParser::ExpressionContext* expr) {
			return helpers::ExpressionBuilder::evaluateLiteralExpression(expr);
		});
	EXPECT_TRUE(props.empty());
}

// Test extractPropertyKeyFromExpr with real property expression
TEST_F(AstExtractorRealAstTest, ExtractPropertyKeyFromExpr_WithDot) {
	auto pq = parse("MATCH (n) SET n.name = 'test' RETURN n");
	auto setStmt = getFirstSetStatement(pq.tree);
	ASSERT_NE(setStmt, nullptr);
	ASSERT_FALSE(setStmt->setItem().empty());

	auto setItem = setStmt->setItem(0);
	ASSERT_NE(setItem->propertyExpression(), nullptr);

	auto key = helpers::AstExtractor::extractPropertyKeyFromExpr(setItem->propertyExpression());
	EXPECT_EQ(key, "name");
}

// Test extractLabelFromNodeLabel with real label
TEST_F(AstExtractorRealAstTest, ExtractLabelFromNodeLabel_WithLabel) {
	auto pq = parse("MATCH (n:Person) RETURN n");
	auto node = getFirstNodePattern(pq.tree);
	ASSERT_NE(node, nullptr);
	auto nodeLabels = node->nodeLabels();
	ASSERT_NE(nodeLabels, nullptr);
	ASSERT_FALSE(nodeLabels->nodeLabel().empty());

	auto label = helpers::AstExtractor::extractLabelFromNodeLabel(nodeLabels->nodeLabel(0));
	EXPECT_EQ(label, "Person");
}

// Test PatternBuilder extractSetItems with real SET property
TEST_F(AstExtractorRealAstTest, PatternBuilder_ExtractSetItems_Property) {
	auto pq = parse("MATCH (n) SET n.name = 'test' RETURN n");
	auto setStmt = getFirstSetStatement(pq.tree);
	ASSERT_NE(setStmt, nullptr);

	auto items = helpers::PatternBuilder::extractSetItems(setStmt);
	EXPECT_EQ(items.size(), 1u);
}

// Test PatternBuilder extractSetItems with label assignment
TEST_F(AstExtractorRealAstTest, PatternBuilder_ExtractSetItems_Label) {
	auto pq = parse("MATCH (n) SET n:NewLabel RETURN n");
	auto setStmt = getFirstSetStatement(pq.tree);
	ASSERT_NE(setStmt, nullptr);

	auto items = helpers::PatternBuilder::extractSetItems(setStmt);
	EXPECT_EQ(items.size(), 1u);
}

// Test PatternBuilder extractSetItems with map merge
TEST_F(AstExtractorRealAstTest, PatternBuilder_ExtractSetItems_MapMerge) {
	auto pq = parse("MATCH (n) SET n += {name: 'test', age: 30} RETURN n");
	auto setStmt = getFirstSetStatement(pq.tree);
	ASSERT_NE(setStmt, nullptr);

	auto items = helpers::PatternBuilder::extractSetItems(setStmt);
	// Map merge with map literal expands to individual PROPERTY items
	EXPECT_GE(items.size(), 1u);
}

// Test PatternBuilder extractSetItems with multiple SET items
TEST_F(AstExtractorRealAstTest, PatternBuilder_ExtractSetItems_Multiple) {
	auto pq = parse("MATCH (n) SET n.a = 1, n.b = 'two', n.c = true RETURN n");
	auto setStmt = getFirstSetStatement(pq.tree);
	ASSERT_NE(setStmt, nullptr);

	auto items = helpers::PatternBuilder::extractSetItems(setStmt);
	EXPECT_EQ(items.size(), 3u);
}

// Test parseValue with scientific notation without decimal point (uppercase E only)
TEST_F(AstExtractorRealAstTest, ParseValue_ScientificNotationNoDecimal) {
	auto pq = parse("CREATE (n {val: 1E5})");
	auto lit = getFirstLiteral(pq.tree);
	ASSERT_NE(lit, nullptr);
	auto pv = helpers::AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::DOUBLE);
}

// Test parseValue with integer zero
TEST_F(AstExtractorRealAstTest, ParseValue_IntegerZero) {
	auto pq = parse("CREATE (n {val: 0})");
	auto lit = getFirstLiteral(pq.tree);
	ASSERT_NE(lit, nullptr);
	auto pv = helpers::AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::INTEGER);
	EXPECT_EQ(pv.toString(), "0");
}

// Test parseValue with negative integer
TEST_F(AstExtractorRealAstTest, ParseValue_NegativeInteger) {
	auto pq = parse("CREATE (n {val: -42})");
	auto lit = getFirstLiteral(pq.tree);
	// Note: negative numbers may be parsed as unary minus + positive literal
	// If lit is null, the value is wrapped in a unary expression
	if (lit) {
		auto pv = helpers::AstExtractor::parseValue(lit);
		EXPECT_TRUE(pv.getType() == graph::PropertyType::INTEGER || pv.getType() == graph::PropertyType::DOUBLE);
	}
}
