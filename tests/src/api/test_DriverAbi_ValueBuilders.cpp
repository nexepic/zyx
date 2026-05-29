#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "api/driver_abi/DriverAbiInternal.hpp"
#include "zyx/zyx_driver_abi.h"

namespace fs = std::filesystem;

namespace {

std::string uniqueDbPath() {
    static std::atomic<unsigned long long> counter{0};
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto sequence = counter.fetch_add(1, std::memory_order_relaxed);
    return (fs::temp_directory_path() / ("zyx_driver_abi_value_builders_" + std::to_string(now) + "_" +
                                        std::to_string(std::rand()) + "_" + std::to_string(sequence)))
        .string();
}

} // namespace

class DriverAbiValueBuildersTest : public ::testing::Test {
protected:
    std::string dbPath;
    zyx_driver_db_t *db = nullptr;
    zyx_driver_error_t *error = nullptr;

    void SetUp() override {
        dbPath = uniqueDbPath();
        cleanup();
        ASSERT_EQ(zyx_driver_db_open(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
        ASSERT_NE(db, nullptr);
        ASSERT_EQ(error, nullptr);
    }

    void TearDown() override {
        if (db != nullptr) {
            EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
            db = nullptr;
        }
        zyx_driver_error_free(error);
        error = nullptr;
        cleanup();
    }

    void cleanup() {
        std::error_code ec;
        fs::remove_all(dbPath, ec);
        fs::remove(dbPath + "-wal", ec);
    }

    void expectError(zyx_driver_status_t status, zyx_driver_status_t expected) {
        EXPECT_EQ(status, expected);
        ASSERT_NE(error, nullptr);
        EXPECT_EQ(zyx_driver_error_code(error), expected);
        zyx_driver_error_free(error);
        error = nullptr;
    }
};

TEST_F(DriverAbiValueBuildersTest, ExecutesQueryWithNestedListAndMapParams) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *items = nullptr;
    zyx_driver_value_t *nested = nullptr;
    zyx_driver_value_t *metadata = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_create(&items, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_int64(items, 7, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_bool(items, true, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_string(items, "leaf", &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_value_list_create(&nested, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_double(nested, 2.5, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_null(nested, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_value(items, nested, &error), ZYX_DRIVER_OK);
    zyx_driver_value_free(nested, &error);
    nested = nullptr;

    ASSERT_EQ(zyx_driver_value_map_create(&metadata, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(metadata, "name", "Ada", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(metadata, "age", 37, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_value(items, metadata, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_params_set_value(params, "items", items, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_string(items, "late-list-mutation", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(metadata, "late", "map mutation", &error), ZYX_DRIVER_OK);
    zyx_driver_value_free(metadata, &error);
    metadata = nullptr;
    zyx_driver_value_free(items, &error);
    items = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $items AS items", params, &result, &error), ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    uint32_t count = 0;
    ASSERT_EQ(zyx_driver_result_get_list_count(result, 0, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 5u);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 0, 0), ZYX_DRIVER_VALUE_INT64);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 0, 1), ZYX_DRIVER_VALUE_BOOL);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 0, 2), ZYX_DRIVER_VALUE_STRING);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 0, 3), ZYX_DRIVER_VALUE_LIST);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 0, 4), ZYX_DRIVER_VALUE_MAP);

    zyx_driver_result_free(result);
    zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiValueBuildersTest, MapBuilderCanStoreNestedListAndMapValues) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *root = nullptr;
    zyx_driver_value_t *tags = nullptr;
    zyx_driver_value_t *details = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_create(&root, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_create(&tags, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_string(tags, "graph", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_string(tags, "abi", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_value(root, "tags", tags, &error), ZYX_DRIVER_OK);
    zyx_driver_value_free(tags, &error);
    tags = nullptr;

    ASSERT_EQ(zyx_driver_value_map_create(&details, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_bool(details, "stable", true, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_null(details, "legacy", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_value(root, "details", details, &error), ZYX_DRIVER_OK);
    zyx_driver_value_free(details, &error);
    details = nullptr;

    ASSERT_EQ(zyx_driver_params_set_value(params, "root", root, &error), ZYX_DRIVER_OK);
    zyx_driver_value_free(root, &error);
    root = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $root AS root", params, &result, &error), ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    EXPECT_EQ(zyx_driver_result_value_type(result, 0), ZYX_DRIVER_VALUE_MAP);

    zyx_driver_result_free(result);
    zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiValueBuildersTest, ParamsSetValueSnapshotsTopLevelMapBeforeSourceMutation) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *root = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_create(&root, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(root, "name", "Ada", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(root, "age", 37, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_value(params, "root", root, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(root, "name", "Grace", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(root, "age", 99, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(root, "late", "mutation", &error), ZYX_DRIVER_OK);
    zyx_driver_value_free(root, &error);
    root = nullptr;

    const auto status = zyx_driver_db_execute(db, "RETURN toString($root) AS snapshot", params, &result, &error);
    ASSERT_EQ(status, ZYX_DRIVER_OK) << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    EXPECT_EQ(zyx_driver_result_value_type(result, 0), ZYX_DRIVER_VALUE_STRING);

    const char *snapshot = nullptr;
    ASSERT_EQ(zyx_driver_result_get_string(result, 0, &snapshot, &error), ZYX_DRIVER_OK);
    ASSERT_NE(snapshot, nullptr);
    const std::string snapshotText(snapshot);
    EXPECT_NE(snapshotText.find("name: Ada"), std::string::npos);
    EXPECT_NE(snapshotText.find("age: 37"), std::string::npos);
    EXPECT_EQ(snapshotText.find("Grace"), std::string::npos);
    EXPECT_EQ(snapshotText.find("99"), std::string::npos);
    EXPECT_EQ(snapshotText.find("late"), std::string::npos);
    EXPECT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_DONE);

    zyx_driver_result_free(result);
    zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiValueBuildersTest, ValueBuildersValidateArgumentsAndOwnership) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *value = nullptr;
    zyx_driver_value_t *scalar = nullptr;

    expectError(zyx_driver_value_list_create(nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_create(&value, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_int64_create(42, &scalar, &error), ZYX_DRIVER_OK);

    expectError(zyx_driver_value_list_append_string(value, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_list_append_value(value, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_map_set_int64(value, "bad", 1, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_list_append_null(scalar, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_params_set_value(nullptr, "items", value, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_params_set_value(params, nullptr, value, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_params_set_value(params, "items", nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);

    zyx_driver_value_free(scalar, &error);
    EXPECT_EQ(error, nullptr);
    zyx_driver_value_free(value, &error);
    EXPECT_EQ(error, nullptr);
    zyx_driver_value_free(nullptr, &error);
    EXPECT_EQ(error, nullptr);
    zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiValueBuildersTest, StandaloneValueConstructorsCoverScalarVariants) {
    zyx_driver_value_t *nullValue = nullptr;
    zyx_driver_value_t *boolValue = nullptr;
    zyx_driver_value_t *doubleValue = nullptr;
    zyx_driver_value_t *stringValue = nullptr;

    ASSERT_EQ(zyx_driver_value_null_create(&nullValue, &error), ZYX_DRIVER_OK);
    ASSERT_NE(nullValue, nullptr);
    ASSERT_EQ(zyx_driver_value_bool_create(true, &boolValue, &error), ZYX_DRIVER_OK);
    ASSERT_NE(boolValue, nullptr);
    ASSERT_EQ(zyx_driver_value_double_create(3.25, &doubleValue, &error), ZYX_DRIVER_OK);
    ASSERT_NE(doubleValue, nullptr);
    ASSERT_EQ(zyx_driver_value_string_create("scalar", &stringValue, &error), ZYX_DRIVER_OK);
    ASSERT_NE(stringValue, nullptr);
    EXPECT_EQ(error, nullptr);

    zyx_driver_value_free(stringValue, &error);
    zyx_driver_value_free(doubleValue, &error);
    zyx_driver_value_free(boolValue, &error);
    zyx_driver_value_free(nullValue, &error);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiValueBuildersTest, ValueConstructorsRejectNullOutputsAndInputs) {
    expectError(zyx_driver_value_null_create(nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_bool_create(false, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_int64_create(1, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_double_create(1.25, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_string_create("value", nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);

    zyx_driver_value_t *value = reinterpret_cast<zyx_driver_value_t *>(0x1);
    expectError(zyx_driver_value_string_create(nullptr, &value, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(value, nullptr);
    expectError(zyx_driver_value_map_create(nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
}

TEST_F(DriverAbiValueBuildersTest, MapBuilderValidatesKeysValuesAndTypeMismatches) {
    zyx_driver_value_t *map = nullptr;
    zyx_driver_value_t *list = nullptr;
    zyx_driver_value_t *nested = nullptr;

    ASSERT_EQ(zyx_driver_value_map_create(&map, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_create(&list, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_double_create(2.5, &nested, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_value_map_set_double(map, "score", 98.25, &error), ZYX_DRIVER_OK);
    expectError(zyx_driver_value_map_set_null(map, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_map_set_bool(nullptr, "ok", true, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_map_set_double(list, "score", 1.0, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_map_set_string(map, nullptr, "value", &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_map_set_string(map, "value", nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_map_set_value(map, nullptr, nested, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_map_set_value(map, "nested", nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_map_set_value(list, "nested", nested, &error), ZYX_DRIVER_TYPE_MISMATCH);

    zyx_driver_value_free(nested, &error);
    zyx_driver_value_free(list, &error);
    zyx_driver_value_free(map, &error);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiValueBuildersTest, MutableContainerHelpersRejectNullBackedHandles) {
    zyx_driver_value_t nullList{std::shared_ptr<zyx::ValueList>{}};
    zyx_driver_value_t nullMap{std::shared_ptr<zyx::ValueMap>{}};
    zyx_driver_value_t scalar{int64_t{7}};

    expectError(zyx_driver_value_list_append_null(&nullList, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_list_append_string(nullptr, "value", &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_list_append_string(&scalar, "value", &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_map_set_null(&nullMap, "key", &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_map_set_string(nullptr, "key", "value", &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_map_set_string(&scalar, "key", "value", &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_map_set_value(nullptr, "key", &scalar, &error), ZYX_DRIVER_INVALID_ARGUMENT);
}

TEST_F(DriverAbiValueBuildersTest, ListBuilderReportsNullListAsInvalidArgument) {
    expectError(zyx_driver_value_list_append_null(nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);

    zyx_driver_value_t *nested = nullptr;
    ASSERT_EQ(zyx_driver_value_int64_create(1, &nested, &error), ZYX_DRIVER_OK);
    expectError(zyx_driver_value_list_append_value(nullptr, nested, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_value_free(nested, &error);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiValueBuildersTest, ParamsSetValueDeepCopiesDefensiveNullContainers) {
    zyx_driver_params_t *params = nullptr;
    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);

    zyx_driver_value_t nullList{std::shared_ptr<zyx::ValueList>{}};
    zyx_driver_value_t nullMap{std::shared_ptr<zyx::ValueMap>{}};

    EXPECT_EQ(zyx_driver_params_set_value(params, "nullList", &nullList, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_params_set_value(params, "nullMap", &nullMap, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);

    zyx_driver_params_free(params, &error);
    EXPECT_EQ(error, nullptr);
}
