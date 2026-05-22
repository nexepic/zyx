#include "zyx/zyx_driver_abi.h"

#include <deque>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>

#include "ProjectConfig.hpp"
#include "zyx/zyx.hpp"

struct zyx_driver_error_t {
    zyx_driver_status_t code;
    std::string message;
};

struct zyx_driver_db_t {
    std::unique_ptr<zyx::Database> db;
};

struct zyx_driver_txn_t {
    zyx::Transaction txn;
};

struct zyx_driver_result_t {
    zyx::Result result;
    std::deque<std::string> string_buffers;
};

struct zyx_driver_params_t {
    std::unordered_map<std::string, zyx::Value> values;
};

namespace {

void clearError(zyx_driver_error_t **out_error) {
    if (out_error != nullptr) {
        *out_error = nullptr;
    }
}

zyx_driver_status_t setError(zyx_driver_error_t **out_error, zyx_driver_status_t code, std::string message) {
    if (out_error != nullptr) {
        *out_error = new zyx_driver_error_t{code, std::move(message)};
    }
    return code;
}

zyx_driver_value_type_t valueType(const zyx::Value &value) {
    if (std::holds_alternative<std::monostate>(value)) return ZYX_DRIVER_VALUE_NULL;
    if (std::holds_alternative<bool>(value)) return ZYX_DRIVER_VALUE_BOOL;
    if (std::holds_alternative<int64_t>(value)) return ZYX_DRIVER_VALUE_INT64;
    if (std::holds_alternative<double>(value)) return ZYX_DRIVER_VALUE_DOUBLE;
    if (std::holds_alternative<std::string>(value)) return ZYX_DRIVER_VALUE_STRING;
    if (std::holds_alternative<std::shared_ptr<zyx::Node>>(value)) return ZYX_DRIVER_VALUE_NODE;
    if (std::holds_alternative<std::shared_ptr<zyx::Edge>>(value)) return ZYX_DRIVER_VALUE_EDGE;
    if (std::holds_alternative<std::vector<float>>(value) || std::holds_alternative<std::vector<std::string>>(value) ||
        std::holds_alternative<std::shared_ptr<zyx::ValueList>>(value)) {
        return ZYX_DRIVER_VALUE_LIST;
    }
    if (std::holds_alternative<std::shared_ptr<zyx::ValueMap>>(value)) return ZYX_DRIVER_VALUE_MAP;
    return ZYX_DRIVER_VALUE_NULL;
}

std::string typeName(zyx_driver_value_type_t type) {
    switch (type) {
        case ZYX_DRIVER_VALUE_NULL: return "null";
        case ZYX_DRIVER_VALUE_BOOL: return "bool";
        case ZYX_DRIVER_VALUE_INT64: return "int64";
        case ZYX_DRIVER_VALUE_DOUBLE: return "double";
        case ZYX_DRIVER_VALUE_STRING: return "string";
        case ZYX_DRIVER_VALUE_NODE: return "node";
        case ZYX_DRIVER_VALUE_EDGE: return "edge";
        case ZYX_DRIVER_VALUE_LIST: return "list";
        case ZYX_DRIVER_VALUE_MAP: return "map";
    }
    return "unknown";
}

zyx_driver_status_t validateColumn(const zyx_driver_result_t *result, uint32_t column, zyx_driver_error_t **out_error) {
    if (result == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "result must not be null");
    }
    if (column >= static_cast<uint32_t>(result->result.getColumnCount())) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "column index is out of range");
    }
    return ZYX_DRIVER_OK;
}

struct NodeValueAccess {
    zyx_driver_status_t status = ZYX_DRIVER_OK;
    std::shared_ptr<zyx::Node> value;
};

struct EdgeValueAccess {
    zyx_driver_status_t status = ZYX_DRIVER_OK;
    std::shared_ptr<zyx::Edge> value;
};

NodeValueAccess getNodeValue(const zyx_driver_result_t *result, uint32_t column,
                             zyx_driver_error_t **out_error) {
    if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
        return {status, nullptr};
    }

    zyx::Value value = result->result.get(static_cast<int>(column));
    if (const auto *node = std::get_if<std::shared_ptr<zyx::Node>>(&value); node != nullptr && *node != nullptr) {
        return {ZYX_DRIVER_OK, *node};
    }

    std::ostringstream message;
    message << "type mismatch: expected node, got " << typeName(valueType(value));
    return {setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str()), nullptr};
}

EdgeValueAccess getEdgeValue(const zyx_driver_result_t *result, uint32_t column,
                             zyx_driver_error_t **out_error) {
    if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
        return {status, nullptr};
    }

    zyx::Value value = result->result.get(static_cast<int>(column));
    if (const auto *edge = std::get_if<std::shared_ptr<zyx::Edge>>(&value); edge != nullptr && *edge != nullptr) {
        return {ZYX_DRIVER_OK, *edge};
    }

    std::ostringstream message;
    message << "type mismatch: expected edge, got " << typeName(valueType(value));
    return {setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str()), nullptr};
}

zyx_driver_status_t internalError(zyx_driver_error_t **out_error, const char *message) {
    return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, message);
}

zyx_driver_status_t resultErrorStatus(const zyx::Result &result) {
    const std::string error = result.getError();
    if (error.find("Read-only transaction cannot execute write queries") != std::string::npos) {
        return ZYX_DRIVER_READ_ONLY_VIOLATION;
    }
    if (error.find("Transaction") != std::string::npos || error.find("transaction") != std::string::npos) {
        return ZYX_DRIVER_TRANSACTION_ERROR;
    }
    return ZYX_DRIVER_EXECUTION_ERROR;
}

zyx_driver_status_t transactionExceptionStatus(const std::exception &ex) {
    const std::string message = ex.what();
    if (message.find("Read-only transaction cannot execute write queries") != std::string::npos) {
        return ZYX_DRIVER_READ_ONLY_VIOLATION;
    }
    return ZYX_DRIVER_TRANSACTION_ERROR;
}


zyx_driver_status_t catchAbiException(zyx_driver_error_t **out_error) {
    try {
        throw;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        return internalError(out_error, ex.what());
    } catch (...) {
        return internalError(out_error, "unknown error");
    }
}

void appendJsonString(std::ostringstream &out, const std::string &value) {
    out << '"';
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch) << std::dec;
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    out << '"';
}

void appendJsonValue(std::ostringstream &out, const zyx::Value &value) {
    if (std::holds_alternative<std::monostate>(value)) {
        out << "null";
    } else if (const auto *typed = std::get_if<bool>(&value)) {
        out << (*typed ? "true" : "false");
    } else if (const auto *typed = std::get_if<int64_t>(&value)) {
        out << *typed;
    } else if (const auto *typed = std::get_if<double>(&value)) {
        out << *typed;
    } else if (const auto *typed = std::get_if<std::string>(&value)) {
        appendJsonString(out, *typed);
    } else {
        out << "null";
    }
}

std::string propertiesToJson(const std::unordered_map<std::string, zyx::Value> &properties) {
    std::ostringstream out;
    out << '{';
    bool first = true;
    for (const auto &[key, value] : properties) {
        if (!first) {
            out << ',';
        }
        first = false;
        appendJsonString(out, key);
        out << ':';
        appendJsonValue(out, value);
    }
    out << '}';
    return out.str();
}

template <typename T>
zyx_driver_status_t getScalar(const zyx_driver_result_t *result, uint32_t column, T *out_value,
                              zyx_driver_error_t **out_error, zyx_driver_value_type_t expected) {
    clearError(out_error);
    if (out_value == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
    }
    if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
        return status;
    }

    zyx::Value value = result->result.get(static_cast<int>(column));
    if (const auto *typed = std::get_if<T>(&value)) {
        *out_value = *typed;
        return ZYX_DRIVER_OK;
    }

    std::ostringstream message;
    message << "type mismatch: expected " << typeName(expected) << ", got " << typeName(valueType(value));
    return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
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
            if (!handle->db->openIfExists()) {
                return setError(out_error, ZYX_DRIVER_NOT_FOUND, "database path does not exist");
            }
        } else {
            handle->db->open();
        }

        *out_db = handle.release();
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        return setError(out_error, ZYX_DRIVER_OPEN_FAILED, ex.what());
    } catch (...) {
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

} // namespace

extern "C" {

uint32_t zyx_driver_abi_version_major(void) { return 1; }

uint32_t zyx_driver_abi_version_minor(void) { return 0; }

uint32_t zyx_driver_abi_version_patch(void) { return 0; }

const char *zyx_driver_runtime_version(void) { return PROJECT_VERSION_STR; }

zyx_driver_status_t zyx_driver_error_code(const zyx_driver_error_t *error) {
    return error == nullptr ? ZYX_DRIVER_OK : error->code;
}

const char *zyx_driver_error_message(const zyx_driver_error_t *error) {
    return error == nullptr ? "" : error->message.c_str();
}

void zyx_driver_error_free(zyx_driver_error_t *error) { delete error; }

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
        if (db->db != nullptr) {
            db->db->close();
        }
        delete db;
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) {
        delete db;
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        delete db;
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, ex.what());
    } catch (...) {
        delete db;
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}



zyx_driver_status_t zyx_driver_txn_begin(zyx_driver_db_t *db, zyx_driver_txn_t **out_txn,
                                         zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (out_txn == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_txn must not be null");
    }
    *out_txn = nullptr;
    if (db == nullptr || db->db == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "db must not be null");
    }

    try {
        auto handle = std::make_unique<zyx_driver_txn_t>(zyx_driver_txn_t{db->db->beginTransaction()});
        *out_txn = handle.release();
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        return setError(out_error, ZYX_DRIVER_TRANSACTION_ERROR, ex.what());
    } catch (...) {
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

zyx_driver_status_t zyx_driver_txn_begin_read_only(zyx_driver_db_t *db, zyx_driver_txn_t **out_txn,
                                                   zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (out_txn == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_txn must not be null");
    }
    *out_txn = nullptr;
    if (db == nullptr || db->db == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "db must not be null");
    }

    try {
        auto handle = std::make_unique<zyx_driver_txn_t>(zyx_driver_txn_t{db->db->beginReadOnlyTransaction()});
        *out_txn = handle.release();
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        return setError(out_error, ZYX_DRIVER_TRANSACTION_ERROR, ex.what());
    } catch (...) {
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
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
    if (txn == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "txn must not be null");
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
        *out_result = handle.release();
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        return setError(out_error, transactionExceptionStatus(ex), ex.what());
    } catch (...) {
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

zyx_driver_status_t zyx_driver_txn_commit(zyx_driver_txn_t *txn, zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (txn == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "txn must not be null");
    }

    try {
        txn->txn.commit();
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        return setError(out_error, transactionExceptionStatus(ex), ex.what());
    } catch (...) {
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

zyx_driver_status_t zyx_driver_txn_rollback(zyx_driver_txn_t *txn, zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (txn == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "txn must not be null");
    }

    try {
        if (txn->txn.isActive()) {
            txn->txn.rollback();
        }
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        return setError(out_error, transactionExceptionStatus(ex), ex.what());
    } catch (...) {
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

zyx_driver_status_t zyx_driver_txn_close(zyx_driver_txn_t *txn, zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (txn == nullptr) {
        return ZYX_DRIVER_OK;
    }

    try {
        if (txn->txn.isActive()) {
            txn->txn.rollback();
        }
        delete txn;
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) {
        delete txn;
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        delete txn;
        return setError(out_error, transactionExceptionStatus(ex), ex.what());
    } catch (...) {
        delete txn;
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

zyx_driver_status_t zyx_driver_params_create(zyx_driver_params_t **out_params, zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (out_params == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_params must not be null");
    }
    *out_params = nullptr;

    try {
        *out_params = new zyx_driver_params_t();
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    }
}

void zyx_driver_params_free(zyx_driver_params_t *params, zyx_driver_error_t **out_error) {
    clearError(out_error);
    delete params;
}

zyx_driver_status_t zyx_driver_params_set_null(zyx_driver_params_t *params, const char *key,
                                               zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (params == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "params must not be null");
    }
    if (key == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "key must not be null");
    }
    try {
        params->values[key] = std::monostate{};
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (...) {
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

zyx_driver_status_t zyx_driver_params_set_bool(zyx_driver_params_t *params, const char *key, bool value,
                                               zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (params == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "params must not be null");
    }
    if (key == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "key must not be null");
    }
    try {
        params->values[key] = value;
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (...) {
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

zyx_driver_status_t zyx_driver_params_set_int64(zyx_driver_params_t *params, const char *key, int64_t value,
                                                zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (params == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "params must not be null");
    }
    if (key == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "key must not be null");
    }
    try {
        params->values[key] = value;
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (...) {
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

zyx_driver_status_t zyx_driver_params_set_double(zyx_driver_params_t *params, const char *key, double value,
                                                 zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (params == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "params must not be null");
    }
    if (key == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "key must not be null");
    }
    try {
        params->values[key] = value;
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (...) {
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

zyx_driver_status_t zyx_driver_params_set_string(zyx_driver_params_t *params, const char *key, const char *value,
                                                 zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (params == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "params must not be null");
    }
    if (key == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "key must not be null");
    }
    if (value == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "value must not be null");
    }
    try {
        params->values[key] = std::string(value);
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (...) {
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

zyx_driver_status_t zyx_driver_db_execute(zyx_driver_db_t *db, const char *cypher, zyx_driver_params_t *params,
                                          zyx_driver_result_t **out_result, zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (out_result == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_result must not be null");
    }
    *out_result = nullptr;
    if (db == nullptr || db->db == nullptr) {
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
        *out_result = handle.release();
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        return setError(out_error, ZYX_DRIVER_EXECUTION_ERROR, ex.what());
    } catch (...) {
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

void zyx_driver_result_free(zyx_driver_result_t *result) { delete result; }

zyx_driver_status_t zyx_driver_result_next(zyx_driver_result_t *result, zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (result == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "result must not be null");
    }
    result->string_buffers.clear();
    try {
        if (!result->result.hasNext()) {
            return ZYX_DRIVER_DONE;
        }
        result->result.next();
        return ZYX_DRIVER_ROW;
    } catch (const std::exception &ex) {
        return setError(out_error, ZYX_DRIVER_EXECUTION_ERROR, ex.what());
    } catch (...) {
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

uint32_t zyx_driver_result_column_count(const zyx_driver_result_t *result) {
    if (result == nullptr) {
        return 0;
    }
    int count = result->result.getColumnCount();
    return count < 0 ? 0u : static_cast<uint32_t>(count);
}

const char *zyx_driver_result_column_name(zyx_driver_result_t *result, uint32_t column) {
    if (result == nullptr || column >= static_cast<uint32_t>(result->result.getColumnCount())) {
        return nullptr;
    }
    result->string_buffers.push_back(result->result.getColumnName(static_cast<int>(column)));
    return result->string_buffers.back().c_str();
}

zyx_driver_value_type_t zyx_driver_result_value_type(const zyx_driver_result_t *result, uint32_t column) {
    if (result == nullptr || column >= zyx_driver_result_column_count(result)) {
        return ZYX_DRIVER_VALUE_NULL;
    }
    return valueType(result->result.get(static_cast<int>(column)));
}

zyx_driver_status_t zyx_driver_result_get_int64(const zyx_driver_result_t *result, uint32_t column, int64_t *out_value,
                                                zyx_driver_error_t **out_error) {
    return getScalar(result, column, out_value, out_error, ZYX_DRIVER_VALUE_INT64);
}

zyx_driver_status_t zyx_driver_result_get_double(const zyx_driver_result_t *result, uint32_t column, double *out_value,
                                                 zyx_driver_error_t **out_error) {
    return getScalar(result, column, out_value, out_error, ZYX_DRIVER_VALUE_DOUBLE);
}

zyx_driver_status_t zyx_driver_result_get_bool(const zyx_driver_result_t *result, uint32_t column, bool *out_value,
                                               zyx_driver_error_t **out_error) {
    return getScalar(result, column, out_value, out_error, ZYX_DRIVER_VALUE_BOOL);
}

zyx_driver_status_t zyx_driver_result_get_string(zyx_driver_result_t *result, uint32_t column, const char **out_value,
                                                 zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (out_value == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
    }
    if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
        return status;
    }

    zyx::Value value = result->result.get(static_cast<int>(column));
    if (const auto *typed = std::get_if<std::string>(&value)) {
        result->string_buffers.push_back(*typed);
        *out_value = result->string_buffers.back().c_str();
        return ZYX_DRIVER_OK;
    }

    std::ostringstream message;
    message << "type mismatch: expected string, got " << typeName(valueType(value));
    return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
}

zyx_driver_status_t zyx_driver_result_get_node_id(const zyx_driver_result_t *result, uint32_t column,
                                                  int64_t *out_value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto node = getNodeValue(result, column, out_error);
        if (node.status != ZYX_DRIVER_OK) {
            return node.status;
        }
        *out_value = node.value->id;
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_node_label_count(const zyx_driver_result_t *result, uint32_t column,
                                                           uint32_t *out_value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto node = getNodeValue(result, column, out_error);
        if (node.status != ZYX_DRIVER_OK) {
            return node.status;
        }
        *out_value = node.value->labels.empty() ? (node.value->label.empty() ? 0u : 1u)
                                                : static_cast<uint32_t>(node.value->labels.size());
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_node_label(zyx_driver_result_t *result, uint32_t column,
                                                     uint32_t label_index, const char **out_value,
                                                     zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto node = getNodeValue(result, column, out_error);
        if (node.status != ZYX_DRIVER_OK) {
            return node.status;
        }

        const uint32_t labelCount = node.value->labels.empty() ? (node.value->label.empty() ? 0u : 1u)
                                                              : static_cast<uint32_t>(node.value->labels.size());
        if (label_index >= labelCount) {
            return setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "node label index is out of range");
        }

        result->string_buffers.push_back(node.value->labels.empty() ? node.value->label : node.value->labels[label_index]);
        *out_value = result->string_buffers.back().c_str();
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_edge_id(const zyx_driver_result_t *result, uint32_t column,
                                                  int64_t *out_value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto edge = getEdgeValue(result, column, out_error);
        if (edge.status != ZYX_DRIVER_OK) {
            return edge.status;
        }
        *out_value = edge.value->id;
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_edge_source_id(const zyx_driver_result_t *result, uint32_t column,
                                                         int64_t *out_value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto edge = getEdgeValue(result, column, out_error);
        if (edge.status != ZYX_DRIVER_OK) {
            return edge.status;
        }
        *out_value = edge.value->sourceId;
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_edge_target_id(const zyx_driver_result_t *result, uint32_t column,
                                                         int64_t *out_value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto edge = getEdgeValue(result, column, out_error);
        if (edge.status != ZYX_DRIVER_OK) {
            return edge.status;
        }
        *out_value = edge.value->targetId;
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_edge_type(zyx_driver_result_t *result, uint32_t column,
                                                    const char **out_value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto edge = getEdgeValue(result, column, out_error);
        if (edge.status != ZYX_DRIVER_OK) {
            return edge.status;
        }
        result->string_buffers.push_back(edge.value->type);
        *out_value = result->string_buffers.back().c_str();
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_entity_properties_json(zyx_driver_result_t *result, uint32_t column,
                                                                 const char **out_value,
                                                                 zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }

        zyx::Value value = result->result.get(static_cast<int>(column));
        if (const auto *node = std::get_if<std::shared_ptr<zyx::Node>>(&value); node != nullptr && *node != nullptr) {
            result->string_buffers.push_back(propertiesToJson((*node)->properties));
            *out_value = result->string_buffers.back().c_str();
            return ZYX_DRIVER_OK;
        }
        if (const auto *edge = std::get_if<std::shared_ptr<zyx::Edge>>(&value); edge != nullptr && *edge != nullptr) {
            result->string_buffers.push_back(propertiesToJson((*edge)->properties));
            *out_value = result->string_buffers.back().c_str();
            return ZYX_DRIVER_OK;
        }

        std::ostringstream message;
        message << "type mismatch: expected node or edge, got " << typeName(valueType(value));
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
    } catch (...) {
        return catchAbiException(out_error);
    }
}

} // extern "C"
