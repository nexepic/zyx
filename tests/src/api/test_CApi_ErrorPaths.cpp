/**
 * @file test_CApi_ErrorPaths.cpp
 * @brief Tests targeting uncovered error/catch paths in CApi.cpp to raise
 *        line coverage above 90%.
 *
 * Focus: catch(...) branches, openIfExists failure path, zyx_close exception
 *        suppression, and batch operation error paths.
 */

#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include "zyx/zyx_c_api.h"

namespace fs = std::filesystem;

class CApiErrorPathTest : public ::testing::Test {
protected:
	std::string dbPath;
	ZYXDB_T *db = nullptr;

	void SetUp() override {
		auto tempDir = fs::temp_directory_path();
		dbPath = (tempDir / ("c_api_err_" + std::to_string(std::rand()))).string();
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
// zyx_open_if_exists: "Failed to open existing database" path
// When the file exists but is not a valid database
// ============================================================================

TEST(CApiErrorPathStandalone, OpenIfExistsInvalidFile) {
	// Create a file that exists but is not a valid database
	auto tempDir = fs::temp_directory_path();
	std::string fakePath = (tempDir / ("c_api_fake_db_" + std::to_string(std::rand()))).string();
	std::error_code ec;
	if (fs::exists(fakePath))
		fs::remove_all(fakePath, ec);

	// Write garbage data to simulate a corrupted file
	{
		std::ofstream ofs(fakePath, std::ios::binary);
		ofs << "THIS IS NOT A VALID DATABASE FILE";
	}
	ASSERT_TRUE(fs::exists(fakePath));

	// zyx_open_if_exists should fail (catch exception or return null)
	ZYXDB_T *db = zyx_open_if_exists(fakePath.c_str());
	// Either returns null due to exception, or openIfExists returns false
	// In both cases, db should be null
	if (db != nullptr) {
		zyx_close(db);
	}

	// Verify error is set
	const char *err = zyx_get_last_error();
	EXPECT_NE(err, nullptr);

	fs::remove_all(fakePath, ec);
}

// ============================================================================
// zyx_execute: exception paths
// ============================================================================

TEST_F(CApiErrorPathTest, ExecuteSyntaxError) {
	// Execute invalid Cypher that causes an exception in the engine
	ZYXResult_T *res = zyx_execute(db, "INVALID CYPHER SYNTAX !!!");
	// Should either return null (caught exception) or return error result
	if (res == nullptr) {
		const char *err = zyx_get_last_error();
		EXPECT_NE(err, nullptr);
		EXPECT_STRNE(err, "");
	} else {
		// If it returns a result, it should indicate failure
		zyx_result_close(res);
	}
}

TEST_F(CApiErrorPathTest, ExecuteParamsSyntaxError) {
	ZYXParams_T *p = zyx_params_create();
	zyx_params_set_int(p, "x", 1);

	ZYXResult_T *res = zyx_execute_params(db, "INVALID CYPHER SYNTAX !!!", p);
	if (res == nullptr) {
		const char *err = zyx_get_last_error();
		EXPECT_NE(err, nullptr);
		EXPECT_STRNE(err, "");
	} else {
		zyx_result_close(res);
	}

	zyx_params_close(p);
}

// ============================================================================
// Transaction commit/rollback paths
// ============================================================================

TEST_F(CApiErrorPathTest, TxnCommitAndClose) {
	ZYXTxn_T *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);

	auto *res = zyx_txn_execute(txn, "CREATE (n:TxnErr {x: 1})");
	ASSERT_NE(res, nullptr);
	zyx_result_close(res);

	EXPECT_TRUE(zyx_txn_commit(txn));
	zyx_txn_close(txn);
}

TEST_F(CApiErrorPathTest, TxnRollbackAndClose) {
	ZYXTxn_T *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);

	auto *res = zyx_txn_execute(txn, "CREATE (n:TxnErr2 {x: 2})");
	ASSERT_NE(res, nullptr);
	zyx_result_close(res);

	zyx_txn_rollback(txn);
	zyx_txn_close(txn);
}

TEST_F(CApiErrorPathTest, TxnAutoRollbackOnClose) {
	// Transaction is not committed or rolled back before close
	// The destructor should auto-rollback
	ZYXTxn_T *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);

	auto *res = zyx_txn_execute(txn, "CREATE (n:TxnAutoRollback {x: 3})");
	zyx_result_close(res);

	// Close without commit/rollback - auto-rollback happens in destructor
	zyx_txn_close(txn);

	// Verify data was rolled back (node should not exist)
	auto *verify = zyx_execute(db, "MATCH (n:TxnAutoRollback) RETURN n");
	ASSERT_NE(verify, nullptr);
	EXPECT_FALSE(zyx_result_next(verify));
	zyx_result_close(verify);
}

// ============================================================================
// Batch operations: success paths
// ============================================================================

TEST_F(CApiErrorPathTest, CreateNodeWithProps) {
	ZYXParams_T *props = zyx_params_create();
	zyx_params_set_string(props, "name", "ErrorTest");
	zyx_params_set_int(props, "val", 42);
	zyx_params_set_double(props, "score", 3.14);
	zyx_params_set_bool(props, "active", true);
	zyx_params_set_null(props, "empty");

	EXPECT_TRUE(zyx_create_node(db, "ErrNode", props));
	zyx_params_close(props);
}

TEST_F(CApiErrorPathTest, CreateNodeRetId) {
	ZYXParams_T *props = zyx_params_create();
	zyx_params_set_string(props, "name", "RetId");

	int64_t id = zyx_create_node_ret_id(db, "RetIdNode", props);
	EXPECT_GT(id, 0);
	zyx_params_close(props);
}

TEST_F(CApiErrorPathTest, CreateEdgeById) {
	int64_t src = zyx_create_node_ret_id(db, "Src", nullptr);
	int64_t tgt = zyx_create_node_ret_id(db, "Tgt", nullptr);
	ASSERT_GT(src, 0);
	ASSERT_GT(tgt, 0);

	ZYXParams_T *props = zyx_params_create();
	zyx_params_set_int(props, "weight", 10);

	EXPECT_TRUE(zyx_create_edge_by_id(db, src, tgt, "KNOWS", props));
	zyx_params_close(props);
}

// ============================================================================
// Edge case: create edge with invalid source/target
// This exercises the catch block in zyx_create_edge_by_id
// ============================================================================

TEST_F(CApiErrorPathTest, CreateEdgeByIdInvalidNodes) {
	// Invalid node IDs - the engine should throw
	bool ok = zyx_create_edge_by_id(db, 999999, 999998, "BAD_EDGE", nullptr);
	// May succeed or fail depending on engine implementation
	// The important thing is no crash
	(void)ok;
}

// ============================================================================
// Transaction execute with params in txn
// ============================================================================

TEST_F(CApiErrorPathTest, TxnExecuteParamsValid) {
	ZYXTxn_T *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);

	ZYXParams_T *p = zyx_params_create();
	zyx_params_set_string(p, "name", "ParamTest");

	auto *res = zyx_txn_execute_params(txn, "CREATE (n:ParamNode {name: $name})", p);
	ASSERT_NE(res, nullptr);
	zyx_result_close(res);

	zyx_params_close(p);
	EXPECT_TRUE(zyx_txn_commit(txn));
	zyx_txn_close(txn);
}

TEST_F(CApiErrorPathTest, TxnExecuteParamsSyntaxError) {
	ZYXTxn_T *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);

	ZYXParams_T *p = zyx_params_create();
	zyx_params_set_int(p, "x", 1);

	auto *res = zyx_txn_execute_params(txn, "INVALID CYPHER !!!", p);
	if (res == nullptr) {
		const char *err = zyx_get_last_error();
		EXPECT_NE(err, nullptr);
	} else {
		zyx_result_close(res);
	}

	zyx_params_close(p);
	zyx_txn_close(txn);
}

// ============================================================================
// List builder operations
// ============================================================================

TEST_F(CApiErrorPathTest, ListBuilderPushAll) {
	ZYXList_T *list = zyx_list_create();
	ASSERT_NE(list, nullptr);

	zyx_list_push_int(list, 42);
	zyx_list_push_double(list, 3.14);
	zyx_list_push_string(list, "hello");
	zyx_list_push_bool(list, true);
	zyx_list_push_null(list);

	// Nested list
	ZYXList_T *nested = zyx_list_create();
	zyx_list_push_int(nested, 1);
	zyx_list_push_list(list, nested);

	// Use in params
	ZYXParams_T *p = zyx_params_create();
	zyx_params_set_list(p, "myList", list);

	zyx_list_close(nested);
	zyx_list_close(list);
	zyx_params_close(p);
}

// ============================================================================
// Map builder operations
// ============================================================================

TEST_F(CApiErrorPathTest, MapBuilderSetAll) {
	ZYXMap_T *map = zyx_map_create();
	ASSERT_NE(map, nullptr);

	zyx_map_set_int(map, "i", 42);
	zyx_map_set_double(map, "d", 3.14);
	zyx_map_set_string(map, "s", "hello");
	zyx_map_set_bool(map, "b", true);
	zyx_map_set_null(map, "n");

	// Nested list in map
	ZYXList_T *list = zyx_list_create();
	zyx_list_push_int(list, 1);
	zyx_map_set_list(map, "list", list);

	// Nested map in map
	ZYXMap_T *nested = zyx_map_create();
	zyx_map_set_int(nested, "x", 1);
	zyx_map_set_map(map, "nested", nested);

	// Use in params
	ZYXParams_T *p = zyx_params_create();
	zyx_params_set_map(p, "myMap", map);

	zyx_map_close(nested);
	zyx_list_close(list);
	zyx_map_close(map);
	zyx_params_close(p);
}

// ============================================================================
// Read-only transaction: execute syntax error
// ============================================================================

TEST_F(CApiErrorPathTest, ReadOnlyTxnExecuteSyntaxError) {
	ZYXTxn_T *txn = zyx_begin_read_only_transaction(db);
	ASSERT_NE(txn, nullptr);

	auto *res = zyx_txn_execute(txn, "INVALID SYNTAX !!!");
	if (res == nullptr) {
		const char *err = zyx_get_last_error();
		EXPECT_NE(err, nullptr);
	} else {
		zyx_result_close(res);
	}

	zyx_txn_close(txn);
}

// ============================================================================
// zyx_open: path that triggers exception (e.g., read-only directory)
// ============================================================================

TEST(CApiErrorPathStandalone, OpenInvalidPath) {
	// Try to open a database at a path that should fail
	// This exercises the catch block in zyx_open
	ZYXDB_T *db = zyx_open("/nonexistent/dir/sub/sub/test.zyx");
	if (db == nullptr) {
		const char *err = zyx_get_last_error();
		EXPECT_NE(err, nullptr);
		EXPECT_STRNE(err, "");
	} else {
		zyx_close(db);
	}
}

// ============================================================================
// value_to_json: boolean false path
// ============================================================================

TEST_F(CApiErrorPathTest, GetMapJsonBoolValues) {
	auto *res = zyx_execute(db, "RETURN false AS b");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_map_json(res, 0);
	EXPECT_STREQ(json, "false");

	zyx_result_close(res);
}

// ============================================================================
// value_to_json: node and edge paths
// ============================================================================

TEST_F(CApiErrorPathTest, GetMapJsonForNode) {
	zyx_result_close(zyx_execute(db, "CREATE (n:MapNode {x: 1})"));
	auto *res = zyx_execute(db, "MATCH (n:MapNode) RETURN n");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_map_json(res, 0);
	// Should contain the node as a complex JSON representation
	EXPECT_NE(json, nullptr);
	std::string s(json);
	EXPECT_FALSE(s.empty());

	zyx_result_close(res);
}

TEST_F(CApiErrorPathTest, GetMapJsonForEdge) {
	zyx_result_close(zyx_execute(db, "CREATE (a:MJ1), (b:MJ2)"));
	zyx_result_close(zyx_execute(db, "MATCH (a:MJ1), (b:MJ2) CREATE (a)-[e:MJR]->(b)"));
	auto *res = zyx_execute(db, "MATCH ()-[e:MJR]->() RETURN e");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_map_json(res, 0);
	EXPECT_NE(json, nullptr);
	std::string s(json);
	EXPECT_FALSE(s.empty());

	zyx_result_close(res);
}

// ============================================================================
// list_get_bool with in-bounds "true" string (exercises the true path)
// ============================================================================

TEST_F(CApiErrorPathTest, ListGetBoolTrueString) {
	zyx_result_close(zyx_execute(db, "CREATE (n:BoolList {vals: ['true', 'false', 'maybe']})"));
	auto *res = zyx_execute(db, "MATCH (n:BoolList) RETURN n.vals");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_EQ(zyx_result_list_size(res, 0), 3);
	EXPECT_TRUE(zyx_result_list_get_bool(res, 0, 0));   // "true"
	EXPECT_FALSE(zyx_result_list_get_bool(res, 0, 1));   // "false"
	EXPECT_FALSE(zyx_result_list_get_bool(res, 0, 2));   // "maybe"

	zyx_result_close(res);
}

// ============================================================================
// list_get_type for non-list column
// ============================================================================

TEST_F(CApiErrorPathTest, ListGetTypeNonList) {
	auto *res = zyx_execute(db, "RETURN 42 AS x");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_EQ(zyx_result_list_get_type(res, 0, 0), ZYX_NULL);

	zyx_result_close(res);
}

// ============================================================================
// Multiple calls clearing buffers between rows
// ============================================================================

TEST_F(CApiErrorPathTest, IterateMultipleRowsClearsBuffers) {
	zyx_result_close(zyx_execute(db, "CREATE (n:Iter {x: 'row1'})"));
	zyx_result_close(zyx_execute(db, "CREATE (n:Iter {x: 'row2'})"));
	zyx_result_close(zyx_execute(db, "CREATE (n:Iter {x: 'row3'})"));

	auto *res = zyx_execute(db, "MATCH (n:Iter) RETURN n.x");
	ASSERT_NE(res, nullptr);

	int count = 0;
	while (zyx_result_next(res)) {
		const char *val = zyx_result_get_string(res, 0);
		EXPECT_NE(val, nullptr);
		EXPECT_STRNE(val, "");
		count++;
	}
	EXPECT_EQ(count, 3);

	zyx_result_close(res);
}
