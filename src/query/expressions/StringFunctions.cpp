/**
 * @file StringFunctions.cpp
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

#include "BuiltinFunctions.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace graph::query::expressions {

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

void registerStringFunctions(FunctionRegistry& registry) {
	registry.registerFunction(makeFn("toString", 1, 1, &toStringImpl));
	registry.registerFunction(makeFn("upper", 1, 1, &upperImpl));
	registry.registerFunction(makeFn("lower", 1, 1, &lowerImpl));
	registry.registerFunction(makeFn("substring", 2, 3, &substringImpl));
	registry.registerFunction(makeFn("trim", 1, 1, &trimImpl));
	registry.registerFunction(makeFn("length", 1, 1, &lengthImpl));
	registry.registerFunction(makeFn("startsWith", 2, 2, &startsWithImpl));
	registry.registerFunction(makeFn("endsWith", 2, 2, &endsWithImpl));
	registry.registerFunction(makeFn("contains", 2, 2, &containsImpl));
	registry.registerFunction(makeFn("replace", 3, 3, &replaceImpl));
	registry.registerFunction(makeFn("left", 2, 2, &leftImpl));
	registry.registerFunction(makeFn("right", 2, 2, &rightImpl));
	registry.registerFunction(makeFn("lTrim", 1, 1, &lTrimImpl));
	registry.registerFunction(makeFn("rTrim", 1, 1, &rTrimImpl));
	registry.registerFunction(makeFn("split", 2, 2, &splitImpl));
	registry.registerFunction(makeFn("reverse", 1, 1, &reverseImpl));
	// String aliases
	registry.registerFunction(makeFn("toUpper", 1, 1, &upperImpl));
	registry.registerFunction(makeFn("toLower", 1, 1, &lowerImpl));
}

} // namespace graph::query::expressions
