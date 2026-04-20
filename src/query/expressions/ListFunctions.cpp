/**
 * @file ListFunctions.cpp
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

namespace graph::query::expressions {

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
		for (int64_t i = start; i <= end; i += step) {
			result.push_back(PropertyValue(static_cast<int64_t>(i)));
		}
	} else {
		for (int64_t i = start; i >= end; i += step) {
			result.push_back(PropertyValue(static_cast<int64_t>(i)));
		}
	}

	return PropertyValue(result);
}

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

void registerListFunctions(FunctionRegistry& registry) {
	registry.registerFunction(makeFn("head", 1, 1, &headImpl));
	registry.registerFunction(makeFn("tail", 1, 1, &tailImpl));
	registry.registerFunction(makeFn("last", 1, 1, &lastImpl));
	registry.registerFunction(makeFn("range", 2, 3, &rangeImpl));
	registry.registerFunction(makeFn("coalesce", 1, SIZE_MAX, &coalesceImpl, true));
	registry.registerFunction(makeFn("size", 1, 1, &sizeImpl));

	// Path functions
	registry.registerFunction(makeFn("nodes", 1, 1, [](const std::vector<PropertyValue>& args, const EvaluationContext&) -> PropertyValue {
		if (args.empty() || args[0].getType() != PropertyType::LIST) {
			return PropertyValue();
		}
		const auto& pathList = args[0].getList();
		std::vector<PropertyValue> nodes;
		for (const auto& elem : pathList) {
			if (elem.getType() == PropertyType::MAP) {
				const auto& map = elem.getMap();
				auto it = map.find("_type");
				if (it != map.end() && it->second.getType() == PropertyType::STRING &&
					std::get<std::string>(it->second.getVariant()) == "node") {
					nodes.push_back(elem);
				}
			}
		}
		return PropertyValue(std::move(nodes));
	}));

	registry.registerFunction(makeFn("relationships", 1, 1, [](const std::vector<PropertyValue>& args, const EvaluationContext&) -> PropertyValue {
		if (args.empty() || args[0].getType() != PropertyType::LIST) {
			return PropertyValue();
		}
		const auto& pathList = args[0].getList();
		std::vector<PropertyValue> rels;
		for (const auto& elem : pathList) {
			if (elem.getType() == PropertyType::MAP) {
				const auto& map = elem.getMap();
				auto it = map.find("_type");
				if (it != map.end() && it->second.getType() == PropertyType::STRING &&
					std::get<std::string>(it->second.getVariant()) == "relationship") {
					rels.push_back(elem);
				}
			}
		}
		return PropertyValue(std::move(rels));
	}));
}

} // namespace graph::query::expressions
