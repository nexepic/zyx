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
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/AllFunction.hpp"
#include "graph/query/expressions/AnyFunction.hpp"
#include "graph/query/expressions/NoneFunction.hpp"
#include "graph/query/expressions/SingleFunction.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <random>
#include <sstream>

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
// Helper: register a lambda scalar function
// ============================================================================

static std::unique_ptr<ScalarFunction> makeFn(const char* name, size_t minArgs, size_t maxArgs, ScalarFnPtr fn, bool variadic = false) {
	return std::make_unique<LambdaScalarFunction>(FunctionSignature(name, minArgs, maxArgs, variadic), fn);
}

// ============================================================================
// String Function Implementations
// ============================================================================

static PropertyValue toStringImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty()) {
		return PropertyValue();
	}
	return PropertyValue(args[0].toString());
}

static PropertyValue upperImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	std::string str = EvaluationContext::toString(args[0]);
	std::transform(str.begin(), str.end(), str.begin(),
	               [](unsigned char c) { return std::toupper(c); });
	return PropertyValue(str);
}

static PropertyValue lowerImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	std::string str = EvaluationContext::toString(args[0]);
	std::transform(str.begin(), str.end(), str.begin(),
	               [](unsigned char c) { return std::tolower(c); });
	return PropertyValue(str);
}

static PropertyValue substringImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.size() < 2 || EvaluationContext::isNull(args[0]) || EvaluationContext::isNull(args[1])) {
		return PropertyValue();
	}

	std::string str = EvaluationContext::toString(args[0]);
	int64_t start = EvaluationContext::toInteger(args[1]);

	if (start < 0) {
		start = std::max<int64_t>(0, static_cast<int64_t>(str.length()) + start);
	}

	if (start >= static_cast<int64_t>(str.length())) {
		return PropertyValue("");
	}

	if (args.size() >= 3) {
		if (EvaluationContext::isNull(args[2])) {
			return PropertyValue();
		}
		int64_t length = EvaluationContext::toInteger(args[2]);

		if (length < 0) {
			length = std::max<int64_t>(0, static_cast<int64_t>(str.length()) - start + length);
		}

		int64_t maxLen = static_cast<int64_t>(str.length()) - start;
		length = std::min(length, maxLen);

		return PropertyValue(str.substr(static_cast<size_t>(start), static_cast<size_t>(length)));
	}

	return PropertyValue(str.substr(static_cast<size_t>(start)));
}

static PropertyValue trimImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}

	std::string str = EvaluationContext::toString(args[0]);

	size_t start = 0;
	while (start < str.length() && std::isspace(static_cast<unsigned char>(str[start]))) {
		start++;
	}

	size_t end = str.length();
	while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
		end--;
	}

	return PropertyValue(str.substr(start, end - start));
}

static PropertyValue lengthImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}

	const PropertyValue& arg = args[0];
	PropertyType type = arg.getType();

	if (type == PropertyType::STRING) {
		const auto& str = std::get<std::string>(arg.getVariant());
		return PropertyValue(static_cast<int64_t>(str.length()));
	} else if (type == PropertyType::LIST) {
		const auto& list = std::get<std::vector<PropertyValue>>(arg.getVariant());
		return PropertyValue(static_cast<int64_t>(list.size()));
	}

	return PropertyValue(static_cast<int64_t>(0));
}

static PropertyValue startsWithImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.size() < 2 || EvaluationContext::isNull(args[0]) || EvaluationContext::isNull(args[1])) {
		return PropertyValue();
	}

	std::string text = EvaluationContext::toString(args[0]);
	std::string prefix = EvaluationContext::toString(args[1]);

	bool result = text.length() >= prefix.length() &&
	              text.compare(0, prefix.length(), prefix) == 0;

	return PropertyValue(result);
}

static PropertyValue endsWithImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.size() < 2 || EvaluationContext::isNull(args[0]) || EvaluationContext::isNull(args[1])) {
		return PropertyValue();
	}

	std::string text = EvaluationContext::toString(args[0]);
	std::string suffix = EvaluationContext::toString(args[1]);

	bool result = text.length() >= suffix.length() &&
	              text.compare(text.length() - suffix.length(), suffix.length(), suffix) == 0;

	return PropertyValue(result);
}

static PropertyValue containsImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.size() < 2 || EvaluationContext::isNull(args[0]) || EvaluationContext::isNull(args[1])) {
		return PropertyValue();
	}

	std::string text = EvaluationContext::toString(args[0]);
	std::string substring = EvaluationContext::toString(args[1]);

	bool result = text.find(substring) != std::string::npos;

	return PropertyValue(result);
}

static PropertyValue replaceImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.size() < 3 || EvaluationContext::isNull(args[0]) ||
	    EvaluationContext::isNull(args[1]) || EvaluationContext::isNull(args[2])) {
		return PropertyValue();
	}

	std::string text = EvaluationContext::toString(args[0]);
	std::string search = EvaluationContext::toString(args[1]);
	std::string replace = EvaluationContext::toString(args[2]);

	if (search.empty()) {
		return PropertyValue(text);
	}

	std::string result = text;
	size_t pos = 0;

	while ((pos = result.find(search, pos)) != std::string::npos) {
		result.replace(pos, search.length(), replace);
		pos += replace.length();
	}

	return PropertyValue(result);
}

// ============================================================================
// Math Function Implementations
// ============================================================================

static PropertyValue absImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}

	const PropertyValue& arg = args[0];
	PropertyType type = arg.getType();

	if (type == PropertyType::INTEGER) {
		int64_t val = std::get<int64_t>(arg.getVariant());
		return PropertyValue(std::abs(val));
	} else if (type == PropertyType::DOUBLE) {
		double val = std::get<double>(arg.getVariant());
		return PropertyValue(std::fabs(val));
	}

	double val = EvaluationContext::toDouble(arg);
	return PropertyValue(std::fabs(val));
}

static PropertyValue ceilImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	double val = EvaluationContext::toDouble(args[0]);
	return PropertyValue(std::ceil(val));
}

static PropertyValue floorImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	double val = EvaluationContext::toDouble(args[0]);
	return PropertyValue(std::floor(val));
}

static PropertyValue roundImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	double val = EvaluationContext::toDouble(args[0]);
	return PropertyValue(std::round(val));
}

static PropertyValue sqrtImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	double val = EvaluationContext::toDouble(args[0]);
	if (val < 0.0) {
		return PropertyValue();
	}
	return PropertyValue(std::sqrt(val));
}

static PropertyValue signImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	double val = EvaluationContext::toDouble(args[0]);
	if (val > 0.0) {
		return PropertyValue(static_cast<int64_t>(1));
	} else if (val < 0.0) {
		return PropertyValue(static_cast<int64_t>(-1));
	} else {
		return PropertyValue(static_cast<int64_t>(0));
	}
}

// ============================================================================
// Utility Function Implementations
// ============================================================================

static PropertyValue coalesceImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	for (const auto& arg : args) {
		if (!EvaluationContext::isNull(arg)) {
			return arg;
		}
	}
	return PropertyValue();
}

static PropertyValue sizeImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}

	const PropertyValue& arg = args[0];
	PropertyType type = arg.getType();

	if (type == PropertyType::STRING) {
		const auto& str = std::get<std::string>(arg.getVariant());
		return PropertyValue(static_cast<int64_t>(str.length()));
	} else if (type == PropertyType::LIST) {
		const auto& list = std::get<std::vector<PropertyValue>>(arg.getVariant());
		return PropertyValue(static_cast<int64_t>(list.size()));
	}

	return PropertyValue();
}

static PropertyValue rangeImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.size() < 2 || args.size() > 3) {
		return PropertyValue();
	}

	for (const auto& arg : args) {
		if (EvaluationContext::isNull(arg)) {
			return PropertyValue();
		}
	}

	int64_t start = EvaluationContext::toInteger(args[0]);
	int64_t end = EvaluationContext::toInteger(args[1]);
	int64_t step = 1;

	if (args.size() == 3) {
		step = EvaluationContext::toInteger(args[2]);
	}

	if (step == 0) {
		throw std::runtime_error("range() step cannot be zero");
	}

	std::vector<PropertyValue> result;
	result.reserve(16);

	if (step > 0) {
		for (int64_t i = start; i < end; i += step) {
			result.push_back(PropertyValue(static_cast<int64_t>(i)));
		}
	} else {
		for (int64_t i = start; i > end; i += step) {
			result.push_back(PropertyValue(static_cast<int64_t>(i)));
		}
	}

	return PropertyValue(result);
}

static PropertyValue reduceImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty()) {
		throw std::runtime_error("REDUCE function requires at least one argument");
	}

	for (const auto& arg : args) {
		if (arg.getType() == PropertyType::LIST) {
			throw std::runtime_error(
				"REDUCE function is not yet fully implemented. "
				"The REDUCE syntax 'reduce(accum = init, x IN list | expression)' "
				"requires special parsing support. "
				"Please use list comprehensions FILTER/EXTRACT as alternatives."
			);
		}
	}

	throw std::runtime_error(
		"REDUCE function is not yet implemented. "
		"REDUCE has special Cypher syntax that requires custom parsing: "
		"'reduce(accumulator = initial, variable IN list | expression)'. "
		"Please use list comprehensions [x IN list | expression] or "
		"[x IN list WHERE condition] as alternatives."
	);
}

// ============================================================================
// Type Conversion Function Implementations
// ============================================================================

static PropertyValue toIntegerImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	return PropertyValue(EvaluationContext::toInteger(args[0]));
}

static PropertyValue toFloatImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	return PropertyValue(EvaluationContext::toDouble(args[0]));
}

static PropertyValue toBooleanImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	return PropertyValue(EvaluationContext::toBoolean(args[0]));
}

// ============================================================================
// Additional String Function Implementations
// ============================================================================

static PropertyValue leftImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.size() < 2 || EvaluationContext::isNull(args[0]) || EvaluationContext::isNull(args[1])) {
		return PropertyValue();
	}
	std::string str = EvaluationContext::toString(args[0]);
	int64_t length = EvaluationContext::toInteger(args[1]);
	if (length < 0) {
		return PropertyValue();
	}
	return PropertyValue(str.substr(0, static_cast<size_t>(length)));
}

static PropertyValue rightImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.size() < 2 || EvaluationContext::isNull(args[0]) || EvaluationContext::isNull(args[1])) {
		return PropertyValue();
	}
	std::string str = EvaluationContext::toString(args[0]);
	int64_t length = EvaluationContext::toInteger(args[1]);
	if (length < 0) {
		return PropertyValue();
	}
	size_t len = static_cast<size_t>(length);
	if (len >= str.size()) {
		return PropertyValue(str);
	}
	return PropertyValue(str.substr(str.size() - len));
}

static PropertyValue lTrimImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	std::string str = EvaluationContext::toString(args[0]);
	size_t start = 0;
	while (start < str.length() && std::isspace(static_cast<unsigned char>(str[start]))) {
		start++;
	}
	return PropertyValue(str.substr(start));
}

static PropertyValue rTrimImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	std::string str = EvaluationContext::toString(args[0]);
	size_t end = str.length();
	while (end > 0 && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
		end--;
	}
	return PropertyValue(str.substr(0, end));
}

static PropertyValue splitImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.size() < 2 || EvaluationContext::isNull(args[0]) || EvaluationContext::isNull(args[1])) {
		return PropertyValue();
	}
	std::string str = EvaluationContext::toString(args[0]);
	std::string delimiter = EvaluationContext::toString(args[1]);

	std::vector<PropertyValue> result;
	if (delimiter.empty()) {
		for (char c : str) {
			result.push_back(PropertyValue(std::string(1, c)));
		}
		return PropertyValue(result);
	}

	size_t pos = 0;
	size_t found;
	while ((found = str.find(delimiter, pos)) != std::string::npos) {
		result.push_back(PropertyValue(str.substr(pos, found - pos)));
		pos = found + delimiter.length();
	}
	result.push_back(PropertyValue(str.substr(pos)));
	return PropertyValue(result);
}

static PropertyValue reverseImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}

	const PropertyValue& arg = args[0];
	if (arg.getType() == PropertyType::STRING) {
		std::string str = std::get<std::string>(arg.getVariant());
		std::reverse(str.begin(), str.end());
		return PropertyValue(str);
	} else if (arg.getType() == PropertyType::LIST) {
		std::vector<PropertyValue> list = std::get<std::vector<PropertyValue>>(arg.getVariant());
		std::reverse(list.begin(), list.end());
		return PropertyValue(list);
	}

	std::string str = EvaluationContext::toString(arg);
	std::reverse(str.begin(), str.end());
	return PropertyValue(str);
}

// ============================================================================
// List Function Implementations
// ============================================================================

static PropertyValue headImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	if (args[0].getType() != PropertyType::LIST) {
		return PropertyValue();
	}
	const auto& list = std::get<std::vector<PropertyValue>>(args[0].getVariant());
	if (list.empty()) {
		return PropertyValue();
	}
	return list.front();
}

static PropertyValue tailImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	if (args[0].getType() != PropertyType::LIST) {
		return PropertyValue();
	}
	const auto& list = std::get<std::vector<PropertyValue>>(args[0].getVariant());
	if (list.empty()) {
		return PropertyValue(std::vector<PropertyValue>{});
	}
	std::vector<PropertyValue> result(list.begin() + 1, list.end());
	return PropertyValue(result);
}

static PropertyValue lastImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	if (args[0].getType() != PropertyType::LIST) {
		return PropertyValue();
	}
	const auto& list = std::get<std::vector<PropertyValue>>(args[0].getVariant());
	if (list.empty()) {
		return PropertyValue();
	}
	return list.back();
}

// ============================================================================
// Utility Function Implementations
// ============================================================================

static PropertyValue timestampImpl(
	[[maybe_unused]] const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	auto now = std::chrono::system_clock::now();
	auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
		now.time_since_epoch()
	).count();
	return PropertyValue(static_cast<int64_t>(millis));
}

static PropertyValue randomUUIDImpl(
	[[maybe_unused]] const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	static thread_local std::mt19937 gen(std::random_device{}());
	std::uniform_int_distribution<uint32_t> dist(0, 15);
	std::uniform_int_distribution<uint32_t> dist2(8, 11);

	const char* hex = "0123456789abcdef";
	std::string uuid(36, '-');
	for (int i = 0; i < 36; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			continue;
		} else if (i == 14) {
			uuid[i] = '4';
		} else if (i == 19) {
			uuid[i] = hex[dist2(gen)];
		} else {
			uuid[i] = hex[dist(gen)];
		}
	}
	return PropertyValue(uuid);
}

// ============================================================================
// Registration
// ============================================================================

void FunctionRegistry::initializeBuiltinFunctions() {
	// String functions
	registerFunction(makeFn("toString", 1, 1, &toStringImpl));
	registerFunction(makeFn("upper", 1, 1, &upperImpl));
	registerFunction(makeFn("lower", 1, 1, &lowerImpl));
	registerFunction(makeFn("substring", 2, 3, &substringImpl));
	registerFunction(makeFn("trim", 1, 1, &trimImpl));
	registerFunction(makeFn("length", 1, 1, &lengthImpl));
	registerFunction(makeFn("startsWith", 2, 2, &startsWithImpl));
	registerFunction(makeFn("endsWith", 2, 2, &endsWithImpl));
	registerFunction(makeFn("contains", 2, 2, &containsImpl));
	registerFunction(makeFn("replace", 3, 3, &replaceImpl));

	// Math functions
	registerFunction(makeFn("abs", 1, 1, &absImpl));
	registerFunction(makeFn("ceil", 1, 1, &ceilImpl));
	registerFunction(makeFn("floor", 1, 1, &floorImpl));
	registerFunction(makeFn("round", 1, 1, &roundImpl));
	registerFunction(makeFn("sqrt", 1, 1, &sqrtImpl));
	registerFunction(makeFn("sign", 1, 1, &signImpl));

	// Utility functions
	registerFunction(makeFn("coalesce", 1, SIZE_MAX, &coalesceImpl, true));
	registerFunction(makeFn("size", 1, 1, &sizeImpl));

	// List functions
	registerFunction(makeFn("range", 2, 3, &rangeImpl));
	registerFunction(makeFn("reduce", 1, SIZE_MAX, &reduceImpl, true));

	// Quantifier functions (keep as separate classes — they have special dispatch)
	registerFunction(std::make_unique<AllFunction>());
	registerFunction(std::make_unique<AnyFunction>());
	registerFunction(std::make_unique<NoneFunction>());
	registerFunction(std::make_unique<SingleFunction>());

	// Type conversion functions
	registerFunction(makeFn("toInteger", 1, 1, &toIntegerImpl));
	registerFunction(makeFn("toFloat", 1, 1, &toFloatImpl));
	registerFunction(makeFn("toBoolean", 1, 1, &toBooleanImpl));

	// Additional string functions
	registerFunction(makeFn("left", 2, 2, &leftImpl));
	registerFunction(makeFn("right", 2, 2, &rightImpl));
	registerFunction(makeFn("lTrim", 1, 1, &lTrimImpl));
	registerFunction(makeFn("rTrim", 1, 1, &rTrimImpl));
	registerFunction(makeFn("split", 2, 2, &splitImpl));
	registerFunction(makeFn("reverse", 1, 1, &reverseImpl));

	// Additional list functions
	registerFunction(makeFn("head", 1, 1, &headImpl));
	registerFunction(makeFn("tail", 1, 1, &tailImpl));
	registerFunction(makeFn("last", 1, 1, &lastImpl));

	// Utility functions
	registerFunction(makeFn("timestamp", 0, 0, &timestampImpl));
	registerFunction(makeFn("randomUUID", 0, 0, &randomUUIDImpl));

	// Entity introspection functions
	registerFunction(makeIdFunction());
	registerFunction(makeLabelsFunction());
	registerFunction(makeTypeFunction());
	registerFunction(makeKeysFunction());
	registerFunction(makePropertiesFunction());
}

} // namespace graph::query::expressions
