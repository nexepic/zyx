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

/* Transaction handles are opaque and must be released with zyx_driver_txn_close.
 * Commit and rollback finalize transaction state. Closing an active transaction rolls back.
 */
zyx_driver_status_t zyx_driver_txn_begin(zyx_driver_db_t *db, zyx_driver_txn_t **out_txn,
                                         zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_txn_begin_read_only(zyx_driver_db_t *db, zyx_driver_txn_t **out_txn,
                                                   zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_txn_execute(zyx_driver_txn_t *txn, const char *cypher,
                                           const zyx_driver_params_t *params,
                                           zyx_driver_result_t **out_result, zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_txn_commit(zyx_driver_txn_t *txn, zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_txn_rollback(zyx_driver_txn_t *txn, zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_txn_close(zyx_driver_txn_t *txn, zyx_driver_error_t **out_error);

zyx_driver_status_t zyx_driver_params_create(zyx_driver_params_t **out_params, zyx_driver_error_t **out_error);
void zyx_driver_params_free(zyx_driver_params_t *params, zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_params_set_null(zyx_driver_params_t *params, const char *key,
                                               zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_params_set_bool(zyx_driver_params_t *params, const char *key, bool value,
                                               zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_params_set_int64(zyx_driver_params_t *params, const char *key, int64_t value,
                                                zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_params_set_double(zyx_driver_params_t *params, const char *key, double value,
                                                 zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_params_set_string(zyx_driver_params_t *params, const char *key, const char *value,
                                                 zyx_driver_error_t **out_error);

zyx_driver_status_t zyx_driver_db_execute(zyx_driver_db_t *db, const char *cypher, zyx_driver_params_t *params,
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
zyx_driver_status_t zyx_driver_result_get_node_id(const zyx_driver_result_t *result, uint32_t column,
                                                  int64_t *out_value, zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_result_get_node_label_count(const zyx_driver_result_t *result, uint32_t column,
                                                           uint32_t *out_value, zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_result_get_node_label(zyx_driver_result_t *result, uint32_t column,
                                                     uint32_t label_index, const char **out_value,
                                                     zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_result_get_edge_id(const zyx_driver_result_t *result, uint32_t column,
                                                  int64_t *out_value, zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_result_get_edge_source_id(const zyx_driver_result_t *result, uint32_t column,
                                                         int64_t *out_value, zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_result_get_edge_target_id(const zyx_driver_result_t *result, uint32_t column,
                                                         int64_t *out_value, zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_result_get_edge_type(zyx_driver_result_t *result, uint32_t column,
                                                    const char **out_value, zyx_driver_error_t **out_error);
zyx_driver_status_t zyx_driver_result_get_entity_properties_json(zyx_driver_result_t *result, uint32_t column,
                                                                 const char **out_value,
                                                                 zyx_driver_error_t **out_error);

#ifdef __cplusplus
}
#endif
