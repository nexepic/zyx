/**
 * @file PropertyValue.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/25
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace graph {

	// For large values, we'll use this reference type
	using PropertyValue = std::variant<std::monostate, bool, int32_t, int64_t, float, double, std::string, std::vector<int64_t>,
									   std::vector<double>, std::vector<std::string>>;

	// Helper functions for property values
	namespace property_utils {
		// Get estimated size of a property value in memory
		size_t getPropertyValueSize(const PropertyValue &value);
	} // namespace property_utils

} // namespace graph