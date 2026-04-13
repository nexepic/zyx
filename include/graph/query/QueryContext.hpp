/**
 * @file QueryContext.hpp
 * @brief Execution-time context threaded through the query pipeline.
 **/

#pragma once

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/ExecMode.hpp"
#include <string>
#include <unordered_map>

namespace graph::query {

/**
 * @brief Holds execution-time context that flows through the query pipeline.
 *
 * Carries query parameters and execution mode; extensible for future needs
 * (timeout, memory limits, etc.).
 */
struct QueryContext {
	std::unordered_map<std::string, PropertyValue> parameters;
	ExecMode execMode = ExecMode::EM_FULL;
};

} // namespace graph::query
