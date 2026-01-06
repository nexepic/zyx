/**
 * @file metrix_c_api.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2026/1/4
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 **/

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

    // ========================================================================
    // Opaque Handles
    // ========================================================================
    typedef struct MetrixDB_T MetrixDB_T;
    typedef struct MetrixResult_T MetrixResult_T;

    // ========================================================================
    // Data Structures (Mapped to Rust structs via repr(C))
    // ========================================================================

    typedef enum {
        MX_NULL = 0,
        MX_BOOL = 1,
        MX_INT = 2,
        MX_DOUBLE = 3,
        MX_STRING = 4,
        MX_NODE = 5,
        MX_EDGE = 6
    } MetrixValueType;

    // Lightweight Node view (Topology optimized)
    typedef struct {
        int64_t id;
        const char* label; // Null-terminated string, owned by Result
    } MetrixNode;

    // Lightweight Edge view (Topology optimized)
    typedef struct {
        int64_t id;
        int64_t source_id;
        int64_t target_id;
        const char* type;  // Null-terminated string, owned by Result
    } MetrixEdge;

    // ========================================================================
    // Database Lifecycle
    // ========================================================================

    /**
     * @brief Opens the database.
     * @param path File system path to the database directory.
     * @return Pointer to DB handle, or NULL if failed.
     */
    MetrixDB_T* metrix_open(const char* path);

    /**
     * @brief Closes and frees the database handle.
     */
    void metrix_close(MetrixDB_T* db);

    /**
     * @brief Gets the last error message if a function returned error/null.
     *        Thread-local storage.
     */
    const char* metrix_get_last_error();

    // ========================================================================
    // Execution
    // ========================================================================

    /**
     * @brief Executes a Cypher query.
     * @return Result handle. Must be freed with metrix_result_close.
     */
    MetrixResult_T* metrix_execute(MetrixDB_T* db, const char* cypher);

    void metrix_result_close(MetrixResult_T* res);

    // ========================================================================
    // Result Iteration
    // ========================================================================

    /**
     * @brief Advances to the next row.
     * @return true if a row exists, false if end of stream.
     */
    bool metrix_result_next(MetrixResult_T* res);

    /**
     * @brief Returns the number of columns in the current result set.
     */
    int metrix_result_column_count(MetrixResult_T* res);

    /**
     * @brief Returns the name of the column at the given index.
     */
    const char* metrix_result_column_name(MetrixResult_T* res, int index);

    // ========================================================================
    // Data Access (Zero-Copy where possible)
    // ========================================================================

    /**
     * @brief Gets the type of the value in the current row at column index.
     */
    MetrixValueType metrix_result_get_type(MetrixResult_T* res, int col_index);

    // Primitives
    int64_t metrix_result_get_int(MetrixResult_T* res, int col_index);
    double metrix_result_get_double(MetrixResult_T* res, int col_index);
    bool metrix_result_get_bool(MetrixResult_T* res, int col_index);

    /**
     * @brief Returns string pointer. Valid until next() or close().
     */
    const char* metrix_result_get_string(MetrixResult_T* res, int col_index);

    /**
     * @brief Populates the struct with Node topology data.
     *        Returns false if value is not a node.
     */
    bool metrix_result_get_node(MetrixResult_T* res, int col_index, MetrixNode* out_node);

    /**
     * @brief Populates the struct with Edge topology data.
     */
    bool metrix_result_get_edge(MetrixResult_T* res, int col_index, MetrixEdge* out_edge);

    /**
     * @brief Returns properties of the Node/Edge at col_index as a JSON string.
     *        This is efficient for passing to Frontend (JS).
     */
    const char* metrix_result_get_props_json(MetrixResult_T* res, int col_index);

#ifdef __cplusplus
}
#endif