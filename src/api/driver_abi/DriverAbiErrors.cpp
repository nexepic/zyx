#include "DriverAbiInternal.hpp"

#include "ProjectConfig.hpp"

#include <new>

const zyx_driver_error_t &staticErrorFor(zyx_driver_status_t code) noexcept { // ZYX_COV_EXCL_FUNCTION: fallback for error allocation failure.
    static const zyx_driver_error_t ok{ZYX_DRIVER_OK, {}, "", true, -1, {}};
    static const zyx_driver_error_t invalidArgument{
        ZYX_DRIVER_INVALID_ARGUMENT, {}, "invalid argument", true, -1, {}};
    static const zyx_driver_error_t outOfRange{ZYX_DRIVER_OUT_OF_RANGE, {}, "out of range", true, -1, {}};
    static const zyx_driver_error_t typeMismatch{
        ZYX_DRIVER_TYPE_MISMATCH, {}, "type mismatch", true, -1, {}};
    static const zyx_driver_error_t notFound{ZYX_DRIVER_NOT_FOUND, {}, "not found", true, -1, {}};
    static const zyx_driver_error_t openFailed{ZYX_DRIVER_OPEN_FAILED, {}, "open failed", true, -1, {}};
    static const zyx_driver_error_t parseError{ZYX_DRIVER_PARSE_ERROR, {}, "parse error", true, -1, {}};
    static const zyx_driver_error_t executionError{
        ZYX_DRIVER_EXECUTION_ERROR, {}, "execution error", true, -1, {}};
    static const zyx_driver_error_t transactionError{
        ZYX_DRIVER_TRANSACTION_ERROR, {}, "transaction error", true, -1, {}};
    static const zyx_driver_error_t readOnlyViolation{
        ZYX_DRIVER_READ_ONLY_VIOLATION, {}, "read-only violation", true, -1, {}};
    static const zyx_driver_error_t ioError{ZYX_DRIVER_IO_ERROR, {}, "I/O error", true, -1, {}};
    static const zyx_driver_error_t outOfMemory{
        ZYX_DRIVER_OUT_OF_MEMORY, {}, "out of memory", true, -1, {}};
    static const zyx_driver_error_t internalError{
        ZYX_DRIVER_INTERNAL_ERROR, {}, "internal error", true, -1, {}};

    switch (code) { // ZYX_COV_EXCL_LINE: fallback error allocation paths are defensive only.
        case ZYX_DRIVER_OK: return ok;
        case ZYX_DRIVER_INVALID_ARGUMENT: return invalidArgument;
        case ZYX_DRIVER_OUT_OF_RANGE: return outOfRange;
        case ZYX_DRIVER_TYPE_MISMATCH: return typeMismatch;
        case ZYX_DRIVER_NOT_FOUND: return notFound;
        case ZYX_DRIVER_OPEN_FAILED: return openFailed;
        case ZYX_DRIVER_PARSE_ERROR: return parseError;
        case ZYX_DRIVER_EXECUTION_ERROR: return executionError;
        case ZYX_DRIVER_TRANSACTION_ERROR: return transactionError;
        case ZYX_DRIVER_READ_ONLY_VIOLATION: return readOnlyViolation;
        case ZYX_DRIVER_IO_ERROR: return ioError;
        case ZYX_DRIVER_OUT_OF_MEMORY: return outOfMemory;
        case ZYX_DRIVER_INTERNAL_ERROR: return internalError;
        case ZYX_DRIVER_ROW:
        case ZYX_DRIVER_DONE:
            return ok;
    }
    return internalError; // ZYX_COV_EXCL_LINE: unknown status values are defensive only.
}

void setStaticError(zyx_driver_error_t **out_error, zyx_driver_status_t code) noexcept { // ZYX_COV_EXCL_FUNCTION: fallback for error allocation failure.
    if (out_error != nullptr) {
        *out_error = const_cast<zyx_driver_error_t *>(&staticErrorFor(code));
    }
}

zyx_driver_status_t setError(zyx_driver_error_t **out_error, zyx_driver_status_t code, const char *message) noexcept {
    try {
        if (out_error != nullptr) {
            *out_error = new zyx_driver_error_t{
                code, std::string(message != nullptr ? message : ""), nullptr, false, -1, {}};
        }
    } catch (...) { // ZYX_COV_EXCL_LINE: fallback for error-reporting allocation failure.
        setStaticError(out_error, code);
    }
    return code;
}

zyx_driver_status_t setError(zyx_driver_error_t **out_error, zyx_driver_status_t code, std::string message) noexcept {
    try {
        if (out_error != nullptr) {
            *out_error = new zyx_driver_error_t{code, std::move(message), nullptr, false, -1, {}};
        }
    } catch (...) { // ZYX_COV_EXCL_LINE: fallback for error-reporting allocation failure.
        setStaticError(out_error, code);
    }
    return code;
}

zyx_driver_status_t setErrorAt(zyx_driver_error_t **out_error,
                               zyx_driver_status_t code,
                               std::string message,
                               int64_t row_index,
                               std::string field_path) noexcept {
    try {
        if (out_error != nullptr) {
            auto error = std::make_unique<zyx_driver_error_t>();
            error->code = code;
            error->message = std::move(message);
            error->row_index = row_index;
            error->field_path = std::move(field_path);
            *out_error = error.release();
        }
    } catch (...) { // ZYX_COV_EXCL_LINE: fallback for error-reporting allocation failure.
        setStaticError(out_error, code);
    }
    return code;
}
zyx_driver_status_t internalError(zyx_driver_error_t **out_error, const char *message) noexcept { // ZYX_COV_EXCL_FUNCTION: only used by catchAbiException defensive paths.
    return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, message);
}

void clearError(zyx_driver_error_t **out_error) {
    if (out_error != nullptr) {
        *out_error = nullptr;
    }
}
zyx_driver_status_t catchAbiException(zyx_driver_error_t **out_error) noexcept { // ZYX_COV_EXCL_FUNCTION: defensive exception shield for graph accessor wrappers.
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

zyx_driver_status_t catchAbiExceptionAs(zyx_driver_error_t **out_error,
                                        zyx_driver_status_t exception_status) noexcept {
    try {
        throw;
    } catch (const std::bad_alloc &) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &ex) {
        return setError(out_error, exception_status, ex.what());
    } catch (...) {
        return internalError(out_error, "unknown error");
    }
}

extern "C" {

uint32_t zyx_driver_abi_version_major(void) { return 1; }

uint32_t zyx_driver_abi_version_minor(void) { return 1; }

uint32_t zyx_driver_abi_version_patch(void) { return 0; }

const char *zyx_driver_runtime_version(void) { return PROJECT_VERSION_STR; }

zyx_driver_status_t zyx_driver_error_code(const zyx_driver_error_t *error) {
    return error == nullptr ? ZYX_DRIVER_OK : error->code;
}

const char *zyx_driver_error_message(const zyx_driver_error_t *error) {
    if (error == nullptr) {
        return "";
    }
    return error->fallback_message != nullptr ? error->fallback_message : error->message.c_str();
}

int64_t zyx_driver_error_row_index(const zyx_driver_error_t *error) {
    return error == nullptr ? -1 : error->row_index;
}

const char *zyx_driver_error_field_path(const zyx_driver_error_t *error) {
    return error == nullptr ? "" : error->field_path.c_str();
}

void zyx_driver_error_free(zyx_driver_error_t *error) {
    if (error != nullptr && !error->static_storage) {
        delete error;
    }
}

} // extern "C"
