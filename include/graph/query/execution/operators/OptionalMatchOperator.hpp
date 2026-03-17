/**
 * @file OptionalMatchOperator.hpp
 * @author ZYX Graph Database Team
 * @date 2025
 *
 * @copyright Copyright (c) 2025 ZYX Graph Database
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

#include <memory>
#include <string>
#include <vector>
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/execution/Record.hpp"

namespace graph::query::execution::operators {

/**
 * @class OptionalMatchOperator
 * @brief Implements OPTIONAL MATCH with left outer join semantics.
 *
 * OPTIONAL MATCH returns NULL for unmatched pattern variables instead of
 * filtering out the row. This implements left outer join behavior:
 *
 * - If the pattern matches: extend the record with matched variables
 * - If the pattern doesn't match: extend the record with NULL values
 *
 * Example:
 *   MATCH (a:Person)
 *   OPTIONAL MATCH (a)-[:KNOWS]->(b)
 *   RETURN a.name, b.name
 *
 *   If Person 'Alice' knows no one, returns: ('Alice', NULL)
 *   Regular MATCH would filter out 'Alice' entirely
 */
class OptionalMatchOperator : public PhysicalOperator {
public:
	/**
	 * @brief Construct an OptionalMatchOperator.
	 *
	 * @param input The input operator (left side of join)
	 * @param optionalPattern The pattern matching operator (right side)
	 * @param requiredVariables Variables that the pattern introduces (will be NULL if unmatched)
	 */
	OptionalMatchOperator(
		std::unique_ptr<PhysicalOperator> input,
		std::unique_ptr<PhysicalOperator> optionalPattern,
		std::vector<std::string> requiredVariables);

	~OptionalMatchOperator() override = default;

	// Disable copy, enable move
	OptionalMatchOperator(const OptionalMatchOperator&) = delete;
	OptionalMatchOperator& operator=(const OptionalMatchOperator&) = delete;
	OptionalMatchOperator(OptionalMatchOperator&&) = default;
	OptionalMatchOperator& operator=(OptionalMatchOperator&&) = default;

	/**
	 * @brief Initialize the operator.
	 *
	 * Opens both the input and optional pattern operators.
	 */
	void open() override;

	/**
	 * @brief Get the next batch of records.
	 *
	 * Implements left outer join logic:
	 * 1. Read record from input
	 * 2. Try to get match from optional pattern
	 * 3. If match exists: return input + matched variables
	 * 4. If no match: return input + NULL values for required variables
	 *
	 * @return RecordBatch with the joined records, or std::nullopt if exhausted
	 */
	std::optional<RecordBatch> next() override;

	/**
	 * @brief Close the operator and release resources.
	 */
	void close() override;

	/**
	 * @brief Get the output variable names from this operator.
	 *
	 * @return Vector of variable names in output records
	 */
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override;

	/**
	 * @brief Get a string representation for debugging.
	 */
	[[nodiscard]] std::string toString() const override;

private:
	std::unique_ptr<PhysicalOperator> input_;
	std::unique_ptr<PhysicalOperator> optionalPattern_;
	std::vector<std::string> requiredVariables_;

	// State management
	RecordBatch currentInputBatch_;
	size_t currentInputIndex_;
	bool inputExhausted_;

	/**
	 * @brief Create a NULL-extended record.
	 *
	 * When the optional pattern doesn't match, extend the input record
	 * with NULL values for all required variables.
	 *
	 * @param inputRecord The record from the input operator
	 * @return A new record with input data + NULL values
	 */
	Record createNullExtendedRecord(const Record& inputRecord);

	/**
	 * @brief Try to match the optional pattern for a given input record.
	 *
	 * @param inputRecord The record to match against
	 * @return Vector of matching records (may be empty)
	 */
	std::vector<Record> tryMatchPattern(const Record& inputRecord);
};

} // namespace graph::query::execution::operators
