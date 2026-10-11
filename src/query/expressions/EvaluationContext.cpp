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
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace graph::query::expressions {

EvaluationContext::EvaluationContext(const execution::Record &record)
	: record_(record) {}

EvaluationContext::EvaluationContext(const execution::Record &record,
                                     const std::unordered_map<std::string, PropertyValue> &parameters,
                                     const ScalarFunctionEvaluationLimits limits)
	: record_(record), parameters_(&parameters), limits_(limits) {}

std::optional<PropertyValue> EvaluationContext::resolveParameter(const std::string &name) const {
	if (!parameters_) return std::nullopt;
	auto it = parameters_->find(name);
	if (it != parameters_->end()) return it->second;
	return std::nullopt;
}

std::optional<PropertyValue> EvaluationContext::resolveVariable(const std::string &variableName) const {
	// Check temporary variables first (for list comprehensions)
	auto it = temporaryVariables_.find(variableName);
	if (it != temporaryVariables_.end()) {
		return it->second;
	}

	// Try to get as a Node first
	if (auto node = record_.getNodeRef(variableName)) {
		return PropertyValue(node->get().getId());
	}

	// Try to get as an Edge
	if (auto edge = record_.getEdgeRef(variableName)) {
		return PropertyValue(edge->get().getId());
	}

	// Try to get as a computed value
	if (auto value = record_.getValueRef(variableName)) {
		return value->get();
	}

	return std::nullopt;
}

std::optional<PropertyValue> EvaluationContext::resolveProperty(const std::string &variableName,
                                                                const std::string &propertyName) const {
	// Try to get property from a Node
	if (auto node = record_.getNodeRef(variableName)) {
		const auto &props = node->get().getProperties();
		auto it = props.find(propertyName);
		if (it != props.end()) {
			return it->second;
		}
		return std::nullopt;
	}

	// Try to get property from an Edge
	if (auto edge = record_.getEdgeRef(variableName)) {
		const auto &props = edge->get().getProperties();
		auto it = props.find(propertyName);
		if (it != props.end()) {
			return it->second;
		}
		return std::nullopt;
	}

	// Try map property access: row.name where row is a MAP PropertyValue
	if (auto val = record_.getValueRef(variableName)) {
		if (val->get().getType() == PropertyType::MAP) {
			const auto &map = val->get().getMap();
			auto it = map.find(propertyName);
			if (it != map.end()) {
				return it->second;
			}
			return std::nullopt;
		}
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
				if (!std::isfinite(arg)) {
					throw TypeMismatchException(
							"Cannot convert a non-finite DOUBLE to BOOLEAN",
							PropertyType::BOOLEAN,
							PropertyType::DOUBLE
					);
				}
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
			} else {
				// Temporal types → true (non-null)
				return true;
			}
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
				// A floating-point conversion outside the integer domain is undefined
				// for a C++ cast. Reject it before conversion rather than exposing a
				// platform-dependent integer to the query layer.
				if (!std::isfinite(arg) || arg < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
					arg >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
					throw TypeMismatchException(
							"Cannot convert a non-finite or out-of-range DOUBLE to INTEGER",
							PropertyType::INTEGER,
							PropertyType::DOUBLE
					);
				}
				// double → truncate after the representable-range check above.
				return static_cast<int64_t>(arg);
			} else if constexpr (std::is_same_v<TPlain, std::string>) {
				// string → parse as integer
				// First try as decimal
				int64_t result = 0;
				auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), result);
				if (ec == std::errc() && ptr == arg.data() + arg.size()) {
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
			} else {
				throw TypeMismatchException(
					"Cannot convert temporal type to INTEGER",
					PropertyType::INTEGER,
					PropertyType::UNKNOWN
				);
			}
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
				if (!std::isfinite(arg)) {
					throw TypeMismatchException(
							"Cannot convert a non-finite DOUBLE to DOUBLE",
							PropertyType::DOUBLE,
							PropertyType::DOUBLE
					);
				}
				// Finite double → as-is
				return arg;
			} else if constexpr (std::is_same_v<TPlain, std::string>) {
				// string → parse as double
				errno = 0;
				char *end = nullptr;
				double result = std::strtod(arg.c_str(), &end);
				if (end == arg.c_str() + arg.size() && errno != ERANGE && std::isfinite(result)) {
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
			} else {
				throw TypeMismatchException(
					"Cannot convert temporal type to DOUBLE",
					PropertyType::DOUBLE,
					PropertyType::UNKNOWN
				);
			}
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

void EvaluationContext::requireCollectionCapacity(const size_t nextSize, const char *const functionName) const {
	if (limits_.maximum_collection_elements != 0U && nextSize > limits_.maximum_collection_elements) {
		throw std::runtime_error(std::string(functionName) + "() exceeded the configured List bound");
	}
}

void EvaluationContext::setVariable(const std::string &variableName, const PropertyValue &value) const {
	temporaryVariables_[variableName] = value;
}

void EvaluationContext::clearVariable(const std::string &variableName) const {
	temporaryVariables_.erase(variableName);
}

} // namespace graph::query::expressions
