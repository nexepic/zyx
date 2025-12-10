/**
 * @file PropertyTypes.hpp
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
#include <sstream>

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
	 *
	 * This struct solves the type ambiguity problem by providing explicit, overloaded
	 * constructors. This allows users to create a PropertyValue from literals
	 * (e.g., `PropertyValue a = 30;`) without ambiguity. The complexity of choosing
	 * the correct variant alternative is handled internally.
	 */
	struct PropertyValue {
	private:
		// The actual data storage.
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
		// They all are stored as int64_t.
		template<std::integral T>
		PropertyValue(T value) : data(static_cast<int64_t>(value)) {}

		// Unambiguous constructor for all floating-point types (float, double).
		// They all are stored as double.
		template<std::floating_point T>
		PropertyValue(T value) : data(static_cast<double>(value)) {}

		// Constructors for string types.
		PropertyValue(const std::string &value) : data(value) {}
		PropertyValue(std::string &&value) : data(std::move(value)) {}
		PropertyValue(const char *value) : data(std::string(value)) {}

		// Expose the underlying variant for pattern matching with std::visit.
		const VariantType &getVariant() const { return data; }

		// Overload the equality operator for easy comparison.
		bool operator==(const PropertyValue &other) const { return this->data == other.data; }

		std::string toString() const {
			return std::visit(
				[](auto const& value) -> std::string {
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

					}
				},
				data
			);
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
		// This function would also need to be updated for the new variant definition.
		size_t getPropertyValueSize(const PropertyValue &value);

		bool checkPropertyMatch(const PropertyValue& storedValue, const PropertyValue& queryValue);
	} // namespace property_utils

} // namespace graph
