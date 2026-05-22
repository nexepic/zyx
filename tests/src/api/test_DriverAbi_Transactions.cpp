#include <cstdlib>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "zyx/zyx_driver_abi.h"

namespace fs = std::filesystem;

class DriverAbiTransactionsTest : public ::testing::Test {
protected:
    std::string dbPath;
    zyx_driver_db_t *db = nullptr;
    zyx_driver_error_t *error = nullptr;

    void SetUp() override {
        dbPath = (fs::temp_directory_path() / ("zyx_driver_abi_transactions_" + std::to_string(std::rand()))).string();
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

    int64_t countPeopleNamed(const char *name) {
        zyx_driver_params_t *params = nullptr;
        zyx_driver_result_t *result = nullptr;
        int64_t count = -1;

        EXPECT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
        EXPECT_EQ(zyx_driver_params_set_string(params, "name", name, &error), ZYX_DRIVER_OK);
        EXPECT_EQ(zyx_driver_db_execute(db, "MATCH (n:Person {name:$name}) RETURN count(n) AS count", params, &result,
                                        &error),
                  ZYX_DRIVER_OK);
        if (result != nullptr && zyx_driver_result_next(result, &error) == ZYX_DRIVER_ROW) {
            EXPECT_EQ(zyx_driver_result_get_int64(result, 0, &count, &error), ZYX_DRIVER_OK);
        }

        zyx_driver_result_free(result);
        zyx_driver_params_free(params, &error);
        return count;
    }
};

TEST_F(DriverAbiTransactionsTest, CommitPersistsWritesExecutedWithParams) {
    zyx_driver_txn_t *txn = nullptr;
    zyx_driver_params_t *params = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_txn_begin(db, &txn, &error), ZYX_DRIVER_OK);
    ASSERT_NE(txn, nullptr);
    ASSERT_EQ(error, nullptr);
    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_string(params, "name", "Ada", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_int64(params, "age", 37, &error), ZYX_DRIVER_OK);

    EXPECT_EQ(zyx_driver_txn_execute(txn, "CREATE (:Person {name:$name, age:$age})", params, &result, &error),
              ZYX_DRIVER_OK);
    EXPECT_NE(result, nullptr);
    EXPECT_EQ(error, nullptr);
    zyx_driver_result_free(result);
    result = nullptr;

    EXPECT_EQ(zyx_driver_txn_commit(txn, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);
    EXPECT_EQ(countPeopleNamed("Ada"), 1);

    zyx_driver_params_free(params, &error);
    EXPECT_EQ(zyx_driver_txn_close(txn, &error), ZYX_DRIVER_OK);
}

TEST_F(DriverAbiTransactionsTest, RollbackDiscardsWrites) {
    zyx_driver_txn_t *txn = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_txn_begin(db, &txn, &error), ZYX_DRIVER_OK);
    ASSERT_NE(txn, nullptr);
    ASSERT_EQ(zyx_driver_txn_execute(txn, "CREATE (:Person {name:'Grace'})", nullptr, &result, &error), ZYX_DRIVER_OK);
    zyx_driver_result_free(result);
    result = nullptr;

    EXPECT_EQ(zyx_driver_txn_rollback(txn, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);
    EXPECT_EQ(countPeopleNamed("Grace"), 0);

    EXPECT_EQ(zyx_driver_txn_close(txn, &error), ZYX_DRIVER_OK);
}

TEST_F(DriverAbiTransactionsTest, CloseWithoutCommitDoesNotPersistWrites) {
    zyx_driver_txn_t *txn = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_txn_begin(db, &txn, &error), ZYX_DRIVER_OK);
    ASSERT_NE(txn, nullptr);
    ASSERT_EQ(zyx_driver_txn_execute(txn, "CREATE (:Person {name:'Linus'})", nullptr, &result, &error), ZYX_DRIVER_OK);
    zyx_driver_result_free(result);
    result = nullptr;

    EXPECT_EQ(zyx_driver_txn_close(txn, &error), ZYX_DRIVER_OK);
    txn = nullptr;
    EXPECT_EQ(error, nullptr);
    EXPECT_EQ(countPeopleNamed("Linus"), 0);
}

TEST_F(DriverAbiTransactionsTest, ReadOnlyTransactionRejectsWritesAndAllowsReads) {
    zyx_driver_result_t *result = nullptr;
    ASSERT_EQ(zyx_driver_db_execute(db, "CREATE (:Person {name:'ReadOnlySeed'})", nullptr, &result, &error),
              ZYX_DRIVER_OK);
    zyx_driver_result_free(result);
    result = nullptr;

    zyx_driver_txn_t *txn = nullptr;
    ASSERT_EQ(zyx_driver_txn_begin_read_only(db, &txn, &error), ZYX_DRIVER_OK);
    ASSERT_NE(txn, nullptr);
    ASSERT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_txn_execute(txn, "MATCH (n:Person) RETURN count(n) AS count", nullptr, &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    int64_t count = 0;
    EXPECT_EQ(zyx_driver_result_get_int64(result, 0, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 1);
    zyx_driver_result_free(result);
    result = nullptr;

    EXPECT_EQ(zyx_driver_txn_execute(txn, "CREATE (:Person {name:'Blocked'})", nullptr, &result, &error),
              ZYX_DRIVER_READ_ONLY_VIOLATION);
    EXPECT_EQ(result, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_READ_ONLY_VIOLATION);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_txn_rollback(txn, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_txn_close(txn, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(countPeopleNamed("Blocked"), 0);
}

TEST_F(DriverAbiTransactionsTest, TransactionErrorsPreserveStatusWithoutErrorOut) {
    zyx_driver_txn_t *txn = nullptr;
    zyx_driver_result_t *result = reinterpret_cast<zyx_driver_result_t *>(0x1);

    EXPECT_EQ(zyx_driver_txn_begin(nullptr, &txn, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(txn, nullptr);
    EXPECT_EQ(zyx_driver_txn_begin(db, nullptr, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_txn_execute(nullptr, "RETURN 1", nullptr, &result, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(result, nullptr);

    ASSERT_EQ(zyx_driver_txn_begin_read_only(db, &txn, &error), ZYX_DRIVER_OK);
    ASSERT_NE(txn, nullptr);
    result = nullptr;
    EXPECT_EQ(zyx_driver_txn_execute(txn, "CREATE (:Person {name:'BlockedNoError'})", nullptr, &result, nullptr),
              ZYX_DRIVER_READ_ONLY_VIOLATION);
    EXPECT_EQ(result, nullptr);
    EXPECT_EQ(zyx_driver_txn_close(txn, &error), ZYX_DRIVER_OK);
}
