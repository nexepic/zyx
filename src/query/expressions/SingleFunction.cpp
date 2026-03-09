/**
 * @file SingleFunction.cpp
 * @date 2025/3/9
 *
 * Implementation of SingleFunction
 */

#include "graph/query/expressions/SingleFunction.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include <stdexcept>

namespace graph::query::expressions {

PropertyValue SingleFunction::evaluate(
	const std::string& variable,
	const std::unique_ptr<Expression>& listExpr,
	const std::unique_ptr<Expression>& whereExpr,
	EvaluationContext& context
) const {
	// Evaluate the list expression to get the list
	ExpressionEvaluator evaluator(context);
	PropertyValue listValue = evaluator.evaluate(listExpr.get());

	// Check if the list expression evaluated to a list type
	if (listValue.getType() != PropertyType::LIST) {
		throw ExpressionEvaluationException(
			"single() function requires a list as the second argument"
		);
	}

	// Extract the list from PropertyValue
	const std::vector<PropertyValue>& list = std::get<std::vector<PropertyValue>>(listValue.getVariant());

	// Evaluate the predicate
	bool result = evaluatePredicate(list, variable, whereExpr, context);

	return PropertyValue(result);
}

bool SingleFunction::evaluatePredicate(
	const std::vector<PropertyValue>& list,
	const std::string& variable,
	const std::unique_ptr<Expression>& whereExpr,
	EvaluationContext& context
) const {
	// Empty list: single() returns false (no elements to satisfy)
	if (list.empty()) {
		return false;
	}

	// Count matching elements (early exit if > 1)
	int matchCount = 0;
	ExpressionEvaluator evaluator(context);
	for (const auto& elem : list) {
		// Set the iteration variable in the context
		context.setVariable(variable, elem);

		// Evaluate the WHERE expression
		PropertyValue whereResult = evaluator.evaluate(whereExpr.get());

		// Check if WHERE result is boolean/truthy
		bool isTrue = false;
		if (EvaluationContext::isNull(whereResult)) {
			isTrue = false;
		} else if (whereResult.getType() == PropertyType::BOOLEAN) {
			isTrue = std::get<bool>(whereResult.getVariant());
		} else if (whereResult.getType() == PropertyType::INTEGER) {
			isTrue = (std::get<int64_t>(whereResult.getVariant()) != 0);
		} else if (whereResult.getType() == PropertyType::DOUBLE) {
			isTrue = (std::get<double>(whereResult.getVariant()) != 0.0);
		} else if (whereResult.getType() == PropertyType::STRING) {
			// Non-empty strings are truthy
			isTrue = !std::get<std::string>(whereResult.getVariant()).empty();
		} else if (whereResult.getType() == PropertyType::LIST) {
			// Non-empty lists are truthy
			isTrue = !std::get<std::vector<PropertyValue>>(whereResult.getVariant()).empty();
		}

		// Clear the temporary variable
		context.clearVariable(variable);

		if (isTrue) {
			matchCount++;
			// Early exit if more than one match found
			if (matchCount > 1) {
				return false;
			}
		}
	}

	// Return true only if exactly one match found
	return matchCount == 1;
}

} // namespace graph::query::expressions
