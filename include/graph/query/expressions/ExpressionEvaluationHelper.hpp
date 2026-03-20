/**
 * @file ExpressionEvaluationHelper.hpp
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

#pragma once

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/expressions/Expression.hpp"
#include <vector>

namespace graph::storage { class DataManager; }

namespace graph::query::expressions {

/**
 * @class ExpressionEvaluationHelper
 * @brief Simplified interface for expression evaluation to reduce boilerplate code.
 *
 * This helper class provides a streamlined API for evaluating expressions,
 * wrapping ExpressionEvaluator and EvaluationContext construction into
 * simple static methods.
 *
 * Benefits:
 * - Reduces repetitive boilerplate code in operators
 * - Provides consistent evaluation patterns
 * - Enables future performance optimizations (batching, caching)
 *
 * Usage:
 * @code
 *   // Instead of:
 *   EvaluationContext context(record);
 *   ExpressionEvaluator evaluator(context);
 *   PropertyValue value = evaluator.evaluate(expr);
 *
 *   // Use:
 *   PropertyValue value = ExpressionEvaluationHelper::evaluate(expr, record);
 * @endcode
 */
class ExpressionEvaluationHelper {
public:
	/**
	 * @brief Evaluates an expression against a record.
	 *
	 * @param expr The expression to evaluate (may be nullptr)
	 * @param record The record containing variable bindings
	 * @return The computed PropertyValue (NULL if expr is nullptr)
	 * @throws ExpressionEvaluationException on evaluation errors
	 */
	[[nodiscard]] static PropertyValue evaluate(const Expression *expr,
	                                            const execution::Record &record,
	                                            storage::DataManager *dataManager = nullptr);

	/**
	 * @brief Evaluates an expression and converts to boolean.
	 *
	 * Convenience method for filter/condition evaluation.
	 *
	 * @param expr The expression to evaluate (may be nullptr)
	 * @param record The record containing variable bindings
	 * @return Boolean value (false if expr is nullptr)
	 * @throws ExpressionEvaluationException on evaluation errors
	 */
	[[nodiscard]] static bool evaluateBool(const Expression *expr,
	                                       const execution::Record &record);

	/**
	 * @brief Evaluates an expression and converts to integer.
	 *
	 * @param expr The expression to evaluate (may be nullptr)
	 * @param record The record containing variable bindings
	 * @return Integer value (0 if expr is nullptr)
	 * @throws ExpressionEvaluationException on evaluation errors
	 */
	[[nodiscard]] static int64_t evaluateInt(const Expression *expr,
	                                         const execution::Record &record);

	/**
	 * @brief Evaluates an expression and converts to double.
	 *
	 * @param expr The expression to evaluate (may be nullptr)
	 * @param record The record containing variable bindings
	 * @return Double value (0.0 if expr is nullptr)
	 * @throws ExpressionEvaluationException on evaluation errors
	 */
	[[nodiscard]] static double evaluateDouble(const Expression *expr,
	                                           const execution::Record &record);

	/**
	 * @brief Batch evaluation for performance optimization.
	 *
	 * Evaluates the same expression against multiple records.
	 * More efficient than calling evaluate() in a loop due to
	 * reduced context construction overhead.
	 *
	 * @param expr The expression to evaluate
	 * @param records Vector of records to evaluate against
	 * @return Vector of computed PropertyValues (one per record)
	 * @throws ExpressionEvaluationException on evaluation errors
	 */
	[[nodiscard]] static std::vector<PropertyValue> evaluateBatch(
	    const Expression *expr, const std::vector<execution::Record> &records);
};

} // namespace graph::query::expressions
