/**
 * @file EvaluationContext.cpp
 * @author ZYX Contributors
 * @date 2025
 *
 * @copyright Copyright (c) 2025
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

#include "graph/query/expressions/EvaluationContext.hpp"
#include <charconv>
#include <cstdlib>

namespace graph::query::expressions {

EvaluationContext::EvaluationContext(const execution::Record &record)
	: record_(record), dataManager_(nullptr) {}

EvaluationContext::EvaluationContext(const execution::Record &record,
                                     storage::DataManager *dataManager)
	: record_(record), dataManager_(dataManager) {}

std::optional<PropertyValue> EvaluationContext::resolveVariable(const std::string &variableName) const {
	// Check temporary variables first (for list comprehensions)
	auto it = temporaryVariables_.find(variableName);
	if (it != temporaryVariables_.end()) {
		return it->second;
	}

	// Try to get as a Node first
	if (auto node = record_.getNode(variableName)) {
		// For now, convert Node to a simple representation
		// In a full implementation, you might return a special type
		return PropertyValue(node->getId());
	}

	// Try to get as an Edge
	if (auto edge = record_.getEdge(variableName)) {
		return PropertyValue(edge->getId());
	}

	// Try to get as a computed value
	if (auto value = record_.getValue(variableName)) {
		return *value;
	}

	return std::nullopt;
}

std::optional<PropertyValue> EvaluationContext::resolveProperty(const std::string &variableName,
                                                                const std::string &propertyName) const {
	// Try to get property from a Node
	if (auto node = record_.getNode(variableName)) {
		const auto &props = node->getProperties();
		auto it = props.find(propertyName);
		if (it != props.end()) {
			return it->second;
		}
		return std::nullopt;
	}

	// Try to get property from an Edge
	if (auto edge = record_.getEdge(variableName)) {
		const auto &props = edge->getProperties();
		auto it = props.find(propertyName);
		if (it != props.end()) {
			return it->second;
		}
		return std::nullopt;
	}

	return std::nullopt;
}

bool EvaluationContext::toBoolean(const PropertyValue &value) {
	return std::visit(
		[]<typename T>(const T &arg) -> bool {
			using TPlain = std::decay_t<T>;

			if constexpr (std::is_same_v<TPlain, std::monostate>) {
				// NULL → false
				return false;
			} else if constexpr (std::is_same_v<TPlain, bool>) {
				// boolean → value as-is
				return arg;
			} else if constexpr (std::is_same_v<TPlain, int64_t>) {
				// 0 → false, non-zero → true
				return arg != 0;
			} else if constexpr (std::is_same_v<TPlain, double>) {
				// 0.0 → false, non-zero → true
				return arg != 0.0;
			} else if constexpr (std::is_same_v<TPlain, std::string>) {
				// "false" → false, "true" → true, non-empty strings → true
				if (arg == "false") return false;
				if (arg == "true") return true;
				return !arg.empty();
			} else if constexpr (std::is_same_v<TPlain, std::vector<PropertyValue>>) {
				// Empty list → false, non-empty → true
				return !arg.empty();
			} else if constexpr (std::is_same_v<TPlain, PropertyValue::MapType>) {
				// Empty map → false, non-empty → true
				return !arg.empty();
			}
			return false;
		},
		value.getVariant());
}

int64_t EvaluationContext::toInteger(const PropertyValue &value) {
	return std::visit(
		[]<typename T>(const T &arg) -> int64_t {
			using TPlain = std::decay_t<T>;

			if constexpr (std::is_same_v<TPlain, std::monostate>) {
				// NULL → 0
				return 0;
			} else if constexpr (std::is_same_v<TPlain, bool>) {
				// boolean → 0 or 1
				return arg ? 1 : 0;
			} else if constexpr (std::is_same_v<TPlain, int64_t>) {
				// integer → as-is
				return arg;
			} else if constexpr (std::is_same_v<TPlain, double>) {
				// double → truncate
				return static_cast<int64_t>(arg);
			} else if constexpr (std::is_same_v<TPlain, std::string>) {
				// string → parse as integer
				// First try as decimal
				int64_t result = 0;
				auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), result);
				if (ec == std::errc()) {
					return result;
				}
				throw TypeMismatchException(
					"Cannot convert string '" + arg + "' to integer",
					PropertyType::INTEGER,
					PropertyType::STRING
				);
			} else if constexpr (std::is_same_v<TPlain, std::vector<PropertyValue>>) {
				throw TypeMismatchException(
					"Cannot convert LIST to INTEGER",
					PropertyType::INTEGER,
					PropertyType::LIST
				);
			} else if constexpr (std::is_same_v<TPlain, PropertyValue::MapType>) {
				throw TypeMismatchException(
					"Cannot convert MAP to INTEGER",
					PropertyType::INTEGER,
					PropertyType::MAP
				);
			}
			return 0;
		},
		value.getVariant());
}

double EvaluationContext::toDouble(const PropertyValue &value) {
	return std::visit(
		[]<typename T>(const T &arg) -> double {
			using TPlain = std::decay_t<T>;

			if constexpr (std::is_same_v<TPlain, std::monostate>) {
				// NULL → 0.0
				return 0.0;
			} else if constexpr (std::is_same_v<TPlain, bool>) {
				// boolean → 0.0 or 1.0
				return arg ? 1.0 : 0.0;
			} else if constexpr (std::is_same_v<TPlain, int64_t>) {
				// integer → cast to double
				return static_cast<double>(arg);
			} else if constexpr (std::is_same_v<TPlain, double>) {
				// double → as-is
				return arg;
			} else if constexpr (std::is_same_v<TPlain, std::string>) {
				// string → parse as double
				char *end = nullptr;
				double result = std::strtod(arg.c_str(), &end);
				if (end == arg.c_str() + arg.size()) {
					return result;
				}
				throw TypeMismatchException(
					"Cannot convert string '" + arg + "' to double",
					PropertyType::DOUBLE,
					PropertyType::STRING
				);
			} else if constexpr (std::is_same_v<TPlain, std::vector<PropertyValue>>) {
				throw TypeMismatchException(
					"Cannot convert LIST to DOUBLE",
					PropertyType::DOUBLE,
					PropertyType::LIST
				);
			} else if constexpr (std::is_same_v<TPlain, PropertyValue::MapType>) {
				throw TypeMismatchException(
					"Cannot convert MAP to DOUBLE",
					PropertyType::DOUBLE,
					PropertyType::MAP
				);
			}
			return 0.0;
		},
		value.getVariant());
}

std::string EvaluationContext::toString(const PropertyValue &value) {
	// PropertyValue already has a toString() method
	return value.toString();
}

bool EvaluationContext::isNull(const PropertyValue &value) {
	return std::holds_alternative<std::monostate>(value.getVariant());
}

void EvaluationContext::setVariable(const std::string &variableName, const PropertyValue &value) const {
	temporaryVariables_[variableName] = value;
}

void EvaluationContext::clearVariable(const std::string &variableName) const {
	temporaryVariables_.erase(variableName);
}

} // namespace graph::query::expressions
