/**
 * @file test_AstExtractor.cpp
 * @brief Unit tests for AstExtractor helper class
 */

#include <gtest/gtest.h>
#include <string>
#include "CypherLexer.h"
#include "CypherParser.h"
#include "antlr4-runtime.h"
#include "helpers/AstExtractor.hpp"

using namespace graph::parser::cypher::helpers;

// Test class for AstExtractor
class AstExtractorUnitTest : public ::testing::Test {
protected:
	// Helper to check if string contains substring
	bool containsSubstring(const std::string& str, const std::string& substr) {
		if (str.empty() || substr.empty()) return false;
		return str.find(substr) != std::string::npos;
	}

	// Parsing infrastructure to create real AST contexts
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

	// Get SingleQuery from a parsed tree
	CypherParser::SingleQueryContext* getSingleQuery(CypherParser::CypherContext* tree) {
		if (!tree || !tree->statement()) return nullptr;
		auto* regularStmt = dynamic_cast<CypherParser::RegularStatementContext*>(tree->statement());
		if (!regularStmt || !regularStmt->query()) return nullptr;
		auto regularQuery = regularStmt->query()->regularQuery();
		if (!regularQuery) return nullptr;
		return regularQuery->singleQuery(0);
	}

	// Get first node pattern from CREATE statement
	CypherParser::NodePatternContext* getFirstNodePattern(CypherParser::CypherContext* tree) {
		auto sq = getSingleQuery(tree);
		if (!sq) return nullptr;
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

	// Find the first LiteralContext anywhere in a subtree (depth-first)
	CypherParser::LiteralContext* findLiteral(antlr4::tree::ParseTree* t) {
		if (!t) return nullptr;
		if (auto* lit = dynamic_cast<CypherParser::LiteralContext*>(t)) return lit;
		for (size_t i = 0; i < t->children.size(); ++i) {
			auto result = findLiteral(t->children[i]);
			if (result) return result;
		}
		return nullptr;
	}

	// Get the literal from a CREATE (n {key: value}) query
	CypherParser::LiteralContext* getLiteralFromCreate(const std::string& query) {
		auto tree = parse(query);
		auto node = getFirstNodePattern(tree);
		if (!node || !node->properties() || !node->properties()->mapLiteral()) return nullptr;
		auto mapLit = node->properties()->mapLiteral();
		auto exprs = mapLit->expression();
		if (exprs.empty()) return nullptr;
		return findLiteral(exprs[0]);
	}

	// Get the first relationship detail from a MATCH pattern
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

	// Get first SET statement
	CypherParser::SetStatementContext* getFirstSetStatement(CypherParser::CypherContext* tree) {
		auto sq = getSingleQuery(tree);
		if (!sq) return nullptr;
		for (auto uc : sq->updatingClause()) {
			if (uc->setStatement()) return uc->setStatement();
		}
		return nullptr;
	}
};

// ============== extractVariable Tests ==============

TEST_F(AstExtractorUnitTest, ExtractVariable_Null) {
	std::string result = AstExtractor::extractVariable(nullptr);
	EXPECT_TRUE(result.empty());
}

// ============== extractLabel Tests ==============

TEST_F(AstExtractorUnitTest, ExtractLabel_Null) {
	std::string result = AstExtractor::extractLabel(nullptr);
	EXPECT_TRUE(result.empty());
}

// ============== extractLabelFromNodeLabel Tests ==============

TEST_F(AstExtractorUnitTest, ExtractLabelFromNodeLabel_Null) {
	std::string result = AstExtractor::extractLabelFromNodeLabel(nullptr);
	EXPECT_TRUE(result.empty());
}

// ============== extractRelType Tests ==============

TEST_F(AstExtractorUnitTest, ExtractRelType_Null) {
	std::string result = AstExtractor::extractRelType(nullptr);
	EXPECT_TRUE(result.empty());
}

// ============== extractPropertyKeyFromExpr Tests ==============

TEST_F(AstExtractorUnitTest, ExtractPropertyKeyFromExpr_Null) {
	std::string result = AstExtractor::extractPropertyKeyFromExpr(nullptr);
	EXPECT_TRUE(result.empty());
}

// ============== parseValue Tests ==============

TEST_F(AstExtractorUnitTest, ParseValue_NullLiteralContext) {
	graph::PropertyValue result = AstExtractor::parseValue(nullptr);
	// Default PropertyValue is NULL_TYPE, not UNKNOWN
	EXPECT_EQ(result.getType(), graph::PropertyType::NULL_TYPE);
}

// ============== extractProperties Tests ==============

TEST_F(AstExtractorUnitTest, ExtractProperties_NullContext) {
	std::function<graph::PropertyValue(CypherParser::ExpressionContext*)> evaluator =
		[](CypherParser::ExpressionContext*) {
			return graph::PropertyValue();
		};
	auto result = AstExtractor::extractProperties(nullptr, evaluator);
	EXPECT_TRUE(result.empty());
}

TEST_F(AstExtractorUnitTest, ExtractProperties_WithEvaluator) {
	// Tests with null context - evaluator should not be called
	int callCount = 0;
	std::function<graph::PropertyValue(CypherParser::ExpressionContext*)> evaluator =
		[&callCount](CypherParser::ExpressionContext*) {
			callCount++;
			return graph::PropertyValue();
		};
	auto result = AstExtractor::extractProperties(nullptr, evaluator);

	EXPECT_EQ(callCount, 0);
	EXPECT_TRUE(result.empty());
}

// ============== parseValue: String Escape Sequences ==============

TEST_F(AstExtractorUnitTest, ParseValue_EscapeBackslashN) {
	auto lit = getLiteralFromCreate("CREATE (n {s: \"hello\\nworld\"})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "hello\nworld");
}

TEST_F(AstExtractorUnitTest, ParseValue_EscapeBackslashT) {
	auto lit = getLiteralFromCreate("CREATE (n {s: \"col1\\tcol2\"})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "col1\tcol2");
}

TEST_F(AstExtractorUnitTest, ParseValue_EscapeBackslashB) {
	auto lit = getLiteralFromCreate("CREATE (n {s: \"back\\bspace\"})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "back\bspace");
}

TEST_F(AstExtractorUnitTest, ParseValue_EscapeBackslashF) {
	auto lit = getLiteralFromCreate("CREATE (n {s: \"form\\ffeed\"})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "form\ffeed");
}

TEST_F(AstExtractorUnitTest, ParseValue_EscapeBackslashR) {
	auto lit = getLiteralFromCreate("CREATE (n {s: \"carriage\\rreturn\"})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "carriage\rreturn");
}

TEST_F(AstExtractorUnitTest, ParseValue_EscapeDoubleQuote) {
	auto lit = getLiteralFromCreate("CREATE (n {s: \"say\\\"hello\\\"\"})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "say\"hello\"");
}

TEST_F(AstExtractorUnitTest, ParseValue_EscapeSingleQuote) {
	auto lit = getLiteralFromCreate("CREATE (n {s: 'it\\'s'})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "it's");
}

TEST_F(AstExtractorUnitTest, ParseValue_EscapeBackslash) {
	auto lit = getLiteralFromCreate("CREATE (n {s: \"path\\\\dir\"})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "path\\dir");
}

TEST_F(AstExtractorUnitTest, ParseValue_UnicodeEscape) {
	// \u0041 is 'A'
	auto lit = getLiteralFromCreate("CREATE (n {s: \"\\u0041\"})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "A");
}

TEST_F(AstExtractorUnitTest, ParseValue_UnicodeEscapeLowerHex) {
	// \u0061 is 'a'
	auto lit = getLiteralFromCreate("CREATE (n {s: \"\\u0061bc\"})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "abc");
}

TEST_F(AstExtractorUnitTest, ParseValue_MultipleEscapes) {
	// Test a string with multiple different escape sequences
	auto lit = getLiteralFromCreate("CREATE (n {s: \"a\\tb\\nc\"})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "a\tb\nc");
}

TEST_F(AstExtractorUnitTest, ParseValue_PlainStringNoEscapes) {
	auto lit = getLiteralFromCreate("CREATE (n {s: \"hello\"})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "hello");
}

TEST_F(AstExtractorUnitTest, ParseValue_SingleQuotedString) {
	auto lit = getLiteralFromCreate("CREATE (n {s: 'world'})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "world");
}

// ============== parseValue: Numeric Literals ==============

TEST_F(AstExtractorUnitTest, ParseValue_IntegerLiteral) {
	auto lit = getLiteralFromCreate("CREATE (n {x: 42})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::INTEGER);
	EXPECT_EQ(pv.toString(), "42");
}

TEST_F(AstExtractorUnitTest, ParseValue_DoubleLiteral) {
	auto lit = getLiteralFromCreate("CREATE (n {x: 3.14})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::DOUBLE);
}

TEST_F(AstExtractorUnitTest, ParseValue_ScientificNotation) {
	auto lit = getLiteralFromCreate("CREATE (n {x: 1.5e10})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::DOUBLE);
}

TEST_F(AstExtractorUnitTest, ParseValue_ScientificNotationUpperE) {
	auto lit = getLiteralFromCreate("CREATE (n {x: 2E5})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::DOUBLE);
}

// ============== parseValue: Boolean and Null Literals ==============

TEST_F(AstExtractorUnitTest, ParseValue_BooleanTrue) {
	auto lit = getLiteralFromCreate("CREATE (n {b: true})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::BOOLEAN);
	EXPECT_EQ(pv.toString(), "true");
}

TEST_F(AstExtractorUnitTest, ParseValue_BooleanFalse) {
	auto lit = getLiteralFromCreate("CREATE (n {b: false})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::BOOLEAN);
	EXPECT_EQ(pv.toString(), "false");
}

TEST_F(AstExtractorUnitTest, ParseValue_NullLiteral) {
	auto lit = getLiteralFromCreate("CREATE (n {x: null})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::NULL_TYPE);
}

// ============== extractVariable with real parse tree ==============

TEST_F(AstExtractorUnitTest, ExtractVariable_RealParseTree) {
	auto tree = parse("MATCH (n) RETURN n");
	auto sq = getSingleQuery(tree);
	ASSERT_NE(sq, nullptr);
	auto rc = sq->readingClause(0);
	ASSERT_NE(rc, nullptr);
	auto match = rc->matchStatement();
	ASSERT_NE(match, nullptr);
	auto pattern = match->pattern();
	ASSERT_NE(pattern, nullptr);
	auto element = pattern->patternPart(0)->patternElement();
	ASSERT_NE(element, nullptr);
	auto node = element->nodePattern();
	ASSERT_NE(node, nullptr);
	auto varCtx = node->variable();
	ASSERT_NE(varCtx, nullptr);
	EXPECT_EQ(AstExtractor::extractVariable(varCtx), "n");
}

// ============== extractLabel with real parse tree ==============

TEST_F(AstExtractorUnitTest, ExtractLabel_RealLabel) {
	auto tree = parse("CREATE (n:Person)");
	auto node = getFirstNodePattern(tree);
	ASSERT_NE(node, nullptr);
	auto label = AstExtractor::extractLabel(node->nodeLabels());
	EXPECT_EQ(label, "Person");
}

TEST_F(AstExtractorUnitTest, ExtractLabel_NoLabel) {
	// Node without labels - nodeLabels() returns null
	auto tree = parse("CREATE (n)");
	auto node = getFirstNodePattern(tree);
	ASSERT_NE(node, nullptr);
	auto label = AstExtractor::extractLabel(node->nodeLabels());
	EXPECT_TRUE(label.empty());
}

TEST_F(AstExtractorUnitTest, ExtractLabels_Multiple) {
	auto tree = parse("CREATE (n:Person:Employee)");
	auto node = getFirstNodePattern(tree);
	ASSERT_NE(node, nullptr);
	auto labels = AstExtractor::extractLabels(node->nodeLabels());
	ASSERT_EQ(labels.size(), 2u);
	EXPECT_EQ(labels[0], "Person");
	EXPECT_EQ(labels[1], "Employee");
}

TEST_F(AstExtractorUnitTest, ExtractLabels_Null) {
	auto labels = AstExtractor::extractLabels(nullptr);
	EXPECT_TRUE(labels.empty());
}

// ============== extractLabelFromNodeLabel with real parse tree ==============

TEST_F(AstExtractorUnitTest, ExtractLabelFromNodeLabel_Real) {
	auto tree = parse("CREATE (n:Person)");
	auto node = getFirstNodePattern(tree);
	ASSERT_NE(node, nullptr);
	auto nodeLabels = node->nodeLabels();
	ASSERT_NE(nodeLabels, nullptr);
	auto label = AstExtractor::extractLabelFromNodeLabel(nodeLabels->nodeLabel(0));
	EXPECT_EQ(label, "Person");
}

// ============== extractRelType with real parse tree ==============

TEST_F(AstExtractorUnitTest, ExtractRelType_RealType) {
	auto tree = parse("MATCH (a)-[r:KNOWS]->(b) RETURN a");
	auto relDetail = getFirstRelDetail(tree);
	ASSERT_NE(relDetail, nullptr);
	auto relType = AstExtractor::extractRelType(relDetail->relationshipTypes());
	EXPECT_EQ(relType, "KNOWS");
}

TEST_F(AstExtractorUnitTest, ExtractRelType_NoType) {
	// Relationship without type - relationshipTypes() returns null
	auto tree = parse("MATCH (a)-[r]->(b) RETURN a");
	auto relDetail = getFirstRelDetail(tree);
	ASSERT_NE(relDetail, nullptr);
	auto relType = AstExtractor::extractRelType(relDetail->relationshipTypes());
	EXPECT_TRUE(relType.empty());
}

// ============== extractPropertyKeyFromExpr with real parse tree ==============

TEST_F(AstExtractorUnitTest, ExtractPropertyKeyFromExpr_DotSyntax) {
	auto tree = parse("MATCH (n) SET n.age = 30 RETURN n");
	auto setStmt = getFirstSetStatement(tree);
	ASSERT_NE(setStmt, nullptr);
	auto setItems = setStmt->setItem();
	ASSERT_FALSE(setItems.empty());
	auto propExpr = setItems[0]->propertyExpression();
	ASSERT_NE(propExpr, nullptr);
	auto key = AstExtractor::extractPropertyKeyFromExpr(propExpr);
	EXPECT_EQ(key, "age");
}

// ============== extractProperties with real parse tree ==============

TEST_F(AstExtractorUnitTest, ExtractProperties_RealProperties) {
	auto tree = parse("CREATE (n {name: 'Alice', age: 30})");
	auto node = getFirstNodePattern(tree);
	ASSERT_NE(node, nullptr);
	auto propsCtx = node->properties();
	ASSERT_NE(propsCtx, nullptr);

	std::function<graph::PropertyValue(CypherParser::ExpressionContext*)> evaluator =
		[](CypherParser::ExpressionContext* expr) -> graph::PropertyValue {
			if (!expr) return graph::PropertyValue();
			// Simple evaluator: try to parse the literal from the expression
			std::function<CypherParser::LiteralContext*(antlr4::tree::ParseTree*)> findLit;
			findLit = [&](antlr4::tree::ParseTree* t) -> CypherParser::LiteralContext* {
				if (!t) return nullptr;
				if (auto* lit = dynamic_cast<CypherParser::LiteralContext*>(t)) return lit;
				for (size_t i = 0; i < t->children.size(); ++i) {
					auto result = findLit(t->children[i]);
					if (result) return result;
				}
				return nullptr;
			};
			auto* lit = findLit(expr);
			return lit ? AstExtractor::parseValue(lit) : graph::PropertyValue();
		};

	auto props = AstExtractor::extractProperties(propsCtx, evaluator);
	EXPECT_EQ(props.size(), 2u);
	EXPECT_EQ(props["name"].toString(), "Alice");
	EXPECT_EQ(props["age"].toString(), "30");
}

// ============== Escape sequences with single-quoted strings ==============

TEST_F(AstExtractorUnitTest, ParseValue_SingleQuotedEscapeN) {
	auto lit = getLiteralFromCreate("CREATE (n {s: 'line1\\nline2'})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "line1\nline2");
}

TEST_F(AstExtractorUnitTest, ParseValue_SingleQuotedEscapeT) {
	auto lit = getLiteralFromCreate("CREATE (n {s: 'col1\\tcol2'})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "col1\tcol2");
}

TEST_F(AstExtractorUnitTest, ParseValue_DoubleQuotedEscapeSingleQuote) {
	// Double-quoted string with escaped single quote
	auto lit = getLiteralFromCreate("CREATE (n {s: \"it\\'s\"})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "it's");
}

TEST_F(AstExtractorUnitTest, ParseValue_SingleQuotedEscapeBackslash) {
	auto lit = getLiteralFromCreate("CREATE (n {s: 'c:\\\\dir'})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "c:\\dir");
}

TEST_F(AstExtractorUnitTest, ParseValue_AllEscapesInOneString) {
	// Combine multiple escapes: \b\t\n\f\r
	auto lit = getLiteralFromCreate("CREATE (n {s: \"\\b\\t\\n\\f\\r\"})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	std::string expected;
	expected += '\b';
	expected += '\t';
	expected += '\n';
	expected += '\f';
	expected += '\r';
	EXPECT_EQ(pv.toString(), expected);
}

TEST_F(AstExtractorUnitTest, ParseValue_UnicodeInMiddleOfString) {
	// Unicode escape in the middle: "x\u0042y" -> "xBy"
	auto lit = getLiteralFromCreate("CREATE (n {s: \"x\\u0042y\"})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "xBy");
}

TEST_F(AstExtractorUnitTest, ParseValue_EmptyString) {
	auto lit = getLiteralFromCreate("CREATE (n {s: \"\"})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "");
}

TEST_F(AstExtractorUnitTest, ParseValue_EmptySingleQuotedString) {
	auto lit = getLiteralFromCreate("CREATE (n {s: ''})");
	ASSERT_NE(lit, nullptr);
	auto pv = AstExtractor::parseValue(lit);
	EXPECT_EQ(pv.getType(), graph::PropertyType::STRING);
	EXPECT_EQ(pv.toString(), "");
}
