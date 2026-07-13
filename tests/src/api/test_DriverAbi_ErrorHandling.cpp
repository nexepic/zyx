#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include "api/driver_abi/DriverAbiInternal.hpp"
#include "zyx/zyx_driver_abi.h"

namespace fs = std::filesystem;

const zyx_driver_error_t &staticErrorFor(zyx_driver_status_t code) noexcept;
void setStaticError(zyx_driver_error_t **out_error, zyx_driver_status_t code) noexcept;
zyx_driver_status_t internalError(zyx_driver_error_t **out_error, const char *message) noexcept;
zyx_driver_status_t transactionExceptionStatus(const std::exception &ex);

namespace {

void freeError(zyx_driver_error_t *&error) {
    zyx_driver_error_free(error);
    error = nullptr;
}

void expectError(zyx_driver_status_t status, zyx_driver_status_t expected, zyx_driver_error_t *&error) {
    EXPECT_EQ(status, expected);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), expected);
    freeError(error);
}

std::string uniquePath(const std::string &prefix) {
    return (fs::temp_directory_path() / (prefix + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                                        "_" + std::to_string(reinterpret_cast<uintptr_t>(&prefix))))
            .string();
}

} // namespace

TEST(DriverAbiErrorHandlingTest, PublicErrorAccessorsHandleFallbackAndStaticStorage) {
    zyx_driver_error_t staticError{ZYX_DRIVER_IO_ERROR, {}, "fallback I/O error", true, -1, {}};

    EXPECT_EQ(zyx_driver_error_code(&staticError), ZYX_DRIVER_IO_ERROR);
    EXPECT_STREQ(zyx_driver_error_message(&staticError), "fallback I/O error");

    zyx_driver_error_free(&staticError);
    EXPECT_STREQ(zyx_driver_error_message(&staticError), "fallback I/O error");
}

TEST(DriverAbiErrorHandlingTest, InternalSetErrorAcceptsNullMessage) {
    zyx_driver_error_t *error = nullptr;

    EXPECT_EQ(setError(&error, ZYX_DRIVER_INTERNAL_ERROR, static_cast<const char *>(nullptr)),
              ZYX_DRIVER_INTERNAL_ERROR);

    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INTERNAL_ERROR);
    EXPECT_STREQ(zyx_driver_error_message(error), "");
    zyx_driver_error_free(error);
}

TEST(DriverAbiErrorHandlingTest, StaticErrorsCoverAllStatusCodes) {
    const zyx_driver_status_t statuses[] = {
            ZYX_DRIVER_OK,
            ZYX_DRIVER_INVALID_ARGUMENT,
            ZYX_DRIVER_OUT_OF_RANGE,
            ZYX_DRIVER_TYPE_MISMATCH,
            ZYX_DRIVER_NOT_FOUND,
            ZYX_DRIVER_OPEN_FAILED,
            ZYX_DRIVER_PARSE_ERROR,
            ZYX_DRIVER_EXECUTION_ERROR,
            ZYX_DRIVER_TRANSACTION_ERROR,
            ZYX_DRIVER_READ_ONLY_VIOLATION,
            ZYX_DRIVER_IO_ERROR,
            ZYX_DRIVER_OUT_OF_MEMORY,
            ZYX_DRIVER_INTERNAL_ERROR,
            ZYX_DRIVER_ROW,
            ZYX_DRIVER_DONE,
    };

    for (const auto status : statuses) {
        const auto &error = staticErrorFor(status);
        const auto expected = (status == ZYX_DRIVER_ROW || status == ZYX_DRIVER_DONE) ? ZYX_DRIVER_OK : status;
        EXPECT_EQ(error.code, expected);
        EXPECT_TRUE(error.static_storage);
        EXPECT_NE(zyx_driver_error_message(&error), nullptr);
    }

    const auto &unknown = staticErrorFor(static_cast<zyx_driver_status_t>(9999));
    EXPECT_EQ(unknown.code, ZYX_DRIVER_INTERNAL_ERROR);
}

TEST(DriverAbiErrorHandlingTest, StaticErrorSetterHandlesNullAndOutputPointers) {
    zyx_driver_error_t *error = nullptr;

    setStaticError(nullptr, ZYX_DRIVER_INVALID_ARGUMENT);
    setStaticError(&error, ZYX_DRIVER_NOT_FOUND);

    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_NOT_FOUND);
    EXPECT_TRUE(error->static_storage);
    zyx_driver_error_free(error);
}

TEST(DriverAbiErrorHandlingTest, CatchAbiExceptionMapsExceptionKinds) {
    zyx_driver_error_t *error = nullptr;

    try {
        throw std::bad_alloc();
    } catch (...) {
        expectError(catchAbiException(&error), ZYX_DRIVER_OUT_OF_MEMORY, error);
    }

    try {
        throw std::runtime_error("runtime failure");
    } catch (...) {
        expectError(catchAbiException(&error), ZYX_DRIVER_INTERNAL_ERROR, error);
    }

    try {
        throw 7;
    } catch (...) {
        expectError(catchAbiException(&error), ZYX_DRIVER_INTERNAL_ERROR, error);
    }

    expectError(internalError(&error, "internal helper"), ZYX_DRIVER_INTERNAL_ERROR, error);

    try {
        throw std::bad_alloc();
    } catch (...) {
        expectError(catchAbiExceptionAs(&error, ZYX_DRIVER_OPEN_FAILED), ZYX_DRIVER_OUT_OF_MEMORY, error);
    }

    try {
        throw std::runtime_error("open failed");
    } catch (...) {
        expectError(catchAbiExceptionAs(&error, ZYX_DRIVER_OPEN_FAILED), ZYX_DRIVER_OPEN_FAILED, error);
    }

    try {
        throw 7;
    } catch (...) {
        expectError(catchAbiExceptionAs(&error, ZYX_DRIVER_OPEN_FAILED), ZYX_DRIVER_INTERNAL_ERROR, error);
    }
}

TEST(DriverAbiErrorHandlingTest, DatabaseAbiRejectsMalformedInternalHandles) {
    zyx_driver_error_t *error = nullptr;
    auto *db = new zyx_driver_db_t();
    int64_t id = -1;
    bool active = true;
    zyx_driver_result_t *result = reinterpret_cast<zyx_driver_result_t *>(0x1);
    const char *labels[] = {"Person", nullptr};

    expectError(zyx_driver_db_save(db, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
    expectError(zyx_driver_db_has_active_transaction(db, &active, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
    expectError(zyx_driver_db_set_thread_pool_size(db, 2, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
    expectError(zyx_driver_db_create_node(db, "Person", nullptr, &id, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
    expectError(zyx_driver_db_create_node_with_labels(db, labels, 1, nullptr, &id, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
    expectError(zyx_driver_db_create_edge(db, 1, 2, "KNOWS", nullptr, &id, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
    expectError(zyx_driver_db_execute(db, "RETURN 1", nullptr, &result, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
    EXPECT_EQ(result, nullptr);
    zyx_driver_txn_t *txn = reinterpret_cast<zyx_driver_txn_t *>(0x1);
    expectError(zyx_driver_txn_begin(db, &txn, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
    EXPECT_EQ(txn, nullptr);
    txn = reinterpret_cast<zyx_driver_txn_t *>(0x1);
    expectError(zyx_driver_txn_begin_read_only(db, &txn, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
    EXPECT_EQ(txn, nullptr);

    EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);
}

TEST(DriverAbiErrorHandlingTest, DatabaseAbiCoversValidationAndExecutionErrors) {
    zyx_driver_error_t *error = nullptr;
    zyx_driver_db_t *db = nullptr;
    zyx_driver_result_t *result = nullptr;
    int64_t id = -1;
    const auto dbPath = uniquePath("zyx_driver_abi_error_handling_db");
    const auto filePath = uniquePath("zyx_driver_abi_error_handling_file");

    {
        std::ofstream file(filePath);
        file << "not a zyx database";
    }
    EXPECT_NE(zyx_driver_db_open_if_exists(filePath.c_str(), &db, &error), ZYX_DRIVER_OK);
    if (error != nullptr) {
        freeError(error);
    }
    EXPECT_EQ(db, nullptr);

    ASSERT_EQ(zyx_driver_db_open(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
    ASSERT_NE(db, nullptr);

    const char *nullLabel[] = {nullptr};
    expectError(zyx_driver_db_create_node_with_labels(db, nullLabel, 1, nullptr, &id, &error),
                ZYX_DRIVER_INVALID_ARGUMENT,
                error);
    expectError(zyx_driver_db_create_edge(db, 12345, 67890, nullptr, nullptr, &id, &error),
                ZYX_DRIVER_INVALID_ARGUMENT,
                error);
    expectError(zyx_driver_db_execute(db, "THIS IS NOT CYPHER", nullptr, &result, &error),
                ZYX_DRIVER_EXECUTION_ERROR,
                error);
    EXPECT_EQ(result, nullptr);

    EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
    std::error_code ec;
    fs::remove_all(dbPath, ec);
    fs::remove_all(filePath, ec);
}

TEST(DriverAbiErrorHandlingTest, ParamsHelpersAndListInputsCoverRawPaths) {
    zyx_driver_error_t *error = nullptr;
    zyx_driver_params_t *params = nullptr;

    EXPECT_TRUE(paramsToMap(nullptr).empty());
    EXPECT_TRUE(paramsMapOrEmpty(nullptr).empty());
    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_NE(params, nullptr);

    ASSERT_EQ(zyx_driver_params_set_null(params, "nil", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_bool(params, "flag", true, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_int64(params, "age", 42, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_double(params, "score", 98.5, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_string(params, "name", "Ada", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_string_list(params, "empty_strings", nullptr, 0, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_float_list(params, "empty_floats", nullptr, 0, &error), ZYX_DRIVER_OK);

    const char *strings[] = {"graph", nullptr};
    expectError(zyx_driver_params_set_string_list(params, "bad_strings", strings, 2, &error),
                ZYX_DRIVER_INVALID_ARGUMENT,
                error);
    expectError(zyx_driver_params_set_float_list(params, "bad_floats", nullptr, 1, &error),
                ZYX_DRIVER_INVALID_ARGUMENT,
                error);

    EXPECT_FALSE(paramsToMap(params).empty());
    EXPECT_FALSE(paramsMapOrEmpty(params).empty());
    zyx_driver_params_free(params, &error);
}

TEST(DriverAbiErrorHandlingTest, ResultListAccessorsRejectScalarColumns) {
    zyx_driver_error_t *error = nullptr;
    zyx_driver_db_t *db = nullptr;
    zyx_driver_result_t *result = nullptr;
    const auto dbPath = uniquePath("zyx_driver_abi_scalar_list_errors");

    ASSERT_EQ(zyx_driver_db_open(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN 7 AS n", nullptr, &result, &error), ZYX_DRIVER_OK)
            << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    uint32_t count = 0;
    expectError(zyx_driver_result_get_list_count(result, 5, &count, &error), ZYX_DRIVER_OUT_OF_RANGE, error);
    expectError(zyx_driver_result_get_list_count(result, 0, &count, &error), ZYX_DRIVER_TYPE_MISMATCH, error);
    EXPECT_EQ(zyx_driver_result_get_list_value_type(result, 5, 0), ZYX_DRIVER_VALUE_NULL);

    int64_t intValue = 0;
    double doubleValue = 0.0;
    bool boolValue = false;
    const char *stringValue = nullptr;
    expectError(zyx_driver_result_get_list_int64(result, 0, 0, &intValue, &error), ZYX_DRIVER_TYPE_MISMATCH, error);
    expectError(zyx_driver_result_get_list_double(result, 0, 0, &doubleValue, &error), ZYX_DRIVER_TYPE_MISMATCH, error);
    expectError(zyx_driver_result_get_list_bool(result, 0, 0, &boolValue, &error), ZYX_DRIVER_TYPE_MISMATCH, error);
    expectError(zyx_driver_result_get_list_string(result, 0, 0, &stringValue, &error), ZYX_DRIVER_TYPE_MISMATCH, error);

    zyx_driver_result_free(result);
    EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	std::error_code ec;
	fs::remove_all(dbPath, ec);
}

TEST(DriverAbiErrorHandlingTest, ResultAccessorsHandleNullResultArguments) {
	zyx_driver_error_t *error = nullptr;

	EXPECT_EQ(zyx_driver_result_next(nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
	ASSERT_NE(error, nullptr);
	freeError(error);
	EXPECT_EQ(zyx_driver_result_column_count(nullptr), 0u);
	EXPECT_EQ(zyx_driver_result_column_name(nullptr, 0), nullptr);
	EXPECT_EQ(zyx_driver_result_value_type(nullptr, 0), ZYX_DRIVER_VALUE_NULL);
	EXPECT_EQ(zyx_driver_result_get_list_value_type(nullptr, 0, 0), ZYX_DRIVER_VALUE_NULL);

	int64_t intValue = 0;
	double doubleValue = 0.0;
	bool boolValue = false;
	const char *stringValue = nullptr;
	uint32_t count = 0;
	zyx_driver_value_ref_t valueRef{};

	expectError(zyx_driver_result_get_int64(nullptr, 0, &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
	expectError(zyx_driver_result_get_double(nullptr, 0, &doubleValue, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
	expectError(zyx_driver_result_get_bool(nullptr, 0, &boolValue, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
	expectError(zyx_driver_result_get_string(nullptr, 0, &stringValue, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
	expectError(zyx_driver_result_get_list_count(nullptr, 0, &count, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
	expectError(zyx_driver_result_get_value(nullptr, 0, &valueRef, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
	expectError(zyx_driver_result_get_node_id(nullptr, 0, &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
	expectError(zyx_driver_result_get_edge_id(nullptr, 0, &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);
	expectError(zyx_driver_result_get_entity_properties_json(nullptr, 0, &stringValue, &error),
				ZYX_DRIVER_INVALID_ARGUMENT,
				error);
}

TEST(DriverAbiErrorHandlingTest, ResultStringBufferHelperStoresStableStrings) {
	zyx_driver_result_t result;
	std::string &first = storeString(&result, "alpha");
	EXPECT_EQ(first, "alpha");
	std::string &second = storeString(&result, "beta");
	EXPECT_EQ(second, "beta");
	ASSERT_EQ(result.string_buffers.size(), 2u);
	EXPECT_EQ(result.string_buffers.front(), "alpha");
	EXPECT_EQ(result.string_buffers.back(), "beta");
	EXPECT_EQ(typeName(static_cast<zyx_driver_value_type_t>(999)), "unknown");
}

TEST(DriverAbiErrorHandlingTest, TransactionInternalHelpersCoverRawPaths) {
	zyx_driver_error_t *error = nullptr;
	std::runtime_error readOnly("Read-only transaction cannot execute write queries");
    std::runtime_error generic("generic transaction failure");

    EXPECT_EQ(transactionExceptionStatus(readOnly), ZYX_DRIVER_READ_ONLY_VIOLATION);
    EXPECT_EQ(transactionExceptionStatus(generic), ZYX_DRIVER_TRANSACTION_ERROR);

    try {
        throw std::bad_alloc();
    } catch (...) {
        expectError(catchTransactionException(&error), ZYX_DRIVER_OUT_OF_MEMORY, error);
    }

    try {
        throw readOnly;
    } catch (...) {
        expectError(catchTransactionException(&error), ZYX_DRIVER_READ_ONLY_VIOLATION, error);
    }

    try {
        throw generic;
    } catch (...) {
        expectError(catchTransactionException(&error), ZYX_DRIVER_TRANSACTION_ERROR, error);
    }

    try {
        throw 7;
    } catch (...) {
        expectError(catchTransactionException(&error), ZYX_DRIVER_INTERNAL_ERROR, error);
    }

    EXPECT_EQ(resultErrorStatus(zyx::Result{}), ZYX_DRIVER_EXECUTION_ERROR);

    unregisterTransaction(nullptr);

    expectError(validateActiveTransaction(nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT, error);

    zyx_driver_db_t *db = nullptr;
    zyx_driver_txn_t *txn = nullptr;
    const auto dbPath = uniquePath("zyx_driver_abi_txn_internal");
    ASSERT_EQ(zyx_driver_db_open(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(zyx_driver_txn_begin(db, &txn, &error), ZYX_DRIVER_OK);
    ASSERT_NE(txn, nullptr);
    EXPECT_EQ(validateActiveTransaction(txn, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);

    unregisterTransaction(txn);
    EXPECT_TRUE(txn->finalized);
    EXPECT_EQ(txn->owner, nullptr);
    EXPECT_TRUE(db->active_txns.empty());
    expectError(validateActiveTransaction(txn, &error), ZYX_DRIVER_TRANSACTION_ERROR, error);
    unregisterTransaction(txn);

    EXPECT_EQ(zyx_driver_txn_close(txn, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
    std::error_code ec;
    fs::remove_all(dbPath, ec);
}

TEST(DriverAbiErrorHandlingTest, ValueRefInternalHelpersCoverInvalidAndUnregisteredStates) {
    zyx_driver_error_t *error = nullptr;

    registerResultHandle(nullptr);
    unregisterResultHandle(nullptr);
    bumpValueRefGeneration(nullptr);
    EXPECT_EQ(resolveValueRef(nullptr, &error), nullptr);
    ASSERT_NE(error, nullptr);
    freeError(error);
    EXPECT_EQ(makeValueRef(nullptr, 0).owner_id, 0u);

    zyx_driver_result_t result;
    EXPECT_EQ(makeValueRef(&result, 0).owner_id, 0u);
    result.value_ref_owner_id = 42;
    result.value_ref_cookie = 0;
    result.value_ref_generation = 1;
    EXPECT_EQ(makeValueRef(&result, 0).owner_id, 0u);
    result.value_ref_owner_id = 42;
    result.value_ref_cookie = 99;
    result.value_ref_generation = 0;
    EXPECT_EQ(makeValueRef(&result, 0).owner_id, 0u);
    result.value_ref_owner_id = 0;
    result.value_ref_cookie = 0;
    result.value_ref_generation = 1;

    registerResultHandle(&result);
    ASSERT_NE(result.value_ref_owner_id, 0u);
    auto ref = makeValueRef(&result, 0);
    EXPECT_NE(ref.owner_id, 0u);

    zyx_driver_value_ref_t badSlot = ref;
    badSlot.slot = 99;
    EXPECT_EQ(resolveValueRef(&badSlot, &error), nullptr);
    ASSERT_NE(error, nullptr);
    freeError(error);

    zyx_driver_value_ref_t stale = ref;
    stale.generation += 1;
    EXPECT_EQ(resolveValueRefOwner(&stale, &error), nullptr);
    ASSERT_NE(error, nullptr);
    freeError(error);

    unregisterResultHandle(&result);
    EXPECT_EQ(result.value_ref_owner_id, 0u);
    EXPECT_EQ(resolveValueRefOwner(&ref, &error), nullptr);
    ASSERT_NE(error, nullptr);
    freeError(error);

    unregisterResultHandle(&result);

    zyx_driver_result_t wrapping;
    wrapping.value_ref_generation = std::numeric_limits<uint64_t>::max();
    bumpValueRefGeneration(&wrapping);
    EXPECT_EQ(wrapping.value_ref_generation, 1u);
}
