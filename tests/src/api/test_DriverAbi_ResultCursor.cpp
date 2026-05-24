#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "zyx/zyx_driver_abi.h"

namespace fs = std::filesystem;

namespace {

std::string uniqueDbPath() {
    static std::atomic<unsigned long long> counter{0};
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto sequence = counter.fetch_add(1, std::memory_order_relaxed);
    return (fs::temp_directory_path() / ("zyx_driver_abi_result_cursor_" + std::to_string(now) + "_" +
                                        std::to_string(std::rand()) + "_" + std::to_string(sequence)))
        .string();
}

} // namespace

class DriverAbiResultCursorTest : public ::testing::Test {
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

TEST_F(DriverAbiResultCursorTest, ResultAccessorsValidateNullAndOutOfRangeInputs) {
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN 7 AS id, 3.5 AS score, true AS ok, 'Alice' AS name", nullptr,
                                    &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(error, nullptr);

    auto expectError = [this](zyx_driver_status_t status, zyx_driver_status_t expected) {
        EXPECT_EQ(status, expected);
        ASSERT_NE(error, nullptr);
        EXPECT_EQ(zyx_driver_error_code(error), expected);
        zyx_driver_error_free(error);
        error = nullptr;
    };

    expectError(zyx_driver_result_next(nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_result_column_count(nullptr), 0u);
    EXPECT_EQ(zyx_driver_result_column_name(nullptr, 0), nullptr);
    EXPECT_EQ(zyx_driver_result_value_type(nullptr, 0), ZYX_DRIVER_VALUE_NULL);

    const uint32_t columnCount = zyx_driver_result_column_count(result);
    ASSERT_EQ(columnCount, 4u);
    EXPECT_EQ(zyx_driver_result_column_name(result, columnCount), nullptr);
    EXPECT_EQ(zyx_driver_result_value_type(result, columnCount), ZYX_DRIVER_VALUE_NULL);
    EXPECT_EQ(zyx_driver_result_value_type(result, 0), ZYX_DRIVER_VALUE_NULL);

    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    ASSERT_EQ(error, nullptr);

    int64_t intValue = 0;
    double doubleValue = 0.0;
    bool boolValue = false;
    const char *stringValue = nullptr;

    expectError(zyx_driver_result_get_int64(result, 0, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_result_get_double(result, 1, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_result_get_bool(result, 2, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_result_get_string(result, 3, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);

    expectError(zyx_driver_result_get_int64(result, columnCount, &intValue, &error), ZYX_DRIVER_OUT_OF_RANGE);
    expectError(zyx_driver_result_get_double(result, columnCount, &doubleValue, &error), ZYX_DRIVER_OUT_OF_RANGE);
    expectError(zyx_driver_result_get_bool(result, columnCount, &boolValue, &error), ZYX_DRIVER_OUT_OF_RANGE);
    expectError(zyx_driver_result_get_string(result, columnCount, &stringValue, &error), ZYX_DRIVER_OUT_OF_RANGE);

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

TEST_F(DriverAbiResultCursorTest, ReadsMixedValueListElements) {
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN [1, true, 2.5, 'ok', null] AS items", nullptr, &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(error, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    ASSERT_EQ(error, nullptr);
    ASSERT_EQ(zyx_driver_result_value_type(result, 0), ZYX_DRIVER_VALUE_LIST);

    uint32_t count = 0;
    ASSERT_EQ(zyx_driver_result_get_list_count(result, 0, &count, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(count, 5u);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 0, 0), ZYX_DRIVER_VALUE_INT64);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 0, 1), ZYX_DRIVER_VALUE_BOOL);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 0, 2), ZYX_DRIVER_VALUE_DOUBLE);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 0, 3), ZYX_DRIVER_VALUE_STRING);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 0, 4), ZYX_DRIVER_VALUE_NULL);

    int64_t intValue = 0;
    bool boolValue = false;
    double doubleValue = 0.0;
    const char *stringValue = nullptr;
    ASSERT_EQ(zyx_driver_result_get_list_int64(result, 0, 0, &intValue, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(intValue, 1);
    ASSERT_EQ(zyx_driver_result_get_list_bool(result, 0, 1, &boolValue, &error), ZYX_DRIVER_OK);
    EXPECT_TRUE(boolValue);
    ASSERT_EQ(zyx_driver_result_get_list_double(result, 0, 2, &doubleValue, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(doubleValue, 2.5);
    ASSERT_EQ(zyx_driver_result_get_list_string(result, 0, 3, &stringValue, &error), ZYX_DRIVER_OK);
    ASSERT_NE(stringValue, nullptr);
    EXPECT_STREQ(stringValue, "ok");

    EXPECT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_DONE);
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

TEST_F(DriverAbiResultCursorTest, ExecuteWithDoubleAndNullParams) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_result_t *result = nullptr;
    struct Cleanup {
        zyx_driver_params_t *&params;
        zyx_driver_result_t *&result;
        zyx_driver_error_t *&error;
        ~Cleanup() {
            zyx_driver_result_free(result);
            result = nullptr;
            zyx_driver_params_free(params, &error);
            params = nullptr;
        }
    } cleanup{params, result, error};

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_NE(params, nullptr);
    ASSERT_EQ(error, nullptr);
    ASSERT_EQ(zyx_driver_params_set_double(params, "score", 98.25, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_null(params, "missing", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(error, nullptr);

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $score AS score, $missing AS missing", params, &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(error, nullptr);

    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    ASSERT_EQ(error, nullptr);
    EXPECT_EQ(zyx_driver_result_value_type(result, 0), ZYX_DRIVER_VALUE_DOUBLE);
    EXPECT_EQ(zyx_driver_result_value_type(result, 1), ZYX_DRIVER_VALUE_NULL);

    double score = 0.0;
    EXPECT_EQ(zyx_driver_result_get_double(result, 0, &score, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(score, 98.25);
    EXPECT_EQ(error, nullptr);
    EXPECT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_DONE);
    EXPECT_EQ(error, nullptr);

    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiResultCursorTest, ParamsCreateAndFreeValidateNullInputs) {
    EXPECT_EQ(zyx_driver_params_create(nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    zyx_driver_params_free(nullptr, &error);
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

    expectInvalidArgument(zyx_driver_params_set_string_list(nullptr, "tags", nullptr, 0, &error));
    expectInvalidArgument(zyx_driver_params_set_string_list(params, nullptr, nullptr, 0, &error));
    expectInvalidArgument(zyx_driver_params_set_float_list(nullptr, "embedding", nullptr, 0, &error));
    expectInvalidArgument(zyx_driver_params_set_float_list(params, nullptr, nullptr, 0, &error));

    expectInvalidArgument(zyx_driver_params_set_value(nullptr, "key", nullptr, &error));
    expectInvalidArgument(zyx_driver_params_set_value(params, nullptr, nullptr, &error));
    expectInvalidArgument(zyx_driver_params_set_value(params, "key", nullptr, &error));

    zyx_driver_params_free(params, &error);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiResultCursorTest, ParamsSetStringAndFloatListsRoundTripThroughResults) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_result_t *result = nullptr;
    struct Cleanup {
        zyx_driver_params_t *&params;
        zyx_driver_result_t *&result;
        zyx_driver_error_t *&error;
        ~Cleanup() {
            zyx_driver_result_free(result);
            result = nullptr;
            zyx_driver_params_free(params, &error);
            params = nullptr;
        }
    } cleanup{params, result, error};

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_NE(params, nullptr);
    const char *tags[] = {"graph", "abi", "python"};
    const float embedding[] = {0.25F, 1.5F, 2.75F};
    ASSERT_EQ(zyx_driver_params_set_string_list(params, "tags", tags, 3, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_float_list(params, "embedding", embedding, 3, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(error, nullptr);

    ASSERT_EQ(zyx_driver_db_execute(db, "CREATE (n:ListRoundTrip {tags:$tags, embedding:$embedding})", params,
                                    &result, &error),
              ZYX_DRIVER_OK);
    zyx_driver_result_free(result);
    result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "MATCH (n:ListRoundTrip) RETURN n.tags AS tags, n.embedding AS embedding",
                                    nullptr, &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    ASSERT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_result_value_type(result, 0), ZYX_DRIVER_VALUE_LIST);
    EXPECT_EQ(zyx_driver_result_value_type(result, 1), ZYX_DRIVER_VALUE_LIST);

    uint32_t tagCount = 0;
    uint32_t embeddingCount = 0;
    ASSERT_EQ(zyx_driver_result_get_list_count(result, 0, &tagCount, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_result_get_list_count(result, 1, &embeddingCount, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(tagCount, 3u);
    EXPECT_EQ(embeddingCount, 3u);

    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 0, 0), ZYX_DRIVER_VALUE_STRING);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 1, 0), ZYX_DRIVER_VALUE_DOUBLE);

    const char *tag = nullptr;
    double embeddingValue = 0.0;
    ASSERT_EQ(zyx_driver_result_get_list_string(result, 0, 1, &tag, &error), ZYX_DRIVER_OK);
    ASSERT_NE(tag, nullptr);
    EXPECT_STREQ(tag, "abi");
    ASSERT_EQ(zyx_driver_result_get_list_double(result, 1, 2, &embeddingValue, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(embeddingValue, 2.75);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiResultCursorTest, ParamsListSettersValidateNullAndEmptyInputs) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_result_t *result = nullptr;
    struct Cleanup {
        zyx_driver_params_t *&params;
        zyx_driver_result_t *&result;
        zyx_driver_error_t *&error;
        ~Cleanup() {
            zyx_driver_result_free(result);
            result = nullptr;
            zyx_driver_params_free(params, &error);
            params = nullptr;
        }
    } cleanup{params, result, error};

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);

    auto expectInvalidArgument = [this](zyx_driver_status_t status, const char *messageFragment) {
        EXPECT_EQ(status, ZYX_DRIVER_INVALID_ARGUMENT);
        ASSERT_NE(error, nullptr);
        EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
        EXPECT_NE(std::string(zyx_driver_error_message(error)).find(messageFragment), std::string::npos);
        zyx_driver_error_free(error);
        error = nullptr;
    };
    auto expectStatus = [this](zyx_driver_status_t status, zyx_driver_status_t expected) {
        EXPECT_EQ(status, expected);
        ASSERT_NE(error, nullptr);
        EXPECT_EQ(zyx_driver_error_code(error), expected);
        zyx_driver_error_free(error);
        error = nullptr;
    };

    ASSERT_EQ(zyx_driver_params_set_string_list(params, "emptyTags", nullptr, 0, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_float_list(params, "emptyEmbedding", nullptr, 0, &error), ZYX_DRIVER_OK);
    expectInvalidArgument(zyx_driver_params_set_string_list(params, "badTags", nullptr, 1, &error), "values");
    expectInvalidArgument(zyx_driver_params_set_float_list(params, "badEmbedding", nullptr, 1, &error), "values");

    const char *tagsWithNull[] = {"ok", nullptr};
    expectInvalidArgument(zyx_driver_params_set_string_list(params, "tagsWithNull", tagsWithNull, 2, &error),
                          "string list values");

    const char *tags[] = {"graph"};
    const float embedding[] = {0.5F};
    ASSERT_EQ(zyx_driver_params_set_string_list(params, "tags", tags, 1, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_float_list(params, "embedding", embedding, 1, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_db_execute(db,
                                    "RETURN $emptyTags AS emptyTags, $emptyEmbedding AS emptyEmbedding, "
                                    "$tags AS tags, $embedding AS embedding",
                                    params, &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    uint32_t count = 1;
    ASSERT_EQ(zyx_driver_result_get_list_count(result, 0, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 0u);
    count = 1;
    ASSERT_EQ(zyx_driver_result_get_list_count(result, 1, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 0u);
    count = 0;
    ASSERT_EQ(zyx_driver_result_get_list_count(result, 2, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 2, 0), ZYX_DRIVER_VALUE_STRING);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 2, 1), ZYX_DRIVER_VALUE_NULL);
    count = 0;
    ASSERT_EQ(zyx_driver_result_get_list_count(result, 3, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 3, 0), ZYX_DRIVER_VALUE_DOUBLE);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 3, 1), ZYX_DRIVER_VALUE_NULL);

    const char *tag = nullptr;
    double embeddingValue = 0.0;
    ASSERT_EQ(zyx_driver_result_get_list_string(result, 2, 0, &tag, &error), ZYX_DRIVER_OK);
    ASSERT_NE(tag, nullptr);
    EXPECT_STREQ(tag, "graph");
    ASSERT_EQ(zyx_driver_result_get_list_double(result, 3, 0, &embeddingValue, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(embeddingValue, 0.5);
    expectStatus(zyx_driver_result_get_list_string(result, 2, 1, &tag, &error), ZYX_DRIVER_OUT_OF_RANGE);
    expectStatus(zyx_driver_result_get_list_double(result, 3, 1, &embeddingValue, &error), ZYX_DRIVER_OUT_OF_RANGE);
    expectStatus(zyx_driver_result_get_list_string(result, 3, 0, &tag, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectStatus(zyx_driver_result_get_list_double(result, 2, 0, &embeddingValue, &error), ZYX_DRIVER_TYPE_MISMATCH);
}

TEST_F(DriverAbiResultCursorTest, ResultListAccessorsValidateErrorsAndConversions) {
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN [1, true, 2.5, 'ok'] AS items, 42 AS scalar, [7] AS nums", nullptr,
                                    &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    auto expectError = [this](zyx_driver_status_t status, zyx_driver_status_t expected) {
        EXPECT_EQ(status, expected);
        ASSERT_NE(error, nullptr);
        EXPECT_EQ(zyx_driver_error_code(error), expected);
        zyx_driver_error_free(error);
        error = nullptr;
    };

    zyx_driver_value_ref_t ref{1, 1, 1, 1};
    expectError(zyx_driver_result_get_value(result, 0, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_result_get_value(result, 3, &ref, &error), ZYX_DRIVER_OUT_OF_RANGE);
    EXPECT_EQ(ref.owner_id, 0u);
    EXPECT_EQ(ref.owner_cookie, 0u);
    EXPECT_EQ(ref.generation, 0u);
    EXPECT_EQ(ref.slot, 0u);

    uint32_t count = 0;
    expectError(zyx_driver_result_get_list_count(result, 0, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_result_get_list_count(result, 1, &count, &error), ZYX_DRIVER_TYPE_MISMATCH);
    ASSERT_EQ(zyx_driver_result_get_list_count(result, 0, &count, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(count, 4u);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 0, count), ZYX_DRIVER_VALUE_NULL);

    int64_t intValue = 0;
    bool boolValue = false;
    double doubleValue = 0.0;
    const char *stringValue = nullptr;

    expectError(zyx_driver_result_get_list_int64(result, 0, 1, &intValue, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_result_get_list_bool(result, 0, 0, &boolValue, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_result_get_list_double(result, 0, 3, &doubleValue, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_result_get_list_string(result, 0, 0, &stringValue, &error), ZYX_DRIVER_TYPE_MISMATCH);

    expectError(zyx_driver_result_get_list_int64(result, 0, count, &intValue, &error), ZYX_DRIVER_OUT_OF_RANGE);
    expectError(zyx_driver_result_get_list_bool(result, 0, count, &boolValue, &error), ZYX_DRIVER_OUT_OF_RANGE);
    expectError(zyx_driver_result_get_list_double(result, 0, count, &doubleValue, &error), ZYX_DRIVER_OUT_OF_RANGE);
    expectError(zyx_driver_result_get_list_string(result, 0, count, &stringValue, &error), ZYX_DRIVER_OUT_OF_RANGE);

    expectError(zyx_driver_result_get_list_int64(result, 0, 0, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_result_get_list_double(result, 0, 0, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_result_get_list_bool(result, 0, 0, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_result_get_list_string(result, 0, 0, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);

    ASSERT_EQ(zyx_driver_result_get_list_double(result, 2, 0, &doubleValue, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(doubleValue, 7.0);

    zyx_driver_result_free(result);
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


TEST_F(DriverAbiResultCursorTest, ScalarTypeMismatchesReportGotTypeNames) {
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db,
                                    "RETURN true AS ok, 98.25 AS score, null AS missing, 7 AS count, 'Ada' AS name",
                                    nullptr, &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    const char *stringValue = nullptr;
    int64_t intValue = 0;

    auto expectMismatchContains = [this](zyx_driver_status_t status, const char *gotType) {
        EXPECT_EQ(status, ZYX_DRIVER_TYPE_MISMATCH);
        ASSERT_NE(error, nullptr);
        EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_TYPE_MISMATCH);
        EXPECT_NE(std::string(zyx_driver_error_message(error)).find(gotType), std::string::npos);
        zyx_driver_error_free(error);
        error = nullptr;
    };

    expectMismatchContains(zyx_driver_result_get_string(result, 0, &stringValue, &error), "bool");
    expectMismatchContains(zyx_driver_result_get_string(result, 1, &stringValue, &error), "double");
    expectMismatchContains(zyx_driver_result_get_string(result, 2, &stringValue, &error), "null");
    expectMismatchContains(zyx_driver_result_get_string(result, 3, &stringValue, &error), "int64");
    expectMismatchContains(zyx_driver_result_get_int64(result, 4, &intValue, &error), "string");

    zyx_driver_result_free(result);
}

TEST_F(DriverAbiResultCursorTest, ListAndMapValuesReportTypeMismatchesWhenSupported) {
    zyx_driver_result_t *result = nullptr;

    const zyx_driver_status_t status =
        zyx_driver_db_execute(db, "RETURN [1, 2] AS listValue, {answer: 42} AS mapValue", nullptr, &result, &error);
    if (status == ZYX_DRIVER_EXECUTION_ERROR) {
        zyx_driver_error_free(error);
        error = nullptr;
        GTEST_SKIP() << "List/map literals are not supported by the current Cypher execution path";
    }
    ASSERT_EQ(status, ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    EXPECT_EQ(zyx_driver_result_value_type(result, 0), ZYX_DRIVER_VALUE_LIST);
    EXPECT_EQ(zyx_driver_result_value_type(result, 1), ZYX_DRIVER_VALUE_MAP);

    int64_t intValue = 0;
    EXPECT_EQ(zyx_driver_result_get_int64(result, 0, &intValue, &error), ZYX_DRIVER_TYPE_MISMATCH);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_TYPE_MISMATCH);
    EXPECT_NE(std::string(zyx_driver_error_message(error)).find("list"), std::string::npos);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_result_get_int64(result, 1, &intValue, &error), ZYX_DRIVER_TYPE_MISMATCH);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_TYPE_MISMATCH);
    EXPECT_NE(std::string(zyx_driver_error_message(error)).find("map"), std::string::npos);
    zyx_driver_error_free(error);
    error = nullptr;

    zyx_driver_result_free(result);
}

TEST_F(DriverAbiResultCursorTest, DbExecuteValidatesInvalidArguments) {
    zyx_driver_result_t *result = reinterpret_cast<zyx_driver_result_t *>(0x1);

    EXPECT_EQ(zyx_driver_db_execute(nullptr, "RETURN 1", nullptr, &result, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(result, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    result = reinterpret_cast<zyx_driver_result_t *>(0x1);
    EXPECT_EQ(zyx_driver_db_execute(db, nullptr, nullptr, &result, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(result, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_db_execute(db, "RETURN 1", nullptr, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;
}

TEST_F(DriverAbiResultCursorTest, DbExecuteInvalidCypherReportsErrorWithOptionalErrorOutput) {
    zyx_driver_result_t *result = nullptr;

    EXPECT_EQ(zyx_driver_db_execute(db, "RETURN", nullptr, &result, &error), ZYX_DRIVER_EXECUTION_ERROR);
    EXPECT_EQ(result, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_EXECUTION_ERROR);
    EXPECT_NE(zyx_driver_error_message(error), nullptr);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_db_execute(db, "RETURN", nullptr, &result, nullptr), ZYX_DRIVER_EXECUTION_ERROR);
    EXPECT_EQ(result, nullptr);
    EXPECT_EQ(error, nullptr);
}
