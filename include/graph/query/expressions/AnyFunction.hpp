/**
 * @file AnyFunction.hpp
 * @date 2025/3/9
 *
 * Implementation of any() quantifier function
 */

#pragma once

#include "PredicateFunction.hpp"
#include <cstddef>

namespace graph::query::expressions {

/**
 * @class AnyFunction
 * @brief Implements the any() quantifier function
 *
 * Syntax: any(variable IN list WHERE condition)
 *
 * Returns TRUE if at least one element in the list satisfies the WHERE condition.
 * Returns FALSE for empty lists (no elements to satisfy).
 * Short-circuits on first matching element.
 *
 * Examples:
 * - any(x IN [1, 2, 3] WHERE x > 2) -> true
 * - any(x IN [1, 2, 3] WHERE x > 5) -> false
 * - any(x IN [] WHERE x > 0) -> false (empty list)
 */
class AnyFunction : public PredicateFunction {
public:
	[[nodiscard]] FunctionSignature getSignature() const override {
		// Signature is for validation only; actual parsing is custom
		return FunctionSignature("any", 1, SIZE_MAX, true);
	}

	[[nodiscard]] std::string getName() const override { return "any"; }

	/**
	 * @brief Evaluates the any() function
	 *
	 * @param variable The iteration variable name
	 * @param listExpr Expression that evaluates to a list
	 * @param whereExpr The WHERE clause expression
	 * @param context The evaluation context
	 * @return Boolean result as PropertyValue
	 */
	[[nodiscard]] PropertyValue evaluate(
		const std::string& variable,
		const std::unique_ptr<Expression>& listExpr,
		const std::unique_ptr<Expression>& whereExpr,
		EvaluationContext& context
	) const override;

protected:
	/**
	 * @brief Evaluates the predicate for any element
	 *
	 * @param list The list of PropertyValue elements
	 * @param variable The iteration variable name
	 * @param whereExpr The WHERE clause expression
	 * @param context The evaluation context
	 * @return true if at least one element satisfies the condition, false otherwise
	 */
	[[nodiscard]] bool evaluatePredicate(
		const std::vector<PropertyValue>& list,
		const std::string& variable,
		const std::unique_ptr<Expression>& whereExpr,
		EvaluationContext& context
	) const override;
};

} // namespace graph::query::expressions
