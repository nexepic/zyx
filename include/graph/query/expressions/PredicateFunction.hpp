/**
 * @file PredicateFunction.hpp
 * @date 2025/3/9
 *
 * Base class for quantifier functions: all(), any(), none(), single()
 */

#pragma once

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/core/PropertyTypes.hpp"
#include <memory>
#include <string>
#include <vector>

namespace graph::query::expressions {

class EvaluationContext;

/**
 * Base class for quantifier functions that iterate over lists
 * and evaluate a WHERE clause for each element.
 *
 * These functions have special Cypher syntax:
 * - all(variable IN list WHERE condition)
 * - any(variable IN list WHERE condition)
 * - none(variable IN list WHERE condition)
 * - single(variable IN list WHERE condition)
 *
 * NOTE: This is a partial implementation. Quantifier functions have special
 * Cypher syntax that requires custom parsing in ExpressionBuilder. Currently,
 * this class provides the structure for the predicate evaluation logic.
 *
 * Full implementation requires:
 * - Special parsing for: all(x IN list WHERE x > 0)
 * - Expression evaluation with variable binding
 * - Iteration over list elements with WHERE clause evaluation
 */
class PredicateFunction {
public:
	virtual ~PredicateFunction() = default;

	/**
	 * @brief Gets the function signature.
	 */
	[[nodiscard]] virtual FunctionSignature getSignature() const = 0;

	/**
	 * @brief Gets the function name.
	 */
	[[nodiscard]] virtual std::string getName() const = 0;

	/**
	 * @brief Evaluates the predicate function.
	 *
	 * @param variable The iteration variable name
	 * @param listExpr Expression that evaluates to a list
	 * @param whereExpr The WHERE clause expression
	 * @param context The evaluation context
	 * @return Boolean result as PropertyValue
	 */
	[[nodiscard]] virtual PropertyValue evaluate(
		const std::string& variable,
		const std::unique_ptr<Expression>& listExpr,
		const std::unique_ptr<Expression>& whereExpr,
		EvaluationContext& context
	) const = 0;

protected:
	/**
	 * @brief Common logic: iterate list + evaluate WHERE + return bool
	 *
	 * @param list The list of PropertyValue elements
	 * @param variable The iteration variable name
	 * @param whereExpr The WHERE clause expression
	 * @param context The evaluation context
	 * @return Boolean result
	 */
	[[nodiscard]] virtual bool evaluatePredicate(
		const std::vector<PropertyValue>& list,
		const std::string& variable,
		const std::unique_ptr<Expression>& whereExpr,
		EvaluationContext& context
	) const = 0;
};

} // namespace graph::query::expressions
