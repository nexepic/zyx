#include <cstdlib>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "zyx/zyx_driver_abi.h"

namespace fs = std::filesystem;

class DriverAbiResultCursorTest : public ::testing::Test {
protected:
    std::string dbPath;
    zyx_driver_db_t *db = nullptr;
    zyx_driver_error_t *error = nullptr;

    void SetUp() override {
        dbPath = (fs::temp_directory_path() / ("zyx_driver_abi_result_cursor_" + std::to_string(std::rand()))).string();
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
};

TEST_F(DriverAbiResultCursorTest, StreamsScalarRowWithColumnMetadataAndTypedGetters) {
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN 7 AS id, 'Alice' AS name, 'blue' AS tag, true AS ok, 3.5 AS score",
                                     nullptr, &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_result_column_count(result), 5u);
    const char *idColumn = zyx_driver_result_column_name(result, 0);
    const char *nameColumn = zyx_driver_result_column_name(result, 1);
    const char *tagColumn = zyx_driver_result_column_name(result, 2);
    const char *okColumn = zyx_driver_result_column_name(result, 3);
    const char *scoreColumn = zyx_driver_result_column_name(result, 4);
    EXPECT_STREQ(idColumn, "id");
    EXPECT_STREQ(nameColumn, "name");
    EXPECT_STREQ(tagColumn, "tag");
    EXPECT_STREQ(okColumn, "ok");
    EXPECT_STREQ(scoreColumn, "score");

    EXPECT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    ASSERT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_result_value_type(result, 0), ZYX_DRIVER_VALUE_INT64);
    EXPECT_EQ(zyx_driver_result_value_type(result, 1), ZYX_DRIVER_VALUE_STRING);
    EXPECT_EQ(zyx_driver_result_value_type(result, 2), ZYX_DRIVER_VALUE_STRING);
    EXPECT_EQ(zyx_driver_result_value_type(result, 3), ZYX_DRIVER_VALUE_BOOL);
    EXPECT_EQ(zyx_driver_result_value_type(result, 4), ZYX_DRIVER_VALUE_DOUBLE);

    int64_t id = 0;
    const char *name = nullptr;
    const char *tag = nullptr;
    bool ok = false;
    double score = 0.0;

    EXPECT_EQ(zyx_driver_result_get_int64(result, 0, &id, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(id, 7);
    EXPECT_EQ(zyx_driver_result_get_string(result, 1, &name, &error), ZYX_DRIVER_OK);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "Alice");
    EXPECT_EQ(zyx_driver_result_get_string(result, 2, &tag, &error), ZYX_DRIVER_OK);
    ASSERT_NE(tag, nullptr);
    EXPECT_STREQ(tag, "blue");
    EXPECT_STREQ(name, "Alice");
    EXPECT_EQ(zyx_driver_result_get_bool(result, 3, &ok, &error), ZYX_DRIVER_OK);
    EXPECT_TRUE(ok);
    EXPECT_EQ(zyx_driver_result_get_double(result, 4, &score, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(score, 3.5);
    EXPECT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_DONE);
    EXPECT_EQ(error, nullptr);

    zyx_driver_result_free(result);
}

TEST_F(DriverAbiResultCursorTest, StringPointersRemainStableUntilNextOrFree) {
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN 'Alice' AS name, 'blue' AS tag", nullptr, &result, &error), ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(error, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    ASSERT_EQ(error, nullptr);

    const char *firstName = nullptr;
    ASSERT_EQ(zyx_driver_result_get_string(result, 0, &firstName, &error), ZYX_DRIVER_OK);
    ASSERT_NE(firstName, nullptr);
    ASSERT_STREQ(firstName, "Alice");

    for (int i = 0; i < 256; ++i) {
        const char *columnName = zyx_driver_result_column_name(result, 1);
        ASSERT_NE(columnName, nullptr);
        ASSERT_STREQ(columnName, "tag");

        const char *tag = nullptr;
        ASSERT_EQ(zyx_driver_result_get_string(result, 1, &tag, &error), ZYX_DRIVER_OK);
        ASSERT_NE(tag, nullptr);
        ASSERT_STREQ(tag, "blue");
    }

    EXPECT_STREQ(firstName, "Alice");
    EXPECT_EQ(error, nullptr);

    zyx_driver_result_free(result);
}

TEST_F(DriverAbiResultCursorTest, ExecuteWithScalarParams) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_NE(params, nullptr);
    ASSERT_EQ(error, nullptr);
    ASSERT_EQ(zyx_driver_params_set_string(params, "name", "Ada", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_int64(params, "age", 37, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_bool(params, "active", true, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(error, nullptr);

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $name AS name, $age AS age, $active AS active", params, &result,
                                    &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(error, nullptr);

    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    ASSERT_EQ(error, nullptr);

    const char *name = nullptr;
    int64_t age = 0;
    bool active = false;
    EXPECT_EQ(zyx_driver_result_get_string(result, 0, &name, &error), ZYX_DRIVER_OK);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "Ada");
    EXPECT_EQ(zyx_driver_result_get_int64(result, 1, &age, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(age, 37);
    EXPECT_EQ(zyx_driver_result_get_bool(result, 2, &active, &error), ZYX_DRIVER_OK);
    EXPECT_TRUE(active);
    EXPECT_EQ(error, nullptr);

    zyx_driver_result_free(result);
    zyx_driver_params_free(params, &error);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiResultCursorTest, ParamsSettersRejectNullInputs) {
    zyx_driver_params_t *params = nullptr;
    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_NE(params, nullptr);
    ASSERT_EQ(error, nullptr);

    auto expectInvalidArgument = [this](zyx_driver_status_t status) {
        EXPECT_EQ(status, ZYX_DRIVER_INVALID_ARGUMENT);
        ASSERT_NE(error, nullptr);
        EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
        zyx_driver_error_free(error);
        error = nullptr;
    };

    expectInvalidArgument(zyx_driver_params_set_null(nullptr, "value", &error));
    expectInvalidArgument(zyx_driver_params_set_null(params, nullptr, &error));
    expectInvalidArgument(zyx_driver_params_set_bool(nullptr, "value", true, &error));
    expectInvalidArgument(zyx_driver_params_set_bool(params, nullptr, true, &error));
    expectInvalidArgument(zyx_driver_params_set_int64(nullptr, "value", 1, &error));
    expectInvalidArgument(zyx_driver_params_set_int64(params, nullptr, 1, &error));
    expectInvalidArgument(zyx_driver_params_set_double(nullptr, "value", 1.0, &error));
    expectInvalidArgument(zyx_driver_params_set_double(params, nullptr, 1.0, &error));
    expectInvalidArgument(zyx_driver_params_set_string(nullptr, "value", "text", &error));
    expectInvalidArgument(zyx_driver_params_set_string(params, nullptr, "text", &error));
    expectInvalidArgument(zyx_driver_params_set_string(params, "value", nullptr, &error));

    zyx_driver_params_free(params, &error);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiResultCursorTest, TypeMismatchReturnsStructuredError) {
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN 'Alice' AS name", nullptr, &result, &error), ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(error, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    int64_t value = 0;
    EXPECT_EQ(zyx_driver_result_get_int64(result, 0, &value, &error), ZYX_DRIVER_TYPE_MISMATCH);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_TYPE_MISMATCH);
    EXPECT_NE(std::string(zyx_driver_error_message(error)).find("type"), std::string::npos);

    zyx_driver_error_free(error);
    error = nullptr;
    zyx_driver_result_free(result);
}
