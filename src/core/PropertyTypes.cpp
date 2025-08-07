/**
 * @file PropertyTypes.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/PropertyTypes.hpp"
#include <variant>

namespace graph::property_utils {

	/**
	 * @brief Calculates the estimated memory size of a PropertyValue.
	 *
	 * This implementation uses std::visit for a robust, efficient, and type-safe
	 * way to handle all alternatives within the PropertyValue's underlying variant.
	 *
	 * @param value The PropertyValue object to measure.
	 * @return The estimated size in bytes.
	 */
	size_t getPropertyValueSize(const PropertyValue &value) {
		// Use std::visit to apply a lambda to the active variant member.
		// This is the modern, preferred way to handle variants.
		return std::visit(
				[]<typename T0>(const T0 &arg) -> size_t {
					// std::decay_t<decltype(arg)> gets the clean, underlying type.
					using T = std::decay_t<T0>;

					if constexpr (std::is_same_v<T, std::monostate>) {
						// A NULL value has a minimal size.
						return 1;
					} else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int64_t> ||
										 std::is_same_v<T, double>) {
						// For fixed-size primitive types.
						return sizeof(T);
					} else if constexpr (std::is_same_v<T, std::string>) {
						// For a string, estimate size as its character data plus overhead.
						// This matches your original logic.
						return arg.size() + sizeof(size_t);
					}

					return 0; // Should be unreachable if all types are handled.
				},
				value.getVariant()); // CRITICAL: Operate on the internal variant.
	}

	bool checkPropertyMatch(const PropertyValue &storedValue, const PropertyValue &queryValue) {
		return storedValue == queryValue;
	}

} // namespace graph::property_utils
