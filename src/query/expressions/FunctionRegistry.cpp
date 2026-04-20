/**
 * @file FunctionRegistry.cpp
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

#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/expressions/AllFunction.hpp"
#include "graph/query/expressions/AnyFunction.hpp"
#include "graph/query/expressions/NoneFunction.hpp"
#include "graph/query/expressions/SingleFunction.hpp"
#include "BuiltinFunctions.hpp"
#include <algorithm>
#include <cctype>

namespace graph::query::expressions {

// ============================================================================
// FunctionRegistry Implementation
// ============================================================================

FunctionRegistry& FunctionRegistry::getInstance() {
	static FunctionRegistry instance;
	return instance;
}

void FunctionRegistry::registerFunction(std::unique_ptr<ScalarFunction> function) {
	auto sig = function->getSignature();
	std::string nameLower = sig.name;
	std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
	               [](unsigned char c) { return std::tolower(c); });

	scalarFunctions_[nameLower] = std::move(function);
}

const ScalarFunction* FunctionRegistry::lookupScalarFunction(const std::string& name) const {
	std::string nameLower = name;
	std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
	               [](unsigned char c) { return std::tolower(c); });

	auto it = scalarFunctions_.find(nameLower);
	if (it != scalarFunctions_.end()) {
		return it->second.get();
	}
	return nullptr;
}

bool FunctionRegistry::hasFunction(const std::string& name) const {
	return lookupScalarFunction(name) != nullptr;
}

std::vector<std::string> FunctionRegistry::getRegisteredFunctionNames() const {
	std::vector<std::string> names;
	names.reserve(scalarFunctions_.size());
	for (const auto& entry : scalarFunctions_) {
		names.push_back(entry.first);
	}
	return names;
}

FunctionRegistry::FunctionRegistry() {
	initializeBuiltinFunctions();
}

// ============================================================================
// Registration
// ============================================================================

void FunctionRegistry::initializeBuiltinFunctions() {
	registerStringFunctions(*this);
	registerMathFunctions(*this);
	registerListFunctions(*this);
	registerTypeConversionFunctions(*this);
	registerTemporalFunctions(*this);
	registerUtilityFunctions(*this);

	// Quantifier functions (keep as separate classes — they have special dispatch)
	registerFunction(std::make_unique<AllFunction>());
	registerFunction(std::make_unique<AnyFunction>());
	registerFunction(std::make_unique<NoneFunction>());
	registerFunction(std::make_unique<SingleFunction>());

	// Entity introspection functions
	registerFunction(makeIdFunction());
	registerFunction(makeLabelsFunction());
	registerFunction(makeTypeFunction());
	registerFunction(makeKeysFunction());
	registerFunction(makePropertiesFunction());
}

} // namespace graph::query::expressions
