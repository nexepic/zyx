#include "zyx/zyx_driver_abi.h"

#include <filesystem>
#include <memory>
#include <sstream>
#include <new>
#include <string>
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

struct zyx_driver_result_t {
    zyx::Result result;
    std::vector<std::string> column_name_buffers;
    std::string string_buffer;
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


zyx_driver_status_t zyx_driver_db_execute(zyx_driver_db_t *db, const char *cypher, zyx_driver_result_t **out_result,
                                          zyx_driver_error_t **out_error) {
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
        handle->result = db->db->execute(cypher);
        if (!handle->result.isSuccess()) {
            return setError(out_error, ZYX_DRIVER_EXECUTION_ERROR, handle->result.getError());
        }
        int column_count = handle->result.getColumnCount();
        if (column_count > 0) {
            handle->column_name_buffers.reserve(static_cast<size_t>(column_count));
            for (int i = 0; i < column_count; ++i) {
                handle->column_name_buffers.push_back(handle->result.getColumnName(i));
            }
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
    result->string_buffer.clear();
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
    if (result == nullptr || column >= result->column_name_buffers.size()) {
        return nullptr;
    }
    return result->column_name_buffers[column].c_str();
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
        result->string_buffer = *typed;
        *out_value = result->string_buffer.c_str();
        return ZYX_DRIVER_OK;
    }

    std::ostringstream message;
    message << "type mismatch: expected string, got " << typeName(valueType(value));
    return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
}

} // extern "C"
