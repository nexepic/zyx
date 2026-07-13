#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <gtest/gtest.h>
#include "zyx/zyx_driver_abi.h"

namespace fs = std::filesystem;

class DriverAbiLifecycleTest : public ::testing::Test {
protected:
    std::string dbPath;

    void SetUp() override {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        dbPath = (fs::temp_directory_path() / ("zyx_driver_abi_lifecycle_" + std::to_string(now) + "_" +
                                             std::to_string(std::rand())))
                     .string();
        cleanup();
    }

    void TearDown() override {
        cleanup();
    }

    void cleanup() {
        std::error_code ec;
        fs::remove_all(dbPath, ec);
        fs::remove(dbPath + "-wal", ec);
    }
};

TEST_F(DriverAbiLifecycleTest, VersionIsStableV1) {
    EXPECT_EQ(zyx_driver_abi_version_major(), 1u);
    EXPECT_EQ(zyx_driver_abi_version_minor(), 1u);
    EXPECT_GE(zyx_driver_abi_version_patch(), 0u);
    ASSERT_NE(zyx_driver_runtime_version(), nullptr);
    EXPECT_NE(std::string(zyx_driver_runtime_version()).empty(), true);
}

TEST_F(DriverAbiLifecycleTest, OpenCloseDatabase) {
    zyx_driver_error_t *error = nullptr;
    zyx_driver_db_t *db = nullptr;

    zyx_driver_status_t status = zyx_driver_db_open(dbPath.c_str(), &db, &error);

    EXPECT_EQ(status, ZYX_DRIVER_OK);
    EXPECT_NE(db, nullptr);
    EXPECT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiLifecycleTest, OpenRejectsNullPathWithStructuredError) {
    zyx_driver_error_t *error = nullptr;
    zyx_driver_db_t *db = nullptr;

    zyx_driver_status_t status = zyx_driver_db_open(nullptr, &db, &error);

    EXPECT_EQ(status, ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(db, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_NE(std::string(zyx_driver_error_message(error)).find("path"), std::string::npos);
    zyx_driver_error_free(error);
}

TEST_F(DriverAbiLifecycleTest, OpenRejectsNullOutputPointerWithStructuredError) {
    zyx_driver_error_t *error = nullptr;

    zyx_driver_status_t status = zyx_driver_db_open(dbPath.c_str(), nullptr, &error);

    EXPECT_EQ(status, ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_NE(std::string(zyx_driver_error_message(error)).find("out"), std::string::npos);
    zyx_driver_error_free(error);
}

TEST_F(DriverAbiLifecycleTest, CloseNullDatabaseIsNoOp) {
    zyx_driver_error_t *error = nullptr;

    EXPECT_EQ(zyx_driver_db_close(nullptr, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiLifecycleTest, SaveFlushesOpenDatabase) {
    zyx_driver_error_t *error = nullptr;
    zyx_driver_db_t *db = nullptr;

    ASSERT_EQ(zyx_driver_db_open(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_db_save(db, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiLifecycleTest, ActiveTransactionStateReflectsOpenTransaction) {
    zyx_driver_error_t *error = nullptr;
    zyx_driver_db_t *db = nullptr;
    zyx_driver_txn_t *txn = nullptr;
    bool hasActiveTransaction = true;

    ASSERT_EQ(zyx_driver_db_open(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_db_has_active_transaction(db, &hasActiveTransaction, &error), ZYX_DRIVER_OK);
    EXPECT_FALSE(hasActiveTransaction);
    EXPECT_EQ(error, nullptr);

    ASSERT_EQ(zyx_driver_txn_begin(db, &txn, &error), ZYX_DRIVER_OK);
    ASSERT_NE(txn, nullptr);
    ASSERT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_db_has_active_transaction(db, &hasActiveTransaction, &error), ZYX_DRIVER_OK);
    EXPECT_TRUE(hasActiveTransaction);
    EXPECT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_txn_rollback(txn, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);
    EXPECT_EQ(zyx_driver_txn_close(txn, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_db_has_active_transaction(db, &hasActiveTransaction, &error), ZYX_DRIVER_OK);
    EXPECT_FALSE(hasActiveTransaction);
    EXPECT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiLifecycleTest, SetThreadPoolSizeValidatesInputs) {
    zyx_driver_error_t *error = nullptr;
    zyx_driver_db_t *db = nullptr;

    EXPECT_EQ(zyx_driver_db_set_thread_pool_size(nullptr, 1, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_NE(std::string(zyx_driver_error_message(error)).find("db"), std::string::npos);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_db_save(nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_NE(std::string(zyx_driver_error_message(error)).find("db"), std::string::npos);
    zyx_driver_error_free(error);
    error = nullptr;

    bool hasActiveTransaction = true;
    EXPECT_EQ(zyx_driver_db_has_active_transaction(nullptr, &hasActiveTransaction, &error),
              ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_NE(std::string(zyx_driver_error_message(error)).find("db"), std::string::npos);
    zyx_driver_error_free(error);
    error = nullptr;

    ASSERT_EQ(zyx_driver_db_open(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_db_set_thread_pool_size(db, 1, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_db_has_active_transaction(db, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_NE(std::string(zyx_driver_error_message(error)).find("out_value"), std::string::npos);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiLifecycleTest, NullErrorHelpersAreSafe) {
    EXPECT_EQ(zyx_driver_error_code(nullptr), ZYX_DRIVER_OK);
    EXPECT_STREQ(zyx_driver_error_message(nullptr), "");
    zyx_driver_error_free(nullptr);
}

TEST_F(DriverAbiLifecycleTest, OpenIfExistsReturnsNotFound) {
    zyx_driver_error_t *error = nullptr;
    zyx_driver_db_t *db = nullptr;

    zyx_driver_status_t status = zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error);

    EXPECT_EQ(status, ZYX_DRIVER_NOT_FOUND);
    EXPECT_EQ(db, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_NOT_FOUND);
    zyx_driver_error_free(error);
}

TEST_F(DriverAbiLifecycleTest, OpenIfExistsSucceedsForExistingDatabase) {
    zyx_driver_error_t *error = nullptr;
    zyx_driver_db_t *db = nullptr;

    ASSERT_EQ(zyx_driver_db_open(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, nullptr);
    ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(error, nullptr);
    db = nullptr;

    EXPECT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
    EXPECT_NE(db, nullptr);
    EXPECT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);
}
