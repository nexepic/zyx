/**
 * @file EntityIntrospectionFunction.cpp
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
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::expressions {

// All entity introspection functions receive args[0] = variable name as STRING.
// The EvaluationContext provides Record and DataManager access.

static PropertyValue idImpl(
	const std::vector<PropertyValue>& args,
	const EvaluationContext& context
) {
	const std::string& varName = std::get<std::string>(args[0].getVariant());
	const auto& record = context.getRecord();

	if (auto node = record.getNode(varName)) {
		return PropertyValue(node->getId());
	}
	if (auto edge = record.getEdge(varName)) {
		return PropertyValue(edge->getId());
	}
	throw ExpressionEvaluationException("Variable '" + varName + "' is not a node or relationship");
}

static PropertyValue labelsImpl(
	const std::vector<PropertyValue>& args,
	const EvaluationContext& context
) {
	const std::string& varName = std::get<std::string>(args[0].getVariant());
	const auto& record = context.getRecord();

	if (auto node = record.getNode(varName)) {
		auto labelIds = node->getLabelIds();
		auto* dm = context.getDataManager();
		std::vector<PropertyValue> labels;
		for (int64_t labelId : labelIds) {
			if (labelId == 0) continue;
			if (dm) {
				labels.emplace_back(dm->resolveTokenName(labelId));
			} else {
				labels.emplace_back(labelId);
			}
		}
		return PropertyValue(labels);
	}
	throw ExpressionEvaluationException("labels() requires a node variable, '" + varName + "' is not a node");
}

static PropertyValue typeImpl(
	const std::vector<PropertyValue>& args,
	const EvaluationContext& context
) {
	const std::string& varName = std::get<std::string>(args[0].getVariant());
	const auto& record = context.getRecord();

	if (auto edge = record.getEdge(varName)) {
		int64_t labelId = edge->getTypeId();
		auto* dm = context.getDataManager();
		if (dm && labelId != 0) {
			return PropertyValue(dm->resolveTokenName(labelId));
		}
		return PropertyValue(labelId);
	}
	throw ExpressionEvaluationException("type() requires a relationship variable, '" + varName + "' is not a relationship");
}

static PropertyValue keysImpl(
	const std::vector<PropertyValue>& args,
	const EvaluationContext& context
) {
	const std::string& varName = std::get<std::string>(args[0].getVariant());
	const auto& record = context.getRecord();

	auto node = record.getNode(varName);
	auto edge = record.getEdge(varName);

	const std::unordered_map<std::string, PropertyValue>* props = nullptr;
	if (node) {
		props = &node->getProperties();
	} else if (edge) {
		props = &edge->getProperties();
	} else {
		throw ExpressionEvaluationException("Variable '" + varName + "' is not a node or relationship");
	}
	std::vector<PropertyValue> keys;
	for (const auto& [key, val] : *props) {
		keys.emplace_back(key);
	}
	return PropertyValue(keys);
}

static PropertyValue propertiesImpl(
	const std::vector<PropertyValue>& args,
	const EvaluationContext& context
) {
	const std::string& varName = std::get<std::string>(args[0].getVariant());
	const auto& record = context.getRecord();

	auto node = record.getNode(varName);
	auto edge = record.getEdge(varName);

	const std::unordered_map<std::string, PropertyValue>* props = nullptr;
	if (node) {
		props = &node->getProperties();
	} else if (edge) {
		props = &edge->getProperties();
	} else {
		throw ExpressionEvaluationException("Variable '" + varName + "' is not a node or relationship");
	}
	// Return as MAP type (Step 6 added MAP support)
	PropertyValue::MapType result;
	for (const auto& [key, val] : *props) {
		result[key] = val;
	}
	return PropertyValue(result);
}

// Factory functions for registration
std::unique_ptr<ScalarFunction> makeIdFunction() {
	return std::make_unique<LambdaEntityIntrospectionFunction>(
		FunctionSignature("id", 1, 1), &idImpl);
}

std::unique_ptr<ScalarFunction> makeLabelsFunction() {
	return std::make_unique<LambdaEntityIntrospectionFunction>(
		FunctionSignature("labels", 1, 1), &labelsImpl);
}

std::unique_ptr<ScalarFunction> makeTypeFunction() {
	return std::make_unique<LambdaEntityIntrospectionFunction>(
		FunctionSignature("type", 1, 1), &typeImpl);
}

std::unique_ptr<ScalarFunction> makeKeysFunction() {
	return std::make_unique<LambdaEntityIntrospectionFunction>(
		FunctionSignature("keys", 1, 1), &keysImpl);
}

std::unique_ptr<ScalarFunction> makePropertiesFunction() {
	return std::make_unique<LambdaEntityIntrospectionFunction>(
		FunctionSignature("properties", 1, 1), &propertiesImpl);
}

} // namespace graph::query::expressions
