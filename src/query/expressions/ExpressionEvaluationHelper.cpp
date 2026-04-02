/**
 * @file ExpressionEvaluationHelper.cpp
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

#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"

namespace graph::query::expressions {

PropertyValue ExpressionEvaluationHelper::evaluate(const Expression *expr,
                                                    const execution::Record &record,
                                                    storage::DataManager *dataManager) {
	if (!expr) {
		return PropertyValue();
	}

	EvaluationContext context(record, dataManager);
	ExpressionEvaluator evaluator(context);
	return evaluator.evaluate(expr);
}

PropertyValue ExpressionEvaluationHelper::evaluate(const Expression *expr,
                                                    const execution::Record &record,
                                                    storage::DataManager *dataManager,
                                                    const std::unordered_map<std::string, PropertyValue> *parameters) {
	if (!expr) {
		return PropertyValue();
	}
	if (!parameters) {
		return evaluate(expr, record, dataManager);
	}

	EvaluationContext context(record, dataManager, *parameters);
	ExpressionEvaluator evaluator(context);
	return evaluator.evaluate(expr);
}

bool ExpressionEvaluationHelper::evaluateBool(const Expression *expr,
                                               const execution::Record &record,
                                               storage::DataManager *dataManager) {
	PropertyValue value = evaluate(expr, record, dataManager);
	return EvaluationContext::toBoolean(value);
}

bool ExpressionEvaluationHelper::evaluateBool(const Expression *expr,
                                               const execution::Record &record,
                                               storage::DataManager *dataManager,
                                               const std::unordered_map<std::string, PropertyValue> *parameters) {
	PropertyValue value = evaluate(expr, record, dataManager, parameters);
	return EvaluationContext::toBoolean(value);
}

int64_t ExpressionEvaluationHelper::evaluateInt(const Expression *expr,
                                                 const execution::Record &record) {
	PropertyValue value = evaluate(expr, record);
	return EvaluationContext::toInteger(value);
}

double ExpressionEvaluationHelper::evaluateDouble(const Expression *expr,
                                                   const execution::Record &record) {
	PropertyValue value = evaluate(expr, record);
	return EvaluationContext::toDouble(value);
}

std::vector<PropertyValue> ExpressionEvaluationHelper::evaluateBatch(
    const Expression *expr, const std::vector<execution::Record> &records) {
	std::vector<PropertyValue> results;
	results.reserve(records.size());

	for (const auto &record : records) {
		results.push_back(evaluate(expr, record));
	}

	return results;
}

} // namespace graph::query::expressions
