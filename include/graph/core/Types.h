/**
 * @file Types.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/18
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <variant>

namespace graph {

	using PropertyValue = std::variant<int64_t, double, std::string, std::vector<double>, bool>;

	enum class Direction {
		INCOMING,
		OUTGOING,
		BOTH
	};

} // namespace graph
