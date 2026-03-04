/**
 * @file Serializer.hpp
 * @author Nexepic
 * @date 2025/2/26
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

#include <iostream>
#include <string>
#include <variant>
#include <vector>
#include "graph/core/PropertyTypes.hpp"

namespace graph::utils {

	class Serializer {
	public:
		// --- Core I/O Primitives ---
		template<typename T>
		static void writePOD(std::ostream &os, const T &pod) {
			static_assert(std::is_trivial_v<T>, "writePOD can only be used with POD types.");
			os.write(reinterpret_cast<const char *>(&pod), sizeof(T));
		}

		template<typename T>
		static T readPOD(std::istream &is) {
			static_assert(std::is_trivial_v<T>, "readPOD can only be used with POD types.");
			T pod;
			is.read(reinterpret_cast<char *>(&pod), sizeof(T));
			return pod;
		}

		static void writeBuffer(std::ostream &os, const char *buffer, size_t size) {
			os.write(buffer, static_cast<std::streamsize>(size));
		}

		static void readBuffer(std::istream &is, char *buffer, size_t size) {
			is.read(buffer, static_cast<std::streamsize>(size));
		}

		// --- Generic Serialize/Deserialize ---
		template<typename T>
		static void serialize(std::ostream &os, const T &value) {
			writePOD(os, value);
		}

		template<typename T>
		static T deserialize(std::istream &is) {
			return readPOD<T>(is);
		}

		// --- PropertyValue methods (declared but not defined yet) ---
		static void writePropertyValue(std::ostream &os, const PropertyValue &value);
		static PropertyValue readPropertyValue(std::istream &is);
	};

	// --- Template Specializations ---
	template<>
	inline void Serializer::serialize<std::string>(std::ostream &os, const std::string &value) {
		writePOD(os, static_cast<uint32_t>(value.size()));
		os.write(value.data(), value.size());
	}

	template<>
	inline std::string Serializer::deserialize<std::string>(std::istream &is) {
		const auto size = readPOD<uint32_t>(is);
		std::string str(size, '\0');
		is.read(&str[0], size);
		return str;
	}

	/**
	 * @brief Specialization for serializing PropertyValue.
	 * Format: [PropertyType tag][value data]
	 */
	template<>
	inline void Serializer::serialize<PropertyValue>(std::ostream &os, const PropertyValue &value) {
		std::visit(
				[&os](const auto &arg) {
					using T = std::decay_t<decltype(arg)>;

					// First, write the type tag. This is more efficient than calling getPropertyType.
					if constexpr (std::is_same_v<T, std::monostate>) {
						writePOD(os, PropertyType::NULL_TYPE);
						// No value data for NULL
					} else if constexpr (std::is_same_v<T, bool>) {
						writePOD(os, PropertyType::BOOLEAN);
						writePOD(os, arg); // Write the bool value
					} else if constexpr (std::is_same_v<T, int64_t>) {
						writePOD(os, PropertyType::INTEGER);
						writePOD(os, arg); // Write the int64_t value
					} else if constexpr (std::is_same_v<T, double>) {
						writePOD(os, PropertyType::DOUBLE);
						writePOD(os, arg); // Write the double value
					} else if constexpr (std::is_same_v<T, std::string>) {
						writePOD(os, PropertyType::STRING);
						serialize(os, arg); // Recursively call the std::string specialization
					} else if constexpr (std::is_same_v<T, std::vector<float>>) {
						writePOD(os, PropertyType::LIST);
						writePOD(os, static_cast<uint32_t>(arg.size()));
						if (!arg.empty()) {
							// Write the raw float array directly
							os.write(reinterpret_cast<const char *>(arg.data()), arg.size() * sizeof(float));
						}
					}
				},
				value.getVariant());
	}

	/**
	 * @brief Specialization for deserializing PropertyValue.
	 */
	template<>
	inline PropertyValue Serializer::deserialize<PropertyValue>(std::istream &is) {
		// First, read the type tag to know what to expect next.
		const auto type = readPOD<PropertyType>(is);

		switch (type) {
			case PropertyType::NULL_TYPE:
				return PropertyValue(std::monostate{});
			case PropertyType::BOOLEAN:
				return PropertyValue(readPOD<bool>(is));
			case PropertyType::INTEGER:
				return PropertyValue(readPOD<int64_t>(is));
			case PropertyType::DOUBLE:
				return PropertyValue(readPOD<double>(is));
			case PropertyType::STRING:
				// Call the std::string specialization to read the value
				return PropertyValue(deserialize<std::string>(is));
			case PropertyType::LIST: {
				uint32_t count = readPOD<uint32_t>(is);
				std::vector<float> vec(count);
				if (count > 0) {
					is.read(reinterpret_cast<char *>(vec.data()), count * sizeof(float));
				}
				// Convert std::vector<float> to std::vector<PropertyValue> for heterogeneous list support
				std::vector<PropertyValue> propVec;
				propVec.reserve(count);
				for (float f : vec) {
					propVec.push_back(PropertyValue(static_cast<double>(f)));
				}
				return PropertyValue(propVec);
			}
			default:
				throw std::runtime_error("Invalid PropertyType tag in stream during deserialization.");
		}
	}

	inline size_t getSerializedSize(const PropertyValue &value) {
		size_t size = sizeof(PropertyType);

		std::visit(
				[&size]<typename T0>(const T0 &arg) {
					using T = std::decay_t<T0>;
					if constexpr (std::is_same_v<T, std::monostate>) {
					} else if constexpr (std::is_same_v<T, std::string>) {
						size += sizeof(uint32_t);
						size += arg.length();
					} else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int64_t> ||
										 std::is_same_v<T, double>) {
						size += sizeof(T);
					} else if constexpr (std::is_same_v<T, std::vector<float>>) {
						size += sizeof(uint32_t); // Count
						size += arg.size() * sizeof(float); // Data
					}
				},
				value.getVariant());

		return size;
	}

} // namespace graph::utils
