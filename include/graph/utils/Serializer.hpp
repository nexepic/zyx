/**
 * @file Serializer.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <iostream>
#include <string>
#include <variant>
#include <vector>

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

	// --- PropertyValue method implementations ---
	inline void Serializer::writePropertyValue(std::ostream &os, const PropertyValue &value) {
		std::visit(
				[&os](const auto &arg) {
					const PropertyType type = getPropertyType(arg);
					serialize(os, type);
					using T = std::decay_t<decltype(arg)>;
					if constexpr (!std::is_same_v<T, std::monostate>) {
						serialize(os, arg);
					}
				},
				value.getVariant());
	}

	inline PropertyValue Serializer::readPropertyValue(std::istream &is) {
		switch (deserialize<PropertyType>(is)) {
			case PropertyType::NULL_TYPE:
				return PropertyValue(std::monostate{});
			case PropertyType::BOOLEAN:
				return PropertyValue(deserialize<bool>(is));
			case PropertyType::INTEGER:
				return PropertyValue(deserialize<int64_t>(is));
			case PropertyType::DOUBLE:
				return PropertyValue(deserialize<double>(is));
			case PropertyType::STRING:
				return PropertyValue(deserialize<std::string>(is));
			default:
				throw std::runtime_error("Invalid PropertyType tag in stream.");
		}
	}

} // namespace graph::utils
