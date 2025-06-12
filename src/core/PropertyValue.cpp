/**
 * @file PropertyValue.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/PropertyValue.hpp"
#include <zlib.h>

namespace graph::property_utils {

	size_t getPropertyValueSize(const PropertyValue &value) {
		if (std::holds_alternative<std::monostate>(value)) {
			return 1;
		}
		if (std::holds_alternative<bool>(value)) {
			return sizeof(bool);
		}
		if (std::holds_alternative<int32_t>(value)) {
			return sizeof(int32_t);
		}
		if (std::holds_alternative<int64_t>(value)) {
			return sizeof(int64_t);
		}
		if (std::holds_alternative<float>(value)) {
			return sizeof(float);
		}
		if (std::holds_alternative<double>(value)) {
			return sizeof(double);
		}
		if (std::holds_alternative<std::string>(value)) {
			return std::get<std::string>(value).size() + sizeof(size_t);
		}
		if (std::holds_alternative<std::vector<int64_t>>(value)) {
			const auto &vec = std::get<std::vector<int64_t>>(value);
			return vec.size() * sizeof(int64_t) + sizeof(size_t);
		}
		if (std::holds_alternative<std::vector<double>>(value)) {
			const auto &vec = std::get<std::vector<double>>(value);
			return vec.size() * sizeof(double) + sizeof(size_t);
		}
		if (std::holds_alternative<std::vector<std::string>>(value)) {
			size_t size = sizeof(size_t);
			const auto &vec = std::get<std::vector<std::string>>(value);
			for (const auto &str: vec) {
				size += str.size() + sizeof(size_t);
			}
			return size;
		}

		return 0;
	}

} // namespace graph::property_utils
