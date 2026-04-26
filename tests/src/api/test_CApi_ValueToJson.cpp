/**
 * @file test_CApi_ValueToJson.cpp
 * @brief Branch coverage tests for CApi.cpp targeting value_to_json paths,
 *        list/map builder null guards (failure branches), and list accessor
 *        edge cases.
 */

#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include "zyx/zyx_c_api.h"

namespace fs = std::filesystem;

class CApiValueToJsonTest : public ::testing::Test {
protected:
	std::string dbPath;
	ZYXDB_T *db = nullptr;

	void SetUp() override {
		auto tempDir = fs::temp_directory_path();
		dbPath = (tempDir / ("c_api_v2j_" + std::to_string(std::rand()))).string();
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
// value_to_json: Cover bool branch (line 169)
// ============================================================================

TEST_F(CApiValueToJsonTest, ValueToJsonBoolTrue) {
	auto *res = zyx_execute(db, "RETURN true AS val");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		const char *json = zyx_result_get_map_json(res, 0);
		EXPECT_NE(json, nullptr);
		EXPECT_STREQ(json, "true");
	}
	zyx_result_close(res);
}

TEST_F(CApiValueToJsonTest, ValueToJsonBoolFalse) {
	auto *res = zyx_execute(db, "RETURN false AS val");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		const char *json = zyx_result_get_map_json(res, 0);
		EXPECT_NE(json, nullptr);
		EXPECT_STREQ(json, "false");
	}
	zyx_result_close(res);
}

// ============================================================================
// value_to_json: Cover int64_t branch
// ============================================================================

TEST_F(CApiValueToJsonTest, ValueToJsonInt) {
	auto *res = zyx_execute(db, "RETURN 42 AS val");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		const char *json = zyx_result_get_map_json(res, 0);
		EXPECT_NE(json, nullptr);
		EXPECT_STREQ(json, "42");
	}
	zyx_result_close(res);
}

// ============================================================================
// value_to_json: Cover double branch
// ============================================================================

TEST_F(CApiValueToJsonTest, ValueToJsonDouble) {
	auto *res = zyx_execute(db, "RETURN 3.14 AS val");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		const char *json = zyx_result_get_map_json(res, 0);
		EXPECT_NE(json, nullptr);
		// Should be a numeric string
		std::string s(json);
		EXPECT_NE(s.find("3.14"), std::string::npos);
	}
	zyx_result_close(res);
}

// ============================================================================
// value_to_json: Cover string branch
// ============================================================================

TEST_F(CApiValueToJsonTest, ValueToJsonString) {
	auto *res = zyx_execute(db, "RETURN 'hello' AS val");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		const char *json = zyx_result_get_map_json(res, 0);
		EXPECT_NE(json, nullptr);
		EXPECT_STREQ(json, "\"hello\"");
	}
	zyx_result_close(res);
}

// ============================================================================
// value_to_json: Cover null/monostate branch
// ============================================================================

TEST_F(CApiValueToJsonTest, ValueToJsonNull) {
	auto *res = zyx_execute(db, "RETURN null AS val");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		const char *json = zyx_result_get_map_json(res, 0);
		EXPECT_NE(json, nullptr);
		EXPECT_STREQ(json, "null");
	}
	zyx_result_close(res);
}

// ============================================================================
// value_to_json: Cover vector<string> branch (lines 183-189)
// ============================================================================

TEST_F(CApiValueToJsonTest, ValueToJsonStringList) {
	auto *res = zyx_execute(db, "RETURN ['hello', 'world'] AS items");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		const char *json = zyx_result_get_map_json(res, 0);
		EXPECT_NE(json, nullptr);
		std::string s(json);
		// Should contain the list elements
		EXPECT_NE(s.find("hello"), std::string::npos);
		EXPECT_NE(s.find("world"), std::string::npos);
	}
	zyx_result_close(res);
}

// ============================================================================
// value_to_json: Cover ValueList branch (lines 190-197)
// ============================================================================

TEST_F(CApiValueToJsonTest, ValueToJsonHeterogeneousList) {
	// Heterogeneous list should produce a ValueList
	auto *res = zyx_execute(db, "RETURN [1, 'two', true, null, 3.14] AS items");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		const char *json = zyx_result_get_map_json(res, 0);
		EXPECT_NE(json, nullptr);
		std::string s(json);
		EXPECT_NE(s.find("["), std::string::npos);
	}
	zyx_result_close(res);
}

// ============================================================================
// value_to_json: Cover ValueMap branch (lines 198-208)
// ============================================================================

TEST_F(CApiValueToJsonTest, ValueToJsonMapLiteral) {
	auto *res = zyx_execute(db, "RETURN {name: 'Alice', age: 30} AS m");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		const char *json = zyx_result_get_map_json(res, 0);
		EXPECT_NE(json, nullptr);
		std::string s(json);
		EXPECT_NE(s.find("Alice"), std::string::npos);
	}
	zyx_result_close(res);
}

// ============================================================================
// value_to_json: Cover nested list within map
// ============================================================================

TEST_F(CApiValueToJsonTest, ValueToJsonNestedMap) {
	auto *res = zyx_execute(db, "RETURN {a: [1, 2], b: {x: 'y'}} AS m");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		const char *json = zyx_result_get_map_json(res, 0);
		EXPECT_NE(json, nullptr);
	}
	zyx_result_close(res);
}

// ============================================================================
// List builder null guard failure branches (lines 473-493)
// ============================================================================

TEST_F(CApiValueToJsonTest, ListPushIntNullList) {
	zyx_list_push_int(nullptr, 42);
	// Should not crash
}

TEST_F(CApiValueToJsonTest, ListPushDoubleNullList) {
	zyx_list_push_double(nullptr, 1.5);
}

TEST_F(CApiValueToJsonTest, ListPushStringNullList) {
	zyx_list_push_string(nullptr, "test");
}

TEST_F(CApiValueToJsonTest, ListPushStringNullVal) {
	auto *list = zyx_list_create();
	zyx_list_push_string(list, nullptr);
	zyx_list_close(list);
}

TEST_F(CApiValueToJsonTest, ListPushBoolNullList) {
	zyx_list_push_bool(nullptr, true);
}

TEST_F(CApiValueToJsonTest, ListPushNullNullList) {
	zyx_list_push_null(nullptr);
}

TEST_F(CApiValueToJsonTest, ListPushListNullOuter) {
	auto *inner = zyx_list_create();
	zyx_list_push_list(nullptr, inner);
	zyx_list_close(inner);
}

TEST_F(CApiValueToJsonTest, ListPushListNullInner) {
	auto *outer = zyx_list_create();
	zyx_list_push_list(outer, nullptr);
	zyx_list_close(outer);
}

// ============================================================================
// Map builder null guard failure branches (lines 513-547)
// ============================================================================

TEST_F(CApiValueToJsonTest, MapSetIntNullMap) {
	zyx_map_set_int(nullptr, "key", 1);
}

TEST_F(CApiValueToJsonTest, MapSetIntNullKey) {
	auto *map = zyx_map_create();
	zyx_map_set_int(map, nullptr, 1);
	zyx_map_close(map);
}

TEST_F(CApiValueToJsonTest, MapSetDoubleNullMap) {
	zyx_map_set_double(nullptr, "key", 1.0);
}

TEST_F(CApiValueToJsonTest, MapSetDoubleNullKey) {
	auto *map = zyx_map_create();
	zyx_map_set_double(map, nullptr, 1.0);
	zyx_map_close(map);
}

TEST_F(CApiValueToJsonTest, MapSetStringNullMap) {
	zyx_map_set_string(nullptr, "key", "val");
}

TEST_F(CApiValueToJsonTest, MapSetStringNullKey) {
	auto *map = zyx_map_create();
	zyx_map_set_string(map, nullptr, "val");
	zyx_map_close(map);
}

TEST_F(CApiValueToJsonTest, MapSetStringNullVal) {
	auto *map = zyx_map_create();
	zyx_map_set_string(map, "key", nullptr);
	zyx_map_close(map);
}

TEST_F(CApiValueToJsonTest, MapSetBoolNullMap) {
	zyx_map_set_bool(nullptr, "key", true);
}

TEST_F(CApiValueToJsonTest, MapSetBoolNullKey) {
	auto *map = zyx_map_create();
	zyx_map_set_bool(map, nullptr, true);
	zyx_map_close(map);
}

TEST_F(CApiValueToJsonTest, MapSetNullNullMap) {
	zyx_map_set_null(nullptr, "key");
}

TEST_F(CApiValueToJsonTest, MapSetNullNullKey) {
	auto *map = zyx_map_create();
	zyx_map_set_null(map, nullptr);
	zyx_map_close(map);
}

TEST_F(CApiValueToJsonTest, MapSetListNullMap) {
	auto *list = zyx_list_create();
	zyx_map_set_list(nullptr, "key", list);
	zyx_list_close(list);
}

TEST_F(CApiValueToJsonTest, MapSetListNullKey) {
	auto *map = zyx_map_create();
	auto *list = zyx_list_create();
	zyx_map_set_list(map, nullptr, list);
	zyx_list_close(list);
	zyx_map_close(map);
}

TEST_F(CApiValueToJsonTest, MapSetListNullList) {
	auto *map = zyx_map_create();
	zyx_map_set_list(map, "key", nullptr);
	zyx_map_close(map);
}

TEST_F(CApiValueToJsonTest, MapSetMapNullOuter) {
	auto *inner = zyx_map_create();
	zyx_map_set_map(nullptr, "key", inner);
	zyx_map_close(inner);
}

TEST_F(CApiValueToJsonTest, MapSetMapNullKey) {
	auto *outer = zyx_map_create();
	auto *inner = zyx_map_create();
	zyx_map_set_map(outer, nullptr, inner);
	zyx_map_close(inner);
	zyx_map_close(outer);
}

TEST_F(CApiValueToJsonTest, MapSetMapNullInner) {
	auto *outer = zyx_map_create();
	zyx_map_set_map(outer, "key", nullptr);
	zyx_map_close(outer);
}

// ============================================================================
// Params builder null guard failure branches (lines 437-460)
// ============================================================================

TEST_F(CApiValueToJsonTest, ParamsSetIntNullParams) {
	zyx_params_set_int(nullptr, "key", 1);
}

TEST_F(CApiValueToJsonTest, ParamsSetIntNullKey) {
	auto *p = zyx_params_create();
	zyx_params_set_int(p, nullptr, 1);
	zyx_params_close(p);
}

TEST_F(CApiValueToJsonTest, ParamsSetDoubleNullParams) {
	zyx_params_set_double(nullptr, "key", 1.0);
}

TEST_F(CApiValueToJsonTest, ParamsSetDoubleNullKey) {
	auto *p = zyx_params_create();
	zyx_params_set_double(p, nullptr, 1.0);
	zyx_params_close(p);
}

TEST_F(CApiValueToJsonTest, ParamsSetStringNullParams) {
	zyx_params_set_string(nullptr, "key", "val");
}

TEST_F(CApiValueToJsonTest, ParamsSetStringNullKey) {
	auto *p = zyx_params_create();
	zyx_params_set_string(p, nullptr, "val");
	zyx_params_close(p);
}

TEST_F(CApiValueToJsonTest, ParamsSetStringNullVal) {
	auto *p = zyx_params_create();
	zyx_params_set_string(p, "key", nullptr);
	zyx_params_close(p);
}

TEST_F(CApiValueToJsonTest, ParamsSetBoolNullParams) {
	zyx_params_set_bool(nullptr, "key", true);
}

TEST_F(CApiValueToJsonTest, ParamsSetBoolNullKey) {
	auto *p = zyx_params_create();
	zyx_params_set_bool(p, nullptr, true);
	zyx_params_close(p);
}

TEST_F(CApiValueToJsonTest, ParamsSetNullNullParams) {
	zyx_params_set_null(nullptr, "key");
}

TEST_F(CApiValueToJsonTest, ParamsSetNullNullKey) {
	auto *p = zyx_params_create();
	zyx_params_set_null(p, nullptr);
	zyx_params_close(p);
}

TEST_F(CApiValueToJsonTest, ParamsSetListNullParams) {
	auto *list = zyx_list_create();
	zyx_params_set_list(nullptr, "key", list);
	zyx_list_close(list);
}

TEST_F(CApiValueToJsonTest, ParamsSetListNullKey) {
	auto *p = zyx_params_create();
	auto *list = zyx_list_create();
	zyx_params_set_list(p, nullptr, list);
	zyx_list_close(list);
	zyx_params_close(p);
}

TEST_F(CApiValueToJsonTest, ParamsSetListNullList) {
	auto *p = zyx_params_create();
	zyx_params_set_list(p, "key", nullptr);
	zyx_params_close(p);
}

TEST_F(CApiValueToJsonTest, ParamsSetMapNullParams) {
	auto *map = zyx_map_create();
	zyx_params_set_map(nullptr, "key", map);
	zyx_map_close(map);
}

TEST_F(CApiValueToJsonTest, ParamsSetMapNullKey) {
	auto *p = zyx_params_create();
	auto *map = zyx_map_create();
	zyx_params_set_map(p, nullptr, map);
	zyx_map_close(map);
	zyx_params_close(p);
}

TEST_F(CApiValueToJsonTest, ParamsSetMapNullMap) {
	auto *p = zyx_params_create();
	zyx_params_set_map(p, "key", nullptr);
	zyx_params_close(p);
}

// ============================================================================
// Result access null guards for list functions (lines 801-870)
// ============================================================================

TEST_F(CApiValueToJsonTest, ListSizeNullResult) {
	EXPECT_EQ(zyx_result_list_size(nullptr, 0), 0);
}

TEST_F(CApiValueToJsonTest, ListGetTypeNullResult) {
	EXPECT_EQ(zyx_result_list_get_type(nullptr, 0, 0), ZYX_NULL);
}

TEST_F(CApiValueToJsonTest, ListGetIntNullResult) {
	EXPECT_EQ(zyx_result_list_get_int(nullptr, 0, 0), 0);
}

TEST_F(CApiValueToJsonTest, ListGetDoubleNullResult) {
	EXPECT_EQ(zyx_result_list_get_double(nullptr, 0, 0), 0.0);
}

TEST_F(CApiValueToJsonTest, ListGetBoolNullResult) {
	EXPECT_FALSE(zyx_result_list_get_bool(nullptr, 0, 0));
}

TEST_F(CApiValueToJsonTest, ListGetStringNullResult) {
	EXPECT_STREQ(zyx_result_list_get_string(nullptr, 0, 0), "");
}

TEST_F(CApiValueToJsonTest, GetMapJsonNullResult) {
	EXPECT_STREQ(zyx_result_get_map_json(nullptr, 0), "{}");
}

// ============================================================================
// Result type checks for non-matching types (False branch)
// ============================================================================

TEST_F(CApiValueToJsonTest, GetIntFromStringColumn) {
	auto *res = zyx_execute(db, "RETURN 'hello' AS val");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		// Try to get int from a string column - should return 0
		EXPECT_EQ(zyx_result_get_int(res, 0), 0);
	}
	zyx_result_close(res);
}

TEST_F(CApiValueToJsonTest, GetDoubleFromStringColumn) {
	auto *res = zyx_execute(db, "RETURN 'hello' AS val");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_EQ(zyx_result_get_double(res, 0), 0.0);
	}
	zyx_result_close(res);
}

TEST_F(CApiValueToJsonTest, GetBoolFromStringColumn) {
	auto *res = zyx_execute(db, "RETURN 'hello' AS val");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_FALSE(zyx_result_get_bool(res, 0));
	}
	zyx_result_close(res);
}

TEST_F(CApiValueToJsonTest, GetStringFromIntColumn) {
	auto *res = zyx_execute(db, "RETURN 42 AS val");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_STREQ(zyx_result_get_string(res, 0), "");
	}
	zyx_result_close(res);
}

TEST_F(CApiValueToJsonTest, GetNodeFromIntColumn) {
	auto *res = zyx_execute(db, "RETURN 42 AS val");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		ZYXNode node;
		EXPECT_FALSE(zyx_result_get_node(res, 0, &node));
	}
	zyx_result_close(res);
}

TEST_F(CApiValueToJsonTest, GetEdgeFromIntColumn) {
	auto *res = zyx_execute(db, "RETURN 42 AS val");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		ZYXEdge edge;
		EXPECT_FALSE(zyx_result_get_edge(res, 0, &edge));
	}
	zyx_result_close(res);
}

// ============================================================================
// List size for non-list column (False branch for get_if)
// ============================================================================

TEST_F(CApiValueToJsonTest, ListSizeNonListColumn) {
	auto *res = zyx_execute(db, "RETURN 42 AS val");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_EQ(zyx_result_list_size(res, 0), 0);
		EXPECT_EQ(zyx_result_list_get_type(res, 0, 0), ZYX_NULL);
	}
	zyx_result_close(res);
}

// ============================================================================
// Props JSON for non-node/edge value (else branch in visitor)
// ============================================================================

TEST_F(CApiValueToJsonTest, PropsJsonForNonNodeEdge) {
	auto *res = zyx_execute(db, "RETURN 42 AS val");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		const char *json = zyx_result_get_props_json(res, 0);
		EXPECT_STREQ(json, "{}");
	}
	zyx_result_close(res);
}

// ============================================================================
// GetNode/GetEdge null output pointers
// ============================================================================

TEST_F(CApiValueToJsonTest, GetNodeNullOutput) {
	auto *res = zyx_execute(db, "CREATE (n:X) RETURN n");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_FALSE(zyx_result_get_node(res, 0, nullptr));
	}
	zyx_result_close(res);
}

TEST_F(CApiValueToJsonTest, GetEdgeNullOutput) {
	zyx_result_close(zyx_execute(db, "CREATE (a:EN1)-[:ER]->(b:EN2)"));
	auto *res = zyx_execute(db, "MATCH ()-[r]->() RETURN r");
	ASSERT_NE(res, nullptr);
	if (zyx_result_next(res)) {
		EXPECT_FALSE(zyx_result_get_edge(res, 0, nullptr));
	}
	zyx_result_close(res);
}

// ============================================================================
// Result next/success/error null guards
// ============================================================================

TEST_F(CApiValueToJsonTest, ResultNextNull) {
	EXPECT_FALSE(zyx_result_next(nullptr));
}

TEST_F(CApiValueToJsonTest, ResultIsSuccessNull) {
	EXPECT_FALSE(zyx_result_is_success(nullptr));
}

TEST_F(CApiValueToJsonTest, ResultGetErrorNull) {
	EXPECT_STREQ(zyx_result_get_error(nullptr), "Invalid Result Handle");
}

// ============================================================================
// Close with null (no-op, no crash)
// ============================================================================

TEST_F(CApiValueToJsonTest, CloseNulls) {
	zyx_result_close(nullptr);
	zyx_close(nullptr);
	zyx_txn_close(nullptr);
	zyx_params_close(nullptr);
	zyx_list_close(nullptr);
	zyx_map_close(nullptr);
}

// ============================================================================
// Transaction null guards
// ============================================================================

TEST_F(CApiValueToJsonTest, TxnCommitNull) {
	EXPECT_FALSE(zyx_txn_commit(nullptr));
}

TEST_F(CApiValueToJsonTest, TxnRollbackNull) {
	zyx_txn_rollback(nullptr);
}

TEST_F(CApiValueToJsonTest, TxnExecuteNull) {
	EXPECT_EQ(zyx_txn_execute(nullptr, "RETURN 1"), nullptr);
	auto *txn = zyx_begin_transaction(db);
	EXPECT_EQ(zyx_txn_execute(txn, nullptr), nullptr);
	zyx_txn_close(txn);
}

TEST_F(CApiValueToJsonTest, BeginTxnNull) {
	EXPECT_EQ(zyx_begin_transaction(nullptr), nullptr);
	EXPECT_EQ(zyx_begin_read_only_transaction(nullptr), nullptr);
}

// ============================================================================
// Create node/edge null guards
// ============================================================================

TEST_F(CApiValueToJsonTest, CreateNodeNullDb) {
	EXPECT_FALSE(zyx_create_node(nullptr, "L", nullptr));
}

TEST_F(CApiValueToJsonTest, CreateNodeNullLabel) {
	EXPECT_FALSE(zyx_create_node(db, nullptr, nullptr));
}

TEST_F(CApiValueToJsonTest, CreateEdgeNullDb) {
	EXPECT_FALSE(zyx_create_edge_by_id(nullptr, 0, 1, "T", nullptr));
}

TEST_F(CApiValueToJsonTest, CreateEdgeNullType) {
	EXPECT_FALSE(zyx_create_edge_by_id(db, 0, 1, nullptr, nullptr));
}

// ============================================================================
// Execute null guards
// ============================================================================

TEST_F(CApiValueToJsonTest, ExecuteNullDb) {
	EXPECT_EQ(zyx_execute(nullptr, "RETURN 1"), nullptr);
}

TEST_F(CApiValueToJsonTest, ExecuteNullCypher) {
	EXPECT_EQ(zyx_execute(db, nullptr), nullptr);
}
