/**
 * @file test_ExpressionBuilder.cpp
 * @brief Unit tests for ExpressionBuilder helper class
 *
 * Tests exercise ExpressionBuilder by parsing real Cypher queries with ANTLR4
 * and calling buildExpression() on the resulting expression contexts.
 */

#include <gtest/gtest.h>
#include <string>
#include "CypherLexer.h"
#include "CypherParser.h"
#include "antlr4-runtime.h"
#include "helpers/ExpressionBuilder.hpp"
#include "graph/query/expressions/Expression.hpp"

using namespace graph::parser::cypher::helpers;
using namespace graph::query::expressions;

// Test class for ExpressionBuilder with ANTLR4 parsing support
class ExpressionBuilderUnitTest : public ::testing::Test {
protected:
	// Parse a query and return expression from RETURN clause's first projection item
	// Usage: parseReturnExpr("RETURN 1 + 2") to get expression for "1 + 2"
	struct ParseResult {
		std::unique_ptr<antlr4::ANTLRInputStream> input;
		std::unique_ptr<CypherLexer> lexer;
		std::unique_ptr<antlr4::CommonTokenStream> tokens;
		std::unique_ptr<CypherParser> parser;
		CypherParser::ExpressionContext* exprCtx = nullptr;
	};

	// Keep parse results alive during the test
	std::vector<std::unique_ptr<ParseResult>> parseResults;

	CypherParser::ExpressionContext* parseReturnExpr(const std::string& query) {
		auto result = std::make_unique<ParseResult>();
		result->input = std::make_unique<antlr4::ANTLRInputStream>(query);
		result->lexer = std::make_unique<CypherLexer>(result->input.get());
		result->lexer->removeErrorListeners();
		result->tokens = std::make_unique<antlr4::CommonTokenStream>(result->lexer.get());
		result->parser = std::make_unique<CypherParser>(result->tokens.get());
		result->parser->removeErrorListeners();

		auto tree = result->parser->cypher();
		if (!tree || !tree->statement())
			return nullptr;

		auto* regularStmt = dynamic_cast<CypherParser::RegularStatementContext*>(tree->statement());
		if (!regularStmt || !regularStmt->query())
			return nullptr;

		auto query_ = regularStmt->query();
		auto regularQuery = query_->regularQuery();
		if (!regularQuery)
			return nullptr;

		auto singleQuery = regularQuery->singleQuery(0);
		if (!singleQuery || !singleQuery->returnStatement())
			return nullptr;

		auto returnStmt = singleQuery->returnStatement();
		auto projBody = returnStmt->projectionBody();
		if (!projBody || !projBody->projectionItems())
			return nullptr;

		auto projItems = projBody->projectionItems();
		if (projItems->projectionItem().empty())
			return nullptr;

		result->exprCtx = projItems->projectionItem(0)->expression();
		auto* exprCtx = result->exprCtx;
		parseResults.push_back(std::move(result));
		return exprCtx;
	}

	// Parse WHERE expression from MATCH ... WHERE ... RETURN n
	CypherParser::ExpressionContext* parseWhereExpr(const std::string& query) {
		auto result = std::make_unique<ParseResult>();
		result->input = std::make_unique<antlr4::ANTLRInputStream>(query);
		result->lexer = std::make_unique<CypherLexer>(result->input.get());
		result->lexer->removeErrorListeners();
		result->tokens = std::make_unique<antlr4::CommonTokenStream>(result->lexer.get());
		result->parser = std::make_unique<CypherParser>(result->tokens.get());
		result->parser->removeErrorListeners();

		auto tree = result->parser->cypher();
		if (!tree || !tree->statement())
			return nullptr;

		auto* regularStmt = dynamic_cast<CypherParser::RegularStatementContext*>(tree->statement());
		if (!regularStmt || !regularStmt->query())
			return nullptr;

		auto query_ = regularStmt->query();
		auto regularQuery = query_->regularQuery();
		if (!regularQuery)
			return nullptr;

		auto singleQuery = regularQuery->singleQuery(0);
		if (!singleQuery)
			return nullptr;

		// Find the MATCH clause with WHERE
		for (auto* readClause : singleQuery->readingClause()) {
			if (readClause->matchStatement() && readClause->matchStatement()->where()) {
				result->exprCtx = readClause->matchStatement()->where()->expression();
				auto* exprCtx = result->exprCtx;
				parseResults.push_back(std::move(result));
				return exprCtx;
			}
		}
		return nullptr;
	}
};

// ============== Null input tests ==============

TEST_F(ExpressionBuilderUnitTest, ExtractListFromExpression_Null) {
	auto list = ExpressionBuilder::extractListFromExpression(nullptr);
	EXPECT_TRUE(list.empty());
}

TEST_F(ExpressionBuilderUnitTest, BuildExpression_NullExpression) {
	EXPECT_THROW(
		ExpressionBuilder::buildExpression(nullptr),
		std::runtime_error
	);
}

TEST_F(ExpressionBuilderUnitTest, BuildExpression_NullCheckErrorMessage) {
	try {
		ExpressionBuilder::buildExpression(nullptr);
		FAIL() << "Expected std::runtime_error";
	} catch (const std::runtime_error& e) {
		EXPECT_STREQ("Invalid expression tree: null context", e.what());
	} catch (...) {
		FAIL() << "Expected std::runtime_error";
	}
}

// ============== Parenthesized expression ==============

TEST_F(ExpressionBuilderUnitTest, ParenthesizedExpression) {
	// Covers: buildAtomExpression ctx->expression() branch (line 441)
	auto* expr = parseReturnExpr("RETURN (1 + 2)");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, NestedParenthesizedExpression) {
	auto* expr = parseReturnExpr("RETURN ((3 + 4) * 2)");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== Unary plus ==============

TEST_F(ExpressionBuilderUnitTest, UnaryPlus) {
	// Covers: buildUnaryExpression ctx->PLUS() True branch (line 378)
	// and hasMinus == false (line 382 False branch)
	auto* expr = parseReturnExpr("RETURN +5");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, UnaryPlusDouble) {
	auto* expr = parseReturnExpr("RETURN +3.14");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== Unary minus on non-literal (variable) ==============

TEST_F(ExpressionBuilderUnitTest, UnaryMinusOnVariable) {
	// This covers: buildUnaryExpression lines 394-397 (UOP_MINUS on non-literal)
	// Minus on a variable expression produces UnaryOpExpression, not folded literal
	// Actually variables go through buildVariableOrProperty, so this might not work.
	// Instead test -count(n) or similar where atom is a function call
	auto* expr = parseReturnExpr("RETURN -count(*)");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== CASE expressions ==============

TEST_F(ExpressionBuilderUnitTest, SearchedCaseExpression) {
	// Covers: buildCaseExpression with searched CASE (no comparison expr)
	// Line 764 False branch: expression().size() <= whenExprs.size() * 2
	auto* expr = parseReturnExpr("RETURN CASE WHEN true THEN 1 WHEN false THEN 2 END");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, SearchedCaseExpressionWithElse) {
	auto* expr = parseReturnExpr("RETURN CASE WHEN true THEN 1 ELSE 0 END");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, SimpleCaseExpression) {
	// Covers: buildCaseExpression with simple CASE (has comparison expr)
	auto* expr = parseReturnExpr("RETURN CASE 1 WHEN 1 THEN 'one' WHEN 2 THEN 'two' ELSE 'other' END");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, SimpleCaseExpressionNoElse) {
	auto* expr = parseReturnExpr("RETURN CASE 1 WHEN 1 THEN 'one' WHEN 2 THEN 'two' END");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== EXISTS function ==============

TEST_F(ExpressionBuilderUnitTest, ExistsFunctionWithArgument) {
	// Covers: buildFunctionCall exists branch (line 557)
	auto* expr = parseReturnExpr("RETURN exists(n.name)");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, ExistsFunctionNoArguments) {
	// Covers: exists with empty arguments (line 575)
	auto* expr = parseReturnExpr("RETURN exists()");
	if (expr) {
		auto result = ExpressionBuilder::buildExpression(expr);
		EXPECT_NE(result, nullptr);
	}
}

// ============== Quantifier functions: all, any, none, single ==============

TEST_F(ExpressionBuilderUnitTest, AllQuantifierFunction) {
	// Covers: buildFunctionCall quantifier branch (line 582)
	// all(x IN [1,2,3] WHERE x > 0)
	auto* expr = parseReturnExpr("RETURN all(x, [1,2,3], x)");
	if (expr) {
		auto result = ExpressionBuilder::buildExpression(expr);
		EXPECT_NE(result, nullptr);
	}
}

TEST_F(ExpressionBuilderUnitTest, AnyQuantifierFunction) {
	auto* expr = parseReturnExpr("RETURN any(x, [1,2,3], x)");
	if (expr) {
		auto result = ExpressionBuilder::buildExpression(expr);
		EXPECT_NE(result, nullptr);
	}
}

TEST_F(ExpressionBuilderUnitTest, NoneQuantifierFunction) {
	auto* expr = parseReturnExpr("RETURN none(x, [1,2,3], x)");
	if (expr) {
		auto result = ExpressionBuilder::buildExpression(expr);
		EXPECT_NE(result, nullptr);
	}
}

TEST_F(ExpressionBuilderUnitTest, SingleQuantifierFunction) {
	auto* expr = parseReturnExpr("RETURN single(x, [1,2,3])");
	if (expr) {
		auto result = ExpressionBuilder::buildExpression(expr);
		EXPECT_NE(result, nullptr);
	}
}

// ============== List comprehension with no WHERE or MAP ==============

TEST_F(ExpressionBuilderUnitTest, ListComprehensionNoWhereNoMap) {
	// Covers: buildListComprehensionExpression default type branch (line 874-875)
	auto* expr = parseReturnExpr("RETURN [x IN [1,2,3]]");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, ListComprehensionWithWhere) {
	auto* expr = parseReturnExpr("RETURN [x IN [1,2,3] WHERE x > 1]");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, ListComprehensionWithMap) {
	auto* expr = parseReturnExpr("RETURN [x IN [1,2,3] | x]");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, ListComprehensionWithWhereAndMap) {
	auto* expr = parseReturnExpr("RETURN [x IN [1,2,3] WHERE x > 1 | x]");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== evaluateLiteralExpression ==============

TEST_F(ExpressionBuilderUnitTest, EvaluateLiteralExpression_Null) {
	// Covers: evaluateLiteralExpression null input (line 58)
	auto result = ExpressionBuilder::evaluateLiteralExpression(nullptr);
	EXPECT_EQ(result.getType(), graph::PropertyType::NULL_TYPE);
}

TEST_F(ExpressionBuilderUnitTest, EvaluateLiteralExpression_Integer) {
	auto* expr = parseReturnExpr("RETURN 42");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::evaluateLiteralExpression(expr);
	EXPECT_EQ(result.getType(), graph::PropertyType::INTEGER);
}

TEST_F(ExpressionBuilderUnitTest, EvaluateLiteralExpression_String) {
	auto* expr = parseReturnExpr("RETURN 'hello'");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::evaluateLiteralExpression(expr);
	EXPECT_EQ(result.getType(), graph::PropertyType::STRING);
}

TEST_F(ExpressionBuilderUnitTest, EvaluateLiteralExpression_Boolean) {
	auto* expr = parseReturnExpr("RETURN true");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::evaluateLiteralExpression(expr);
	EXPECT_EQ(result.getType(), graph::PropertyType::BOOLEAN);
}

TEST_F(ExpressionBuilderUnitTest, EvaluateLiteralExpression_NullLiteral) {
	auto* expr = parseReturnExpr("RETURN null");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::evaluateLiteralExpression(expr);
	EXPECT_EQ(result.getType(), graph::PropertyType::NULL_TYPE);
}

TEST_F(ExpressionBuilderUnitTest, EvaluateLiteralExpression_Double) {
	auto* expr = parseReturnExpr("RETURN 3.14");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::evaluateLiteralExpression(expr);
	EXPECT_EQ(result.getType(), graph::PropertyType::DOUBLE);
}

TEST_F(ExpressionBuilderUnitTest, EvaluateLiteralExpression_ListLiteral) {
	// Covers: evaluateLiteralExpression list literal path (line 65-68)
	auto* expr = parseReturnExpr("RETURN [1, 2, 3]");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::evaluateLiteralExpression(expr);
	EXPECT_EQ(result.getType(), graph::PropertyType::LIST);
}

TEST_F(ExpressionBuilderUnitTest, EvaluateLiteralExpression_VariableReturnsEmpty) {
	// Covers: evaluateLiteralExpression with non-literal (variable)
	// evaluator.evaluate will throw, caught by the try/catch (line 102-107)
	auto* expr = parseReturnExpr("RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::evaluateLiteralExpression(expr);
	// Should return empty PropertyValue since variables cannot be evaluated
	EXPECT_EQ(result.getType(), graph::PropertyType::NULL_TYPE);
}

// ============== Logical operators ==============

TEST_F(ExpressionBuilderUnitTest, OrExpression) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.a = 1 OR n.b = 2 RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, XorExpression) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.a = 1 XOR n.b = 2 RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, AndExpression) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.a = 1 AND n.b = 2 RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, NotExpression) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE NOT n.active RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== Comparison operators ==============

TEST_F(ExpressionBuilderUnitTest, EqualOperator) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.x = 5 RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, NotEqualOperator) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.x <> 5 RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, LessThanOperator) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.x < 5 RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, GreaterThanOperator) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.x > 5 RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, LessEqualOperator) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.x <= 5 RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, GreaterEqualOperator) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.x >= 5 RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, StartsWithOperator) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.name STARTS WITH 'A' RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, EndsWithOperator) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.name ENDS WITH 'z' RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, ContainsOperator) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.name CONTAINS 'oo' RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, InOperatorWithList) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.x IN [1, 2, 3] RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, IsNullOperator) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.x IS NULL RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, IsNotNullOperator) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.x IS NOT NULL RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== Arithmetic operators ==============

TEST_F(ExpressionBuilderUnitTest, AdditionExpression) {
	auto* expr = parseReturnExpr("RETURN 1 + 2");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, SubtractionExpression) {
	auto* expr = parseReturnExpr("RETURN 5 - 3");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, MultiplicationExpression) {
	auto* expr = parseReturnExpr("RETURN 2 * 3");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, DivisionExpression) {
	auto* expr = parseReturnExpr("RETURN 10 / 2");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, ModuloExpression) {
	auto* expr = parseReturnExpr("RETURN 10 % 3");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, PowerExpression) {
	auto* expr = parseReturnExpr("RETURN 2 ^ 3");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== Function calls ==============

TEST_F(ExpressionBuilderUnitTest, CountStarFunction) {
	auto* expr = parseReturnExpr("RETURN count(*)");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, ToUpperFunction) {
	auto* expr = parseReturnExpr("RETURN toUpper('hello')");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, SumFunction) {
	auto* expr = parseReturnExpr("RETURN sum(n.value)");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== Variable and property access ==============

TEST_F(ExpressionBuilderUnitTest, SimpleVariable) {
	auto* expr = parseReturnExpr("RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, PropertyAccess) {
	auto* expr = parseReturnExpr("RETURN n.name");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, PropertyAccessWithListIndex) {
	// Covers: buildVariableOrProperty with list index after property
	auto* expr = parseReturnExpr("RETURN n.list[0]");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== List slice on non-variable atoms ==============

TEST_F(ExpressionBuilderUnitTest, ListLiteralWithIndex) {
	// Covers: buildUnaryExpression accessor branch on non-variable (line 402)
	auto* expr = parseReturnExpr("RETURN [1,2,3][0]");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, ListLiteralWithRange) {
	auto* expr = parseReturnExpr("RETURN [1,2,3][0..2]");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, ListLiteralWithOpenEndRange) {
	// Covers: buildListSliceFromAccessor with range but no end (line 826 False)
	auto* expr = parseReturnExpr("RETURN [1,2,3][1..]");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== Literal types ==============

TEST_F(ExpressionBuilderUnitTest, IntegerLiteral) {
	auto* expr = parseReturnExpr("RETURN 42");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, DoubleLiteral) {
	auto* expr = parseReturnExpr("RETURN 3.14");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, StringLiteral) {
	auto* expr = parseReturnExpr("RETURN 'hello world'");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, BooleanTrueLiteral) {
	auto* expr = parseReturnExpr("RETURN true");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, BooleanFalseLiteral) {
	auto* expr = parseReturnExpr("RETURN false");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, NullLiteral) {
	auto* expr = parseReturnExpr("RETURN null");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== Unary minus ==============

TEST_F(ExpressionBuilderUnitTest, UnaryMinusInteger) {
	auto* expr = parseReturnExpr("RETURN -10");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, UnaryMinusDouble) {
	auto* expr = parseReturnExpr("RETURN -3.14");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== isAggregateFunction ==============

TEST_F(ExpressionBuilderUnitTest, IsAggregateFunction_Count) {
	EXPECT_TRUE(ExpressionBuilder::isAggregateFunction("count"));
	EXPECT_TRUE(ExpressionBuilder::isAggregateFunction("COUNT"));
	EXPECT_TRUE(ExpressionBuilder::isAggregateFunction("Count"));
}

TEST_F(ExpressionBuilderUnitTest, IsAggregateFunction_Sum) {
	EXPECT_TRUE(ExpressionBuilder::isAggregateFunction("sum"));
	EXPECT_TRUE(ExpressionBuilder::isAggregateFunction("SUM"));
}

TEST_F(ExpressionBuilderUnitTest, IsAggregateFunction_Avg) {
	EXPECT_TRUE(ExpressionBuilder::isAggregateFunction("avg"));
	EXPECT_TRUE(ExpressionBuilder::isAggregateFunction("AVG"));
}

TEST_F(ExpressionBuilderUnitTest, IsAggregateFunction_Min) {
	EXPECT_TRUE(ExpressionBuilder::isAggregateFunction("min"));
}

TEST_F(ExpressionBuilderUnitTest, IsAggregateFunction_Max) {
	EXPECT_TRUE(ExpressionBuilder::isAggregateFunction("max"));
}

TEST_F(ExpressionBuilderUnitTest, IsAggregateFunction_Collect) {
	EXPECT_TRUE(ExpressionBuilder::isAggregateFunction("collect"));
}

TEST_F(ExpressionBuilderUnitTest, IsAggregateFunction_NonAggregate) {
	EXPECT_FALSE(ExpressionBuilder::isAggregateFunction("toUpper"));
	EXPECT_FALSE(ExpressionBuilder::isAggregateFunction("toString"));
	EXPECT_FALSE(ExpressionBuilder::isAggregateFunction("size"));
}

// ============== Complex expressions ==============

TEST_F(ExpressionBuilderUnitTest, ComplexArithmeticExpression) {
	auto* expr = parseReturnExpr("RETURN 1 + 2 * 3 - 4 / 2");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, ComplexLogicalExpression) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.a = 1 AND n.b > 2 OR n.c < 3 RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, MultipleOrConditions) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.x = 1 OR n.x = 2 OR n.x = 3 RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, MultipleAndConditions) {
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.a = 1 AND n.b = 2 AND n.c = 3 RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, ScientificNotation) {
	auto* expr = parseReturnExpr("RETURN 1.5e10");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, ScientificNotationUpperE) {
	auto* expr = parseReturnExpr("RETURN 2.0E5");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== IN operator with variable RHS ==============

TEST_F(ExpressionBuilderUnitTest, InOperatorWithVariableList) {
	// Covers: IN operator falling through to BinaryOpExpression (not InExpression)
	// because the RHS is a variable, not a list literal
	auto* expr = parseWhereExpr("MATCH (n) WHERE n.x IN n.list RETURN n");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== List literal ==============

TEST_F(ExpressionBuilderUnitTest, EmptyListLiteral) {
	auto* expr = parseReturnExpr("RETURN []");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, MixedTypeListLiteral) {
	auto* expr = parseReturnExpr("RETURN [1, 'two', true, null, 3.14]");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== getAtomFromExpression / extractListFromExpression ==============

TEST_F(ExpressionBuilderUnitTest, GetAtomFromExpression_Null) {
	auto result = ExpressionBuilder::getAtomFromExpression(nullptr);
	EXPECT_EQ(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, ExtractListFromExpression_ValidList) {
	auto* expr = parseReturnExpr("RETURN [1, 2, 3]");
	ASSERT_NE(expr, nullptr);
	auto list = ExpressionBuilder::extractListFromExpression(expr);
	EXPECT_EQ(list.size(), 3u);
}

TEST_F(ExpressionBuilderUnitTest, ExtractListFromExpression_NonList) {
	auto* expr = parseReturnExpr("RETURN 42");
	ASSERT_NE(expr, nullptr);
	auto list = ExpressionBuilder::extractListFromExpression(expr);
	EXPECT_TRUE(list.empty());
}

TEST_F(ExpressionBuilderUnitTest, ExtractListFromExpression_ListWithStrings) {
	auto* expr = parseReturnExpr("RETURN ['a', 'b', 'c']");
	ASSERT_NE(expr, nullptr);
	auto list = ExpressionBuilder::extractListFromExpression(expr);
	EXPECT_EQ(list.size(), 3u);
}

// ============== Scientific notation without decimal point ==============

TEST_F(ExpressionBuilderUnitTest, ScientificNotation_LowercaseE_NoDecimal) {
	// Covers: parseValue line 713, s.find('e') branch (True: 0)
	// Number like 1e5 has 'e' but no decimal point
	auto* expr = parseReturnExpr("RETURN 1e5");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, ScientificNotation_UppercaseE_NoDecimal) {
	// Covers: parseValue line 713, s.find('E') branch (True: 0)
	auto* expr = parseReturnExpr("RETURN 1E5");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== EXISTS with two arguments ==============

TEST_F(ExpressionBuilderUnitTest, ExistsFunctionWithTwoArguments) {
	// Covers: buildFunctionCall EXISTS with exprs.size() >= 2 (line 567)
	auto* expr = parseReturnExpr("RETURN exists(n.name, n.age)");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== Quantifier without enough args ==============

TEST_F(ExpressionBuilderUnitTest, QuantifierFunction_SingleArg) {
	// Covers: quantifier with < 2 args falls through (line 586 False)
	auto* expr = parseReturnExpr("RETURN all(x)");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== Variable list indexing without property ==============

TEST_F(ExpressionBuilderUnitTest, VariableDirectListIndex) {
	// Covers: buildVariableOrProperty -> LBRACK without DOT first (line 519)
	auto* expr = parseReturnExpr("RETURN n[0]");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

TEST_F(ExpressionBuilderUnitTest, VariableDirectListRange) {
	auto* expr = parseReturnExpr("RETURN n[0..2]");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::buildExpression(expr);
	ASSERT_NE(result, nullptr);
}

// ============== evaluateLiteralExpression with non-literal (complex) ==============

TEST_F(ExpressionBuilderUnitTest, EvaluateLiteralExpression_NegativeInteger) {
	// -42 is parsed as unary minus on literal, covered by evaluator path (line 96-101)
	auto* expr = parseReturnExpr("RETURN -42");
	ASSERT_NE(expr, nullptr);
	auto result = ExpressionBuilder::evaluateLiteralExpression(expr);
	EXPECT_EQ(result.getType(), graph::PropertyType::INTEGER);
}

// ============== parseValue ==============

TEST_F(ExpressionBuilderUnitTest, ParseValue_Null) {
	auto result = ExpressionBuilder::parseValue(nullptr);
	EXPECT_EQ(result.getType(), graph::PropertyType::NULL_TYPE);
}
