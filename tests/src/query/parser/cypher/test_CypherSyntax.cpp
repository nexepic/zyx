/**
 * @file test_CypherSyntax.cpp
 * @author Nexepic
 * @date 2025/12/17
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
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

// ============================================================================
// 6. Pagination Syntax Tests (LIMIT & SKIP)
// ============================================================================

TEST_F(CypherSyntaxTest, ValidPagination) {
	// Limit only
	EXPECT_TRUE(validate("MATCH (n) RETURN n LIMIT 10"));

	// Skip only
	EXPECT_TRUE(validate("MATCH (n) RETURN n SKIP 5"));

	// Skip and Limit (Standard order)
	EXPECT_TRUE(validate("MATCH (n) RETURN n SKIP 5 LIMIT 10"));

	// With Where clause
	EXPECT_TRUE(validate("MATCH (n) WHERE n.age > 20 RETURN n LIMIT 1"));
}

TEST_F(CypherSyntaxTest, ValidPaginationWithExpressions) {
	// Although the visitor currently implements simple literals,
	// the grammar supports expressions. We validate the syntax accepts them.
	EXPECT_TRUE(validate("MATCH (n) RETURN n LIMIT 1 + 2"));
}

TEST_F(CypherSyntaxTest, InvalidPaginationSyntax) {
	// Missing value
	EXPECT_FALSE(validate("MATCH (n) RETURN n LIMIT"));

	// Wrong order (In Cypher, Order By -> Skip -> Limit)
	// While specific parsers might be lenient, standard grammar usually enforces order.
	// Based on your grammar: projectionBody : ... order? skip? limit?
	// So LIMIT SKIP is invalid.
	EXPECT_FALSE(validate("MATCH (n) RETURN n LIMIT 10 SKIP 5"));
}

// ============================================================================
// 7. Order By Syntax Tests
// ============================================================================

TEST_F(CypherSyntaxTest, ValidOrderBy) {
	EXPECT_TRUE(validate("MATCH (n) RETURN n ORDER BY n.age"));
	EXPECT_TRUE(validate("MATCH (n) RETURN n ORDER BY n.age ASC"));
	EXPECT_TRUE(validate("MATCH (n) RETURN n ORDER BY n.age DESC"));
	// Multiple keys
	EXPECT_TRUE(validate("MATCH (n) RETURN n ORDER BY n.age DESC, n.name ASC"));
	// Pagination combined
	EXPECT_TRUE(validate("MATCH (n) RETURN n ORDER BY n.age SKIP 5 LIMIT 10"));
}

// ============================================================================
// 8. Hops Syntax Tests
// ============================================================================

TEST_F(CypherSyntaxTest, ValidVarLengthPath) {
	// 1. Any length (* defaults)
	EXPECT_TRUE(validate("MATCH (a)-[*]->(b) RETURN b"));

	// 2. Fixed length (*3)
	EXPECT_TRUE(validate("MATCH (a)-[*3]->(b) RETURN b"));

	// 3. Range (*1..5)
	EXPECT_TRUE(validate("MATCH (a)-[*1..5]->(b) RETURN b"));

	// 4. Open-ended ranges
	EXPECT_TRUE(validate("MATCH (a)-[*2..]->(b) RETURN b"));
	EXPECT_TRUE(validate("MATCH (a)-[*..5]->(b) RETURN b"));

	// 5. With Label and Properties
	EXPECT_TRUE(validate("MATCH (a)-[:ROAD*1..5]->(b) RETURN b"));
}

// ============================================================================
// Cartesian Product Tests (Cross Joins)
// ============================================================================

TEST_F(CypherSyntaxTest, ValidCartesianProduct) {
	// 1. Comma-separated patterns (Implicit Cartesian Product)
	// MATCH (a), (b) -> Cartesian Product of a and b
	EXPECT_TRUE(validate("MATCH (a), (b) RETURN a, b"));

	// 2. Multiple MATCH clauses (Explicit Cartesian Product)
	EXPECT_TRUE(validate("MATCH (a) MATCH (b) RETURN a, b"));

	// 3. Mixed Pattern with Relationship and Independent Node
	// (a)->(b) AND (c) are disconnected
	EXPECT_TRUE(validate("MATCH (a)-[:REL]->(b), (c) RETURN a, b, c"));

	// 4. Complex Chain with multiple MATCH
	EXPECT_TRUE(validate("MATCH (a:User) WHERE a.age > 10 MATCH (b:Product) RETURN a, b"));
}

// ============================================================================
// UNWIND Syntax Tests
// ============================================================================

TEST_F(CypherSyntaxTest, ValidUnwind) {
	// 1. Simple Integer List
	EXPECT_TRUE(validate("UNWIND [1, 2, 3] AS x RETURN x"));

	// 2. Mixed Types
	EXPECT_TRUE(validate("UNWIND [1, 'two', true] AS x RETURN x"));

	// 3. Unwind after Match
	EXPECT_TRUE(validate("MATCH (n) UNWIND [1, 2] AS id RETURN n, id"));

	// 4. Batch Insert Pattern
	EXPECT_TRUE(validate("UNWIND [1, 2] AS id CREATE (n {id: id})"));
}

TEST_F(CypherSyntaxTest, InvalidUnwind) {
	// Missing AS
	EXPECT_FALSE(validate("UNWIND [1, 2] x RETURN x"));

	// Missing Expression
	EXPECT_FALSE(validate("UNWIND AS x RETURN x"));

	// Not a list (Grammatically, UNWIND accepts any expression, but runtime checks type.
	// However, syntax-wise 'UNWIND 1 AS x' is valid Cypher, though it might fail or unwind to single item at runtime.
	// So we test syntax error, e.g. malformed list)
	EXPECT_FALSE(validate("UNWIND [1, 2 AS x")); // Missing bracket
}
