/**
 * @file FunctionRegistry.cpp
 * @author Metrix Contributors
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
#include <algorithm>
#include <cmath>
#include <cctype>
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

void FunctionRegistry::initializeBuiltinFunctions() {
	// String functions
	registerFunction(std::make_unique<ToStringFunction>());
	registerFunction(std::make_unique<UpperFunction>());
	registerFunction(std::make_unique<LowerFunction>());
	registerFunction(std::make_unique<SubstringFunction>());
	registerFunction(std::make_unique<TrimFunction>());
	registerFunction(std::make_unique<LengthFunction>());
	registerFunction(std::make_unique<StartsWithFunction>());
	registerFunction(std::make_unique<EndsWithFunction>());
	registerFunction(std::make_unique<ContainsFunction>());
	registerFunction(std::make_unique<ReplaceFunction>());

	// Math functions
	registerFunction(std::make_unique<AbsFunction>());
	registerFunction(std::make_unique<CeilFunction>());
	registerFunction(std::make_unique<FloorFunction>());
	registerFunction(std::make_unique<RoundFunction>());
	registerFunction(std::make_unique<SqrtFunction>());
	registerFunction(std::make_unique<SignFunction>());

	// Utility functions
	registerFunction(std::make_unique<CoalesceFunction>());
	registerFunction(std::make_unique<SizeFunction>());

	// List functions
	registerFunction(std::make_unique<RangeFunction>());
}

// ============================================================================
// String Function Implementations
// ============================================================================

PropertyValue ToStringFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	if (args.empty()) {
		return PropertyValue();
	}

	return PropertyValue(args[0].toString());
}

PropertyValue UpperFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}

	std::string str = EvaluationContext::toString(args[0]);
	std::transform(str.begin(), str.end(), str.begin(),
	               [](unsigned char c) { return std::toupper(c); });
	return PropertyValue(str);
}

PropertyValue LowerFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}

	std::string str = EvaluationContext::toString(args[0]);
	std::transform(str.begin(), str.end(), str.begin(),
	               [](unsigned char c) { return std::tolower(c); });
	return PropertyValue(str);
}

PropertyValue SubstringFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	if (args.size() < 2 || EvaluationContext::isNull(args[0]) || EvaluationContext::isNull(args[1])) {
		return PropertyValue();
	}

	std::string str = EvaluationContext::toString(args[0]);
	int64_t start = EvaluationContext::toInteger(args[1]);

	// Handle negative start (from end)
	if (start < 0) {
		start = std::max<int64_t>(0, static_cast<int64_t>(str.length()) + start);
	}

	// If start is beyond string length, return empty string
	if (start >= static_cast<int64_t>(str.length())) {
		return PropertyValue("");
	}

	// Check for length parameter
	if (args.size() >= 3) {
		if (EvaluationContext::isNull(args[2])) {
			return PropertyValue();
		}
		int64_t length = EvaluationContext::toInteger(args[2]);

		// Handle negative length (from end)
		if (length < 0) {
			length = std::max<int64_t>(0, static_cast<int64_t>(str.length()) - start + length);
		}

		// Clamp length to string bounds
		int64_t maxLen = static_cast<int64_t>(str.length()) - start;
		length = std::min(length, maxLen);

		return PropertyValue(str.substr(static_cast<size_t>(start), static_cast<size_t>(length)));
	}

	// No length specified, return from start to end
	return PropertyValue(str.substr(static_cast<size_t>(start)));
}

PropertyValue TrimFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}

	std::string str = EvaluationContext::toString(args[0]);

	// Trim leading whitespace
	size_t start = 0;
	while (start < str.length() && std::isspace(static_cast<unsigned char>(str[start]))) {
		start++;
	}

	// Trim trailing whitespace
	size_t end = str.length();
	while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
		end--;
	}

	return PropertyValue(str.substr(start, end - start));
}

PropertyValue LengthFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}

	const PropertyValue& arg = args[0];
	PropertyType type = arg.getType();

	if (type == PropertyType::STRING) {
		const auto& str = std::get<std::string>(arg.getVariant());
		return PropertyValue(static_cast<int64_t>(str.length()));
	} else if (type == PropertyType::LIST) {
		const auto& list = std::get<std::vector<float>>(arg.getVariant());
		return PropertyValue(static_cast<int64_t>(list.size()));
	}

	// For other types, return 0
	return PropertyValue(static_cast<int64_t>(0));
}

PropertyValue StartsWithFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	if (args.size() < 2 || EvaluationContext::isNull(args[0]) || EvaluationContext::isNull(args[1])) {
		return PropertyValue();
	}

	std::string text = EvaluationContext::toString(args[0]);
	std::string prefix = EvaluationContext::toString(args[1]);

	bool result = text.length() >= prefix.length() &&
	              text.compare(0, prefix.length(), prefix) == 0;

	return PropertyValue(result);
}

PropertyValue EndsWithFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	if (args.size() < 2 || EvaluationContext::isNull(args[0]) || EvaluationContext::isNull(args[1])) {
		return PropertyValue();
	}

	std::string text = EvaluationContext::toString(args[0]);
	std::string suffix = EvaluationContext::toString(args[1]);

	bool result = text.length() >= suffix.length() &&
	              text.compare(text.length() - suffix.length(), suffix.length(), suffix) == 0;

	return PropertyValue(result);
}

PropertyValue ContainsFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	if (args.size() < 2 || EvaluationContext::isNull(args[0]) || EvaluationContext::isNull(args[1])) {
		return PropertyValue();
	}

	std::string text = EvaluationContext::toString(args[0]);
	std::string substring = EvaluationContext::toString(args[1]);

	bool result = text.find(substring) != std::string::npos;

	return PropertyValue(result);
}

PropertyValue ReplaceFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	if (args.size() < 3 || EvaluationContext::isNull(args[0]) ||
	    EvaluationContext::isNull(args[1]) || EvaluationContext::isNull(args[2])) {
		return PropertyValue();
	}

	std::string text = EvaluationContext::toString(args[0]);
	std::string search = EvaluationContext::toString(args[1]);
	std::string replace = EvaluationContext::toString(args[2]);

	if (search.empty()) {
		// If search string is empty, return original
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

PropertyValue AbsFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
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

	// For other types, try to convert to double
	double val = EvaluationContext::toDouble(arg);
	return PropertyValue(std::fabs(val));
}

PropertyValue CeilFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}

	double val = EvaluationContext::toDouble(args[0]);
	return PropertyValue(std::ceil(val));
}

PropertyValue FloorFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}

	double val = EvaluationContext::toDouble(args[0]);
	return PropertyValue(std::floor(val));
}

PropertyValue RoundFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}

	double val = EvaluationContext::toDouble(args[0]);
	return PropertyValue(std::round(val));
}

PropertyValue SqrtFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}

	double val = EvaluationContext::toDouble(args[0]);

	if (val < 0.0) {
		// sqrt of negative number is undefined, return NULL
		return PropertyValue();
	}

	return PropertyValue(std::sqrt(val));
}

PropertyValue SignFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
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

PropertyValue CoalesceFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	for (const auto& arg : args) {
		if (!EvaluationContext::isNull(arg)) {
			return arg;
		}
	}
	// All arguments are NULL
	return PropertyValue();
}

PropertyValue SizeFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}

	const PropertyValue& arg = args[0];
	PropertyType type = arg.getType();

	if (type == PropertyType::STRING) {
		const auto& str = std::get<std::string>(arg.getVariant());
		return PropertyValue(static_cast<int64_t>(str.length()));
	} else if (type == PropertyType::LIST) {
		const auto& list = std::get<std::vector<float>>(arg.getVariant());
		return PropertyValue(static_cast<int64_t>(list.size()));
	}

	// For other types, throw error or return NULL
	return PropertyValue();
}

PropertyValue RangeFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	// Validate argument count (should be 2 or 3, handled by signature validation)
	if (args.size() < 2 || args.size() > 3) {
		return PropertyValue();
	}

	// Check for NULL arguments
	for (const auto& arg : args) {
		if (EvaluationContext::isNull(arg)) {
			return PropertyValue();
		}
	}

	// Extract start and end
	int64_t start = EvaluationContext::toInteger(args[0]);
	int64_t end = EvaluationContext::toInteger(args[1]);
	int64_t step = 1;

	// Extract step if provided
	if (args.size() == 3) {
		step = EvaluationContext::toInteger(args[2]);
	}

	// Validate step is not zero
	if (step == 0) {
		throw std::runtime_error("range() step cannot be zero");
	}

	// Generate the range
	std::vector<PropertyValue> result;
	result.reserve(16); // Reserve space for typical ranges

	if (step > 0) {
		// Incrementing range
		for (int64_t i = start; i < end; i += step) {
			result.push_back(PropertyValue(static_cast<int64_t>(i)));
		}
	} else {
		// Decrementing range
		for (int64_t i = start; i > end; i += step) {
			result.push_back(PropertyValue(static_cast<int64_t>(i)));
		}
	}

	return PropertyValue(result);
}

} // namespace graph::query::expressions
