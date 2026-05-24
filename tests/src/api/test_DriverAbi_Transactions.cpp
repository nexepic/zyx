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
    return (fs::temp_directory_path() / ("zyx_driver_abi_transactions_" + std::to_string(now) + "_" +
                                        std::to_string(std::rand()) + "_" + std::to_string(sequence)))
        .string();
}

} // namespace

class DriverAbiTransactionsTest : public ::testing::Test {
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


TEST_F(DriverAbiTransactionsTest, DbCloseRejectsActiveTransactionAndSucceedsAfterRollbackClose) {
    zyx_driver_txn_t *txn = nullptr;
    ASSERT_EQ(zyx_driver_txn_begin(db, &txn, &error), ZYX_DRIVER_OK);
    ASSERT_NE(txn, nullptr);

    EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_TRANSACTION_ERROR);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_TRANSACTION_ERROR);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_txn_rollback(txn, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_txn_close(txn, &error), ZYX_DRIVER_OK);
    txn = nullptr;

    EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);
    db = nullptr;
}

TEST_F(DriverAbiTransactionsTest, DbCloseActiveTransactionPreservesStatusWithoutErrorOut) {
    zyx_driver_txn_t *txn = nullptr;
    ASSERT_EQ(zyx_driver_txn_begin(db, &txn, &error), ZYX_DRIVER_OK);
    ASSERT_NE(txn, nullptr);

    EXPECT_EQ(zyx_driver_db_close(db, nullptr), ZYX_DRIVER_TRANSACTION_ERROR);

    EXPECT_EQ(zyx_driver_txn_rollback(txn, nullptr), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_txn_close(txn, nullptr), ZYX_DRIVER_OK);
    txn = nullptr;

    EXPECT_EQ(zyx_driver_db_close(db, nullptr), ZYX_DRIVER_OK);
    db = nullptr;
}

TEST_F(DriverAbiTransactionsTest, DirectGraphCreationRejectsActiveTransaction) {
    zyx_driver_txn_t *txn = nullptr;
    int64_t nodeId = 0;
    ASSERT_EQ(zyx_driver_txn_begin(db, &txn, &error), ZYX_DRIVER_OK);
    ASSERT_NE(txn, nullptr);

    EXPECT_EQ(zyx_driver_db_create_node(db, "Person", nullptr, &nodeId, &error), ZYX_DRIVER_TRANSACTION_ERROR);
    EXPECT_EQ(nodeId, 0);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_TRANSACTION_ERROR);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_txn_rollback(txn, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_txn_close(txn, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(countPeopleNamed("Person"), 0);
}

TEST_F(DriverAbiTransactionsTest, DirectGraphCreationRejectsCypherBeginTransaction) {
    zyx_driver_result_t *result = nullptr;
    int64_t nodeId = 0;

    ASSERT_EQ(zyx_driver_db_execute(db, "BEGIN", nullptr, &result, &error), ZYX_DRIVER_OK);
    zyx_driver_result_free(result);
    result = nullptr;

    EXPECT_EQ(zyx_driver_db_create_node(db, "Person", nullptr, &nodeId, &error), ZYX_DRIVER_TRANSACTION_ERROR);
    EXPECT_EQ(nodeId, 0);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_TRANSACTION_ERROR);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_db_execute(db, "ROLLBACK", nullptr, &result, &error), ZYX_DRIVER_OK);
    zyx_driver_result_free(result);
    EXPECT_EQ(countPeopleNamed("Person"), 0);
}

TEST_F(DriverAbiTransactionsTest, DirectEdgeCreationRejectsCypherBeginTransaction) {
    int64_t sourceId = 0;
    int64_t targetId = 0;
    int64_t edgeId = 0;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_db_create_node(db, "Source", nullptr, &sourceId, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_db_create_node(db, "Target", nullptr, &targetId, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_db_execute(db, "BEGIN", nullptr, &result, &error), ZYX_DRIVER_OK);
    zyx_driver_result_free(result);
    result = nullptr;

    EXPECT_EQ(zyx_driver_db_create_edge(db, sourceId, targetId, "REL", nullptr, &edgeId, &error),
              ZYX_DRIVER_TRANSACTION_ERROR);
    EXPECT_EQ(edgeId, 0);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_TRANSACTION_ERROR);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_db_execute(db, "ROLLBACK", nullptr, &result, &error), ZYX_DRIVER_OK);
    zyx_driver_result_free(result);
}

TEST_F(DriverAbiTransactionsTest, DbCloseRejectsCypherBeginTransaction) {
    zyx_driver_result_t *result = nullptr;
    ASSERT_EQ(zyx_driver_db_execute(db, "BEGIN", nullptr, &result, &error), ZYX_DRIVER_OK);
    zyx_driver_result_free(result);
    result = nullptr;

    const auto closeStatus = zyx_driver_db_close(db, &error);
    EXPECT_EQ(closeStatus, ZYX_DRIVER_TRANSACTION_ERROR);
    if (closeStatus == ZYX_DRIVER_OK) {
        db = nullptr;
        return;
    }
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_TRANSACTION_ERROR);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_db_execute(db, "ROLLBACK", nullptr, &result, &error), ZYX_DRIVER_OK);
    zyx_driver_result_free(result);
}

TEST_F(DriverAbiTransactionsTest, CommitFinalizedTransactionRejectsRepeatedOperations) {
    zyx_driver_txn_t *txn = nullptr;
    zyx_driver_result_t *result = nullptr;
    ASSERT_EQ(zyx_driver_txn_begin(db, &txn, &error), ZYX_DRIVER_OK);
    ASSERT_NE(txn, nullptr);

    EXPECT_EQ(zyx_driver_txn_commit(txn, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_txn_execute(txn, "RETURN 1", nullptr, &result, &error), ZYX_DRIVER_TRANSACTION_ERROR);
    EXPECT_EQ(result, nullptr);
    zyx_driver_error_free(error);
    error = nullptr;
    EXPECT_EQ(zyx_driver_txn_commit(txn, nullptr), ZYX_DRIVER_TRANSACTION_ERROR);
    EXPECT_EQ(zyx_driver_txn_rollback(txn, nullptr), ZYX_DRIVER_TRANSACTION_ERROR);
    EXPECT_EQ(zyx_driver_txn_close(txn, &error), ZYX_DRIVER_OK);
}

TEST_F(DriverAbiTransactionsTest, RollbackFinalizedTransactionRejectsRepeatedOperations) {
    zyx_driver_txn_t *txn = nullptr;
    zyx_driver_result_t *result = nullptr;
    ASSERT_EQ(zyx_driver_txn_begin(db, &txn, &error), ZYX_DRIVER_OK);
    ASSERT_NE(txn, nullptr);

    EXPECT_EQ(zyx_driver_txn_rollback(txn, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_txn_rollback(txn, &error), ZYX_DRIVER_TRANSACTION_ERROR);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_TRANSACTION_ERROR);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_txn_execute(txn, "RETURN 1", nullptr, &result, &error), ZYX_DRIVER_TRANSACTION_ERROR);
    EXPECT_EQ(result, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_TRANSACTION_ERROR);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_txn_close(txn, &error), ZYX_DRIVER_OK);
}


TEST_F(DriverAbiTransactionsTest, TransactionValidationFailuresSetErrors) {
    zyx_driver_txn_t *txn = nullptr;
    zyx_driver_result_t *result = nullptr;
    struct Cleanup {
        zyx_driver_txn_t *&txn;
        zyx_driver_error_t *&error;
        ~Cleanup() {
            zyx_driver_txn_close(txn, &error);
            txn = nullptr;
        }
    } cleanup{txn, error};

    EXPECT_EQ(zyx_driver_txn_begin_read_only(nullptr, &txn, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(txn, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_txn_begin_read_only(db, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    ASSERT_EQ(zyx_driver_txn_begin(db, &txn, &error), ZYX_DRIVER_OK);
    ASSERT_NE(txn, nullptr);

    EXPECT_EQ(zyx_driver_txn_execute(txn, "RETURN 1", nullptr, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_txn_execute(txn, nullptr, nullptr, &result, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(result, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_txn_rollback(txn, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_txn_close(txn, &error), ZYX_DRIVER_OK);
    txn = nullptr;
}

TEST_F(DriverAbiTransactionsTest, TransactionErrorsPreserveStatusWithoutErrorOut) {
    zyx_driver_txn_t *txn = nullptr;
    zyx_driver_result_t *result = reinterpret_cast<zyx_driver_result_t *>(0x1);

    EXPECT_EQ(zyx_driver_txn_begin(nullptr, &txn, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(txn, nullptr);
    EXPECT_EQ(zyx_driver_txn_begin(db, nullptr, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_txn_begin_read_only(nullptr, &txn, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(txn, nullptr);
    EXPECT_EQ(zyx_driver_txn_begin_read_only(db, nullptr, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_txn_execute(nullptr, "RETURN 1", nullptr, &result, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(result, nullptr);
    EXPECT_EQ(zyx_driver_txn_close(nullptr, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);

    ASSERT_EQ(zyx_driver_txn_begin_read_only(db, &txn, &error), ZYX_DRIVER_OK);
    ASSERT_NE(txn, nullptr);
    result = nullptr;
    EXPECT_EQ(zyx_driver_txn_execute(txn, "CREATE (:Person {name:'BlockedNoError'})", nullptr, &result, nullptr),
              ZYX_DRIVER_READ_ONLY_VIOLATION);
    EXPECT_EQ(result, nullptr);
    EXPECT_EQ(zyx_driver_txn_close(txn, &error), ZYX_DRIVER_OK);
}
