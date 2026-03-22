#pragma once

namespace graph::query::expressions {

/**
 * @brief Direction for pattern traversal
 */
enum class PatternDirection {
	PAT_OUTGOING,
	PAT_INCOMING,
	PAT_BOTH
};

} // namespace graph::query::expressions
