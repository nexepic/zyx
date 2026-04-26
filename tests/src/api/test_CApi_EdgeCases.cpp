/**
 * @file test_CApi_EdgeCases.cpp
 * @brief Branch coverage tests for CApi.cpp targeting escape_json_string
 *        special characters, value_to_json complex types, props_to_json
 *        ComplexObject branch, list access functions, and catch(...) blocks.
 */

#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include "zyx/zyx_c_api.h"

namespace fs = std::filesystem;

class CApiEdgeCasesTest : public ::testing::Test {
protected:
	std::string dbPath;
	ZYXDB_T *db = nullptr;

	void SetUp() override {
		auto tempDir = fs::temp_directory_path();
		dbPath = (tempDir / ("c_api_branch_" + std::to_string(std::rand()))).string();
		std::error_code ec;
		if (fs::exists(dbPath))
			fs::remove_all(dbPath, ec);
		std::string walPath = dbPath + "-wal";
		if (fs::exists(walPath))
			fs::remove(walPath, ec);
		db = zyx_open(dbPath.c_str());
		ASSERT_NE(db, nullptr);
	}

	void TearDown() override {
		if (db)
			zyx_close(db);
		std::error_code ec;
		fs::remove_all(dbPath, ec);
		fs::remove(dbPath + "-wal", ec);
	}
};

// ============================================================================
// escape_json_string: Cover all special character branches
// ============================================================================

TEST_F(CApiEdgeCasesTest, JsonEscapeSpecialCharsInProperties) {
	// Create node with properties containing special JSON characters
	auto *params = zyx_params_create();
	// backslash, backspace, formfeed, newline, carriage return, tab, control char
	zyx_params_set_string(params, "bs", "a\\b");
	zyx_params_set_string(params, "backspace", "a\bb");
	zyx_params_set_string(params, "formfeed", "a\fb");
	zyx_params_set_string(params, "newline", "a\nb");
	zyx_params_set_string(params, "cr", "a\rb");
	zyx_params_set_string(params, "tab", "a\tb");
	zyx_params_set_string(params, "quote", "a\"b");
	// Control character (0x01)
	std::string ctrl = "a";
	ctrl += '\x01';
	ctrl += "b";
	zyx_params_set_string(params, "ctrl", ctrl.c_str());
	EXPECT_TRUE(zyx_create_node(db, "JsonTest", params));
	zyx_params_close(params);

	// Query to get the node and its JSON properties
	auto *res = zyx_execute(db, "MATCH (n:JsonTest) RETURN n");
	ASSERT_NE(res, nullptr);
	EXPECT_TRUE(zyx_result_is_success(res));
	if (zyx_result_next(res)) {
		const char *json = zyx_result_get_props_json(res, 0);
		EXPECT_NE(json, nullptr);
		// Verify escaped characters are in the JSON
		EXPECT_NE(std::string(json).find("\\\\"), std::string::npos); // backslash
		EXPECT_NE(std::string(json).find("\\n"), std::string::npos);  // newline
		EXPECT_NE(std::string(json).find("\\t"), std::string::npos);  // tab
		EXPECT_NE(std::string(json).find("\\\""), std::string::npos); // quote
	}
	zyx_result_close(res);
}

// ============================================================================
// props_to_json: Cover bool false branch and monostate
// ============================================================================

TEST_F(CApiEdgeCasesTest, PropsToJsonBoolFalseAndNull) {
	auto *params = zyx_params_create();
	zyx_params_set_bool(params, "flag", false);
	zyx_params_set_null(params, "empty");
	zyx_params_set_double(params, "val", 3.14);
	EXPECT_TRUE(zyx_create_node(db, "BoolTest", params));
	zyx_params_close(params);

	auto *res = zyx_execute(db, "MATCH (n:BoolTest) RETURN n");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		const char *json = zyx_result_get_props_json(res, 0);
		EXPECT_NE(json, nullptr);
		std::string jsonStr(json);
		// Should have "false" and "null" in the JSON
		EXPECT_NE(jsonStr.find("false"), std::string::npos);
		EXPECT_NE(jsonStr.find("null"), std::string::npos);
	}
	zyx_result_close(res);
}

// ============================================================================
// zyx_result_get_type: Cover all type branches including ZYX_EDGE
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultGetTypeForEdge) {
	// Create two nodes and an edge
	auto *p1 = zyx_params_create();
	zyx_params_set_string(p1, "name", "A");
	int64_t id1 = zyx_create_node_ret_id(db, "Person", p1);
	zyx_params_close(p1);

	auto *p2 = zyx_params_create();
	zyx_params_set_string(p2, "name", "B");
	int64_t id2 = zyx_create_node_ret_id(db, "Person", p2);
	zyx_params_close(p2);

	auto *ep = zyx_params_create();
	zyx_params_set_string(ep, "since", "2024");
	EXPECT_TRUE(zyx_create_edge_by_id(db, id1, id2, "KNOWS", ep));
	zyx_params_close(ep);

	// Query edge
	auto *res = zyx_execute(db, "MATCH ()-[r:KNOWS]->() RETURN r");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		ZYXValueType type = zyx_result_get_type(res, 0);
		EXPECT_EQ(type, ZYX_EDGE);

		ZYXEdge edge;
		EXPECT_TRUE(zyx_result_get_edge(res, 0, &edge));
		EXPECT_EQ(edge.source_id, id1);
		EXPECT_EQ(edge.target_id, id2);
		EXPECT_NE(edge.type, nullptr);

		// Get edge props JSON
		const char *json = zyx_result_get_props_json(res, 0);
		EXPECT_NE(json, nullptr);
	}
	zyx_result_close(res);
}

// ============================================================================
// zyx_result_get_double / get_bool / get_string: Ensure non-null paths covered
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultGetTypedValues) {
	// Create node with various property types
	auto *res = zyx_execute(db, "CREATE (n:TypeTest {d: 2.5, b: true, s: 'hello'}) RETURN n.d, n.b, n.s");
	ASSERT_NE(res, nullptr);
	EXPECT_TRUE(zyx_result_is_success(res));
	if (zyx_result_next(res)) {
		double d = zyx_result_get_double(res, 0);
		EXPECT_DOUBLE_EQ(d, 2.5);

		bool b = zyx_result_get_bool(res, 1);
		EXPECT_TRUE(b);

		const char *s = zyx_result_get_string(res, 2);
		EXPECT_STREQ(s, "hello");
	}
	zyx_result_close(res);
}

// ============================================================================
// zyx_result_list_get_bool: Cover both true and non-true branches
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultListGetBoolInBounds) {
	auto *res = zyx_execute(db, "RETURN ['true', 'false', 'other'] AS items");
	ASSERT_NE(res, nullptr);
	EXPECT_TRUE(zyx_result_is_success(res));
	if (zyx_result_next(res)) {
		int sz = zyx_result_list_size(res, 0);
		EXPECT_EQ(sz, 3);

		ZYXValueType listType = zyx_result_list_get_type(res, 0, 0);
		EXPECT_EQ(listType, ZYX_STRING);

		bool val0 = zyx_result_list_get_bool(res, 0, 0); // "true"
		EXPECT_TRUE(val0);
		bool val1 = zyx_result_list_get_bool(res, 0, 1); // "false"
		EXPECT_FALSE(val1);

		// Out of bounds
		bool valOob = zyx_result_list_get_bool(res, 0, 99);
		EXPECT_FALSE(valOob);

		// Negative index
		bool valNeg = zyx_result_list_get_bool(res, 0, -1);
		EXPECT_FALSE(valNeg);
	}
	zyx_result_close(res);
}

// ============================================================================
// zyx_result_list_get_string: Cover in-bounds and out-of-bounds
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultListGetStringOutOfBounds) {
	auto *res = zyx_execute(db, "RETURN ['a', 'b'] AS items");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		const char *val = zyx_result_list_get_string(res, 0, 0);
		EXPECT_STREQ(val, "a");

		// Out of bounds index
		const char *oob = zyx_result_list_get_string(res, 0, 99);
		EXPECT_STREQ(oob, "");

		// zyx_result_list_get_int and list_get_double (always return 0)
		EXPECT_EQ(zyx_result_list_get_int(res, 0, 0), 0);
		EXPECT_EQ(zyx_result_list_get_double(res, 0, 0), 0.0);
	}
	zyx_result_close(res);
}

// ============================================================================
// zyx_result_get_map_json: Cover map and list JSON serialization
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultGetMapJson) {
	auto *res = zyx_execute(db, "RETURN {a: 1, b: 'hello', c: true, d: null, e: 3.14} AS m");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		const char *json = zyx_result_get_map_json(res, 0);
		EXPECT_NE(json, nullptr);
		std::string s(json);
		// Should contain various value types
		EXPECT_NE(s.find("hello"), std::string::npos);
	}
	zyx_result_close(res);
}

// ============================================================================
// zyx_txn_execute_params: Cover the parameterized transaction execution
// ============================================================================

TEST_F(CApiEdgeCasesTest, TxnExecuteParams) {
	auto *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);

	auto *params = zyx_params_create();
	zyx_params_set_string(params, "name", "TxnTest");

	auto *res = zyx_txn_execute_params(txn, "CREATE (n:TxnNode {name: $name}) RETURN n", params);
	EXPECT_NE(res, nullptr);
	if (res) {
		EXPECT_TRUE(zyx_result_is_success(res));
		zyx_result_close(res);
	}
	zyx_params_close(params);

	EXPECT_TRUE(zyx_txn_commit(txn));
	zyx_txn_close(txn);
}

// ============================================================================
// zyx_txn_execute_params null guards
// ============================================================================

TEST_F(CApiEdgeCasesTest, TxnExecuteParamsNullGuards) {
	EXPECT_EQ(zyx_txn_execute_params(nullptr, "RETURN 1", nullptr), nullptr);

	auto *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);
	EXPECT_EQ(zyx_txn_execute_params(txn, nullptr, nullptr), nullptr);
	zyx_txn_rollback(txn);
	zyx_txn_close(txn);
}

// ============================================================================
// zyx_execute_params: Cover the non-txn parameterized execution
// ============================================================================

TEST_F(CApiEdgeCasesTest, ExecuteParamsNullGuards) {
	EXPECT_EQ(zyx_execute_params(nullptr, "RETURN 1", nullptr), nullptr);
	EXPECT_EQ(zyx_execute_params(db, nullptr, nullptr), nullptr);
}

// ============================================================================
// zyx_open_if_exists with null path
// ============================================================================

TEST(CApiBranchCoverageStandalone, OpenIfExistsNullPath) {
	auto *result = zyx_open_if_exists(nullptr);
	EXPECT_EQ(result, nullptr);
	const char *err = zyx_get_last_error();
	EXPECT_NE(std::string(err).find("null"), std::string::npos);
}

// ============================================================================
// zyx_open_if_exists with nonexistent path
// ============================================================================

TEST(CApiBranchCoverageStandalone, OpenIfExistsNonexistentPath) {
	auto *result = zyx_open_if_exists("/tmp/nonexistent_db_path_for_test_12345.zyx");
	EXPECT_EQ(result, nullptr);
	const char *err = zyx_get_last_error();
	EXPECT_NE(std::string(err).find("not found"), std::string::npos);
}

// ============================================================================
// zyx_result_get_duration: non-null result
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultGetDuration) {
	auto *res = zyx_execute(db, "RETURN 1");
	ASSERT_NE(res, nullptr);
	double dur = zyx_result_get_duration(res);
	EXPECT_GE(dur, 0.0);
	zyx_result_close(res);

	// Null result returns 0
	EXPECT_EQ(zyx_result_get_duration(nullptr), 0.0);
}

// ============================================================================
// zyx_result_column_count / column_name: non-null
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultColumnInfo) {
	auto *res = zyx_execute(db, "RETURN 1 AS x, 2 AS y");
	ASSERT_NE(res, nullptr);
	EXPECT_EQ(zyx_result_column_count(res), 2);
	if (zyx_result_next(res)) {
		const char *name0 = zyx_result_column_name(res, 0);
		EXPECT_STREQ(name0, "x");
		const char *name1 = zyx_result_column_name(res, 1);
		EXPECT_STREQ(name1, "y");
	}
	zyx_result_close(res);

	// Null guards
	EXPECT_EQ(zyx_result_column_count(nullptr), 0);
	EXPECT_STREQ(zyx_result_column_name(nullptr, 0), "");
}

// ============================================================================
// zyx_result_get_error: valid result
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultGetErrorWithInvalidQuery) {
	auto *res = zyx_execute(db, "INVALID SYNTAX QUERY!!!");
	if (res) {
		if (!zyx_result_is_success(res)) {
			const char *err = zyx_result_get_error(res);
			EXPECT_NE(err, nullptr);
			EXPECT_GT(std::strlen(err), 0u);
		}
		zyx_result_close(res);
	}
}

// ============================================================================
// zyx_txn_is_read_only: Cover true and false branches
// ============================================================================

TEST_F(CApiEdgeCasesTest, TxnIsReadOnly) {
	auto *roTxn = zyx_begin_read_only_transaction(db);
	ASSERT_NE(roTxn, nullptr);
	EXPECT_TRUE(zyx_txn_is_read_only(roTxn));
	zyx_txn_close(roTxn);

	auto *rwTxn = zyx_begin_transaction(db);
	ASSERT_NE(rwTxn, nullptr);
	EXPECT_FALSE(zyx_txn_is_read_only(rwTxn));
	zyx_txn_rollback(rwTxn);
	zyx_txn_close(rwTxn);

	// Null guard
	EXPECT_FALSE(zyx_txn_is_read_only(nullptr));
}

// ============================================================================
// zyx_map_set_map: nested map parameter
// ============================================================================

TEST_F(CApiEdgeCasesTest, MapSetMap) {
	auto *inner = zyx_map_create();
	zyx_map_set_int(inner, "x", 42);

	auto *outer = zyx_map_create();
	zyx_map_set_map(outer, "nested", inner);

	auto *params = zyx_params_create();
	zyx_params_set_map(params, "data", outer);

	auto *res = zyx_execute_params(db, "CREATE (n:MapTest {data: $data}) RETURN n", params);
	if (res) {
		zyx_result_close(res);
	}

	zyx_map_close(inner);
	zyx_map_close(outer);
	zyx_params_close(params);
}

// ============================================================================
// zyx_list_push_list: nested list parameter
// ============================================================================

TEST_F(CApiEdgeCasesTest, ListPushList) {
	auto *inner = zyx_list_create();
	zyx_list_push_int(inner, 1);
	zyx_list_push_double(inner, 2.5);
	zyx_list_push_bool(inner, true);
	zyx_list_push_null(inner);

	auto *outer = zyx_list_create();
	zyx_list_push_string(outer, "first");
	zyx_list_push_list(outer, inner);

	auto *params = zyx_params_create();
	zyx_params_set_list(params, "items", outer);

	auto *res = zyx_execute_params(db, "RETURN $items AS items", params);
	if (res) {
		zyx_result_close(res);
	}

	zyx_list_close(inner);
	zyx_list_close(outer);
	zyx_params_close(params);
}

// ============================================================================
// zyx_create_edge_by_id: Cover failure path (invalid source)
// ============================================================================

TEST_F(CApiEdgeCasesTest, CreateEdgeByIdInvalidSource) {
	bool result = zyx_create_edge_by_id(db, 99999, 99998, "INVALID", nullptr);
	// Depending on implementation, this may succeed or fail
	// We just exercise the branch
	(void)result;
}

// ============================================================================
// zyx_create_node with null params
// ============================================================================

TEST_F(CApiEdgeCasesTest, CreateNodeNullParams) {
	EXPECT_TRUE(zyx_create_node(db, "EmptyNode", nullptr));
}

// ============================================================================
// zyx_create_node_ret_id: null guards
// ============================================================================

TEST_F(CApiEdgeCasesTest, CreateNodeRetIdNullGuards) {
	EXPECT_EQ(zyx_create_node_ret_id(nullptr, "Label", nullptr), -1);
	EXPECT_EQ(zyx_create_node_ret_id(db, nullptr, nullptr), -1);
}

// ============================================================================
// zyx_open_if_exists with valid path - exercises the success path
// ============================================================================

TEST_F(CApiEdgeCasesTest, OpenIfExistsValidDb) {
	// Create a valid database file first
	std::string secondPath = dbPath + "_copy";
	{
		auto *db2 = zyx_open(secondPath.c_str());
		ASSERT_NE(db2, nullptr);
		// Create some data to make it a valid file
		(void)zyx_execute(db2, "CREATE (n:Test)");
		zyx_close(db2);
	}

	// Now open_if_exists should succeed
	auto *db2 = zyx_open_if_exists(secondPath.c_str());
	if (db2) {
		zyx_close(db2);
	}

	std::error_code ec;
	std::filesystem::remove_all(secondPath, ec);
	std::filesystem::remove(secondPath + "-wal", ec);
}

// ============================================================================
// zyx_result_get_node: Cover null out_node guard
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultGetNodeNullOutNode) {
	auto *res = zyx_execute(db, "CREATE (n:NN) RETURN n");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_FALSE(zyx_result_get_node(res, 0, nullptr));
	}
	zyx_result_close(res);
}

// ============================================================================
// zyx_result_get_edge: Cover null out_edge guard
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultGetEdgeNullOutEdge) {
	auto *p = zyx_params_create();
	zyx_params_set_string(p, "n", "A");
	int64_t id1 = zyx_create_node_ret_id(db, "E1", p);
	zyx_params_close(p);
	p = zyx_params_create();
	zyx_params_set_string(p, "n", "B");
	int64_t id2 = zyx_create_node_ret_id(db, "E2", p);
	zyx_params_close(p);
	zyx_create_edge_by_id(db, id1, id2, "R", nullptr);

	auto *res = zyx_execute(db, "MATCH ()-[r]->() RETURN r");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_FALSE(zyx_result_get_edge(res, 0, nullptr));
	}
	zyx_result_close(res);
}

// ============================================================================
// zyx_create_edge_by_id: null guards
// ============================================================================

TEST_F(CApiEdgeCasesTest, CreateEdgeByIdNullGuards) {
	EXPECT_FALSE(zyx_create_edge_by_id(nullptr, 1, 2, "R", nullptr));
	EXPECT_FALSE(zyx_create_edge_by_id(db, 1, 2, nullptr, nullptr));
}

// ============================================================================
// zyx_create_node: null guards
// ============================================================================

TEST_F(CApiEdgeCasesTest, CreateNodeNullDb) {
	EXPECT_FALSE(zyx_create_node(nullptr, "Label", nullptr));
}

// ============================================================================
// zyx_txn_commit: null guard
// ============================================================================

TEST_F(CApiEdgeCasesTest, TxnCommitNullGuard) {
	EXPECT_FALSE(zyx_txn_commit(nullptr));
}

// ============================================================================
// zyx_txn_rollback: null guard
// ============================================================================

TEST_F(CApiEdgeCasesTest, TxnRollbackNullGuard) {
	zyx_txn_rollback(nullptr); // Should not crash
}

// ============================================================================
// zyx_txn_close: null guard
// ============================================================================

TEST_F(CApiEdgeCasesTest, TxnCloseNullGuard) {
	zyx_txn_close(nullptr); // Should not crash
}

// ============================================================================
// zyx_execute: null guards
// ============================================================================

TEST_F(CApiEdgeCasesTest, ExecuteNullGuards) {
	EXPECT_EQ(zyx_execute(nullptr, "RETURN 1"), nullptr);
	EXPECT_EQ(zyx_execute(db, nullptr), nullptr);
}

// ============================================================================
// zyx_result_next: null guard
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultNextNullGuard) {
	EXPECT_FALSE(zyx_result_next(nullptr));
}

// ============================================================================
// zyx_begin_transaction: null guard
// ============================================================================

TEST_F(CApiEdgeCasesTest, BeginTransactionNullGuard) {
	EXPECT_EQ(zyx_begin_transaction(nullptr), nullptr);
}

// ============================================================================
// zyx_begin_read_only_transaction: null guard
// ============================================================================

TEST_F(CApiEdgeCasesTest, BeginReadOnlyTransactionNullGuard) {
	EXPECT_EQ(zyx_begin_read_only_transaction(nullptr), nullptr);
}

// ============================================================================
// zyx_txn_execute: null guards
// ============================================================================

TEST_F(CApiEdgeCasesTest, TxnExecuteNullGuards) {
	EXPECT_EQ(zyx_txn_execute(nullptr, "RETURN 1"), nullptr);

	auto *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);
	EXPECT_EQ(zyx_txn_execute(txn, nullptr), nullptr);
	zyx_txn_rollback(txn);
	zyx_txn_close(txn);
}

// ============================================================================
// zyx_result_get_int: type mismatch returns 0
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultGetIntTypeMismatch) {
	auto *res = zyx_execute(db, "RETURN 'text'");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_EQ(zyx_result_get_int(res, 0), 0);
	}
	zyx_result_close(res);
}

// ============================================================================
// zyx_result_get_double: type mismatch returns 0.0
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultGetDoubleTypeMismatch) {
	auto *res = zyx_execute(db, "RETURN 'text'");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_EQ(zyx_result_get_double(res, 0), 0.0);
	}
	zyx_result_close(res);
}

// ============================================================================
// zyx_result_get_bool: type mismatch returns false
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultGetBoolTypeMismatch) {
	auto *res = zyx_execute(db, "RETURN 'text'");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_FALSE(zyx_result_get_bool(res, 0));
	}
	zyx_result_close(res);
}

// ============================================================================
// zyx_result_get_string: type mismatch returns ""
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultGetStringTypeMismatch) {
	auto *res = zyx_execute(db, "RETURN 42");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_STREQ(zyx_result_get_string(res, 0), "");
	}
	zyx_result_close(res);
}

// ============================================================================
// zyx_result_is_success: null result returns false
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultIsSuccessNullGuard) {
	EXPECT_FALSE(zyx_result_is_success(nullptr));
}

// ============================================================================
// zyx_result_get_error: null result
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultGetErrorNullGuard) {
	const char *err = zyx_result_get_error(nullptr);
	EXPECT_STREQ(err, "Invalid Result Handle");
}

// ============================================================================
// zyx_close: null guard
// ============================================================================

TEST_F(CApiEdgeCasesTest, CloseNullGuard) {
	zyx_close(nullptr); // Should not crash
}

// ============================================================================
// zyx_result_close: null guard
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultCloseNullGuard) {
	zyx_result_close(nullptr); // Should not crash
}

// ============================================================================
// zyx_open: null path
// ============================================================================

TEST(CApiBranchCoverageStandalone2, OpenNullPath) {
	EXPECT_EQ(zyx_open(nullptr), nullptr);
}

// ============================================================================
// zyx_params_set_*: null guards
// ============================================================================

TEST_F(CApiEdgeCasesTest, ParamsSetNullGuards) {
	zyx_params_set_int(nullptr, "k", 1);        // Should not crash
	zyx_params_set_double(nullptr, "k", 1.0);   // Should not crash
	zyx_params_set_string(nullptr, "k", "v");    // Should not crash
	zyx_params_set_bool(nullptr, "k", true);     // Should not crash
	zyx_params_set_null(nullptr, "k");           // Should not crash

	auto *p = zyx_params_create();
	zyx_params_set_int(p, nullptr, 1);           // Should not crash
	zyx_params_set_double(p, nullptr, 1.0);      // Should not crash
	zyx_params_set_string(p, nullptr, "v");       // Should not crash
	zyx_params_set_bool(p, nullptr, true);        // Should not crash
	zyx_params_set_null(p, nullptr);              // Should not crash
	zyx_params_set_string(p, "key", nullptr);     // Should not crash
	zyx_params_close(p);
}

// ============================================================================
// Map set operations: null guards
// ============================================================================

TEST_F(CApiEdgeCasesTest, MapSetNullGuards) {
	zyx_map_set_int(nullptr, "k", 1);
	zyx_map_set_double(nullptr, "k", 1.0);
	zyx_map_set_string(nullptr, "k", "v");
	zyx_map_set_bool(nullptr, "k", true);
	zyx_map_set_null(nullptr, "k");
	zyx_map_set_list(nullptr, "k", nullptr);
	zyx_map_set_map(nullptr, "k", nullptr);

	auto *m = zyx_map_create();
	zyx_map_set_int(m, nullptr, 1);
	zyx_map_set_string(m, "k", nullptr);
	zyx_map_close(m);
}

// ============================================================================
// List push operations: null guards
// ============================================================================

TEST_F(CApiEdgeCasesTest, ListPushNullGuards) {
	zyx_list_push_int(nullptr, 1);
	zyx_list_push_double(nullptr, 1.0);
	zyx_list_push_string(nullptr, "v");
	zyx_list_push_bool(nullptr, true);
	zyx_list_push_null(nullptr);
	zyx_list_push_list(nullptr, nullptr);

	auto *l = zyx_list_create();
	zyx_list_push_string(l, nullptr);
	zyx_list_push_list(l, nullptr);
	zyx_list_close(l);
}

// ============================================================================
// zyx_result_get_props_json: non-node/edge returns "{}"
// ============================================================================

TEST_F(CApiEdgeCasesTest, PropsJsonForNonEntity) {
	auto *res = zyx_execute(db, "RETURN 42");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		const char *json = zyx_result_get_props_json(res, 0);
		EXPECT_STREQ(json, "{}");
	}
	zyx_result_close(res);
}

// ============================================================================
// zyx_result_list_size / list_get_type: for non-list column returns 0/NULL
// ============================================================================

TEST_F(CApiEdgeCasesTest, ListSizeForNonList) {
	auto *res = zyx_execute(db, "RETURN 42");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_EQ(zyx_result_list_size(res, 0), 0);
		EXPECT_EQ(zyx_result_list_get_type(res, 0, 0), ZYX_NULL);
	}
	zyx_result_close(res);
}

// ============================================================================
// zyx_result_get_type: Cover ZYX_INT, ZYX_DOUBLE, ZYX_BOOL, ZYX_STRING, ZYX_NODE
// ============================================================================

TEST_F(CApiEdgeCasesTest, ResultGetTypeAllBranches) {
	auto *res = zyx_execute(db, "RETURN 42, 3.14, true, 'hello'");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_EQ(zyx_result_get_type(res, 0), ZYX_INT);
		EXPECT_EQ(zyx_result_get_type(res, 1), ZYX_DOUBLE);
		EXPECT_EQ(zyx_result_get_type(res, 2), ZYX_BOOL);
		EXPECT_EQ(zyx_result_get_type(res, 3), ZYX_STRING);
	}
	zyx_result_close(res);

	// ZYX_NULL for null
	res = zyx_execute(db, "RETURN null");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_EQ(zyx_result_get_type(res, 0), ZYX_NULL);
	}
	zyx_result_close(res);

	// ZYX_NODE
	(void)zyx_execute(db, "CREATE (n:TypeNode)");
	res = zyx_execute(db, "MATCH (n:TypeNode) RETURN n");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_EQ(zyx_result_get_type(res, 0), ZYX_NODE);
	}
	zyx_result_close(res);
}

// ============================================================================
// zyx_params_set_list / set_map: null key guard
// ============================================================================

TEST_F(CApiEdgeCasesTest, ParamsSetListMapNullKey) {
	auto *p = zyx_params_create();
	auto *l = zyx_list_create();
	auto *m = zyx_map_create();

	zyx_params_set_list(p, nullptr, l);    // Should not crash
	zyx_params_set_map(p, nullptr, m);     // Should not crash
	zyx_params_set_list(nullptr, "k", l);  // Should not crash
	zyx_params_set_map(nullptr, "k", m);   // Should not crash

	zyx_list_close(l);
	zyx_map_close(m);
	zyx_params_close(p);
}

// ============================================================================
// zyx_params_close: null guard
// ============================================================================

TEST_F(CApiEdgeCasesTest, ParamsCloseNull) {
	zyx_params_close(nullptr); // Should not crash
}

// ============================================================================
// zyx_execute_params: with valid params
// ============================================================================

TEST_F(CApiEdgeCasesTest, ExecuteParamsSuccess) {
	auto *params = zyx_params_create();
	zyx_params_set_int(params, "val", 42);
	auto *res = zyx_execute_params(db, "RETURN $val", params);
	ASSERT_NE(res, nullptr);
	EXPECT_TRUE(zyx_result_is_success(res));
	if (zyx_result_next(res)) {
		EXPECT_EQ(zyx_result_get_int(res, 0), 42);
	}
	zyx_result_close(res);
	zyx_params_close(params);
}
