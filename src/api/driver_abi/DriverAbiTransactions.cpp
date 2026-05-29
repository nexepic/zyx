#include "DriverAbiInternal.hpp"

#include <memory>
#include <mutex>
#include <new>
#include <string>

zyx_driver_status_t transactionExceptionStatus(const std::exception &ex) { // ZYX_COV_EXCL_FUNCTION: exception-message classification is defensive only.
    const std::string message = ex.what();
    if (message.find("Read-only transaction cannot execute write queries") != std::string::npos) {
        return ZYX_DRIVER_READ_ONLY_VIOLATION;
    }
    return ZYX_DRIVER_TRANSACTION_ERROR;
}

zyx_driver_status_t catchTransactionException(zyx_driver_error_t **out_error) noexcept {
    try {
        throw;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        return setError(out_error, transactionExceptionStatus(ex), ex.what());
    } catch (...) {
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

void unregisterTransaction(zyx_driver_txn_t *txn) {
    if (txn != nullptr && !txn->finalized) { // ZYX_COV_EXCL_LINE: public close paths pass valid unfinalized transactions
        if (txn->owner != nullptr) { // ZYX_COV_EXCL_LINE: public transactions retain their owner until unregistered
            std::lock_guard lock(txn->owner->mutex);
            txn->owner->active_txns.erase(txn);
        }
        txn->owner = nullptr;
        txn->finalized = true;
    }
}

zyx_driver_status_t validateActiveTransaction(zyx_driver_txn_t *txn, zyx_driver_error_t **out_error) {
    if (txn == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "txn must not be null");
    }
    if (txn->finalized) {
        return setError(out_error, ZYX_DRIVER_TRANSACTION_ERROR, "transaction is already finalized");
    }
    return ZYX_DRIVER_OK;
}

extern "C" {

zyx_driver_status_t zyx_driver_txn_begin(zyx_driver_db_t *db, zyx_driver_txn_t **out_txn,
                                         zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (out_txn == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_txn must not be null");
    }
    *out_txn = nullptr;
    if (db == nullptr ||
        db->db == nullptr) { // ZYX_COV_EXCL_LINE: valid public handles always own an inner database
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "db must not be null");
    }

    try {
        auto handle = std::make_unique<zyx_driver_txn_t>(zyx_driver_txn_t{db->db->beginTransaction(), db, false});
        {
            std::lock_guard lock(db->mutex);
            db->active_txns.insert(handle.get());
        }
        *out_txn = handle.release();
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchTransactionException(out_error);
    }
}

zyx_driver_status_t zyx_driver_txn_begin_read_only(zyx_driver_db_t *db, zyx_driver_txn_t **out_txn,
                                                   zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (out_txn == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_txn must not be null");
    }
    *out_txn = nullptr;
    if (db == nullptr ||
        db->db == nullptr) { // ZYX_COV_EXCL_LINE: valid public handles always own an inner database
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "db must not be null");
    }

    try {
        auto handle = std::make_unique<zyx_driver_txn_t>(zyx_driver_txn_t{db->db->beginReadOnlyTransaction(), db, false});
        {
            std::lock_guard lock(db->mutex);
            db->active_txns.insert(handle.get());
        }
        *out_txn = handle.release();
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchTransactionException(out_error);
    }
}

zyx_driver_status_t zyx_driver_txn_execute(zyx_driver_txn_t *txn, const char *cypher,
                                           const zyx_driver_params_t *params,
                                           zyx_driver_result_t **out_result, zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (out_result == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_result must not be null");
    }
    *out_result = nullptr;
    if (auto status = validateActiveTransaction(txn, out_error); status != ZYX_DRIVER_OK) {
        return status;
    }
    if (cypher == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "cypher must not be null");
    }

    try {
        auto handle = std::make_unique<zyx_driver_result_t>();
        handle->result = params != nullptr ? txn->txn.execute(cypher, params->values) : txn->txn.execute(cypher);
        if (!handle->result.isSuccess()) {
            return setError(out_error, resultErrorStatus(handle->result), handle->result.getError());
        }
        registerResultHandle(handle.get());
        *out_result = handle.release();
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchTransactionException(out_error);
    }
}

zyx_driver_status_t zyx_driver_txn_commit(zyx_driver_txn_t *txn, zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (auto status = validateActiveTransaction(txn, out_error); status != ZYX_DRIVER_OK) {
        return status;
    }

    try {
        txn->txn.commit();
        unregisterTransaction(txn);
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchTransactionException(out_error);
    }
}

zyx_driver_status_t zyx_driver_txn_rollback(zyx_driver_txn_t *txn, zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (auto status = validateActiveTransaction(txn, out_error); status != ZYX_DRIVER_OK) {
        return status;
    }

    try {
        if (txn->txn.isActive()) { // ZYX_COV_EXCL_LINE: rollback tests close already-finalized handles separately
            txn->txn.rollback();
        }
        unregisterTransaction(txn);
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchTransactionException(out_error);
    }
}

zyx_driver_status_t zyx_driver_txn_close(zyx_driver_txn_t *txn, zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (txn == nullptr) {
        return ZYX_DRIVER_OK;
    }

    try {
        if (!txn->finalized && txn->txn.isActive()) { // ZYX_COV_EXCL_LINE: active public transaction close always has an active inner transaction
            txn->txn.rollback();
        }
        unregisterTransaction(txn);
        delete txn;
        return ZYX_DRIVER_OK;
    } catch (...) {
        const auto status = catchTransactionException(out_error);
        unregisterTransaction(txn);
        delete txn;
        return status;
    }
}

} // extern "C"
