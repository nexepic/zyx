#pragma once

#include "zyx/zyx.hpp"
#include "zyx/zyx_driver_abi.h"

#include <atomic>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct zyx_driver_error_t {
    zyx_driver_status_t code;
    std::string message;
    const char *fallback_message = nullptr;
    bool static_storage = false;
    int64_t row_index = -1;
    std::string field_path;
};

struct zyx_driver_db_t {
    std::unique_ptr<zyx::Database> db;
    std::mutex mutex;
    std::unordered_set<zyx_driver_txn_t *> active_txns;
    std::unordered_set<zyx_driver_ingest_t *> active_ingests;
};

struct zyx_driver_txn_t {
    zyx::Transaction txn;
    zyx_driver_db_t *owner;
    bool finalized;
};

struct zyx_driver_result_t {
    zyx::Result result;
    std::deque<std::string> string_buffers;
    mutable std::deque<zyx::Value> value_buffers;
    uint64_t value_ref_owner_id = 0;
    uint64_t value_ref_cookie = 0;
    mutable uint64_t value_ref_generation = 1;
};

struct zyx_driver_params_t {
    std::unordered_map<std::string, zyx::Value> values;
};

struct zyx_driver_value_t {
    zyx::Value value;
};

void clearError(zyx_driver_error_t **out_error);
zyx_driver_status_t setError(zyx_driver_error_t **out_error, zyx_driver_status_t code, const char *message) noexcept;
zyx_driver_status_t setError(zyx_driver_error_t **out_error, zyx_driver_status_t code, std::string message) noexcept;
zyx_driver_status_t setErrorAt(zyx_driver_error_t **out_error,
                               zyx_driver_status_t code,
                               std::string message,
                               int64_t row_index,
                               std::string field_path) noexcept;
zyx_driver_status_t catchAbiException(zyx_driver_error_t **out_error) noexcept;
zyx_driver_status_t catchAbiExceptionAs(zyx_driver_error_t **out_error,
                                        zyx_driver_status_t exception_status) noexcept;
zyx_driver_status_t validateColumn(const zyx_driver_result_t *result, uint32_t column, zyx_driver_error_t **out_error);
zyx::Value resultValue(const zyx_driver_result_t *result, uint32_t column);
zyx_driver_value_type_t valueType(const zyx::Value &value);
std::string typeName(zyx_driver_value_type_t type);
std::unordered_map<std::string, zyx::Value> paramsToMap(const zyx_driver_params_t *params);
const std::unordered_map<std::string, zyx::Value> &paramsMapOrEmpty(const zyx_driver_params_t *params);
zyx_driver_status_t validateNoActiveTransaction(zyx_driver_db_t *db, zyx_driver_error_t **out_error);
std::string &storeString(zyx_driver_result_t *result, std::string value);
zyx::Value deepCopyValue(const zyx::Value &value);
zyx_driver_status_t resultErrorStatus(const zyx::Result &result);
zyx_driver_status_t transactionExceptionStatus(const std::exception &ex);
zyx_driver_status_t catchTransactionException(zyx_driver_error_t **out_error) noexcept;
zyx_driver_status_t validateActiveTransaction(zyx_driver_txn_t *txn, zyx_driver_error_t **out_error);
void unregisterTransaction(zyx_driver_txn_t *txn);

void registerResultHandle(zyx_driver_result_t *result);
void unregisterResultHandle(zyx_driver_result_t *result);
const zyx::Value *resolveValueRef(const zyx_driver_value_ref_t *ref, zyx_driver_error_t **out_error);
zyx_driver_value_ref_t makeValueRef(const zyx_driver_result_t *owner, size_t slot);
zyx_driver_value_ref_t nullValueRef();
void bumpValueRefGeneration(zyx_driver_result_t *result) noexcept;
size_t appendValueRefBuffer(const zyx_driver_result_t *result, zyx::Value value);
zyx_driver_result_t *resolveValueRefOwner(const zyx_driver_value_ref_t *ref, zyx_driver_error_t **out_error);
