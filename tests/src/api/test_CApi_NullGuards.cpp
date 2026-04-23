/**
 * @file test_CApi_NullGuards.cpp
 * @brief Tests for CApi.cpp null-pointer guards, error paths, and edge cases
 *        to improve branch coverage.
 */

#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include "zyx/zyx_c_api.h"

namespace fs = std::filesystem;

class CApiNullGuardTest : public ::testing::Test {
protected:
	std::string dbPath;
	ZYXDB_T *db = nullptr;

	void SetUp() override {
		auto tempDir = fs::temp_directory_path();
		dbPath = (tempDir / ("capi_null_guard_" + std::to_string(std::rand()))).string();
		std::error_code ec;
		if (fs::exists(dbPath)) fs::remove_all(dbPath, ec);
		db = zyx_open(dbPath.c_str());
		ASSERT_NE(db, nullptr);
	}

	void TearDown() override {
		if (db) zyx_close(db);
		std::error_code ec;
		fs::remove_all(dbPath, ec);
		fs::remove(dbPath + "-wal", ec);
	}
};

TEST_F(CApiNullGuardTest, OpenNullPath) {
	EXPECT_EQ(zyx_open(nullptr), nullptr);
}

TEST_F(CApiNullGuardTest, OpenIfExistsNullPath) {
	EXPECT_EQ(zyx_open_if_exists(nullptr), nullptr);
}

TEST_F(CApiNullGuardTest, OpenIfExistsNonExistent) {
	EXPECT_EQ(zyx_open_if_exists("/nonexistent/path/to/db"), nullptr);
}

TEST_F(CApiNullGuardTest, OpenIfExistsValidDb) {
	// db was opened in SetUp, so the file exists
	auto *db2 = zyx_open_if_exists(dbPath.c_str());
	ASSERT_NE(db2, nullptr);
	zyx_close(db2);
}

TEST_F(CApiNullGuardTest, CloseNull) {
	zyx_close(nullptr); // should not crash
}

TEST_F(CApiNullGuardTest, ExecuteNullDb) {
	EXPECT_EQ(zyx_execute(nullptr, "RETURN 1"), nullptr);
}

TEST_F(CApiNullGuardTest, ExecuteNullCypher) {
	EXPECT_EQ(zyx_execute(db, nullptr), nullptr);
}

TEST_F(CApiNullGuardTest, ExecuteParamsNullDb) {
	auto *p = zyx_params_create();
	EXPECT_EQ(zyx_execute_params(nullptr, "RETURN 1", p), nullptr);
	zyx_params_close(p);
}

TEST_F(CApiNullGuardTest, ExecuteParamsNullCypher) {
	auto *p = zyx_params_create();
	EXPECT_EQ(zyx_execute_params(db, nullptr, p), nullptr);
	zyx_params_close(p);
}

TEST_F(CApiNullGuardTest, ExecuteParamsNullParams) {
	auto *res = zyx_execute_params(db, "RETURN 1", nullptr);
	ASSERT_NE(res, nullptr);
	EXPECT_TRUE(zyx_result_is_success(res));
	zyx_result_close(res);
}

TEST_F(CApiNullGuardTest, ResultNextNull) {
	EXPECT_FALSE(zyx_result_next(nullptr));
}

TEST_F(CApiNullGuardTest, ResultColumnCountNull) {
	EXPECT_EQ(zyx_result_column_count(nullptr), 0);
}

TEST_F(CApiNullGuardTest, ResultColumnNameNull) {
	EXPECT_STREQ(zyx_result_column_name(nullptr, 0), "");
}

TEST_F(CApiNullGuardTest, ResultGetDurationNull) {
	EXPECT_DOUBLE_EQ(zyx_result_get_duration(nullptr), 0.0);
}

TEST_F(CApiNullGuardTest, ResultGetTypeNull) {
	EXPECT_EQ(zyx_result_get_type(nullptr, 0), ZYX_NULL);
}

TEST_F(CApiNullGuardTest, ResultGetIntNull) {
	EXPECT_EQ(zyx_result_get_int(nullptr, 0), 0);
}

TEST_F(CApiNullGuardTest, ResultGetDoubleNull) {
	EXPECT_DOUBLE_EQ(zyx_result_get_double(nullptr, 0), 0.0);
}

TEST_F(CApiNullGuardTest, ResultGetBoolNull) {
	EXPECT_FALSE(zyx_result_get_bool(nullptr, 0));
}

TEST_F(CApiNullGuardTest, ResultGetStringNull) {
	EXPECT_STREQ(zyx_result_get_string(nullptr, 0), "");
}

TEST_F(CApiNullGuardTest, ResultGetNodeNull) {
	ZYXNode node{};
	EXPECT_FALSE(zyx_result_get_node(nullptr, 0, &node));
}

TEST_F(CApiNullGuardTest, ResultGetNodeNullOut) {
	auto *res = zyx_execute(db, "RETURN 1");
	ASSERT_NE(res, nullptr);
	EXPECT_FALSE(zyx_result_get_node(res, 0, nullptr));
	zyx_result_close(res);
}

TEST_F(CApiNullGuardTest, ResultGetEdgeNull) {
	ZYXEdge edge{};
	EXPECT_FALSE(zyx_result_get_edge(nullptr, 0, &edge));
}

TEST_F(CApiNullGuardTest, ResultGetEdgeNullOut) {
	auto *res = zyx_execute(db, "RETURN 1");
	ASSERT_NE(res, nullptr);
	EXPECT_FALSE(zyx_result_get_edge(res, 0, nullptr));
	zyx_result_close(res);
}

TEST_F(CApiNullGuardTest, ResultGetPropsJsonNull) {
	EXPECT_STREQ(zyx_result_get_props_json(nullptr, 0), "{}");
}

TEST_F(CApiNullGuardTest, ResultListSizeNull) {
	EXPECT_EQ(zyx_result_list_size(nullptr, 0), 0);
}

TEST_F(CApiNullGuardTest, ResultListGetTypeNull) {
	EXPECT_EQ(zyx_result_list_get_type(nullptr, 0, 0), ZYX_NULL);
}

TEST_F(CApiNullGuardTest, ResultListGetIntNull) {
	EXPECT_EQ(zyx_result_list_get_int(nullptr, 0, 0), 0);
}

TEST_F(CApiNullGuardTest, ResultListGetDoubleNull) {
	EXPECT_DOUBLE_EQ(zyx_result_list_get_double(nullptr, 0, 0), 0.0);
}

TEST_F(CApiNullGuardTest, ResultListGetBoolNull) {
	EXPECT_FALSE(zyx_result_list_get_bool(nullptr, 0, 0));
}

TEST_F(CApiNullGuardTest, ResultListGetStringNull) {
	EXPECT_STREQ(zyx_result_list_get_string(nullptr, 0, 0), "");
}

TEST_F(CApiNullGuardTest, ResultGetMapJsonNull) {
	EXPECT_STREQ(zyx_result_get_map_json(nullptr, 0), "{}");
}

TEST_F(CApiNullGuardTest, ResultIsSuccessNull) {
	EXPECT_FALSE(zyx_result_is_success(nullptr));
}

TEST_F(CApiNullGuardTest, ResultGetErrorNull) {
	EXPECT_STREQ(zyx_result_get_error(nullptr), "Invalid Result Handle");
}

TEST_F(CApiNullGuardTest, ResultCloseNull) {
	zyx_result_close(nullptr); // should not crash
}

TEST_F(CApiNullGuardTest, BeginTransactionNull) {
	EXPECT_EQ(zyx_begin_transaction(nullptr), nullptr);
}

TEST_F(CApiNullGuardTest, BeginReadOnlyTransactionNull) {
	EXPECT_EQ(zyx_begin_read_only_transaction(nullptr), nullptr);
}

TEST_F(CApiNullGuardTest, TxnIsReadOnlyNull) {
	EXPECT_FALSE(zyx_txn_is_read_only(nullptr));
}

TEST_F(CApiNullGuardTest, TxnExecuteNull) {
	EXPECT_EQ(zyx_txn_execute(nullptr, "RETURN 1"), nullptr);
}

TEST_F(CApiNullGuardTest, TxnExecuteNullCypher) {
	auto *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);
	EXPECT_EQ(zyx_txn_execute(txn, nullptr), nullptr);
	zyx_txn_close(txn);
}

TEST_F(CApiNullGuardTest, TxnExecuteParamsNull) {
	EXPECT_EQ(zyx_txn_execute_params(nullptr, "RETURN 1", nullptr), nullptr);
}

TEST_F(CApiNullGuardTest, TxnExecuteParamsNullCypher) {
	auto *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);
	EXPECT_EQ(zyx_txn_execute_params(txn, nullptr, nullptr), nullptr);
	zyx_txn_close(txn);
}

TEST_F(CApiNullGuardTest, TxnCommitNull) {
	EXPECT_FALSE(zyx_txn_commit(nullptr));
}

TEST_F(CApiNullGuardTest, TxnRollbackNull) {
	zyx_txn_rollback(nullptr); // should not crash
}

TEST_F(CApiNullGuardTest, TxnCloseNull) {
	zyx_txn_close(nullptr); // should not crash
}

TEST_F(CApiNullGuardTest, CreateNodeNullDb) {
	EXPECT_FALSE(zyx_create_node(nullptr, "Label", nullptr));
}

TEST_F(CApiNullGuardTest, CreateNodeNullLabel) {
	EXPECT_FALSE(zyx_create_node(db, nullptr, nullptr));
}

TEST_F(CApiNullGuardTest, CreateNodeRetIdNullDb) {
	EXPECT_EQ(zyx_create_node_ret_id(nullptr, "L", nullptr), -1);
}

TEST_F(CApiNullGuardTest, CreateNodeRetIdNullLabel) {
	EXPECT_EQ(zyx_create_node_ret_id(db, nullptr, nullptr), -1);
}

TEST_F(CApiNullGuardTest, CreateEdgeByIdNullDb) {
	EXPECT_FALSE(zyx_create_edge_by_id(nullptr, 1, 2, "T", nullptr));
}

TEST_F(CApiNullGuardTest, CreateEdgeByIdNullType) {
	EXPECT_FALSE(zyx_create_edge_by_id(db, 1, 2, nullptr, nullptr));
}

TEST_F(CApiNullGuardTest, ParamsSetNullGuards) {
	zyx_params_set_int(nullptr, "k", 1);
	zyx_params_set_double(nullptr, "k", 1.0);
	zyx_params_set_string(nullptr, "k", "v");
	zyx_params_set_bool(nullptr, "k", true);
	zyx_params_set_null(nullptr, "k");

	auto *p = zyx_params_create();
	zyx_params_set_int(p, nullptr, 1);
	zyx_params_set_double(p, nullptr, 1.0);
	zyx_params_set_string(p, nullptr, "v");
	zyx_params_set_string(p, "k", nullptr);
	zyx_params_set_bool(p, nullptr, true);
	zyx_params_set_null(p, nullptr);
	zyx_params_close(p);
}

TEST_F(CApiNullGuardTest, ListNullGuards) {
	zyx_list_push_int(nullptr, 1);
	zyx_list_push_double(nullptr, 1.0);
	zyx_list_push_string(nullptr, "v");
	zyx_list_push_bool(nullptr, true);
	zyx_list_push_null(nullptr);
	zyx_list_push_list(nullptr, nullptr);
	zyx_list_close(nullptr);

	auto *list = zyx_list_create();
	zyx_list_push_string(list, nullptr);
	zyx_list_push_list(list, nullptr);
	zyx_list_close(list);
}

TEST_F(CApiNullGuardTest, MapNullGuards) {
	zyx_map_set_int(nullptr, "k", 1);
	zyx_map_set_double(nullptr, "k", 1.0);
	zyx_map_set_string(nullptr, "k", "v");
	zyx_map_set_bool(nullptr, "k", true);
	zyx_map_set_null(nullptr, "k");
	zyx_map_set_list(nullptr, "k", nullptr);
	zyx_map_set_map(nullptr, "k", nullptr);
	zyx_map_close(nullptr);

	auto *map = zyx_map_create();
	zyx_map_set_int(map, nullptr, 1);
	zyx_map_set_double(map, nullptr, 1.0);
	zyx_map_set_string(map, nullptr, "v");
	zyx_map_set_string(map, "k", nullptr);
	zyx_map_set_bool(map, nullptr, true);
	zyx_map_set_null(map, nullptr);
	zyx_map_set_list(map, nullptr, nullptr);
	zyx_map_set_map(map, nullptr, nullptr);
	zyx_map_close(map);
}

TEST_F(CApiNullGuardTest, ParamsSetListNull) {
	auto *p = zyx_params_create();
	zyx_params_set_list(nullptr, "k", nullptr);
	zyx_params_set_list(p, nullptr, nullptr);
	zyx_params_set_list(p, "k", nullptr);
	zyx_params_close(p);
}

TEST_F(CApiNullGuardTest, ParamsSetMapNull) {
	auto *p = zyx_params_create();
	zyx_params_set_map(nullptr, "k", nullptr);
	zyx_params_set_map(p, nullptr, nullptr);
	zyx_params_set_map(p, "k", nullptr);
	zyx_params_close(p);
}

TEST_F(CApiNullGuardTest, ReadOnlyTransactionIsReadOnly) {
	auto *txn = zyx_begin_read_only_transaction(db);
	ASSERT_NE(txn, nullptr);
	EXPECT_TRUE(zyx_txn_is_read_only(txn));
	zyx_txn_close(txn);
}

TEST_F(CApiNullGuardTest, ReadOnlyTxnExecuteAndCommit) {
	zyx_result_close(zyx_execute(db, "CREATE (n:RO {v: 1})"));

	auto *txn = zyx_begin_read_only_transaction(db);
	ASSERT_NE(txn, nullptr);

	auto *res = zyx_txn_execute(txn, "MATCH (n:RO) RETURN n.v");
	ASSERT_NE(res, nullptr);
	EXPECT_TRUE(zyx_result_is_success(res));
	zyx_result_close(res);

	EXPECT_TRUE(zyx_txn_commit(txn));
	zyx_txn_close(txn);
}

TEST_F(CApiNullGuardTest, TxnExecuteParamsValid) {
	auto *txn = zyx_begin_transaction(db);
	ASSERT_NE(txn, nullptr);

	auto *p = zyx_params_create();
	zyx_params_set_int(p, "x", 42);

	auto *res = zyx_txn_execute_params(txn, "RETURN $x", p);
	ASSERT_NE(res, nullptr);
	EXPECT_TRUE(zyx_result_is_success(res));
	zyx_result_close(res);

	zyx_params_close(p);
	EXPECT_TRUE(zyx_txn_commit(txn));
	zyx_txn_close(txn);
}

TEST_F(CApiNullGuardTest, ResultGetPropsJsonForNonEntityReturnsEmpty) {
	zyx_result_close(zyx_execute(db, "CREATE (n:PJ {a: 1})"));
	auto *res = zyx_execute(db, "MATCH (n:PJ) RETURN n.a");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));
	EXPECT_STREQ(zyx_result_get_props_json(res, 0), "{}");
	zyx_result_close(res);
}

TEST_F(CApiNullGuardTest, ResultGetMapJsonForIntValue) {
	auto *res = zyx_execute(db, "RETURN 42");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));
	const char *json = zyx_result_get_map_json(res, 0);
	EXPECT_STREQ(json, "42");
	zyx_result_close(res);
}

TEST_F(CApiNullGuardTest, ListAccessOutOfBounds) {
	zyx_result_close(zyx_execute(db, "CREATE (n:LB {items: ['a', 'b']})"));
	auto *res = zyx_execute(db, "MATCH (n:LB) RETURN n.items");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_EQ(zyx_result_list_size(res, 0), 2);
	EXPECT_STREQ(zyx_result_list_get_string(res, 0, 0), "a");
	EXPECT_STREQ(zyx_result_list_get_string(res, 0, 99), "");
	EXPECT_FALSE(zyx_result_list_get_bool(res, 0, 99));

	zyx_result_close(res);
}

TEST_F(CApiNullGuardTest, ListGetBoolTrueValue) {
	zyx_result_close(zyx_execute(db, "CREATE (n:BL {flags: ['true', 'false']})"));
	auto *res = zyx_execute(db, "MATCH (n:BL) RETURN n.flags");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_TRUE(zyx_result_list_get_bool(res, 0, 0));
	EXPECT_FALSE(zyx_result_list_get_bool(res, 0, 1));

	zyx_result_close(res);
}

TEST_F(CApiNullGuardTest, GetLastErrorReturnsString) {
	const char *err = zyx_get_last_error();
	EXPECT_NE(err, nullptr);
}
