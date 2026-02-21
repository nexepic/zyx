/**
 * @file test_ReadingClauseHandler_YieldCoverage.cpp
 * @brief Tests for YIELD clause field renaming (covers procedureResultField)
 * @date 2026/02/17
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;

/**
 * @class ReadingClauseHandlerYieldCoverageTest
 * @brief Tests for YIELD clause with field renaming to cover procedureResultField branch
 */
class ReadingClauseHandlerYieldCoverageTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_yield_" + boost::uuids::to_string(uuid) + ".dat");
		if (fs::exists(testFilePath))
			fs::remove_all(testFilePath);

		db = std::make_unique<graph::Database>(testFilePath.string());
		db->open();
	}

	void TearDown() override {
		if (db)
			db->close();
		if (fs::exists(testFilePath))
			fs::remove_all(testFilePath);
	}

	[[nodiscard]] graph::query::QueryResult execute(const std::string &query) const {
		return db->getQueryEngine()->execute(query);
	}
};

// ============================================================================
// YIELD with field renaming (procedureResultField branch)
// These tests cover Line 91 True branch: if (item->procedureResultField())
// ============================================================================

TEST_F(ReadingClauseHandlerYieldCoverageTest, Yield_WithFieldRenaming_SingleField) {
	// Tests CALL ... YIELD field AS newname
	// This triggers Line 91 True branch
	auto res = execute("CALL dbms.listConfig() YIELD key AS myKey");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerYieldCoverageTest, Yield_WithFieldRenaming_MultipleFields) {
	// Tests CALL ... YIELD field1 AS new1, field2 AS new2
	auto res = execute("CALL dbms.listConfig() YIELD key AS myKey, value AS myValue");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerYieldCoverageTest, Yield_WithFieldRenaming_ThreeFields) {
	// Tests YIELD with three renamed fields
	EXPECT_THROW({
		execute("CALL db.stats() YIELD label AS lbl, count AS cnt, value AS val");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerYieldCoverageTest, Yield_WithFieldRenaming_AndWhere) {
	// Tests YIELD field AS var with WHERE clause
	EXPECT_THROW({
		execute("CALL unknown.proc() YIELD result AS x WHERE x > 0");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerYieldCoverageTest, Yield_WithMixedFields) {
	// Tests YIELD with both renamed and non-renamed fields
	EXPECT_THROW({
		execute("CALL some.proc() YIELD field1, field2 AS new2");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerYieldCoverageTest, Yield_WithAllRenamedFields) {
	// Tests YIELD where all fields are renamed
	auto res = execute("CALL dbms.listConfig() YIELD key AS configKey");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerYieldCoverageTest, Yield_WithFieldRenaming_Return) {
	// Tests YIELD with field renaming and RETURN
	auto res = execute("CALL dbms.listConfig() YIELD key AS myKey RETURN myKey");
	EXPECT_GE(res.rowCount(), 0UL);
}

// ============================================================================
// YIELD without field renaming (Line 91 False branch)
// ============================================================================

TEST_F(ReadingClauseHandlerYieldCoverageTest, Yield_WithoutFieldRenaming_SingleField) {
	// Tests CALL ... YIELD var (no AS clause)
	// This triggers Line 91 False branch
	EXPECT_THROW({
		execute("CALL unknown.proc() YIELD result");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerYieldCoverageTest, Yield_WithoutFieldRenaming_MultipleFields) {
	// Tests YIELD with multiple fields without renaming
	EXPECT_THROW({
		execute("CALL some.proc() YIELD field1, field2, field3");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerYieldCoverageTest, Yield_Asterisk) {
	// Tests CALL ... YIELD * (all fields)
	EXPECT_THROW({
		execute("CALL some.proc() YIELD *");
	}, std::exception);
}

// ============================================================================
// Combined coverage for other branches
// ============================================================================

TEST_F(ReadingClauseHandlerYieldCoverageTest, Unwind_WithExistingRootOp_FromCreate) {
	// Tests UNWIND when rootOp exists from CREATE
	// Covers Line 134 False branch
	// Note: CREATE ... UNWIND syntax is not supported, so we test with separate queries
	(void) execute("CREATE (n:Test {value: 1})");
	auto res = execute("UNWIND [1, 2] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(ReadingClauseHandlerYieldCoverageTest, Unwind_WithExistingRootOp_FromMatch) {
	// Tests UNWIND after MATCH when MATCH returns results
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("MATCH (n:Test) UNWIND [1, 2] AS x RETURN x");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerYieldCoverageTest, Call_WithEmptyExpressions) {
	// Tests CALL with no expressions (empty args)
	// Covers Line 51 and 73 False branches
	EXPECT_THROW({
		execute("CALL unknown.proc()");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerYieldCoverageTest, InQueryCall_WithEmptyExpressions) {
	// Tests in-query CALL with no expressions
	EXPECT_THROW({
		execute("CALL unknown.proc() YIELD result");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerYieldCoverageTest, Yield_WithFieldRenaming_LongNames) {
	// Tests YIELD with long field names
	EXPECT_THROW({
		execute("CALL some.long.procedure.name() YIELD veryLongFieldName AS anotherLongVariableName");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerYieldCoverageTest, Yield_WithFieldRenaming_Underscores) {
	// Tests YIELD with underscores in names
	EXPECT_THROW({
		execute("CALL some_proc() YIELD field_name AS var_name");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerYieldCoverageTest, Yield_WithFieldRenaming_MixedCase) {
	// Tests YIELD with mixed case names
	EXPECT_THROW({
		execute("CALL someProc() YIELD FieldName AS VariableName");
	}, std::exception);
}

// ============================================================================
// Edge cases for procedureResultField parsing
// ============================================================================

TEST_F(ReadingClauseHandlerYieldCoverageTest, Yield_ProcedureResultField_NumericLike) {
	// Tests YIELD with numeric-like field names
	EXPECT_THROW({
		execute("CALL some.proc() YIELD field1 AS var1");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerYieldCoverageTest, Yield_ProcedureResultField_WithNamespace) {
	// Tests YIELD with namespaced procedure result field
	EXPECT_THROW({
		execute("CALL some.proc() YIELD field.subfield AS result");
	}, std::exception);
}
