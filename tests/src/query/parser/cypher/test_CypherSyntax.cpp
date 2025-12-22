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
	void syntaxError(antlr4::Recognizer * /*recognizer*/, antlr4::Token * /*offendingSymbol*/, size_t line,
					 size_t charPositionInLine, const std::string &msg, std::exception_ptr /*e*/) override {
		errorMessages.push_back("Line " + std::to_string(line) + ":" + std::to_string(charPositionInLine) + " " + msg);
	}

	bool hasErrors() const { return !errorMessages.empty(); }
	std::vector<std::string> errorMessages;
};

class CypherSyntaxTest : public ::testing::Test {
protected:
	// Helper to run the parser against a query string
	// Returns true if parsing was successful (no syntax errors)
	static bool validate(const std::string &query) {
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
		if (lexerErrorListener.hasErrors() || parserErrorListener.hasErrors()) {
			// Optional: Print errors for debugging failing tests
			// for (const auto& err : parserErrorListener.errorMessages) std::cerr << err << std::endl;
			return false;
		}
		return true;
	}
};

// ============================================================================
// 1. Read Operations (MATCH)
// ============================================================================

TEST_F(CypherSyntaxTest, ValidMatchWhere) {
	// Basic
	EXPECT_TRUE(validate("MATCH (n) RETURN n"));

	// Property Access
	EXPECT_TRUE(validate("MATCH (n) WHERE n.age > 10 RETURN n"));
	EXPECT_TRUE(validate("MATCH (n) WHERE n.age = 30 RETURN n"));
	EXPECT_TRUE(validate("MATCH (n) WHERE n.price <= 100.5 RETURN n"));

	// String Literal
	EXPECT_TRUE(validate("MATCH (n) WHERE n.name = 'Alice' RETURN n"));

	// Logic Operators
	EXPECT_TRUE(validate("MATCH (n) WHERE n.age > 10 AND n.name = 'Bob' RETURN n"));
}

TEST_F(CypherSyntaxTest, ValidPatterns) {
	// Multi-hop
	EXPECT_TRUE(validate("MATCH (a)-[r1]->(b)-[r2]->(c) RETURN a, c"));
	// Directions
	EXPECT_TRUE(validate("MATCH (a)<-[r]-(b) RETURN a"));
	EXPECT_TRUE(validate("MATCH (a)-[r]-(b) RETURN a"));
}

TEST_F(CypherSyntaxTest, ValidMatchReturn) {
	EXPECT_TRUE(validate("MATCH (n:Person) RETURN n"));
	EXPECT_TRUE(validate("MATCH (n)-[r:KNOWS]->(m) RETURN r"));
	// Test Property Access in WHERE
	EXPECT_TRUE(validate("MATCH (n) WHERE n.age > 10 RETURN n"));
	// Test multiple paths
	EXPECT_TRUE(validate("MATCH (a), (b) RETURN a, b"));
}

// ============================================================================
// 2. Write Operations (CREATE, DELETE, SET)
// ============================================================================

TEST_F(CypherSyntaxTest, ValidCreate) {
	EXPECT_TRUE(validate("CREATE (n:User {name: 'Alice'})"));
	EXPECT_TRUE(validate("CREATE (a)-[:REL]->(b)"));
	EXPECT_TRUE(validate("CREATE (a), (b)"));
}

TEST_F(CypherSyntaxTest, ValidDelete) {
	// Basic Delete
	EXPECT_TRUE(validate("MATCH (n) DELETE n"));
	// Detach Delete
	EXPECT_TRUE(validate("MATCH (n) DETACH DELETE n"));
	// Delete multiple
	EXPECT_TRUE(validate("MATCH (n)-[r]->(m) DELETE n, r, m"));
}

TEST_F(CypherSyntaxTest, ValidSet) {
	// Set Property
	EXPECT_TRUE(validate("MATCH (n) SET n.age = 30"));
	// Set Multiple Properties
	EXPECT_TRUE(validate("MATCH (n) SET n.age = 30, n.name = 'Bob'"));
	// Set Label
	EXPECT_TRUE(validate("MATCH (n) SET n:Active"));
}

TEST_F(CypherSyntaxTest, ValidMerge) {
	// MERGE matches standard syntax
	EXPECT_TRUE(validate("MERGE (n:User {name: 'Alice'})"));
	EXPECT_TRUE(validate("MERGE (n:User {name: 'Alice'}) ON CREATE SET n.created = 123"));
	EXPECT_TRUE(validate("MERGE (n:User {name: 'Alice'}) ON MATCH SET n.accessed = 123"));
}

// ============================================================================
// 3. Administration & Procedures
// ============================================================================

TEST_F(CypherSyntaxTest, ValidIndexCommands) {
	// 1. Legacy Syntax (Anonymous)
	EXPECT_TRUE(validate("CREATE INDEX ON :User(name)"));
	EXPECT_TRUE(validate("DROP INDEX ON :User(name)"));

	// 2. Named Index Syntax
	// CREATE INDEX name FOR (n:Label) ON (n.prop)
	EXPECT_TRUE(validate("CREATE INDEX my_idx FOR (n:User) ON (n.name)"));

	// DROP INDEX name
	EXPECT_TRUE(validate("DROP INDEX my_idx"));

	// SHOW
	EXPECT_TRUE(validate("SHOW INDEXES"));
}

TEST_F(CypherSyntaxTest, ValidProcedures) {
	EXPECT_TRUE(validate("CALL dbms.setConfig('key', 'value')"));
	EXPECT_TRUE(validate("CALL dbms.listConfig()"));
	// Test WITH/YIELD if grammar supports it
	EXPECT_TRUE(validate("CALL dbms.listConfig() YIELD key, value"));
}

// ============================================================================
// 4. Invalid Syntax Tests (Should Fail)
// ============================================================================

TEST_F(CypherSyntaxTest, InvalidKeywords) {
	// misspelled MATCH
	EXPECT_FALSE(validate("MTCH (n) RETURN n"));
}

TEST_F(CypherSyntaxTest, MissingParentheses) {
	// Node missing parens
	EXPECT_FALSE(validate("MATCH n:User RETURN n"));
	// Index syntax error (Missing ON)
	EXPECT_FALSE(validate("CREATE INDEX :User(name)"));
}

TEST_F(CypherSyntaxTest, InvalidDelete) {
	// DELETE requires variables, not patterns
	// Note: Standard Cypher DELETE takes expressions, usually variables.
	// "DELETE (n)" is actually valid in some contexts as expression, but let's test strict logic if possible.
	// Actually, DETACH DELETE must be used if relationships exist, but that's a runtime check.

	// Syntax error: DELETE keyword alone
	EXPECT_FALSE(validate("MATCH (n) DELETE"));
}

TEST_F(CypherSyntaxTest, UnfinishedQuery) {
	EXPECT_FALSE(validate("MATCH (n) WHERE n.age >"));
	EXPECT_FALSE(validate("CREATE (n:User {name: 'A'"));
}

TEST_F(CypherSyntaxTest, InvalidWhereSyntax) {
	// Missing operator
	EXPECT_FALSE(validate("MATCH (n) WHERE n.age 10"));
	// Missing property
	EXPECT_FALSE(validate("MATCH (n) WHERE > 10"));
}

TEST_F(CypherSyntaxTest, InvalidIndexSyntax) {
	// Missing ON
	EXPECT_FALSE(validate("CREATE INDEX :User(name)"));
	// Missing Parens (if grammar enforces them)
	EXPECT_FALSE(validate("CREATE INDEX ON :User name"));
}

// ============================================================================
// 5. Extended Write Syntax Tests (MERGE, REMOVE, SET Label)
// ============================================================================

TEST_F(CypherSyntaxTest, ValidMergeComplex) {
	// Standard Merge
	EXPECT_TRUE(validate("MERGE (n:User {id: 1})"));

	// Merge with On Create
	EXPECT_TRUE(validate("MERGE (n:User {id: 1}) ON CREATE SET n.created = 123"));

	// Merge with On Match
	EXPECT_TRUE(validate("MERGE (n:User {id: 1}) ON MATCH SET n.updated = true"));

	// Full Merge
	EXPECT_TRUE(validate("MERGE (n:User {id: 1}) ON CREATE SET n.c = 1 ON MATCH SET n.u = 1"));

	// Merge with multiple SET items
	EXPECT_TRUE(validate("MERGE (n:User {id: 1}) ON CREATE SET n.prop1 = 1, n.prop2 = 'A'"));
}

TEST_F(CypherSyntaxTest, ValidSetLabel) {
	// Set Label
	EXPECT_TRUE(validate("MATCH (n) SET n:Active"));
	// Set Multiple Labels (Standard Cypher allows this, though MetrixDB might support only one currently)
	EXPECT_TRUE(validate("MATCH (n) SET n:Active:Admin"));
	// Mixed Set (Property and Label)
	EXPECT_TRUE(validate("MATCH (n) SET n.age = 10, n:Person"));
}

TEST_F(CypherSyntaxTest, ValidRemove) {
	// Remove Property
	EXPECT_TRUE(validate("MATCH (n) REMOVE n.prop"));
	// Remove Label
	EXPECT_TRUE(validate("MATCH (n) REMOVE n:OldLabel"));
	// Mixed Remove
	EXPECT_TRUE(validate("MATCH (n) REMOVE n.prop, n:Label"));
}

TEST_F(CypherSyntaxTest, InvalidMergeSyntax) {
	// MERGE requires a pattern
	EXPECT_FALSE(validate("MERGE"));
	// Invalid action keyword
	EXPECT_FALSE(validate("MERGE (n) ON DELETE SET n.a=1"));
}
