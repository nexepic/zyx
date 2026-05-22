#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zyx_driver_db_t zyx_driver_db_t;
typedef struct zyx_driver_txn_t zyx_driver_txn_t;
typedef struct zyx_driver_result_t zyx_driver_result_t;
typedef struct zyx_driver_params_t zyx_driver_params_t;
typedef struct zyx_driver_error_t zyx_driver_error_t;

typedef enum zyx_driver_status_t {
    ZYX_DRIVER_OK = 0,
    ZYX_DRIVER_ROW = 1,
    ZYX_DRIVER_DONE = 2,
    ZYX_DRIVER_INVALID_ARGUMENT = 100,
    ZYX_DRIVER_NOT_FOUND = 101,
    ZYX_DRIVER_OPEN_FAILED = 102,
    ZYX_DRIVER_PARSE_ERROR = 200,
    ZYX_DRIVER_EXECUTION_ERROR = 201,
    ZYX_DRIVER_TRANSACTION_ERROR = 300,
    ZYX_DRIVER_READ_ONLY_VIOLATION = 301,
    ZYX_DRIVER_TYPE_MISMATCH = 400,
    ZYX_DRIVER_OUT_OF_RANGE = 401,
    ZYX_DRIVER_IO_ERROR = 500,
    ZYX_DRIVER_OUT_OF_MEMORY = 600,
    ZYX_DRIVER_INTERNAL_ERROR = 900
} zyx_driver_status_t;

typedef enum zyx_driver_value_type_t {
    ZYX_DRIVER_VALUE_NULL = 0,
    ZYX_DRIVER_VALUE_BOOL = 1,
    ZYX_DRIVER_VALUE_INT64 = 2,
    ZYX_DRIVER_VALUE_DOUBLE = 3,
    ZYX_DRIVER_VALUE_STRING = 4,
    ZYX_DRIVER_VALUE_NODE = 5,
    ZYX_DRIVER_VALUE_EDGE = 6,
    ZYX_DRIVER_VALUE_LIST = 7,
    ZYX_DRIVER_VALUE_MAP = 8
} zyx_driver_value_type_t;

uint32_t zyx_driver_abi_version_major(void);
uint32_t zyx_driver_abi_version_minor(void);
uint32_t zyx_driver_abi_version_patch(void);
const char *zyx_driver_runtime_version(void);

zyx_driver_status_t zyx_driver_error_code(const zyx_driver_error_t *error);
const char *zyx_driver_error_message(const zyx_driver_error_t *error);
void zyx_driver_error_free(zyx_driver_error_t *error);

zyx_driver_status_t zyx_driver_db_open(const char *path, zyx_driver_db_t **out_db, zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_db_open_if_exists(const char *path, zyx_driver_db_t **out_db, zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_db_close(zyx_driver_db_t *db, zyx_driver_error_t **out_error);

zyx_driver_status_t zyx_driver_db_execute(zyx_driver_db_t *db, const char *cypher,
                                          zyx_driver_result_t **out_result, zyx_driver_error_t **out_error);
void zyx_driver_result_free(zyx_driver_result_t *result);
zyx_driver_status_t zyx_driver_result_next(zyx_driver_result_t *result, zyx_driver_error_t **out_error);
uint32_t zyx_driver_result_column_count(const zyx_driver_result_t *result);
const char *zyx_driver_result_column_name(zyx_driver_result_t *result, uint32_t column);
zyx_driver_value_type_t zyx_driver_result_value_type(const zyx_driver_result_t *result, uint32_t column);
zyx_driver_status_t zyx_driver_result_get_int64(const zyx_driver_result_t *result, uint32_t column, int64_t *out_value,
                                                zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_result_get_double(const zyx_driver_result_t *result, uint32_t column, double *out_value,
                                                 zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_result_get_bool(const zyx_driver_result_t *result, uint32_t column, bool *out_value,
                                               zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_result_get_string(zyx_driver_result_t *result, uint32_t column, const char **out_value,
                                                 zyx_driver_error_t **out_error);

#ifdef __cplusplus
}
#endif
