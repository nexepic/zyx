#include "DriverAbiInternal.hpp"

#include <new>
#include <utility>
#include <vector>

namespace {

const std::unordered_map<std::string, zyx::Value> &emptyParams() {
    static const std::unordered_map<std::string, zyx::Value> empty;
    return empty;
}

} // namespace

std::unordered_map<std::string, zyx::Value> paramsToMap(const zyx_driver_params_t *params) {
    return params != nullptr ? params->values : emptyParams();
}

const std::unordered_map<std::string, zyx::Value> &paramsMapOrEmpty(const zyx_driver_params_t *params) {
    return params != nullptr ? params->values : emptyParams();
}

extern "C" {

zyx_driver_status_t zyx_driver_params_create(zyx_driver_params_t **out_params, zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (out_params == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_params must not be null");
    }
    *out_params = nullptr;

    try {
        *out_params = new zyx_driver_params_t();
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, ex.what());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
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
    } catch (const std::bad_alloc &) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
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
    } catch (const std::bad_alloc &) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
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
    } catch (const std::bad_alloc &) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
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
    } catch (const std::bad_alloc &) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
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
    } catch (const std::bad_alloc &) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

zyx_driver_status_t zyx_driver_params_set_string_list(zyx_driver_params_t *params, const char *key,
                                                      const char *const *values, uint32_t count,
                                                      zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (params == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "params must not be null");
    }
    if (key == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "key must not be null");
    }
    if (values == nullptr && count > 0) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "values must not be null when count is non-zero");
    }
    try {
        std::vector<std::string> list;
        list.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (values[i] == nullptr) {
                return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "string list values must not be null");
            }
            list.emplace_back(values[i]);
        }
        params->values[key] = std::move(list);
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

zyx_driver_status_t zyx_driver_params_set_float_list(zyx_driver_params_t *params, const char *key,
                                                     const float *values, uint32_t count,
                                                     zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (params == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "params must not be null");
    }
    if (key == nullptr) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "key must not be null");
    }
    if (values == nullptr && count > 0) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "values must not be null when count is non-zero");
    }
    try {
        std::vector<float> list;
        list.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            list.push_back(values[i]);
        }
        params->values[key] = std::move(list);
        return ZYX_DRIVER_OK;
    } catch (const std::bad_alloc &) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

zyx_driver_status_t zyx_driver_params_set_value(zyx_driver_params_t *params, const char *key,
                                                const zyx_driver_value_t *value,
                                                zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (params == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "params must not be null");
        }
        if (key == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "key must not be null");
        }
        if (value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "value must not be null");
        }
        params->values[key] = deepCopyValue(value->value);
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

} // extern "C"
