/**
 * @file test_CypherSyntax.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/17
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "CypherLexer.h"
#include "CypherParser.h"
#include "antlr4-runtime.h"

using namespace graph::parser::cypher;

// Helper class to capture syntax errors for testing
class TestErrorListener : public antlr4::BaseErrorListener {
public:
    void syntaxError(antlr4::Recognizer * /*recognizer*/, antlr4::Token * /*offendingSymbol*/,
                     size_t line, size_t charPositionInLine,
                     const std::string &msg, std::exception_ptr /*e*/) override {
        errorMessages.push_back("Line " + std::to_string(line) + ":" + std::to_string(charPositionInLine) + " " + msg);
    }

    bool hasErrors() const { return !errorMessages.empty(); }
    std::vector<std::string> errorMessages;
};

class CypherSyntaxTest : public ::testing::Test {
protected:
    // Helper to run the parser against a query string
    // Returns true if parsing was successful (no syntax errors)
	static bool validate(const std::string& query) {
        antlr4::ANTLRInputStream input(query);
        CypherLexer lexer(&input);

        // Remove console listener to keep test output clean
        lexer.removeErrorListeners();
        TestErrorListener lexerErrorListener;
        lexer.addErrorListener(&lexerErrorListener);

        antlr4::CommonTokenStream tokens(&lexer);
        CypherParser parser(&tokens);

        parser.removeErrorListeners();
        TestErrorListener parserErrorListener;
        parser.addErrorListener(&parserErrorListener);

        // Entry point
        parser.cypher();

        // Check for errors
        if (lexerErrorListener.hasErrors()) {
            // Optional: Print errors for debugging
            // for (const auto& err : lexerErrorListener.errorMessages) std::cout << err << std::endl;
            return false;
        }
        if (parserErrorListener.hasErrors()) {
            // for (const auto& err : parserErrorListener.errorMessages) std::cout << err << std::endl;
            return false;
        }
        return true;
    }
};

// ============================================================================
// 1. Valid Syntax Tests (Should Pass)
// ============================================================================

TEST_F(CypherSyntaxTest, ValidMatchReturn) {
    EXPECT_TRUE(validate("MATCH (n:Person) RETURN n"));
    EXPECT_TRUE(validate("MATCH (n)-[r:KNOWS]->(m) RETURN r"));
    EXPECT_TRUE(validate("MATCH (n) WHERE n.age > 10 RETURN n"));
}

TEST_F(CypherSyntaxTest, ValidCreate) {
    EXPECT_TRUE(validate("CREATE (n:User {name: 'Alice'})"));
    EXPECT_TRUE(validate("CREATE (a)-[:REL]->(b)"));
    // Test multiple paths
    EXPECT_TRUE(validate("CREATE (a), (b)"));
}

TEST_F(CypherSyntaxTest, ValidAdministration) {
    EXPECT_TRUE(validate("CREATE INDEX ON :User(name)"));
    EXPECT_TRUE(validate("DROP INDEX ON :User(name)"));
    EXPECT_TRUE(validate("SHOW INDEXES"));
}

TEST_F(CypherSyntaxTest, ValidProcedures) {
    EXPECT_TRUE(validate("CALL dbms.setConfig('key', 'value')"));
    EXPECT_TRUE(validate("CALL dbms.listConfig()"));
}

// ============================================================================
// 2. Invalid Syntax Tests (Should Fail)
// ============================================================================

TEST_F(CypherSyntaxTest, InvalidKeywords) {
    // misspelled MATCH
    EXPECT_FALSE(validate("MTCH (n) RETURN n"));
}

TEST_F(CypherSyntaxTest, MissingParentheses) {
    // Node missing parens
    EXPECT_FALSE(validate("MATCH n:User RETURN n"));
    // Index syntax error
    EXPECT_FALSE(validate("CREATE INDEX :User(name)")); // Missing ON
}

TEST_F(CypherSyntaxTest, UnfinishedQuery) {
    EXPECT_FALSE(validate("MATCH (n) WHERE n.age >"));
    EXPECT_FALSE(validate("CREATE (n:User {name: 'A'")); // Missing closing brace/paren
}