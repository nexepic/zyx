/**
 * @file ExecMode.hpp
 * @brief Execution mode enum for query access control.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

namespace graph::query {

/**
 * @enum ExecMode
 * @brief 3-level execution permission model.
 *
 * Prefixed with EM_ to avoid Windows macro conflicts.
 */
enum class ExecMode {
	EM_FULL,       // All operations (default)
	EM_READ_WRITE, // Data read/write, no schema changes
	EM_READ_ONLY   // Read only
};

/** @brief Returns true if the mode allows data mutation (CREATE, SET, DELETE, MERGE, REMOVE). */
inline bool allowsDataWrite(ExecMode m) { return m != ExecMode::EM_READ_ONLY; }

/** @brief Returns true if the mode allows schema mutation (CREATE INDEX, DROP INDEX, etc.). */
inline bool allowsSchemaWrite(ExecMode m) { return m == ExecMode::EM_FULL; }

} // namespace graph::query
