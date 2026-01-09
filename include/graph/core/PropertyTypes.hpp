/**
 * @file PropertyTypes.hpp
 * @author Nexepic
 * @date 2025/3/25
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

#pragma once

#include <concepts>
#include <cstdint>
#include <sstream>
#include <string>
#include <variant>

namespace graph {

	// The PropertyType enum remains the same.
	enum class PropertyType : uint8_t {
		UNKNOWN = 0,
		NULL_TYPE = 1,
		BOOLEAN = 2,
		INTEGER = 3,
		DOUBLE = 4,
		STRING = 5,
	};

	/**
	 * @struct PropertyValue
	 * @brief A robust wrapper around a std::variant to represent a property's value.
	 */
	struct PropertyValue {
	private:
		// The actual data storage.
		// Order matters for default comparison: null < bool < int < double < string
		using VariantType = std::variant<std::monostate, bool, int64_t, double, std::string>;
		VariantType data;

	public:
		// Default constructor creates a NULL value.
		PropertyValue() : data(std::monostate{}) {}

		// Constructor for NULL type.
		PropertyValue(std::monostate m) : data(m) {}

		// Constructor for booleans.
		PropertyValue(bool value) : data(value) {}

		// Unambiguous constructor for all integral types (short, int, long, etc.).
		template<std::integral T>
		PropertyValue(T value) : data(static_cast<int64_t>(value)) {}

		// Unambiguous constructor for all floating-point types (float, double).
		template<std::floating_point T>
		PropertyValue(T value) : data(static_cast<double>(value)) {}

		// Constructors for string types.
		PropertyValue(const std::string &value) : data(value) {}
		PropertyValue(std::string &&value) : data(std::move(value)) {}
		PropertyValue(const char *value) : data(std::string(value)) {}

		// Expose the underlying variant for pattern matching with std::visit.
		const VariantType &getVariant() const { return data; }

		// --- Comparison Operators (Required for WHERE clause filtering) ---

		bool operator==(const PropertyValue &other) const { return data == other.data; }
		bool operator!=(const PropertyValue &other) const { return data != other.data; }

		// Note: std::variant default comparison compares the index first, then the value.
		// e.g., int(5) < double(3.0) might return true because index(int) < index(double).
		// For a more robust DB, you might want to add type coercion logic here later.
		bool operator<(const PropertyValue &other) const { return data < other.data; }
		bool operator>(const PropertyValue &other) const { return data > other.data; }
		bool operator<=(const PropertyValue &other) const { return data <= other.data; }
		bool operator>=(const PropertyValue &other) const { return data >= other.data; }

		// --- Utilities ---

		std::string toString() const {
			return std::visit(
					[](auto const &value) -> std::string {
						using T = std::decay_t<decltype(value)>;

						if constexpr (std::is_same_v<T, std::monostate>) {
							return "null";
						} else if constexpr (std::is_same_v<T, bool>) {
							return value ? "true" : "false";
						} else if constexpr (std::is_same_v<T, int64_t>) {
							return std::to_string(value);
						} else if constexpr (std::is_same_v<T, double>) {
							std::ostringstream oss;
							oss << value;
							return oss.str();
						} else if constexpr (std::is_same_v<T, std::string>) {
							return value;
						} else {
							return "";
						}
					},
					data);
		}

		// Helper to get type name for debugging
		std::string typeName() const {
			return std::visit(
					[](auto &&arg) -> std::string {
						using T = std::decay_t<decltype(arg)>;
						if constexpr (std::is_same_v<T, std::monostate>)
							return "NULL";
						else if constexpr (std::is_same_v<T, bool>)
							return "BOOLEAN";
						else if constexpr (std::is_same_v<T, int64_t>)
							return "INTEGER";
						else if constexpr (std::is_same_v<T, double>)
							return "DOUBLE";
						else if constexpr (std::is_same_v<T, std::string>)
							return "STRING";
					},
					data);
		}
	};


	// The centralized helper function now needs to operate on the wrapped variant.
	inline PropertyType getPropertyType(const PropertyValue &value) {
		PropertyType type = PropertyType::UNKNOWN;
		std::visit(
				[&type]<typename T0>([[maybe_unused]] const T0 &arg) {
					using T = std::decay_t<T0>;
					if constexpr (std::is_same_v<T, std::monostate>) {
						type = PropertyType::NULL_TYPE;
					} else if constexpr (std::is_same_v<T, bool>) {
						type = PropertyType::BOOLEAN;
					} else if constexpr (std::is_same_v<T, int64_t>) {
						type = PropertyType::INTEGER;
					} else if constexpr (std::is_same_v<T, double>) {
						type = PropertyType::DOUBLE;
					} else if constexpr (std::is_same_v<T, std::string>) {
						type = PropertyType::STRING;
					}
				},
				value.getVariant());
		return type;
	}

	namespace property_utils {
		size_t getPropertyValueSize(const PropertyValue &value);
		bool checkPropertyMatch(const PropertyValue &storedValue, const PropertyValue &queryValue);
	} // namespace property_utils

} // namespace graph
