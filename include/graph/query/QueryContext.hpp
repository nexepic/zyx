/**
 * @file QueryContext.hpp
 * @brief Execution-time context threaded through the query pipeline.
 **/

#pragma once

#include "graph/core/PropertyTypes.hpp"
#include <string>
#include <unordered_map>

namespace graph::query {

/**
 * @brief Holds execution-time context that flows through the query pipeline.
 *
 * Currently carries query parameters; extensible for future needs
 * (timeout, memory limits, etc.).
 */
struct QueryContext {
	std::unordered_map<std::string, PropertyValue> parameters;
};

} // namespace graph::query
