#include "zyx/zyx_driver_abi.h"

#include <filesystem>
#include <memory>
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

} // extern "C"
