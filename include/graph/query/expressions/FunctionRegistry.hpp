/**
 * @file FunctionRegistry.hpp
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
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <memory>

namespace graph::query::expressions {

class EvaluationContext;

/**
 * @class FunctionSignature
 * @brief Describes the signature of a Cypher function.
 */
struct FunctionSignature {
	std::string name;
	size_t minArgs;
	size_t maxArgs;
	bool variadic;

	FunctionSignature(std::string n, size_t min, size_t max, bool var = false)
		: name(std::move(n)), minArgs(min), maxArgs(max), variadic(var) {}
};

/**
 * @class ScalarFunction
 * @brief Base class for scalar functions (functions that return a single value).
 *
 * Scalar functions operate on a single row of data and return a single value.
 * Examples: toString(), upper(), lower(), abs(), sqrt(), coalesce()
 */
class ScalarFunction {
public:
	virtual ~ScalarFunction() = default;

	/**
	 * @brief Evaluates the function with the given arguments.
	 * @param args The function arguments
	 * @param context The evaluation context (for variable resolution)
	 * @return The computed result
	 */
	[[nodiscard]] virtual PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const = 0;

	/**
	 * @brief Returns the function signature.
	 */
	[[nodiscard]] virtual FunctionSignature getSignature() const = 0;

	/**
	 * @brief Validates argument count against signature.
	 */
	[[nodiscard]] bool validateArgCount(size_t argCount) const {
		const auto& sig = getSignature();
		if (sig.variadic) {
			return argCount >= sig.minArgs;
		}
		return argCount >= sig.minArgs && argCount <= sig.maxArgs;
	}
};

/**
 * @class FunctionRegistry
 * @brief Registry for all Cypher functions (scalar and aggregate).
 *
 * The FunctionRegistry is responsible for:
 * - Registering built-in functions
 * - Looking up functions by name
 * - Validating function calls
 * - Dispatching function execution
 *
 * Thread-safe: singleton pattern with lazy initialization.
 */
class FunctionRegistry {
public:
	/**
	 * @brief Gets the singleton instance of the FunctionRegistry.
	 */
	static FunctionRegistry& getInstance();

	/**
	 * @brief Registers a scalar function.
	 * @param function The function to register (takes ownership)
	 */
	void registerFunction(std::unique_ptr<ScalarFunction> function);

	/**
	 * @brief Looks up a scalar function by name.
	 * @param name The function name (case-insensitive)
	 * @return Pointer to the function, or nullptr if not found
	 */
	[[nodiscard]] const ScalarFunction* lookupScalarFunction(const std::string& name) const;

	/**
	 * @brief Checks if a function exists.
	 * @param name The function name (case-insensitive)
	 */
	[[nodiscard]] bool hasFunction(const std::string& name) const;

	/**
	 * @brief Returns all registered function names (for debugging/testing).
	 */
	[[nodiscard]] std::vector<std::string> getRegisteredFunctionNames() const;

	// Delete copy and move operations (singleton)
	FunctionRegistry(const FunctionRegistry&) = delete;
	FunctionRegistry& operator=(const FunctionRegistry&) = delete;
	FunctionRegistry(FunctionRegistry&&) = delete;
	FunctionRegistry& operator=(FunctionRegistry&&) = delete;

private:
	FunctionRegistry();
	void initializeBuiltinFunctions();

	std::unordered_map<std::string, std::unique_ptr<ScalarFunction>> scalarFunctions_;
};

// ============================================================================
// Built-in Scalar Functions
// ============================================================================

/**
 * @class ToStringFunction
 * @brief Converts any value to its string representation.
 * Signature: toString(value :: ANY) :: STRING
 */
class ToStringFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("toString", 1, 1);
	}
};

/**
 * @class UpperFunction
 * @brief Converts a string to uppercase.
 * Signature: upper(text :: STRING) :: STRING
 */
class UpperFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("upper", 1, 1);
	}
};

/**
 * @class LowerFunction
 * @brief Converts a string to lowercase.
 * Signature: lower(text :: STRING) :: STRING
 */
class LowerFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("lower", 1, 1);
	}
};

/**
 * @class SubstringFunction
 * @brief Extracts a substring from a string.
 * Signature: substring(original :: STRING, start :: INTEGER, length :: INTEGER?) :: STRING
 */
class SubstringFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("substring", 2, 3);
	}
};

/**
 * @class TrimFunction
 * @brief Removes leading and trailing whitespace from a string.
 * Signature: trim(text :: STRING) :: STRING
 */
class TrimFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("trim", 1, 1);
	}
};

/**
 * @class LengthFunction
 * @brief Returns the length of a string or size of a list.
 * Signature: length(value :: STRING | LIST) :: INTEGER
 */
class LengthFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("length", 1, 1);
	}
};

/**
 * @class StartsWithFunction
 * @brief Checks if a string starts with a prefix.
 * Signature: startsWith(text :: STRING, prefix :: STRING) :: BOOLEAN
 */
class StartsWithFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("startsWith", 2, 2);
	}
};

/**
 * @class EndsWithFunction
 * @brief Checks if a string ends with a suffix.
 * Signature: endsWith(text :: STRING, suffix :: STRING) :: BOOLEAN
 */
class EndsWithFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("endsWith", 2, 2);
	}
};

/**
 * @class ContainsFunction
 * @brief Checks if a string contains a substring.
 * Signature: contains(text :: STRING, substring :: STRING) :: BOOLEAN
 */
class ContainsFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("contains", 2, 2);
	}
};

/**
 * @class ReplaceFunction
 * @brief Replaces all occurrences of a substring in a string.
 * Signature: replace(original :: STRING, search :: STRING, replace :: STRING) :: STRING
 */
class ReplaceFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("replace", 3, 3);
	}
};

/**
 * @class AbsFunction
 * @brief Returns the absolute value of a number.
 * Signature: abs(value :: INTEGER | FLOAT) :: INTEGER | FLOAT
 */
class AbsFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("abs", 1, 1);
	}
};

/**
 * @class CeilFunction
 * @brief Rounds a number up to the nearest integer.
 * Signature: ceil(value :: INTEGER | FLOAT) :: FLOAT
 */
class CeilFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("ceil", 1, 1);
	}
};

/**
 * @class FloorFunction
 * @brief Rounds a number down to the nearest integer.
 * Signature: floor(value :: INTEGER | FLOAT) :: FLOAT
 */
class FloorFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("floor", 1, 1);
	}
};

/**
 * @class RoundFunction
 * @brief Rounds a number to the nearest integer.
 * Signature: round(value :: INTEGER | FLOAT) :: FLOAT
 */
class RoundFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("round", 1, 1);
	}
};

/**
 * @class SqrtFunction
 * @brief Returns the square root of a number.
 * Signature: sqrt(value :: INTEGER | FLOAT) :: FLOAT
 */
class SqrtFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("sqrt", 1, 1);
	}
};

/**
 * @class SignFunction
 * @brief Returns the sign of a number (-1, 0, or 1).
 * Signature: sign(value :: INTEGER | FLOAT) :: INTEGER
 */
class SignFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("sign", 1, 1);
	}
};

/**
 * @class CoalesceFunction
 * @brief Returns the first non-NULL value from a list of arguments.
 * Signature: coalesce(value1 :: ANY?, value2 :: ANY?, ...) :: ANY
 */
class CoalesceFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("coalesce", 1, SIZE_MAX, true);
	}
};

/**
 * @class SizeFunction
 * @brief Returns the size of a list or string.
 * Signature: size(value :: LIST | STRING) :: INTEGER
 */
class SizeFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("size", 1, 1);
	}
};

/**
 * @class RangeFunction
 * @brief Generates a list of values in a specified range.
 * Signature: range(start :: INTEGER, end :: INTEGER, step :: INTEGER?) :: LIST<INTEGER>
 *
 * Creates a list of integers from start (inclusive) to end (exclusive).
 * If step is not specified, defaults to 1.
 * If step is positive, generates incrementing values.
 * If step is negative, generates decrementing values.
 * If step is 0, throws an error.
 */
class RangeFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		return FunctionSignature("range", 2, 3);
	}
};

/**
 * @class ReduceFunction
 * @brief Reduces a list to a single value using an accumulator expression.
 * Signature: reduce(accumulator = initial, variable IN list | expression) :: ANY
 *
 * NOTE: This is a partial implementation. REDUCE has special Cypher syntax that requires
 * custom parsing in ExpressionBuilder. Currently, this function validates arguments and
 * throws a "not yet implemented" exception.
 *
 * Full implementation requires:
 * - Special parsing for: reduce(accum = init, x IN list | expression)
 * - Expression evaluation with accumulator variable binding
 * - Iteration over list elements
 *
 * Example: reduce(total = 0, x IN [1, 2, 3] | total + x) returns 6
 */
class ReduceFunction : public ScalarFunction {
public:
	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override;

	[[nodiscard]] FunctionSignature getSignature() const override {
		// REDUCE has special syntax, signature is for validation only
		return FunctionSignature("reduce", 1, SIZE_MAX, true);
	}
};

} // namespace graph::query::expressions
