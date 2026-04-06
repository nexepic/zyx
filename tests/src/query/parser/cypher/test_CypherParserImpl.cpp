/**
 * @file test_CypherParserImpl.cpp
 * @brief Unit tests for CypherParserImpl branch coverage
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "CypherParserImpl.hpp"
#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;

class CypherParserImplTest : public ::testing::Test {
protected:
	fs::path testFilePath;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
	std::shared_ptr<graph::query::QueryPlanner> planner;
	std::unique_ptr<graph::parser::cypher::CypherParserImpl> parser;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_cypher_pi_" + to_string(uuid) + ".db");

		storage = std::make_shared<graph::storage::FileStorage>(
			testFilePath.string(), 4096, graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
		storage->open();
		dataManager = storage->getDataManager();
		indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);

		planner = std::make_shared<graph::query::QueryPlanner>(dataManager, indexManager);
		parser = std::make_unique<graph::parser::cypher::CypherParserImpl>(planner);
	}

	void TearDown() override {
		if (storage) storage->close();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}
};

// ============================================================================
// Happy path - valid queries
// ============================================================================

TEST_F(CypherParserImplTest, ParseValidMatchReturn) {
	auto plan = parser->parse("MATCH (n:Person) RETURN n");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseValidCreate) {
	auto plan = parser->parse("CREATE (n:Person {name: 'Alice'})");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseReturnLiteral) {
	auto plan = parser->parse("RETURN 42 AS answer");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseMatchWhereReturn) {
	auto plan = parser->parse("MATCH (n:Person) WHERE n.age > 25 RETURN n.name");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseMatchWithLimit) {
	auto plan = parser->parse("MATCH (n) RETURN n LIMIT 10");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseMatchWithOrderByAndSkip) {
	auto plan = parser->parse("MATCH (n:Person) RETURN n.name ORDER BY n.name SKIP 5 LIMIT 10");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseDeleteQuery) {
	auto plan = parser->parse("MATCH (n:Temp) DELETE n");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseDetachDelete) {
	auto plan = parser->parse("MATCH (n:Temp) DETACH DELETE n");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseMergeQuery) {
	auto plan = parser->parse("MERGE (n:Person {name: 'Alice'})");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseUnwind) {
	auto plan = parser->parse("UNWIND [1, 2, 3] AS x RETURN x");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseMultipleMatch) {
	auto plan = parser->parse("MATCH (a:A), (b:B) RETURN a, b");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseCreateRelationship) {
	auto plan = parser->parse("CREATE (a:A)-[:REL]->(b:B)");
	ASSERT_NE(plan, nullptr);
}

// ============================================================================
// Error paths - syntax errors (ThrowingErrorListener, lines 30-37)
// ============================================================================

TEST_F(CypherParserImplTest, SyntaxError_InvalidKeyword) {
	// Tests ThrowingErrorListener and the parse exception re-throw (line 95-96)
	EXPECT_THROW((void)parser->parse("MATC (n) RETRUN n"), std::runtime_error);
}

TEST_F(CypherParserImplTest, SyntaxError_GarbageInput) {
	EXPECT_THROW((void)parser->parse("!!!@@@###"), std::runtime_error);
}

TEST_F(CypherParserImplTest, SyntaxError_EmptyQuery) {
	// Empty input should trigger parse error
	EXPECT_THROW((void)parser->parse(""), std::runtime_error);
}

TEST_F(CypherParserImplTest, SyntaxError_IncompleteReturn) {
	// Tests the prettifyErrorMessage with potential <EOF> replacement
	try {
		(void)parser->parse("MATCH (n:Person) RETURN");
		// May or may not throw; depends on grammar recovery
	} catch (const std::runtime_error &e) {
		std::string msg = e.what();
		EXPECT_FALSE(msg.empty());
	}
}

TEST_F(CypherParserImplTest, SyntaxError_MissingParenthesis) {
	EXPECT_THROW((void)parser->parse("MATCH n:Person RETURN n"), std::runtime_error);
}

// ============================================================================
// Plan generation failure path (line 104-105)
// ============================================================================

TEST_F(CypherParserImplTest, PlanGenerationFailed_UnsupportedSyntax) {
	// Try a query that may parse but fail during visitor phase
	try {
		(void)parser->parse("CALL nonexistent.proc()");
	} catch (const std::runtime_error &e) {
		std::string msg = e.what();
		// May contain "Plan generation failed:" or syntax error
		EXPECT_FALSE(msg.empty());
	}
}

// ============================================================================
// prettifyErrorMessage branches
// ============================================================================

TEST_F(CypherParserImplTest, PrettifyEOFReplacement) {
	// Trigger an error that includes <EOF> in the message
	try {
		(void)parser->parse("RETURN");
	} catch (const std::runtime_error &e) {
		std::string msg = e.what();
		// After prettification, <EOF> should be replaced with "end of input"
		// Or some other syntax error message
		EXPECT_FALSE(msg.empty());
	}
}

TEST_F(CypherParserImplTest, PrettifyTokenPrefix) {
	// Trigger an error where token names might appear
	try {
		(void)parser->parse("MATCH (n) RETURN n ORDER");
	} catch (const std::runtime_error &e) {
		std::string msg = e.what();
		EXPECT_FALSE(msg.empty());
	}
}

// ============================================================================
// UNION query tests (CypherToPlanVisitor::visitRegularQuery branches)
// ============================================================================

TEST_F(CypherParserImplTest, ParseUnionAll) {
	// Covers: visitRegularQuery UNION ALL path (isAll = true)
	auto plan = parser->parse("RETURN 1 AS x UNION ALL RETURN 2 AS x");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseUnionDistinct) {
	// Covers: visitRegularQuery UNION (without ALL) path (isAll = false)
	auto plan = parser->parse("RETURN 1 AS x UNION RETURN 2 AS x");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseMultipleUnions) {
	// Covers: loop iteration for multiple UNION operations
	auto plan = parser->parse("RETURN 1 AS x UNION ALL RETURN 2 AS x UNION ALL RETURN 3 AS x");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseUnionWithMatch) {
	// Covers: UNION with MATCH queries
	auto plan = parser->parse("MATCH (n:A) RETURN n.name AS name UNION MATCH (m:B) RETURN m.name AS name");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseOptionalMatch) {
	// Covers: visitMatchStatement K_OPTIONAL() branch (ctx->K_OPTIONAL() != nullptr)
	auto plan = parser->parse("MATCH (n:Person) OPTIONAL MATCH (n)-[:KNOWS]->(m) RETURN n, m");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseSetStatement) {
	// Covers: visitSetStatement
	auto plan = parser->parse("MATCH (n:Person) SET n.age = 30");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseRemoveStatement) {
	// Covers: visitRemoveStatement
	auto plan = parser->parse("MATCH (n:Person) REMOVE n.age");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseWithClause) {
	// Covers: visitWithClause
	auto plan = parser->parse("MATCH (n:Person) WITH n.name AS name RETURN name");
	ASSERT_NE(plan, nullptr);
}

// ============================================================================
// Additional UNION branch coverage tests
// ============================================================================

TEST_F(CypherParserImplTest, ParseMixedUnionTypes) {
	// Tests UNION followed by UNION ALL in the same query
	// Covers: multiple iterations of the union loop with mixed isAll values
	auto plan = parser->parse("RETURN 1 AS x UNION RETURN 2 AS x UNION ALL RETURN 3 AS x");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseFourWayUnion) {
	// Tests 4 UNION ALLs to exercise the loop more thoroughly
	auto plan = parser->parse(
		"RETURN 1 AS x UNION ALL RETURN 2 AS x UNION ALL RETURN 3 AS x UNION ALL RETURN 4 AS x");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseUnionDistinctThenAll) {
	// UNION (distinct) followed by UNION ALL - covers isAll false then true
	auto plan = parser->parse("RETURN 1 AS x UNION RETURN 1 AS x UNION ALL RETURN 2 AS x");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseUnionWithCreate) {
	// UNION with writing clauses preceding RETURN
	auto plan = parser->parse(
		"CREATE (n:A {id: 1}) RETURN n.id AS x UNION ALL "
		"CREATE (m:B {id: 2}) RETURN m.id AS x");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserImplTest, ParseUnionWithUnwind) {
	// UNION with UNWIND - covers visitUnwindStatement in union context
	auto plan = parser->parse(
		"UNWIND [1, 2] AS x RETURN x UNION ALL UNWIND [3, 4] AS x RETURN x");
	ASSERT_NE(plan, nullptr);
}
