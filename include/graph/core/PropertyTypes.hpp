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

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace graph {

	// The PropertyType enum remains the same.
	enum class PropertyType : uint8_t {
		UNKNOWN = 0,
		NULL_TYPE = 1,
		BOOLEAN = 2,
		INTEGER = 3,
		DOUBLE = 4,
		STRING = 5,
		LIST = 6, // Support for Vector/List
		MAP = 7,  // Support for Map/Dictionary
		COMPOSITE = 8 // Multi-property composite key
	};

	// CompositeKey is defined after PropertyValue (uses PropertyValue in components)

	/**
	 * @struct PropertyValue
	 * @brief A robust wrapper around a std::variant to represent a property's value.
	 */
	struct PropertyValue {
		using MapType = std::unordered_map<std::string, PropertyValue>;

	private:
		// The actual data storage.
		// Order matters for default comparison: null < bool < int < double < string
		using VariantType = std::variant<std::monostate, bool, int64_t, double, std::string, std::vector<PropertyValue>, MapType>;
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

		explicit PropertyValue(std::vector<PropertyValue> vec) : data(std::move(vec)) {}

		explicit PropertyValue(MapType map) : data(std::move(map)) {}

		// Expose the underlying variant for pattern matching with std::visit.
		const VariantType &getVariant() const { return data; }

		// --- Comparison Operators (Required for WHERE clause filtering) ---

		bool operator==(const PropertyValue &other) const { return data == other.data; }
		bool operator!=(const PropertyValue &other) const { return data != other.data; }

		// Custom ordering: variant index-based comparison, with MAP type
		// throwing on ordering (maps are not orderable, only equality).
		bool operator<(const PropertyValue &other) const {
			if (data.index() != other.data.index()) return data.index() < other.data.index();
			// Same type — MAP is not orderable
			if (std::holds_alternative<MapType>(data)) return false;
			// For non-MAP types, delegate to variant comparison
			return std::visit([&other]<typename T>(const T& val) -> bool {
				if constexpr (std::is_same_v<std::decay_t<T>, MapType>) {
					return false;
				} else {
					return val < std::get<T>(other.data);
				}
			}, data);
		}
		bool operator>(const PropertyValue &other) const { return other < *this; }
		bool operator<=(const PropertyValue &other) const { return !(other < *this); }
		bool operator>=(const PropertyValue &other) const { return !(*this < other); }

		// --- Utilities ---

		std::string toString() const {
			return std::visit(
					[]<typename T0>(T0 const &value) -> std::string {
						using T = std::decay_t<T0>;

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
						} else if constexpr (std::is_same_v<T, std::vector<PropertyValue>>) {
							std::ostringstream oss;
							oss << "[";
							for (size_t i = 0; i < value.size(); ++i) {
								oss << value[i].toString() << (i < value.size() - 1 ? ", " : "");
							}
							oss << "]";
							return oss.str();
						} else if constexpr (std::is_same_v<T, MapType>) {
							std::ostringstream oss;
							oss << "{";
							bool first = true;
							for (const auto& [k, v] : value) {
								if (!first) oss << ", ";
								oss << k << ": " << v.toString();
								first = false;
							}
							oss << "}";
							return oss.str();
						} else {
							return "";
						}
					},
					data);
		}

		// Helper to get type name for debugging
		std::string typeName() const {
			return std::visit(
					[]<typename T0>([[maybe_unused]] T0 &&arg) -> std::string {
						using T = std::decay_t<T0>;
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
						else if constexpr (std::is_same_v<T, std::vector<PropertyValue>>)
							return "LIST";
						else if constexpr (std::is_same_v<T, MapType>)
							return "MAP";
						else
							return "UNKNOWN";
					},
					data);
		}

		/**
		 * @brief Returns the type of the stored value.
		 */
		PropertyType getType() const {
			if (std::holds_alternative<std::monostate>(data))
				return PropertyType::NULL_TYPE;
			if (std::holds_alternative<bool>(data))
				return PropertyType::BOOLEAN;
			if (std::holds_alternative<int64_t>(data))
				return PropertyType::INTEGER;
			if (std::holds_alternative<double>(data))
				return PropertyType::DOUBLE;
			if (std::holds_alternative<std::string>(data))
				return PropertyType::STRING;
			if (std::holds_alternative<std::vector<PropertyValue>>(data))
				return PropertyType::LIST;
			if (std::holds_alternative<MapType>(data))
				return PropertyType::MAP;
			return PropertyType::UNKNOWN;
		}

		/**
		 * @brief Retrieves the list value if the type is LIST.
		 * @throws std::runtime_error if the type is not LIST.
		 */
		const std::vector<PropertyValue> &getList() const {
			if (auto *val = std::get_if<std::vector<PropertyValue>>(&data)) {
				return *val;
			}
			throw std::runtime_error("PropertyValue is not a List/Vector");
		}

		const MapType &getMap() const {
			if (auto *val = std::get_if<MapType>(&data)) {
				return *val;
			}
			throw std::runtime_error("PropertyValue is not a Map");
		}
	};

	/**
	 * @struct CompositeKey
	 * @brief A multi-component key for composite indexes.
	 *
	 * Supports lexicographic comparison for prefix matching and range scans.
	 * NOT stored in PropertyValue variant — used only at the index layer.
	 */
	struct CompositeKey {
		std::vector<PropertyValue> components;

		bool operator==(const CompositeKey &other) const {
			return components == other.components;
		}

		bool operator<(const CompositeKey &other) const {
			size_t len = std::min(components.size(), other.components.size());
			for (size_t i = 0; i < len; ++i) {
				if (components[i] < other.components[i]) return true;
				if (other.components[i] < components[i]) return false;
			}
			return components.size() < other.components.size();
		}

		bool operator>(const CompositeKey &other) const { return other < *this; }
		bool operator<=(const CompositeKey &other) const { return !(other < *this); }
		bool operator>=(const CompositeKey &other) const { return !(*this < other); }

		std::string toString() const {
			std::ostringstream oss;
			oss << "(";
			for (size_t i = 0; i < components.size(); ++i) {
				if (i > 0) oss << ", ";
				oss << components[i].toString();
			}
			oss << ")";
			return oss.str();
		}
	};

	// The centralized helper function now needs to operate on the wrapped variant.
	inline PropertyType getPropertyType(const PropertyValue &value) {
		auto type = PropertyType::UNKNOWN;
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
					} else if constexpr (std::is_same_v<T, std::vector<PropertyValue>>) {
						type = PropertyType::LIST;
					} else if constexpr (std::is_same_v<T, PropertyValue::MapType>) {
						type = PropertyType::MAP;
					}
				},
				value.getVariant());
		return type;
	}

	/**
	 * @struct PropertyValueHash
	 * @brief Hash functor for PropertyValue, enabling use in unordered containers.
	 */
	struct PropertyValueHash {
		size_t operator()(const PropertyValue &value) const {
			return std::visit(
				[](const auto &arg) -> size_t {
					using T = std::decay_t<decltype(arg)>;
					if constexpr (std::is_same_v<T, std::monostate>) {
						return 0;
					} else if constexpr (std::is_same_v<T, bool>) {
						return std::hash<bool>{}(arg);
					} else if constexpr (std::is_same_v<T, int64_t>) {
						return std::hash<int64_t>{}(arg);
					} else if constexpr (std::is_same_v<T, double>) {
						return std::hash<double>{}(arg);
					} else if constexpr (std::is_same_v<T, std::string>) {
						return std::hash<std::string>{}(arg);
					} else if constexpr (std::is_same_v<T, std::vector<PropertyValue>>) {
						size_t seed = arg.size();
						for (const auto &elem : arg) {
							seed ^= PropertyValueHash{}(elem) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
						}
						return seed;
					} else if constexpr (std::is_same_v<T, PropertyValue::MapType>) {
						size_t seed = arg.size();
						for (const auto &[k, v] : arg) {
							// XOR so order doesn't matter (unordered_map)
							seed ^= std::hash<std::string>{}(k) ^ PropertyValueHash{}(v);
						}
						return seed;
					} else {
						return 0;
					}
				},
				value.getVariant());
		}
	};

	namespace property_utils {
		size_t getPropertyValueSize(const PropertyValue &value);
		bool checkPropertyMatch(const PropertyValue &storedValue, const PropertyValue &queryValue);
	} // namespace property_utils

} // namespace graph
