/**
 * @file test_CApi_ListMapParams.cpp
 * @author ZYX Contributors
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

#include <filesystem>
#include <gtest/gtest.h>
#include "zyx/zyx_c_api.h"

#include <cstring>
#include <string>

// ============================================================================
// List Builder
// ============================================================================

TEST(CApiListMapParams, ListCreate_PushAllTypes_Close) {
	ZYXList_T *list = zyx_list_create();
	ASSERT_NE(list, nullptr);

	zyx_list_push_int(list, 42);
	zyx_list_push_double(list, 3.14);
	zyx_list_push_string(list, "hello");
	zyx_list_push_bool(list, true);
	zyx_list_push_null(list);

	zyx_list_close(list);
}

// ============================================================================
// Nested List
// ============================================================================

TEST(CApiListMapParams, ListPushList_Nested) {
	ZYXList_T *inner = zyx_list_create();
	ASSERT_NE(inner, nullptr);
	zyx_list_push_int(inner, 1);
	zyx_list_push_int(inner, 2);

	ZYXList_T *outer = zyx_list_create();
	ASSERT_NE(outer, nullptr);
	zyx_list_push_string(outer, "before");
	zyx_list_push_list(outer, inner);
	zyx_list_push_string(outer, "after");

	zyx_list_close(outer);
	// inner ownership transferred; do not close inner
}

// ============================================================================
// Map Builder
// ============================================================================

TEST(CApiListMapParams, MapCreate_SetAllTypes_Close) {
	ZYXMap_T *map = zyx_map_create();
	ASSERT_NE(map, nullptr);

	zyx_map_set_int(map, "i", 99);
	zyx_map_set_double(map, "d", 2.718);
	zyx_map_set_string(map, "s", "world");
	zyx_map_set_bool(map, "b", false);
	zyx_map_set_null(map, "n");

	zyx_map_close(map);
}

// ============================================================================
// Nested Map / List inside Map
// ============================================================================

TEST(CApiListMapParams, MapSetList_And_MapSetMap) {
	ZYXList_T *list = zyx_list_create();
	zyx_list_push_int(list, 10);
	zyx_list_push_int(list, 20);

	ZYXMap_T *nested = zyx_map_create();
	zyx_map_set_string(nested, "key", "val");

	ZYXMap_T *root = zyx_map_create();
	ASSERT_NE(root, nullptr);
	zyx_map_set_list(root, "myList", list);
	zyx_map_set_map(root, "myMap", nested);
	zyx_map_set_int(root, "x", 1);

	zyx_map_close(root);
	// list and nested ownership transferred; do not close them
}

// ============================================================================
// Parameter Integration: list param used in a query
// ============================================================================

TEST(CApiListMapParams, ParamsSetList_QueryIntegration) {
	// Note: ZYX doesn't support ":memory:" databases, use temp file instead
	std::string dbPath = (std::filesystem::temp_directory_path() / "test_zyx_list_params.graph").string();
	ZYXDB_T *db = zyx_open(dbPath.c_str());
	ASSERT_NE(db, nullptr);

	// Build a list parameter [42, 99]
	ZYXList_T *list = zyx_list_create();
	zyx_list_push_int(list, 42);
	zyx_list_push_int(list, 99);

	ZYXParams_T *params = zyx_params_create();
	zyx_params_set_list(params, "vals", list);

	// Verify the list parameter can be passed and returned
	ZYXResult_T *res = zyx_execute_params(
		db, "RETURN $vals AS v", params);
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_is_success(res));

	// The result should have at least one row
	ASSERT_TRUE(zyx_result_next(res));

	zyx_result_close(res);
	zyx_params_close(params);
	zyx_close(db);

	// Clean up temp file
	std::error_code ec;
	std::filesystem::remove(dbPath, ec);
}

// ============================================================================
// Parameter Integration: map param used in a query
// ============================================================================

TEST(CApiListMapParams, ParamsSetMap_QueryIntegration) {
	// Note: ZYX doesn't support ":memory:" databases, use temp file instead
	std::string dbPath = (std::filesystem::temp_directory_path() / "test_zyx_map_params.graph").string();
	ZYXDB_T *db = zyx_open(dbPath.c_str());
	ASSERT_NE(db, nullptr);

	// Build a map parameter {name: "Alice", age: 30}
	ZYXMap_T *map = zyx_map_create();
	zyx_map_set_string(map, "name", "Alice");
	zyx_map_set_int(map, "age", 30);

	ZYXParams_T *params = zyx_params_create();
	zyx_params_set_map(params, "props", map);

	// Verify the map parameter can be passed and returned
	ZYXResult_T *res = zyx_execute_params(
		db, "RETURN $props AS p", params);
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_is_success(res));

	// The result should have at least one row
	ASSERT_TRUE(zyx_result_next(res));

	zyx_result_close(res);
	zyx_params_close(params);
	zyx_close(db);

	// Clean up temp file
	std::error_code ec;
	std::filesystem::remove(dbPath, ec);
}

// ============================================================================
// Null Safety: null handles must not crash
// ============================================================================

TEST(CApiListMapParams, NullSafety_ListFunctions) {
	// Null list handle -- all should be no-ops
	zyx_list_push_int(nullptr, 1);
	zyx_list_push_double(nullptr, 1.0);
	zyx_list_push_string(nullptr, "x");
	zyx_list_push_bool(nullptr, true);
	zyx_list_push_null(nullptr);
	zyx_list_push_list(nullptr, nullptr);
	zyx_list_close(nullptr);

	// Push null string into a valid list
	ZYXList_T *list = zyx_list_create();
	zyx_list_push_string(list, nullptr);
	zyx_list_close(list);

	SUCCEED();
}

TEST(CApiListMapParams, NullSafety_MapFunctions) {
	// Null map handle -- all should be no-ops
	zyx_map_set_int(nullptr, "k", 1);
	zyx_map_set_double(nullptr, "k", 1.0);
	zyx_map_set_string(nullptr, "k", "v");
	zyx_map_set_bool(nullptr, "k", true);
	zyx_map_set_null(nullptr, "k");
	zyx_map_set_list(nullptr, "k", nullptr);
	zyx_map_set_map(nullptr, "k", nullptr);
	zyx_map_close(nullptr);

	// Null key into a valid map
	ZYXMap_T *map = zyx_map_create();
	zyx_map_set_int(map, nullptr, 1);
	zyx_map_set_string(map, nullptr, "v");
	zyx_map_close(map);

	SUCCEED();
}

TEST(CApiListMapParams, NullSafety_ParamsSetListMap) {
	// Null params handle
	zyx_params_set_list(nullptr, "k", nullptr);
	zyx_params_set_map(nullptr, "k", nullptr);

	// Null key
	ZYXParams_T *params = zyx_params_create();
	zyx_params_set_list(params, nullptr, nullptr);
	zyx_params_set_map(params, nullptr, nullptr);
	zyx_params_close(params);

	SUCCEED();
}
