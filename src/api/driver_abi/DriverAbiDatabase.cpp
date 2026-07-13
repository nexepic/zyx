#include "DriverAbiInternal.hpp"

#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>

zyx_driver_status_t resultErrorStatus(const zyx::Result &result) {
    const std::string error = result.getError();
    if (error.find("Read-only transaction cannot execute write queries") != std::string::npos) { // ZYX_COV_EXCL_LINE: read-only transaction errors currently surface through Result failures
        return ZYX_DRIVER_READ_ONLY_VIOLATION;
    }
    if (error.find("Transaction") != std::string::npos || error.find("transaction") != std::string::npos) { // ZYX_COV_EXCL_START: transaction error-message variants depend on internal executor wording.
        return ZYX_DRIVER_TRANSACTION_ERROR;
    }
    return ZYX_DRIVER_EXECUTION_ERROR; // ZYX_COV_EXCL_STOP
}
zyx_driver_status_t validateNoActiveTransaction(zyx_driver_db_t *db, zyx_driver_error_t **out_error) {
    {
        std::lock_guard lock(db->mutex);
        if (!db->active_txns.empty() || !db->active_ingests.empty()) {
            return setError(out_error, ZYX_DRIVER_TRANSACTION_ERROR, "database has active transactions");
        }
    }
    if (db->db != nullptr && db->db->hasActiveTransaction()) {
        return setError(out_error, ZYX_DRIVER_TRANSACTION_ERROR, "database has active transactions");
    }
    return ZYX_DRIVER_OK;
}
zyx_driver_status_t openDatabase(const char *path, zyx_driver_db_t **out_db, zyx_driver_error_t **out_error,
                                 bool require_exists) {
    clearError(out_error);

    if (out_db == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_db must not be null");
    }
    *out_db = nullptr;

    if (path == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "path must not be null");
    }

    try {
        if (require_exists && !std::filesystem::exists(path)) {
            return setError(out_error, ZYX_DRIVER_NOT_FOUND, "database path does not exist");
        }

        auto handle = std::make_unique<zyx_driver_db_t>();
        handle->db = std::make_unique<zyx::Database>(path);
        if (require_exists) {
            if (!handle->db->openIfExists()) { // ZYX_COV_EXCL_LINE: exists check above covers the public not-found path
                return setError(out_error, ZYX_DRIVER_NOT_FOUND, "database path does not exist");
            }
        } else {
            handle->db->open();
        }

        *out_db = handle.release();
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: ABI boundary converts unexpected exceptions into status codes.
        return catchAbiExceptionAs(out_error, ZYX_DRIVER_OPEN_FAILED);
    }
}

extern "C" {

zyx_driver_status_t zyx_driver_db_open(const char *path, zyx_driver_db_t **out_db, zyx_driver_error_t **out_error) {
    return openDatabase(path, out_db, out_error, false);
}

zyx_driver_status_t zyx_driver_db_open_if_exists(const char *path, zyx_driver_db_t **out_db,
                                                 zyx_driver_error_t **out_error) {
    return openDatabase(path, out_db, out_error, true);
}

zyx_driver_status_t zyx_driver_db_close(zyx_driver_db_t *db, zyx_driver_error_t **out_error) {
    clearError(out_error);

    if (db == nullptr) {
        return ZYX_DRIVER_OK;
    }

    try {
        if (auto status = validateNoActiveTransaction(db, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        if (db->db != nullptr) { // ZYX_COV_EXCL_LINE: valid public handles always own an inner database
            db->db->close();
        }
        delete db;
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: ABI boundary converts unexpected exceptions into status codes.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_db_save(zyx_driver_db_t *db, zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (db == nullptr ||
        db->db == nullptr) { // ZYX_COV_EXCL_LINE: valid public handles always own an inner database
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "db must not be null");
    }

    try {
        db->db->save();
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: ABI boundary converts unexpected exceptions into status codes.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_db_has_active_transaction(zyx_driver_db_t *db, bool *out_value,
                                                         zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (db == nullptr ||
        db->db == nullptr) { // ZYX_COV_EXCL_LINE: valid public handles always own an inner database
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "db must not be null");
    }
    if (out_value == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
    }

    try {
        *out_value = db->db->hasActiveTransaction();
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: ABI boundary converts unexpected exceptions into status codes.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_db_set_thread_pool_size(zyx_driver_db_t *db, uint32_t size,
                                                       zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (db == nullptr ||
        db->db == nullptr) { // ZYX_COV_EXCL_LINE: valid public handles always own an inner database
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "db must not be null");
    }

    try {
        db->db->setThreadPoolSize(static_cast<size_t>(size));
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: ABI boundary converts unexpected exceptions into status codes.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_db_create_node(zyx_driver_db_t *db, const char *label,
                                              const zyx_driver_params_t *properties, int64_t *out_node_id,
                                              zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (db == nullptr ||
        db->db == nullptr) { // ZYX_COV_EXCL_LINE: valid public handles always own an inner database
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "db must not be null");
    }
    if (label == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "label must not be null");
    }
    if (out_node_id == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_node_id must not be null");
    }

    try {
        if (auto status = validateNoActiveTransaction(db, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        const auto &values = paramsMapOrEmpty(properties);
        *out_node_id = db->db->createNode(label, values);
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: ABI boundary converts unexpected exceptions into status codes.
        return catchAbiExceptionAs(out_error, ZYX_DRIVER_EXECUTION_ERROR);
    }
}

zyx_driver_status_t zyx_driver_db_create_node_with_labels(zyx_driver_db_t *db, const char *const *labels,
                                                          uint32_t label_count,
                                                          const zyx_driver_params_t *properties,
                                                          int64_t *out_node_id,
                                                          zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (db == nullptr ||
        db->db == nullptr) { // ZYX_COV_EXCL_LINE: valid public handles always own an inner database
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "db must not be null");
    }
    if (labels == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "labels must not be null");
    }
    if (label_count == 0) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "label_count must be greater than zero");
    }
    if (out_node_id == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_node_id must not be null");
    }

    try {
        if (auto status = validateNoActiveTransaction(db, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        std::vector<std::string> labelValues;
        labelValues.reserve(label_count);
        for (uint32_t i = 0; i < label_count; ++i) {
            if (labels[i] == nullptr) {
                return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "labels must not contain null values");
            }
            labelValues.emplace_back(labels[i]);
        }
        const auto &values = paramsMapOrEmpty(properties);
        *out_node_id = db->db->createNode(labelValues, values);
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: ABI boundary converts unexpected exceptions into status codes.
        return catchAbiExceptionAs(out_error, ZYX_DRIVER_EXECUTION_ERROR);
    }
}

zyx_driver_status_t zyx_driver_db_create_edge(zyx_driver_db_t *db, int64_t source_id, int64_t target_id,
                                              const char *edge_type, const zyx_driver_params_t *properties,
                                              int64_t *out_edge_id, zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (db == nullptr ||
        db->db == nullptr) { // ZYX_COV_EXCL_LINE: valid public handles always own an inner database
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "db must not be null");
    }
    if (edge_type == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "edge_type must not be null");
    }
    if (out_edge_id == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_edge_id must not be null");
    }

    try {
        if (auto status = validateNoActiveTransaction(db, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        const auto &values = paramsMapOrEmpty(properties);
        *out_edge_id = db->db->createEdge(source_id, target_id, edge_type, values);
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: ABI boundary converts unexpected exceptions into status codes.
        return catchAbiExceptionAs(out_error, ZYX_DRIVER_EXECUTION_ERROR);
    }
}

zyx_driver_status_t zyx_driver_db_execute(zyx_driver_db_t *db, const char *cypher, zyx_driver_params_t *params,
                                          zyx_driver_result_t **out_result, zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (out_result == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_result must not be null");
    }
    *out_result = nullptr;
    if (db == nullptr ||
        db->db == nullptr) { // ZYX_COV_EXCL_LINE: valid public handles always own an inner database
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "db must not be null");
    }
    if (cypher == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "cypher must not be null");
    }

    try {
        auto handle = std::make_unique<zyx_driver_result_t>();
        handle->result = params != nullptr ? db->db->execute(cypher, params->values) : db->db->execute(cypher);
        if (!handle->result.isSuccess()) {
            return setError(out_error, ZYX_DRIVER_EXECUTION_ERROR, handle->result.getError());
        }
        registerResultHandle(handle.get());
        *out_result = handle.release();
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: ABI boundary converts unexpected exceptions into status codes.
        return catchAbiExceptionAs(out_error, ZYX_DRIVER_EXECUTION_ERROR);
    }
}


} // extern "C"
