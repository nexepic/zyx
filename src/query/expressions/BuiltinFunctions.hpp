/**
 * @file BuiltinFunctions.hpp
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

#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include <memory>

namespace graph::query::expressions {

inline std::unique_ptr<ScalarFunction> makeFn(const char* name, size_t minArgs, size_t maxArgs, ScalarFnPtr fn, bool variadic = false) {
	return std::make_unique<LambdaScalarFunction>(FunctionSignature(name, minArgs, maxArgs, variadic), fn);
}

void registerStringFunctions(FunctionRegistry& registry);
void registerMathFunctions(FunctionRegistry& registry);
void registerListFunctions(FunctionRegistry& registry);
void registerTypeConversionFunctions(FunctionRegistry& registry);
void registerTemporalFunctions(FunctionRegistry& registry);
void registerUtilityFunctions(FunctionRegistry& registry);

} // namespace graph::query::expressions
