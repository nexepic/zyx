#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zyx/arrow_c_data.h"

#if defined(_WIN32) && defined(ZYX_DRIVER_ABI_EXPORTS)
#define ZYX_DRIVER_API __declspec(dllexport)
#else
#define ZYX_DRIVER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zyx_driver_db_t zyx_driver_db_t;
typedef struct zyx_driver_txn_t zyx_driver_txn_t;
typedef struct zyx_driver_result_t zyx_driver_result_t;
typedef struct zyx_driver_params_t zyx_driver_params_t;
typedef struct zyx_driver_error_t zyx_driver_error_t;
typedef struct zyx_driver_value_t zyx_driver_value_t;
typedef struct zyx_driver_ingest_t zyx_driver_ingest_t;
typedef struct zyx_driver_edge_ingestor_t zyx_driver_edge_ingestor_t;

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

typedef struct zyx_driver_value_ref_t {
    uint64_t owner_id;
    uint64_t owner_cookie;
    uint64_t generation;
    uint64_t slot;
} zyx_driver_value_ref_t;

typedef struct zyx_driver_id_range_t {
    int64_t first_id;
    int64_t count;
} zyx_driver_id_range_t;

ZYX_DRIVER_API uint32_t zyx_driver_abi_version_major(void);
ZYX_DRIVER_API uint32_t zyx_driver_abi_version_minor(void);
ZYX_DRIVER_API uint32_t zyx_driver_abi_version_patch(void);
ZYX_DRIVER_API const char *zyx_driver_runtime_version(void);

ZYX_DRIVER_API zyx_driver_status_t zyx_driver_error_code(const zyx_driver_error_t *error);
ZYX_DRIVER_API const char *zyx_driver_error_message(const zyx_driver_error_t *error);
/* Returns -1 when an error is not associated with one input row. */
ZYX_DRIVER_API int64_t zyx_driver_error_row_index(const zyx_driver_error_t *error);
/* Returns an empty string when an error is not associated with one input field. */
ZYX_DRIVER_API const char *zyx_driver_error_field_path(const zyx_driver_error_t *error);
ZYX_DRIVER_API void zyx_driver_error_free(zyx_driver_error_t *error);

ZYX_DRIVER_API zyx_driver_status_t zyx_driver_db_open(const char *path, zyx_driver_db_t **out_db, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_db_open_if_exists(const char *path, zyx_driver_db_t **out_db, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_db_close(zyx_driver_db_t *db, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_db_save(zyx_driver_db_t *db, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_db_has_active_transaction(zyx_driver_db_t *db, bool *out_value,
                                                         zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_db_set_thread_pool_size(zyx_driver_db_t *db, uint32_t size,
                                                       zyx_driver_error_t **out_error);

/* High-throughput Arrow C Data ingestion. Ingest and edge-ingestor handles are
 * not thread-safe. zyx_driver_ingest_close rolls back an active or failed
 * session. Arrow schemas are compiled during prepare; Arrow array buffers are
 * borrowed only for the duration of zyx_driver_edge_ingestor_write.
 * Callers must provide buffers that satisfy the Arrow C Data Interface sizes;
 * ArrowArray does not carry physical buffer byte lengths for ZYX to verify.
 *
 * Edge schemas must be struct<source_id:int64, target_id:int64,
 * properties:struct<...>>. A prepared ingestor accepts any number of record
 * batches with that schema. A write failure poisons the ingest session: it can
 * only be rolled back or closed afterward. One batch is limited to 10,000,000
 * rows and 512 MiB of adapter-side decoded memory.
 */
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_ingest_begin(zyx_driver_db_t *db, zyx_driver_ingest_t **out_ingest,
												   zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_ingest_prepare_edges(zyx_driver_ingest_t *ingest, const char *edge_type,
													   const struct ArrowSchema *schema,
													   zyx_driver_edge_ingestor_t **out_ingestor,
													   zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_edge_ingestor_write(zyx_driver_edge_ingestor_t *ingestor,
													  const struct ArrowArray *record_batch,
													  zyx_driver_id_range_t *out_ids,
													  zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_ingest_commit(zyx_driver_ingest_t *ingest,
													zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_ingest_rollback(zyx_driver_ingest_t *ingest,
													  zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_edge_ingestor_close(zyx_driver_edge_ingestor_t *ingestor,
													  zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_ingest_close(zyx_driver_ingest_t *ingest, zyx_driver_error_t **out_error);

/* Transaction handles are opaque and must be released with zyx_driver_txn_close.
 * Commit and rollback finalize transaction state. Closing an active transaction rolls back.
 */
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_txn_begin(zyx_driver_db_t *db, zyx_driver_txn_t **out_txn,
                                         zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_txn_begin_read_only(zyx_driver_db_t *db, zyx_driver_txn_t **out_txn,
                                                   zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_txn_execute(zyx_driver_txn_t *txn, const char *cypher,
                                           const zyx_driver_params_t *params,
                                           zyx_driver_result_t **out_result, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_txn_commit(zyx_driver_txn_t *txn, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_txn_rollback(zyx_driver_txn_t *txn, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_txn_close(zyx_driver_txn_t *txn, zyx_driver_error_t **out_error);

ZYX_DRIVER_API zyx_driver_status_t zyx_driver_params_create(zyx_driver_params_t **out_params, zyx_driver_error_t **out_error);
ZYX_DRIVER_API void zyx_driver_params_free(zyx_driver_params_t *params, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_params_set_null(zyx_driver_params_t *params, const char *key,
                                               zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_params_set_bool(zyx_driver_params_t *params, const char *key, bool value,
                                               zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_params_set_int64(zyx_driver_params_t *params, const char *key, int64_t value,
                                                zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_params_set_double(zyx_driver_params_t *params, const char *key, double value,
                                                 zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_params_set_string(zyx_driver_params_t *params, const char *key, const char *value,
                                                 zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_params_set_string_list(zyx_driver_params_t *params, const char *key,
                                                      const char *const *values, uint32_t count,
                                                      zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_params_set_float_list(zyx_driver_params_t *params, const char *key,
                                                     const float *values, uint32_t count,
                                                     zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_params_set_value(zyx_driver_params_t *params, const char *key,
                                                const zyx_driver_value_t *value,
                                                zyx_driver_error_t **out_error);

ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_null_create(zyx_driver_value_t **out_value,
                                                 zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_bool_create(bool value, zyx_driver_value_t **out_value,
                                                 zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_int64_create(int64_t value, zyx_driver_value_t **out_value,
                                                  zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_double_create(double value, zyx_driver_value_t **out_value,
                                                   zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_string_create(const char *value, zyx_driver_value_t **out_value,
                                                   zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_list_create(zyx_driver_value_t **out_value,
                                                 zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_map_create(zyx_driver_value_t **out_value,
                                                zyx_driver_error_t **out_error);
ZYX_DRIVER_API void zyx_driver_value_free(zyx_driver_value_t *value, zyx_driver_error_t **out_error);

ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_list_append_null(zyx_driver_value_t *list,
                                                      zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_list_append_bool(zyx_driver_value_t *list, bool value,
                                                      zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_list_append_int64(zyx_driver_value_t *list, int64_t value,
                                                       zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_list_append_double(zyx_driver_value_t *list, double value,
                                                        zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_list_append_string(zyx_driver_value_t *list, const char *value,
                                                        zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_list_append_value(zyx_driver_value_t *list,
                                                       const zyx_driver_value_t *value,
                                                       zyx_driver_error_t **out_error);

ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_map_set_null(zyx_driver_value_t *map, const char *key,
                                                  zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_map_set_bool(zyx_driver_value_t *map, const char *key, bool value,
                                                  zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_map_set_int64(zyx_driver_value_t *map, const char *key, int64_t value,
                                                   zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_map_set_double(zyx_driver_value_t *map, const char *key, double value,
                                                    zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_map_set_string(zyx_driver_value_t *map, const char *key,
                                                    const char *value, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_map_set_value(zyx_driver_value_t *map, const char *key,
                                                   const zyx_driver_value_t *value,
                                                   zyx_driver_error_t **out_error);

ZYX_DRIVER_API zyx_driver_status_t zyx_driver_db_create_node(zyx_driver_db_t *db, const char *label,
                                              const zyx_driver_params_t *properties, int64_t *out_node_id,
                                              zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_db_create_node_with_labels(zyx_driver_db_t *db,
                                                          const char *const *labels, uint32_t label_count,
                                                          const zyx_driver_params_t *properties,
                                                          int64_t *out_node_id,
                                                          zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_db_create_edge(zyx_driver_db_t *db, int64_t source_id,
                                              int64_t target_id, const char *edge_type,
                                              const zyx_driver_params_t *properties, int64_t *out_edge_id,
                                              zyx_driver_error_t **out_error);

ZYX_DRIVER_API zyx_driver_status_t zyx_driver_db_execute(zyx_driver_db_t *db, const char *cypher, zyx_driver_params_t *params,
                                          zyx_driver_result_t **out_result, zyx_driver_error_t **out_error);
ZYX_DRIVER_API void zyx_driver_result_free(zyx_driver_result_t *result);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_next(zyx_driver_result_t *result, zyx_driver_error_t **out_error);
ZYX_DRIVER_API uint32_t zyx_driver_result_column_count(const zyx_driver_result_t *result);
ZYX_DRIVER_API const char *zyx_driver_result_column_name(zyx_driver_result_t *result, uint32_t column);
ZYX_DRIVER_API zyx_driver_value_type_t zyx_driver_result_value_type(const zyx_driver_result_t *result, uint32_t column);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_int64(const zyx_driver_result_t *result, uint32_t column, int64_t *out_value,
                                                zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_double(const zyx_driver_result_t *result, uint32_t column, double *out_value,
                                                 zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_bool(const zyx_driver_result_t *result, uint32_t column, bool *out_value,
                                               zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_string(zyx_driver_result_t *result, uint32_t column, const char **out_value,
                                                 zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_list_count(const zyx_driver_result_t *result, uint32_t column,
                                                     uint32_t *out_value, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_value_type_t zyx_driver_result_get_list_value_type(const zyx_driver_result_t *result,
                                                              uint32_t column, uint32_t index);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_list_int64(const zyx_driver_result_t *result,
                                                     uint32_t column, uint32_t index, int64_t *out_value,
                                                     zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_list_double(const zyx_driver_result_t *result,
                                                      uint32_t column, uint32_t index, double *out_value,
                                                      zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_list_bool(const zyx_driver_result_t *result,
                                                    uint32_t column, uint32_t index, bool *out_value,
                                                    zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_list_string(zyx_driver_result_t *result,
                                                      uint32_t column, uint32_t index, const char **out_value,
                                                      zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_value(const zyx_driver_result_t *result, uint32_t column,
                                                zyx_driver_value_ref_t *out_value,
                                                zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_value_type_t zyx_driver_value_ref_type(const zyx_driver_value_ref_t *value);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_get_int64(const zyx_driver_value_ref_t *value, int64_t *out_value,
                                                   zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_get_double(const zyx_driver_value_ref_t *value, double *out_value,
                                                    zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_get_bool(const zyx_driver_value_ref_t *value, bool *out_value,
                                                  zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_get_string(zyx_driver_result_t *result,
                                                    const zyx_driver_value_ref_t *value,
                                                    const char **out_value,
                                                    zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_list_count(const zyx_driver_value_ref_t *value,
                                                    uint32_t *out_value,
                                                    zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_list_get(const zyx_driver_value_ref_t *value, uint32_t index,
                                                  zyx_driver_value_ref_t *out_value,
                                                  zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_list_get_int64(const zyx_driver_value_ref_t *value,
                                                        uint32_t index, int64_t *out_value,
                                                        zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_list_get_double(const zyx_driver_value_ref_t *value,
                                                         uint32_t index, double *out_value,
                                                         zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_list_get_bool(const zyx_driver_value_ref_t *value,
                                                       uint32_t index, bool *out_value,
                                                       zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_list_get_string(zyx_driver_result_t *result,
                                                         const zyx_driver_value_ref_t *value,
                                                         uint32_t index,
                                                         const char **out_value,
                                                         zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_map_count(const zyx_driver_value_ref_t *value,
                                                   uint32_t *out_value,
                                                   zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_map_key(zyx_driver_result_t *result,
                                                 const zyx_driver_value_ref_t *value,
                                                 uint32_t index,
                                                 const char **out_key,
                                                 zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_map_get(const zyx_driver_value_ref_t *value,
                                                 const char *key,
                                                 zyx_driver_value_ref_t *out_value,
                                                 zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_map_get_int64(const zyx_driver_value_ref_t *value,
                                                       const char *key,
                                                       int64_t *out_value,
                                                       zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_map_get_double(const zyx_driver_value_ref_t *value,
                                                        const char *key,
                                                        double *out_value,
                                                        zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_map_get_bool(const zyx_driver_value_ref_t *value,
                                                      const char *key,
                                                      bool *out_value,
                                                      zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_map_get_string(zyx_driver_result_t *result,
                                                        const zyx_driver_value_ref_t *value,
                                                        const char *key,
                                                        const char **out_value,
                                                        zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_get_node_id(const zyx_driver_value_ref_t *value,
                                                            int64_t *out_value,
                                                            zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_get_node_label_count(const zyx_driver_value_ref_t *value,
                                                                     uint32_t *out_value,
                                                                     zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_get_node_label(zyx_driver_result_t *result,
                                                               const zyx_driver_value_ref_t *value,
                                                               uint32_t label_index,
                                                               const char **out_value,
                                                               zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_get_edge_id(const zyx_driver_value_ref_t *value,
                                                            int64_t *out_value,
                                                            zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_get_edge_source_id(const zyx_driver_value_ref_t *value,
                                                                   int64_t *out_value,
                                                                   zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_get_edge_target_id(const zyx_driver_value_ref_t *value,
                                                                   int64_t *out_value,
                                                                   zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_get_edge_type(zyx_driver_result_t *result,
                                                              const zyx_driver_value_ref_t *value,
                                                              const char **out_value,
                                                              zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_value_ref_get_entity_properties_json(zyx_driver_result_t *result,
                                                                          const zyx_driver_value_ref_t *value,
                                                                          const char **out_value,
                                                                          zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_node_id(const zyx_driver_result_t *result, uint32_t column,
                                                  int64_t *out_value, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_node_label_count(const zyx_driver_result_t *result, uint32_t column,
                                                           uint32_t *out_value, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_node_label(zyx_driver_result_t *result, uint32_t column,
                                                     uint32_t label_index, const char **out_value,
                                                     zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_edge_id(const zyx_driver_result_t *result, uint32_t column,
                                                  int64_t *out_value, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_edge_source_id(const zyx_driver_result_t *result, uint32_t column,
                                                         int64_t *out_value, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_edge_target_id(const zyx_driver_result_t *result, uint32_t column,
                                                         int64_t *out_value, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_edge_type(zyx_driver_result_t *result, uint32_t column,
                                                    const char **out_value, zyx_driver_error_t **out_error);
ZYX_DRIVER_API zyx_driver_status_t zyx_driver_result_get_entity_properties_json(zyx_driver_result_t *result, uint32_t column,
                                                                 const char **out_value,
                                                                 zyx_driver_error_t **out_error);

#ifdef __cplusplus
}
#endif
