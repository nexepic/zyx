/**
 * @file value.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/16
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <string>
#include <variant>
#include <vector>
#include <cstdint>

namespace metrix {

	// Using standard types for ABI stability
	using Value = std::variant<std::monostate, bool, int64_t, double, std::string>;

	// Helper to create values easily
	inline Value null() { return std::monostate{}; }

	// Public Node Representation
	struct Node {
		int64_t id;
		std::string label;
		std::unordered_map<std::string, Value> properties;
	};

	// Public Edge Representation
	struct Edge {
		int64_t id;
		int64_t sourceId;
		int64_t targetId;
		std::string label;
		std::unordered_map<std::string, Value> properties;
	};

} // namespace metrix