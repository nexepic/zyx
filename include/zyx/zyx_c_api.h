/**
 * @file zyx_c_api.h
 * @author Nexepic
 * @date 2026/1/4
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// ========================================================================
// Opaque Handles
// ========================================================================
typedef struct ZYXDB_T ZYXDB_T;
typedef struct ZYXResult_T ZYXResult_T;
typedef struct ZYXTxn_T ZYXTxn_T;
typedef struct ZYXParams_T ZYXParams_T;
typedef struct ZYXList_T ZYXList_T;
typedef struct ZYXMap_T ZYXMap_T;

// ========================================================================
// Data Structures (Mapped to Rust structs via repr(C))
// ========================================================================

typedef enum {
	ZYX_NULL = 0,
	ZYX_BOOL = 1,
	ZYX_INT = 2,
	ZYX_DOUBLE = 3,
	ZYX_STRING = 4,
	ZYX_NODE = 5,
	ZYX_EDGE = 6,
	ZYX_LIST = 7,
	ZYX_MAP = 8
} ZYXValueType;

// Lightweight Node view (Topology optimized)
typedef struct {
	int64_t id;
	const char *label; // Null-terminated string, owned by Result
} ZYXNode;

// Lightweight Edge view (Topology optimized)
typedef struct {
	int64_t id;
	int64_t source_id;
	int64_t target_id;
	const char *type; // Null-terminated string, owned by Result
} ZYXEdge;

// ========================================================================
// Database Lifecycle
// ========================================================================

/**
 * @brief Opens the database.
 * @param path File system path to the database directory.
 * @return Pointer to DB handle, or NULL if failed.
 */
ZYXDB_T *zyx_open(const char *path);

/**
 * @brief Opens the database only if it exists.
 * @param path File system path to the database directory.
 * @return Pointer to DB handle, or NULL if failed or does not exist.
 */
ZYXDB_T *zyx_open_if_exists(const char *path);

/**
 * @brief Closes and frees the database handle.
 */
void zyx_close(const ZYXDB_T *db);

/**
 * @brief Gets the last error message if a function returned error/null.
 *        Thread-local storage.
 */
const char *zyx_get_last_error();

// ========================================================================
// Execution
// ========================================================================

/**
 * @brief Executes a Cypher query.
 * @return Result handle. Must be freed with zyx_result_close.
 */
ZYXResult_T *zyx_execute(const ZYXDB_T *db, const char *cypher);

/**
 * @brief Executes a parameterized Cypher query.
 * @return Result handle. Must be freed with zyx_result_close.
 */
ZYXResult_T *zyx_execute_params(const ZYXDB_T *db, const char *cypher, const ZYXParams_T *params);

void zyx_result_close(const ZYXResult_T *res);

// ========================================================================
// Transaction Control
// ========================================================================

/**
 * @brief Begins a new transaction.
 * @return Transaction handle, or NULL on failure. Must be freed with zyx_txn_close.
 */
ZYXTxn_T *zyx_begin_transaction(ZYXDB_T *db);

/**
 * @brief Begins a read-only transaction.
 *        Write queries executed within this transaction will fail.
 * @return Transaction handle, or NULL on failure. Must be freed with zyx_txn_close.
 */
ZYXTxn_T *zyx_begin_read_only_transaction(ZYXDB_T *db);

/**
 * @brief Returns true if the transaction is read-only.
 */
bool zyx_txn_is_read_only(const ZYXTxn_T *txn);

/**
 * @brief Executes a Cypher query within a transaction.
 */
ZYXResult_T *zyx_txn_execute(ZYXTxn_T *txn, const char *cypher);

/**
 * @brief Executes a parameterized Cypher query within a transaction.
 */
ZYXResult_T *zyx_txn_execute_params(ZYXTxn_T *txn, const char *cypher, const ZYXParams_T *params);

/**
 * @brief Commits the transaction. Returns true on success.
 */
bool zyx_txn_commit(ZYXTxn_T *txn);

/**
 * @brief Rolls back the transaction.
 */
void zyx_txn_rollback(ZYXTxn_T *txn);

/**
 * @brief Closes and frees the transaction handle.
 *        Auto-rolls back if not committed.
 */
void zyx_txn_close(ZYXTxn_T *txn);

// ========================================================================
// Parameters
// ========================================================================

/**
 * @brief Creates a new parameter set for parameterized queries.
 */
ZYXParams_T *zyx_params_create();

void zyx_params_set_int(ZYXParams_T *p, const char *key, int64_t val);
void zyx_params_set_double(ZYXParams_T *p, const char *key, double val);
void zyx_params_set_string(ZYXParams_T *p, const char *key, const char *val);
void zyx_params_set_bool(ZYXParams_T *p, const char *key, bool val);
void zyx_params_set_null(ZYXParams_T *p, const char *key);

/**
 * @brief Frees the parameter set.
 */
void zyx_params_close(ZYXParams_T *p);

// ========================================================================
// List Builder
// ========================================================================

ZYXList_T *zyx_list_create();
void zyx_list_push_int(ZYXList_T *list, int64_t val);
void zyx_list_push_double(ZYXList_T *list, double val);
void zyx_list_push_string(ZYXList_T *list, const char *val);
void zyx_list_push_bool(ZYXList_T *list, bool val);
void zyx_list_push_null(ZYXList_T *list);
void zyx_list_push_list(ZYXList_T *list, ZYXList_T *nested);
void zyx_list_close(ZYXList_T *list);
void zyx_params_set_list(ZYXParams_T *p, const char *key, ZYXList_T *list);

// ========================================================================
// Map Builder
// ========================================================================

ZYXMap_T *zyx_map_create();
void zyx_map_set_int(ZYXMap_T *map, const char *key, int64_t val);
void zyx_map_set_double(ZYXMap_T *map, const char *key, double val);
void zyx_map_set_string(ZYXMap_T *map, const char *key, const char *val);
void zyx_map_set_bool(ZYXMap_T *map, const char *key, bool val);
void zyx_map_set_null(ZYXMap_T *map, const char *key);
void zyx_map_set_list(ZYXMap_T *map, const char *key, ZYXList_T *list);
void zyx_map_set_map(ZYXMap_T *map, const char *key, ZYXMap_T *nested);
void zyx_map_close(ZYXMap_T *map);
void zyx_params_set_map(ZYXParams_T *p, const char *key, ZYXMap_T *map);

// ========================================================================
// Batch Operations
// ========================================================================

/**
 * @brief Creates a node with a label and properties.
 * @return true on success.
 */
bool zyx_create_node(ZYXDB_T *db, const char *label, const ZYXParams_T *props);

/**
 * @brief Creates a node and returns its internal ID.
 * @return The node ID, or -1 on failure.
 */
int64_t zyx_create_node_ret_id(ZYXDB_T *db, const char *label, const ZYXParams_T *props);

/**
 * @brief Creates an edge between two nodes by their internal IDs.
 * @return true on success.
 */
bool zyx_create_edge_by_id(ZYXDB_T *db, int64_t source_id, int64_t target_id,
                            const char *type, const ZYXParams_T *props);

// ========================================================================
// Result Iteration
// ========================================================================

/**
 * @brief Advances to the next row.
 * @return true if a row exists, false if end of stream.
 */
bool zyx_result_next(ZYXResult_T *res);

/**
 * @brief Returns the number of columns in the current result set.
 */
int zyx_result_column_count(const ZYXResult_T *res);

/**
 * @brief Returns the name of the column at the given index.
 */
const char *zyx_result_column_name(ZYXResult_T *res, int index);

/**
 * @brief Returns the execution duration in milliseconds.
 */
double zyx_result_get_duration(const ZYXResult_T *res);

// ========================================================================
// Data Access (Zero-Copy where possible)
// ========================================================================

/**
 * @brief Gets the type of the value in the current row at column index.
 */
ZYXValueType zyx_result_get_type(const ZYXResult_T *res, int col_index);

// Primitives
int64_t zyx_result_get_int(const ZYXResult_T *res, int col_index);
double zyx_result_get_double(const ZYXResult_T *res, int col_index);
bool zyx_result_get_bool(const ZYXResult_T *res, int col_index);

/**
 * @brief Returns string pointer. Valid until next() or close().
 */
const char *zyx_result_get_string(ZYXResult_T *res, int col_index);

/**
 * @brief Populates the struct with Node topology data.
 *        Returns false if value is not a node.
 */
bool zyx_result_get_node(ZYXResult_T *res, int col_index, ZYXNode *out_node);

/**
 * @brief Populates the struct with Edge topology data.
 */
bool zyx_result_get_edge(ZYXResult_T *res, int col_index, ZYXEdge *out_edge);

/**
 * @brief Returns properties of the Node/Edge at col_index as a JSON string.
 *        This is efficient for passing to Frontend (JS).
 */
const char *zyx_result_get_props_json(ZYXResult_T *res, int col_index);

// ========================================================================
// List Access
// ========================================================================

/**
 * @brief Returns the number of elements in a list value.
 */
int zyx_result_list_size(const ZYXResult_T *res, int col_index);

/**
 * @brief Gets the type of a list element.
 */
ZYXValueType zyx_result_list_get_type(const ZYXResult_T *res, int col_index, int list_index);

int64_t zyx_result_list_get_int(const ZYXResult_T *res, int col_index, int list_index);
double zyx_result_list_get_double(const ZYXResult_T *res, int col_index, int list_index);
bool zyx_result_list_get_bool(const ZYXResult_T *res, int col_index, int list_index);
const char *zyx_result_list_get_string(ZYXResult_T *res, int col_index, int list_index);

// ========================================================================
// Map Access
// ========================================================================

/**
 * @brief Returns a map value as a JSON string.
 */
const char *zyx_result_get_map_json(ZYXResult_T *res, int col_index);

// ========================================================================
// Status
// ========================================================================

bool zyx_result_is_success(const ZYXResult_T *res);

const char *zyx_result_get_error(ZYXResult_T *res);

#ifdef __cplusplus
}
#endif
