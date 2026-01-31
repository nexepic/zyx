/**
 * @file PropertyTypes.cpp
 * @author Nexepic
 * @date 2025/3/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
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
					} else if constexpr (std::is_same_v<T, std::vector<float>>) {
						// Size = Count (4 bytes) + Data (N * 4 bytes)
						return sizeof(uint32_t) + (arg.size() * sizeof(float));
					}

					return 0; // Should be unreachable if all types are handled.
				},
				value.getVariant()); // CRITICAL: Operate on the internal variant.
	}

	bool checkPropertyMatch(const PropertyValue &storedValue, const PropertyValue &queryValue) {
		return storedValue == queryValue;
	}

} // namespace graph::property_utils
