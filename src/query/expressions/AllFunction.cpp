/**
 * @file AllFunction.cpp
 * @date 2025/3/9
 *
 * Implementation of AllFunction
 */

#include "graph/query/expressions/AllFunction.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include <stdexcept>

namespace graph::query::expressions {

PropertyValue AllFunction::evaluate(
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
			"all() function requires a list as the second argument"
		);
	}

	// Extract the list from PropertyValue
	const std::vector<PropertyValue>& list = std::get<std::vector<PropertyValue>>(listValue.getVariant());

	// Evaluate the predicate
	bool result = evaluatePredicate(list, variable, whereExpr, context);

	return PropertyValue(result);
}

bool AllFunction::evaluatePredicate(
	const std::vector<PropertyValue>& list,
	const std::string& variable,
	const std::unique_ptr<Expression>& whereExpr,
	EvaluationContext& context
) const {
	// Empty list: all() returns true (vacuous truth)
	if (list.empty()) {
		return true;
	}

	// Short-circuit: return false on first non-matching element
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

		if (!isTrue) {
			return false; // Short-circuit
		}
	}

	return true; // All elements passed
}

} // namespace graph::query::expressions
