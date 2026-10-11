/**
 * @file QueryContext.hpp
 * @brief Execution-time context threaded through the query pipeline.
 **/

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include "graph/core/PropertyTypes.hpp"
#include "graph/query/ExecMode.hpp"
#include <string>
#include <unordered_map>

namespace graph::query {

class QueryGuard;

/**
 * @brief Explicit, bounded policy for the local-file LOAD CSV surface.
 *
 * Remote URLs are intentionally unsupported. An unset import root disables
 * LOAD CSV instead of allowing a query to read arbitrary host files.
 */
struct CsvImportPolicy {
	std::optional<std::filesystem::path> import_root;
	size_t maximum_file_bytes = 64U * 1024U * 1024U;
	size_t maximum_rows = 1'000'000U;
	size_t maximum_record_bytes = 4U * 1024U * 1024U;
	size_t maximum_columns = 16'384U;
	size_t maximum_field_bytes = 1U * 1024U * 1024U;
};

/**
 * @brief Holds execution-time context that flows through the query pipeline.
 *
 * Carries query parameters and execution mode; extensible for future needs
 * (timeout, memory limits, etc.).
 */
struct QueryContext {
	std::unordered_map<std::string, PropertyValue> parameters;
	ExecMode execMode = ExecMode::EM_FULL;
	QueryGuard *guard = nullptr;
	// An unset value delegates to the Database-owned policy. A caller can supply
	// a smaller value for one execution, but QueryEngine never lets it raise the
	// durable database ceiling.
	std::optional<int> maxVarLengthDepth;
	CsvImportPolicy csv_import_policy;

	void checkGuard() const;
	void checkGuardNow() const;
};

} // namespace graph::query
