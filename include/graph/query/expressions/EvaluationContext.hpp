/**
 * @file EvaluationContext.hpp
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

#pragma once

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/Record.hpp"
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>

namespace graph::query::expressions {

// Forward declaration
class Expression;

/**
 * @class ExpressionEvaluationException
 * @brief Base exception for expression evaluation errors.
 */
class ExpressionEvaluationException : public std::runtime_error {
public:
	explicit ExpressionEvaluationException(const std::string &message)
		: std::runtime_error(message) {}
};

/**
 * @class UndefinedVariableException
 * @brief Exception thrown when a variable reference cannot be resolved.
 */
class UndefinedVariableException : public ExpressionEvaluationException {
public:
	explicit UndefinedVariableException(const std::string &variableName)
		: ExpressionEvaluationException("Undefined variable: " + variableName),
		  variableName_(variableName) {}

	[[nodiscard]] const std::string &getVariableName() const { return variableName_; }

private:
	std::string variableName_;
};

/**
 * @class TypeMismatchException
 * @brief Exception thrown when type conversion fails or operation is invalid for types.
 */
class TypeMismatchException : public ExpressionEvaluationException {
public:
	TypeMismatchException(const std::string &message, PropertyType expected, PropertyType actual)
		: ExpressionEvaluationException(message), expected_(expected), actual_(actual) {}

	[[nodiscard]] PropertyType getExpectedType() const { return expected_; }
	[[nodiscard]] PropertyType getActualType() const { return actual_; }

private:
	PropertyType expected_;
	PropertyType actual_;
};

/**
 * @class DivisionByZeroException
 * @brief Exception thrown when division or modulo by zero is attempted.
 */
class DivisionByZeroException : public ExpressionEvaluationException {
public:
	explicit DivisionByZeroException()
		: ExpressionEvaluationException("Division by zero") {}
};

/**
 * @class EvaluationContext
 * @brief Context for expression evaluation, providing variable resolution and type coercion.
 *
 * The EvaluationContext bridges the Expression AST with the runtime Record data.
 * It maintains a reference to the current Record being evaluated and provides
 * utilities for resolving variables and coercing types.
 */
class EvaluationContext {
public:
	/**
	 * @brief Constructs an evaluation context for a given record.
	 * @param record The record to evaluate expressions against
	 */
	explicit EvaluationContext(const execution::Record &record);

	/**
	 * @brief Resolves a variable reference to its PropertyValue.
	 *
	 * @param variableName The name of the variable (e.g., "n")
	 * @return The Node converted to PropertyValue (or nullopt if not found)
	 */
	[[nodiscard]] std::optional<PropertyValue> resolveVariable(const std::string &variableName) const;

	/**
	 * @brief Resolves a property access to its PropertyValue.
	 *
	 * @param variableName The variable name (e.g., "n")
	 * @param propertyName The property name (e.g., "age")
	 * @return The property value (or nullopt if variable/property not found)
	 */
	[[nodiscard]] std::optional<PropertyValue> resolveProperty(const std::string &variableName,
	                                                           const std::string &propertyName) const;

	/**
	 * @brief Gets the underlying record.
	 */
	[[nodiscard]] const execution::Record &getRecord() const { return record_; }

	/**
	 * @brief Sets a temporary variable for list comprehension iteration.
	 *
	 * This method is used by list comprehensions to temporarily bind
	 * the iteration variable during evaluation of WHERE and MAP expressions.
	 *
	 * @param variableName The name of the variable to set
	 * @param value The value to bind to the variable
	 */
	void setVariable(const std::string &variableName, const PropertyValue &value) const;

	/**
	 * @brief Clears a temporary variable set by setVariable.
	 *
	 * @param variableName The name of the variable to clear
	 */
	void clearVariable(const std::string &variableName) const;

	// =========================================================================
	// Type Coercion Helpers (following Cypher semantics)
	// =========================================================================

	/**
	 * @brief Converts a PropertyValue to boolean (Cypher truthy/falsy rules).
	 *
	 * Rules:
	 * - NULL → false
	 * - boolean → value as-is
	 * - 0 → false, non-zero → true
	 * - "false" → false, "true" → true, other strings → true (unless empty)
	 * - Lists/LIST → false if empty, true if non-empty
	 *
	 * @param value The value to convert
	 * @return Boolean representation
	 * @throws TypeMismatchException if conversion is not possible
	 */
	[[nodiscard]] static bool toBoolean(const PropertyValue &value);

	/**
	 * @brief Converts a PropertyValue to integer.
	 *
	 * Rules:
	 * - NULL → 0
	 * - boolean → 0 or 1
	 * - double → truncated to integer
	 * - string → parsed as integer (throws if not numeric)
	 *
	 * @param value The value to convert
	 * @return Integer representation
	 * @throws TypeMismatchException if conversion is not possible
	 */
	[[nodiscard]] static int64_t toInteger(const PropertyValue &value);

	/**
	 * @brief Converts a PropertyValue to double.
	 *
	 * Rules:
	 * - NULL → 0.0
	 * - boolean → 0.0 or 1.0
	 * - integer → cast to double
	 * - string → parsed as double (throws if not numeric)
	 *
	 * @param value The value to convert
	 * @return Double representation
	 * @throws TypeMismatchException if conversion is not possible
	 */
	[[nodiscard]] static double toDouble(const PropertyValue &value);

	/**
	 * @brief Converts a PropertyValue to string.
	 *
	 * Rules:
	 * - NULL → "null"
	 * - boolean → "true" or "false"
	 * - numeric → decimal string representation
	 * - string → value as-is
	 * - list → "[...]" representation
	 *
	 * @param value The value to convert
	 * @return String representation
	 */
	[[nodiscard]] static std::string toString(const PropertyValue &value);

	/**
	 * @brief Checks if a value is NULL (monostate in PropertyValue).
	 */
	[[nodiscard]] static bool isNull(const PropertyValue &value);

private:
	const execution::Record &record_;
	mutable std::unordered_map<std::string, PropertyValue> temporaryVariables_;
};

} // namespace graph::query::expressions
