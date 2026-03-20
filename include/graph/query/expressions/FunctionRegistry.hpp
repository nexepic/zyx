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
	explicit ScalarFunction(FunctionSignature signature)
		: signature_(std::move(signature)) {}

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
	 * @brief Returns the cached function signature.
	 */
	[[nodiscard]] const FunctionSignature& getSignature() const { return signature_; }

	/**
	 * @brief Validates argument count against signature.
	 */
	[[nodiscard]] bool validateArgCount(size_t argCount) const {
		if (signature_.variadic) {
			return argCount >= signature_.minArgs;
		}
		return argCount >= signature_.minArgs && argCount <= signature_.maxArgs;
	}

private:
	FunctionSignature signature_;
};

/**
 * @brief Function pointer type for scalar function implementations.
 */
using ScalarFnPtr = PropertyValue(*)(const std::vector<PropertyValue>&, const EvaluationContext&);

/**
 * @class LambdaScalarFunction
 * @brief A ScalarFunction that delegates to a function pointer.
 *
 * Eliminates the need for a separate class per simple function.
 */
class LambdaScalarFunction : public ScalarFunction {
public:
	LambdaScalarFunction(FunctionSignature sig, ScalarFnPtr fn)
		: ScalarFunction(std::move(sig)), fn_(fn) {}

	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override {
		return fn_(args, context);
	}

private:
	ScalarFnPtr fn_;
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

/**
 * @class EntityIntrospectionFunction
 * @brief Marker base class for entity introspection functions (id, labels, type, keys, properties).
 *
 * These functions require direct Record access rather than pre-evaluated arguments.
 * The evaluator passes the variable name as a string in args[0] instead of
 * evaluating the argument expression.
 */
class EntityIntrospectionFunction : public ScalarFunction {
public:
	using ScalarFunction::ScalarFunction;
};

/**
 * @class LambdaEntityIntrospectionFunction
 * @brief An EntityIntrospectionFunction that delegates to a function pointer.
 */
class LambdaEntityIntrospectionFunction : public EntityIntrospectionFunction {
public:
	LambdaEntityIntrospectionFunction(FunctionSignature sig, ScalarFnPtr fn)
		: EntityIntrospectionFunction(std::move(sig)), fn_(fn) {}

	[[nodiscard]] PropertyValue evaluate(
		const std::vector<PropertyValue>& args,
		const EvaluationContext& context
	) const override {
		return fn_(args, context);
	}

private:
	ScalarFnPtr fn_;
};

// Factory functions for entity introspection functions
std::unique_ptr<ScalarFunction> makeIdFunction();
std::unique_ptr<ScalarFunction> makeLabelsFunction();
std::unique_ptr<ScalarFunction> makeTypeFunction();
std::unique_ptr<ScalarFunction> makeKeysFunction();
std::unique_ptr<ScalarFunction> makePropertiesFunction();

} // namespace graph::query::expressions
