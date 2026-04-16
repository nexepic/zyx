/**
 * @file test_CApiTransactionParams.cpp
 * @brief Unit tests for C API transaction, parameterized query, batch operation,
 *        and list access functions.
 **/

#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include "zyx/zyx_c_api.h"

namespace fs = std::filesystem;

class CApiTxnParamsTest : public ::testing::Test {
protected:
	std::string dbPath;
	ZYXDB_T *db = nullptr;

	void SetUp() override {
		auto tempDir = fs::temp_directory_path();
		dbPath = (tempDir / ("c_api_txn_test_" + std::to_string(std::rand()))).string();
		// Clean up any leftover DB and WAL files from previous runs
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
		if (fs::exists(dbPath))
			fs::remove_all(dbPath, ec);
		// Also clean up WAL file
		std::string walPath = dbPath + "-wal";
		if (fs::exists(walPath))
			fs::remove(walPath, ec);
	}
};

// ============================================================================
// Transaction API
// ============================================================================

TEST_F(CApiTxnParamsTest, BeginTransaction_Success) {
	ZYXTxn_T *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);
	zyx_txn_close(txn);
}

TEST_F(CApiTxnParamsTest, BeginTransaction_NullDb) {
	ZYXTxn_T *txn = zyx_begin_transaction(nullptr);
	EXPECT_EQ(txn, nullptr);
}

TEST_F(CApiTxnParamsTest, TxnExecute_CreateAndQuery) {
	ZYXTxn_T *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);

	auto *createRes = zyx_txn_execute(txn, "CREATE (n:TxnNode {val: 42})");
	ASSERT_NE(createRes, nullptr);
	zyx_result_close(createRes);

	auto *queryRes = zyx_txn_execute(txn, "MATCH (n:TxnNode) RETURN n.val");
	ASSERT_NE(queryRes, nullptr);
	ASSERT_TRUE(zyx_result_next(queryRes));
	EXPECT_EQ(zyx_result_get_int(queryRes, 0), 42);
	zyx_result_close(queryRes);

	zyx_txn_close(txn);
}

TEST_F(CApiTxnParamsTest, TxnExecute_NullTxn) {
	auto *res = zyx_txn_execute(nullptr, "RETURN 1");
	EXPECT_EQ(res, nullptr);
}

TEST_F(CApiTxnParamsTest, TxnExecute_NullCypher) {
	ZYXTxn_T *txn = zyx_begin_transaction(db);
	auto *res = zyx_txn_execute(txn, nullptr);
	EXPECT_EQ(res, nullptr);
	zyx_txn_close(txn);
}

TEST_F(CApiTxnParamsTest, TxnCommit_Success) {
	ZYXTxn_T *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);

	auto *res = zyx_txn_execute(txn, "CREATE (n:CommitNode {x: 1})");
	zyx_result_close(res);

	bool committed = zyx_txn_commit(txn);
	EXPECT_TRUE(committed);
	zyx_txn_close(txn);

	// Verify data persists after commit
	auto *verifyRes = zyx_execute(db, "MATCH (n:CommitNode) RETURN n.x");
	ASSERT_NE(verifyRes, nullptr);
	ASSERT_TRUE(zyx_result_next(verifyRes));
	EXPECT_EQ(zyx_result_get_int(verifyRes, 0), 1);
	zyx_result_close(verifyRes);
}

TEST_F(CApiTxnParamsTest, TxnCommit_NullTxn) {
	EXPECT_FALSE(zyx_txn_commit(nullptr));
}

TEST_F(CApiTxnParamsTest, TxnRollback_Success) {
	ZYXTxn_T *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);

	auto *res = zyx_txn_execute(txn, "CREATE (n:RollbackNode {x: 1})");
	zyx_result_close(res);

	zyx_txn_rollback(txn);
	zyx_txn_close(txn);
}

TEST_F(CApiTxnParamsTest, TxnRollback_NullTxn) {
	// Should not crash
	zyx_txn_rollback(nullptr);
	SUCCEED();
}

TEST_F(CApiTxnParamsTest, TxnClose_NullTxn) {
	zyx_txn_close(nullptr);
	SUCCEED();
}

// ============================================================================
// Parameterized Queries
// ============================================================================

TEST_F(CApiTxnParamsTest, ParamsCreate_And_Close) {
	ZYXParams_T *params = zyx_params_create();
	ASSERT_NE(params, nullptr);
	zyx_params_close(params);
}

TEST_F(CApiTxnParamsTest, ParamsSetInt) {
	ZYXParams_T *params = zyx_params_create();
	zyx_params_set_int(params, "age", 25);
	zyx_params_close(params);
}

TEST_F(CApiTxnParamsTest, ParamsSetDouble) {
	ZYXParams_T *params = zyx_params_create();
	zyx_params_set_double(params, "score", 99.5);
	zyx_params_close(params);
}

TEST_F(CApiTxnParamsTest, ParamsSetString) {
	ZYXParams_T *params = zyx_params_create();
	zyx_params_set_string(params, "name", "Alice");
	zyx_params_close(params);
}

TEST_F(CApiTxnParamsTest, ParamsSetBool) {
	ZYXParams_T *params = zyx_params_create();
	zyx_params_set_bool(params, "active", true);
	zyx_params_close(params);
}

TEST_F(CApiTxnParamsTest, ParamsSetNull) {
	ZYXParams_T *params = zyx_params_create();
	zyx_params_set_null(params, "nothing");
	zyx_params_close(params);
}

TEST_F(CApiTxnParamsTest, ParamsSetWithNullPointers) {
	// All set functions should be no-ops with null params
	zyx_params_set_int(nullptr, "key", 1);
	zyx_params_set_double(nullptr, "key", 1.0);
	zyx_params_set_string(nullptr, "key", "val");
	zyx_params_set_bool(nullptr, "key", true);
	zyx_params_set_null(nullptr, "key");

	// Null key
	ZYXParams_T *params = zyx_params_create();
	zyx_params_set_int(params, nullptr, 1);
	zyx_params_set_double(params, nullptr, 1.0);
	zyx_params_set_string(params, nullptr, "val");
	zyx_params_set_bool(params, nullptr, true);
	zyx_params_set_null(params, nullptr);

	// Null value for string
	zyx_params_set_string(params, "key", nullptr);
	zyx_params_close(params);
}

TEST_F(CApiTxnParamsTest, ExecuteParams_Basic) {
	// Create data first
	zyx_result_close(zyx_execute(db, "CREATE (n:Param {val: 10})"));
	zyx_result_close(zyx_execute(db, "CREATE (n:Param {val: 20})"));
	zyx_result_close(zyx_execute(db, "CREATE (n:Param {val: 30})"));

	ZYXParams_T *params = zyx_params_create();
	zyx_params_set_int(params, "threshold", 15);

	auto *res = zyx_execute_params(db, "MATCH (n:Param) WHERE n.val > $threshold RETURN n.val", params);
	ASSERT_NE(res, nullptr);

	int count = 0;
	while (zyx_result_next(res)) count++;
	EXPECT_EQ(count, 2); // val=20, val=30

	zyx_result_close(res);
	zyx_params_close(params);
}

TEST_F(CApiTxnParamsTest, ExecuteParams_NullDb) {
	ZYXParams_T *params = zyx_params_create();
	auto *res = zyx_execute_params(nullptr, "RETURN 1", params);
	EXPECT_EQ(res, nullptr);
	zyx_params_close(params);
}

TEST_F(CApiTxnParamsTest, ExecuteParams_NullCypher) {
	auto *res = zyx_execute_params(db, nullptr, nullptr);
	EXPECT_EQ(res, nullptr);
}

TEST_F(CApiTxnParamsTest, ExecuteParams_NullParams) {
	auto *res = zyx_execute_params(db, "RETURN 1 AS x", nullptr);
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));
	EXPECT_EQ(zyx_result_get_int(res, 0), 1);
	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, TxnExecuteParams_Basic) {
	ZYXTxn_T *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);

	ZYXParams_T *params = zyx_params_create();
	zyx_params_set_string(params, "name", "Bob");

	auto *res = zyx_txn_execute_params(txn, "CREATE (n:P {name: $name})", params);
	ASSERT_NE(res, nullptr);
	zyx_result_close(res);

	zyx_params_close(params);
	zyx_txn_close(txn);
}

TEST_F(CApiTxnParamsTest, TxnExecuteParams_NullTxn) {
	auto *res = zyx_txn_execute_params(nullptr, "RETURN 1", nullptr);
	EXPECT_EQ(res, nullptr);
}

TEST_F(CApiTxnParamsTest, TxnExecuteParams_NullCypher) {
	ZYXTxn_T *txn = zyx_begin_transaction(db);
	auto *res = zyx_txn_execute_params(txn, nullptr, nullptr);
	EXPECT_EQ(res, nullptr);
	zyx_txn_close(txn);
}

// ============================================================================
// Batch Operations
// ============================================================================

TEST_F(CApiTxnParamsTest, CreateNode_Success) {
	ZYXParams_T *props = zyx_params_create();
	zyx_params_set_string(props, "name", "BatchNode");
	zyx_params_set_int(props, "age", 30);

	bool ok = zyx_create_node(db, "BatchLabel", props);
	EXPECT_TRUE(ok);

	zyx_params_close(props);

	// Verify
	auto *res = zyx_execute(db, "MATCH (n:BatchLabel) RETURN n.name");
	ASSERT_TRUE(zyx_result_next(res));
	EXPECT_STREQ(zyx_result_get_string(res, 0), "BatchNode");
	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, CreateNode_NullDb) {
	EXPECT_FALSE(zyx_create_node(nullptr, "Label", nullptr));
}

TEST_F(CApiTxnParamsTest, CreateNode_NullLabel) {
	EXPECT_FALSE(zyx_create_node(db, nullptr, nullptr));
}

TEST_F(CApiTxnParamsTest, CreateNode_NullProps) {
	bool ok = zyx_create_node(db, "EmptyProps", nullptr);
	EXPECT_TRUE(ok);

	auto *res = zyx_execute(db, "MATCH (n:EmptyProps) RETURN n");
	ASSERT_TRUE(zyx_result_next(res));
	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, CreateNodeRetId_Success) {
	ZYXParams_T *props = zyx_params_create();
	zyx_params_set_string(props, "val", "test");

	int64_t id = zyx_create_node_ret_id(db, "RetIdLabel", props);
	EXPECT_GT(id, 0);

	zyx_params_close(props);
}

TEST_F(CApiTxnParamsTest, CreateNodeRetId_NullDb) {
	EXPECT_EQ(zyx_create_node_ret_id(nullptr, "Label", nullptr), -1);
}

TEST_F(CApiTxnParamsTest, CreateNodeRetId_NullLabel) {
	EXPECT_EQ(zyx_create_node_ret_id(db, nullptr, nullptr), -1);
}

TEST_F(CApiTxnParamsTest, CreateEdgeById_Success) {
	int64_t id1 = zyx_create_node_ret_id(db, "E1", nullptr);
	int64_t id2 = zyx_create_node_ret_id(db, "E2", nullptr);
	ASSERT_GT(id1, 0);
	ASSERT_GT(id2, 0);

	ZYXParams_T *props = zyx_params_create();
	zyx_params_set_int(props, "weight", 5);

	bool ok = zyx_create_edge_by_id(db, id1, id2, "CONNECTS", props);
	EXPECT_TRUE(ok);

	zyx_params_close(props);

	// Verify edge exists
	auto *res = zyx_execute(db, "MATCH ()-[e:CONNECTS]->() RETURN e");
	ASSERT_TRUE(zyx_result_next(res));
	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, CreateEdgeById_NullDb) {
	EXPECT_FALSE(zyx_create_edge_by_id(nullptr, 1, 2, "REL", nullptr));
}

TEST_F(CApiTxnParamsTest, CreateEdgeById_NullLabel) {
	EXPECT_FALSE(zyx_create_edge_by_id(db, 1, 2, nullptr, nullptr));
}

// ============================================================================
// List Access
// ============================================================================

TEST_F(CApiTxnParamsTest, ListSize_StringList) {
	zyx_result_close(zyx_execute(db, "CREATE (n:LblNode {x: 1})"));
	auto *res = zyx_execute(db, "MATCH (n:LblNode) RETURN labels(n) AS lbls");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	int size = zyx_result_list_size(res, 0);
	EXPECT_GE(size, 1); // At least "LblNode"

	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, ListSize_NullResult) {
	EXPECT_EQ(zyx_result_list_size(nullptr, 0), 0);
}

TEST_F(CApiTxnParamsTest, ListGetType_StringList) {
	zyx_result_close(zyx_execute(db, "CREATE (n:TypeNode)"));
	auto *res = zyx_execute(db, "MATCH (n:TypeNode) RETURN labels(n)");
	ASSERT_TRUE(zyx_result_next(res));

	ZYXValueType t = zyx_result_list_get_type(res, 0, 0);
	EXPECT_EQ(t, ZYX_STRING);

	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, ListGetType_NullResult) {
	EXPECT_EQ(zyx_result_list_get_type(nullptr, 0, 0), ZYX_NULL);
}

TEST_F(CApiTxnParamsTest, ListGetString_FromStringList) {
	zyx_result_close(zyx_execute(db, "CREATE (n:StrListNode)"));
	auto *res = zyx_execute(db, "MATCH (n:StrListNode) RETURN labels(n) AS lbls");
	ASSERT_TRUE(zyx_result_next(res));

	const char *val = zyx_result_list_get_string(res, 0, 0);
	EXPECT_STREQ(val, "StrListNode");

	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, ListGetString_NullResult) {
	EXPECT_STREQ(zyx_result_list_get_string(nullptr, 0, 0), "");
}

TEST_F(CApiTxnParamsTest, ListGetBool_FromStringList) {
	zyx_result_close(zyx_execute(db, "CREATE (n:BoolListNode)"));
	auto *res = zyx_execute(db, "MATCH (n:BoolListNode) RETURN labels(n)");
	ASSERT_TRUE(zyx_result_next(res));

	bool val = zyx_result_list_get_bool(res, 0, 0);
	// "BoolListNode" != "true", so should be false
	EXPECT_FALSE(val);

	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, ListGetBool_NullResult) {
	EXPECT_FALSE(zyx_result_list_get_bool(nullptr, 0, 0));
}

TEST_F(CApiTxnParamsTest, ListGetInt_NullResult) {
	EXPECT_EQ(zyx_result_list_get_int(nullptr, 0, 0), 0);
}

TEST_F(CApiTxnParamsTest, ListGetDouble_NullResult) {
	EXPECT_DOUBLE_EQ(zyx_result_list_get_double(nullptr, 0, 0), 0.0);
}

TEST_F(CApiTxnParamsTest, ListGetInt_NonListColumn) {
	auto *res = zyx_execute(db, "RETURN 42 AS x");
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_EQ(zyx_result_list_get_int(res, 0, 0), 0);
	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, ListGetDouble_NonListColumn) {
	auto *res = zyx_execute(db, "RETURN 42 AS x");
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_DOUBLE_EQ(zyx_result_list_get_double(res, 0, 0), 0.0);
	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, ListSize_NonListColumn) {
	auto *res = zyx_execute(db, "RETURN 42 AS x");
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_EQ(zyx_result_list_size(res, 0), 0);
	zyx_result_close(res);
}

// ============================================================================
// Map Access
// ============================================================================

TEST_F(CApiTxnParamsTest, GetMapJson_IntValue) {
	auto *res = zyx_execute(db, "RETURN 42 AS x");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_map_json(res, 0);
	EXPECT_STREQ(json, "42");
	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, GetMapJson_StringValue) {
	auto *res = zyx_execute(db, "RETURN 'hello' AS x");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_map_json(res, 0);
	EXPECT_STREQ(json, "\"hello\"");
	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, GetMapJson_BoolValue) {
	auto *res = zyx_execute(db, "RETURN true AS x");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_map_json(res, 0);
	EXPECT_STREQ(json, "true");
	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, GetMapJson_DoubleValue) {
	auto *res = zyx_execute(db, "RETURN 3.14 AS x");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_map_json(res, 0);
	std::string jsonStr(json);
	EXPECT_NE(jsonStr.find("3.14"), std::string::npos);
	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, GetMapJson_NullValue) {
	auto *res = zyx_execute(db, "RETURN null AS x");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_map_json(res, 0);
	EXPECT_STREQ(json, "null");
	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, GetMapJson_NullResult) {
	EXPECT_STREQ(zyx_result_get_map_json(nullptr, 0), "{}");
}

TEST_F(CApiTxnParamsTest, GetMapJson_StringListValue) {
	zyx_result_close(zyx_execute(db, "CREATE (n:MapNode {a: 1})"));
	auto *res = zyx_execute(db, "MATCH (n:MapNode) RETURN labels(n) AS lbls");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_map_json(res, 0);
	std::string jsonStr(json);
	// Should contain "MapNode" in array format
	EXPECT_NE(jsonStr.find("MapNode"), std::string::npos);
	zyx_result_close(res);
}

TEST_F(CApiTxnParamsTest, ParamsClose_Null) {
	// Should not crash
	zyx_params_close(nullptr);
	SUCCEED();
}
