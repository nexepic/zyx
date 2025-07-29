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
#include <graph/core/Node.hpp>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace graph::utils {

	class Serializer {
	public:
		template<typename T>
		static void writePOD(std::ostream &os, const T &pod);

		template<typename T>
		static T readPOD(std::istream &is);

		static void writeBuffer(std::ostream &os, const char *buffer, size_t size);
		static void readBuffer(std::istream &is, char *buffer, size_t size);

		static void writeString(std::ostream &os, const std::string &str);
		static std::string readString(std::istream &is);

		static void writePropertyValue(std::ostream &os, const PropertyValue &value);
		static PropertyValue readPropertyValue(std::istream &is);

		template<typename T>
		static void serialize(std::ostream &os, const T &value);

		template<typename T>
		static T deserialize(std::istream &is);

		static PropertyValue deserializeVariant(std::istream &is);
	};

} // namespace graph::utils