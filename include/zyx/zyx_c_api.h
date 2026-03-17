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
	ZYX_EDGE = 6
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

void zyx_result_close(const ZYXResult_T *res);

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

bool zyx_result_is_success(const ZYXResult_T *res);

const char *zyx_result_get_error(ZYXResult_T *res);

#ifdef __cplusplus
}
#endif
