/**
 * @file test_CypherToPlanVisitor_Branches.cpp
 * @brief Focused branch tests for CypherToPlanVisitor control paths.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>

#include "query/parser/cypher/CypherToPlanVisitor.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;

class CypherToPlanVisitorBranchesTest : public ::testing::Test {
protected:
	fs::path testFilePath;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
	std::shared_ptr<graph::query::QueryPlanner> planner;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_cypher_visitor_" + boost::uuids::to_string(uuid) + ".db");

		storage = std::make_shared<graph::storage::FileStorage>(
			testFilePath.string(), 4096, graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
		storage->open();
		dataManager = storage->getDataManager();
		indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
		planner = std::make_shared<graph::query::QueryPlanner>(dataManager, indexManager);
	}

	void TearDown() override {
		if (storage) {
			storage->close();
		storage.reset();
		}
		std::error_code ec;
		if (fs::exists(testFilePath)) {
			fs::remove(testFilePath, ec);
		}
	}
};

TEST_F(CypherToPlanVisitorBranchesTest, GetPlanAndLogicalPlanReturnNullWhenRootIsMissing) {
	graph::parser::cypher::CypherToPlanVisitor visitor(planner);

	auto logical = visitor.getLogicalPlan();
	EXPECT_EQ(logical, nullptr);

	auto physical = visitor.getPlan();
	EXPECT_EQ(physical, nullptr);
}

TEST_F(CypherToPlanVisitorBranchesTest, VisitTxnBeginBuildsTransactionControlPlan) {
	graph::parser::cypher::CypherToPlanVisitor visitor(planner);

	visitor.visitTxnBegin(nullptr);
	auto logical = visitor.getLogicalPlan();
	ASSERT_NE(logical, nullptr);
	EXPECT_EQ(logical->getType(), graph::query::logical::LogicalOpType::LOP_TRANSACTION_CONTROL);
	EXPECT_NE(logical->toString().find("BEGIN"), std::string::npos);
}

TEST_F(CypherToPlanVisitorBranchesTest, VisitTxnCommitBuildsTransactionControlPlan) {
	graph::parser::cypher::CypherToPlanVisitor visitor(planner);

	visitor.visitTxnCommit(nullptr);
	auto logical = visitor.getLogicalPlan();
	ASSERT_NE(logical, nullptr);
	EXPECT_EQ(logical->getType(), graph::query::logical::LogicalOpType::LOP_TRANSACTION_CONTROL);
	EXPECT_NE(logical->toString().find("COMMIT"), std::string::npos);
}

TEST_F(CypherToPlanVisitorBranchesTest, VisitTxnRollbackBuildsTransactionControlPlan) {
	graph::parser::cypher::CypherToPlanVisitor visitor(planner);

	visitor.visitTxnRollback(nullptr);
	auto logical = visitor.getLogicalPlan();
	ASSERT_NE(logical, nullptr);
	EXPECT_EQ(logical->getType(), graph::query::logical::LogicalOpType::LOP_TRANSACTION_CONTROL);
	EXPECT_NE(logical->toString().find("ROLLBACK"), std::string::npos);
}

// ============================================================================
// Tests that exercise CypherToPlanVisitor through the full parse pipeline
// ============================================================================

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

class CypherToPlanVisitorEndToEndTest : public ::testing::Test {
protected:
	fs::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_visitor_e2e_" + boost::uuids::to_string(uuid) + ".db");
		db = std::make_unique<graph::Database>(testFilePath.string());
		db->open();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}

	[[nodiscard]] graph::query::QueryResult execute(const std::string &q) const {
		return db->getQueryEngine()->execute(q);
	}
};

TEST_F(CypherToPlanVisitorEndToEndTest, ExplainStatement) {
	EXPECT_NO_THROW(execute("EXPLAIN MATCH (n) RETURN n"));
}

TEST_F(CypherToPlanVisitorEndToEndTest, ProfileStatement) {
	EXPECT_NO_THROW(execute("PROFILE MATCH (n) RETURN n"));
}

TEST_F(CypherToPlanVisitorEndToEndTest, ForeachStatement_CreateNodes) {
	EXPECT_NO_THROW(execute("FOREACH (x IN [1, 2, 3] | CREATE (n:FE {val: x}))"));

	auto check = execute("MATCH (n:FE) RETURN n.val AS val");
	EXPECT_EQ(check.rowCount(), 3UL);
}

TEST_F(CypherToPlanVisitorEndToEndTest, CallSubquery_Basic) {
	(void) execute("CREATE (n:Sub {name: 'Alice'})");
	(void) execute("CREATE (n:Sub {name: 'Bob'})");

	auto res = execute("MATCH (n:Sub) CALL { WITH n RETURN n.name AS subName } RETURN subName");
	EXPECT_EQ(res.rowCount(), 2UL);
}

TEST_F(CypherToPlanVisitorEndToEndTest, ExplainWithCreate) {
	EXPECT_NO_THROW(execute("EXPLAIN CREATE (n:Test {name: 'x'})"));
}

TEST_F(CypherToPlanVisitorEndToEndTest, ProfileWithMatch) {
	(void) execute("CREATE (n:Prof {v: 1})");
	EXPECT_NO_THROW(execute("PROFILE MATCH (n:Prof) RETURN n.v"));
}

TEST_F(CypherToPlanVisitorEndToEndTest, GetLogicalPlanOptimizesReadPlan) {
	// This exercises the optimization branch in getLogicalPlan()
	(void) execute("CREATE (a:Opt {x: 1})-[:R]->(b:Opt {x: 2})");
	auto res = execute("MATCH (a:Opt)-[:R]->(b:Opt) WHERE a.x = 1 RETURN b.x");
	EXPECT_FALSE(res.isEmpty());
}

TEST_F(CypherToPlanVisitorEndToEndTest, ShowIndexesSkipsOptimizer) {
	// DDL commands should skip the optimizer branch
	EXPECT_NO_THROW(execute("SHOW INDEXES"));
}

TEST_F(CypherToPlanVisitorEndToEndTest, ForeachWithSet) {
	(void) execute("CREATE (n:FS {val: 0})");
	EXPECT_NO_THROW(execute("MATCH (n:FS) FOREACH (x IN [10] | SET n.val = x)"));
}

// ============================================================================
// Branch coverage: isOptimizable chain in getLogicalPlan() (lines 52-64)
// Each test forces a specific DDL/admin type as root to cover its != branch.
// ============================================================================

TEST_F(CypherToPlanVisitorEndToEndTest, CreateIndexSkipsOptimizer) {
	EXPECT_NO_THROW(execute("CREATE INDEX ON :CILabel(prop)"));
}

TEST_F(CypherToPlanVisitorEndToEndTest, DropIndexSkipsOptimizer) {
	(void) execute("CREATE INDEX ON :DILabel(prop)");
	EXPECT_NO_THROW(execute("DROP INDEX ON :DILabel(prop)"));
}

TEST_F(CypherToPlanVisitorEndToEndTest, CreateVectorIndexSkipsOptimizer) {
	EXPECT_NO_THROW(execute("CREATE VECTOR INDEX ON :VILabel(emb) OPTIONS {dimension: 4}"));
}

TEST_F(CypherToPlanVisitorEndToEndTest, CreateConstraintSkipsOptimizer) {
	EXPECT_NO_THROW(execute("CREATE CONSTRAINT cc1 FOR (n:CCLabel) REQUIRE n.id IS UNIQUE"));
}

TEST_F(CypherToPlanVisitorEndToEndTest, DropConstraintSkipsOptimizer) {
	EXPECT_NO_THROW(execute("CREATE CONSTRAINT dc1 FOR (n:DCLabel) REQUIRE n.id IS UNIQUE"));
	EXPECT_NO_THROW(execute("DROP CONSTRAINT dc1"));
}

TEST_F(CypherToPlanVisitorEndToEndTest, ShowConstraintsSkipsOptimizer) {
	EXPECT_NO_THROW(execute("SHOW CONSTRAINT"));
}

TEST_F(CypherToPlanVisitorEndToEndTest, CallProcedureSkipsOptimizer) {
	// Standalone CALL procedure — exercises LOP_CALL_PROCEDURE root type
	EXPECT_NO_THROW(execute("CALL dbms.listConfig()"));
}

// ============================================================================
// Branch coverage: visitCallSubquery IN TRANSACTIONS (lines 372-381)
// ============================================================================

TEST_F(CypherToPlanVisitorEndToEndTest, CallSubquery_InTransactions) {
	(void) execute("CREATE (n:TxSub {val: 1})");
	EXPECT_NO_THROW(
		execute("MATCH (n:TxSub) CALL { WITH n CREATE (:TxResult {v: n.val}) } IN TRANSACTIONS RETURN n.val AS v"));
}

TEST_F(CypherToPlanVisitorEndToEndTest, CallSubquery_InTransactionsOfNRows) {
	(void) execute("CREATE (n:TxBatch {val: 1})");
	EXPECT_NO_THROW(
		execute("MATCH (n:TxBatch) CALL { WITH n CREATE (:TxBatchResult {v: n.val}) } IN TRANSACTIONS OF 10 ROWS RETURN n.val AS v"));
}

// ============================================================================
// Branch coverage: visitLoadCsvStatement FIELDTERMINATOR (lines 400-408)
// ============================================================================

#include <fstream>

class CypherToPlanVisitorCsvTest : public CypherToPlanVisitorEndToEndTest {
protected:
	std::vector<fs::path> csvFiles;

	void TearDown() override {
		CypherToPlanVisitorEndToEndTest::TearDown();
		std::error_code ec;
		for (auto& f : csvFiles) {
			if (fs::exists(f)) fs::remove(f, ec);
		}
	}

	std::string createTempCsv(const std::string& content) {
		auto uuid = boost::uuids::random_generator()();
		auto path = fs::temp_directory_path() / ("test_csv_visitor_" + boost::uuids::to_string(uuid) + ".csv");
		std::ofstream out(path);
		out << content;
		out.close();
		csvFiles.push_back(path);
		// Convert path to use forward slashes for Cypher compatibility
		// Windows backslashes cause Cypher lexer to treat \U as escape sequence
		auto pathStr = path.string();
		std::replace(pathStr.begin(), pathStr.end(), '\\', '/');
		return pathStr;
	}
};

TEST_F(CypherToPlanVisitorCsvTest, LoadCsv_FieldTerminator_Semicolon) {
	// Covers FIELDTERMINATOR branch with non-\t delimiter (line 407 false branch)
	auto csvPath = createTempCsv("name;age\nAlice;30\nBob;25\n");
	auto res = execute(
		"LOAD CSV WITH HEADERS FROM 'file:///" + csvPath +
		"' AS row FIELDTERMINATOR ';' "
		"RETURN row.name AS name ORDER BY name");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(CypherToPlanVisitorCsvTest, LoadCsv_FieldTerminator_Tab) {
	// Covers FIELDTERMINATOR branch with \t (line 407 true branch)
	auto csvPath = createTempCsv("name\tage\nAlice\t30\n");
	auto res = execute(
		"LOAD CSV WITH HEADERS FROM 'file:///" + csvPath +
		"' AS row FIELDTERMINATOR '\\t' "
		"RETURN row.name AS name");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherToPlanVisitorCsvTest, LoadCsv_WithoutHeaders) {
	// Covers withHeaders = false branch (line 397)
	auto csvPath = createTempCsv("Alice,30\nBob,25\n");
	auto res = execute(
		"LOAD CSV FROM 'file:///" + csvPath +
		"' AS row "
		"RETURN row[0] AS name ORDER BY name");
	ASSERT_EQ(res.rowCount(), 2UL);
}

// ============================================================================
// Branch coverage: visitCallSubquery importedVars detection (lines 358-362)
// ============================================================================

TEST_F(CypherToPlanVisitorEndToEndTest, CallSubquery_ImportsOuterVariable) {
	// The WITH n in subquery imports 'n' from outer scope,
	// covering the savedScope.count(var) == true branch
	(void) execute("CREATE (n:ImportTest {name: 'Alice'})");
	auto res = execute(
		"MATCH (n:ImportTest) "
		"CALL { WITH n RETURN n.name AS imported } "
		"RETURN imported");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherToPlanVisitorEndToEndTest, CallSubquery_NoImportedVars) {
	// Subquery without WITH — no imported vars loop
	(void) execute("CREATE (n:NoImport {val: 1})");
	auto res = execute(
		"MATCH (n:NoImport) "
		"CALL { MATCH (m:NoImport) RETURN m.val AS inner } "
		"RETURN inner");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(CypherToPlanVisitorEndToEndTest, CallSubquery_WithStar) {
	// WITH * imports all outer variables — covers MULTIPLY() true branch
	(void) execute("CREATE (n:StarImport {name: 'Alice'})");
	auto res = execute(
		"MATCH (n:StarImport) "
		"CALL { WITH * RETURN n.name AS imported } "
		"RETURN imported");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherToPlanVisitorEndToEndTest, CallSubquery_WithAlias) {
	// WITH n AS m — covers K_AS() true branch and savedScope.count false for alias
	(void) execute("CREATE (n:AliasImport {name: 'Bob'})");
	auto res = execute(
		"MATCH (n:AliasImport) "
		"CALL { WITH n AS m RETURN m.name AS imported } "
		"RETURN imported");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// Branch coverage: visitRegularQuery — UNION paths
// ============================================================================

TEST_F(CypherToPlanVisitorEndToEndTest, UnionAll) {
	auto res = execute("RETURN 1 AS val UNION ALL RETURN 2 AS val");
	EXPECT_EQ(res.rowCount(), 2UL);
}

TEST_F(CypherToPlanVisitorEndToEndTest, UnionDistinct) {
	auto res = execute("RETURN 1 AS val UNION RETURN 1 AS val");
	EXPECT_EQ(res.rowCount(), 1UL);
}
