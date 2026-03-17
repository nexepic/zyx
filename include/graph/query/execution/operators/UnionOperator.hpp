/**
 * @file UnionOperator.hpp
 * @author ZYX Contributors
 * @date 2026
 *
 * @copyright Copyright (c) 2026
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
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>
#include "../PhysicalOperator.hpp"
#include "../Record.hpp"

namespace graph::query::execution::operators {

/**
 * @class UnionOperator
 * @brief Combines results from two queries using UNION or UNION ALL.
 *
 * UNION ALL: Concatenates results from both queries (no deduplication)
 * UNION: Concatenates results and removes duplicates
 *
 * The operator:
 * 1. Consumes all records from the left query
 * 2. Consumes all records from the right query
 * 3. For UNION: filters out duplicates from the right query
 * 4. Returns the combined result set
 *
 * Deduplication uses serialized records for hash comparison.
 */
class UnionOperator : public PhysicalOperator {
public:
	/**
	 * @brief Constructs a UnionOperator.
	 *
	 * @param left The left query operator.
	 * @param right The right query operator.
	 * @param all If true, performs UNION ALL (no deduplication); if false, performs UNION (with deduplication).
	 */
	UnionOperator(std::unique_ptr<PhysicalOperator> left,
	              std::unique_ptr<PhysicalOperator> right,
	              bool all = false);

	/**
	 * @brief Destructor
	 */
	~UnionOperator() override = default;

	/**
	 * @brief Opens the operator pipeline.
	 * Opens both child operators and buffers the right query results.
	 */
	void open() override;

	/**
	 * @brief Returns the next batch of records.
	 * Returns records from left query first, then from right query (with deduplication if UNION).
	 *
	 * @return Optional RecordBatch, or nullopt if exhausted.
	 */
	std::optional<RecordBatch> next() override;

	/**
	 * @brief Closes the operator pipeline.
	 * Closes both child operators.
	 */
	void close() override;

	/**
	 * @brief Gets the output variable names.
	 * Validates that both queries have the same output columns.
	 *
	 * @return Vector of variable names.
	 */
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override;

	// --- Visualization ---

	/**
	 * @brief Returns a string representation for debugging.
	 */
	[[nodiscard]] std::string toString() const override;

	/**
	 * @brief Returns child operators for visualization.
	 */
	[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override;

private:
	std::unique_ptr<PhysicalOperator> left_;
	std::unique_ptr<PhysicalOperator> right_;
	bool all_; // true = UNION ALL, false = UNION
	std::vector<std::string> outputVariables_; // Cached output variables

	// State management
	enum class State {
		NOT_OPENED,
		CONSUMING_LEFT,
		CONSUMING_RIGHT,
		FINISHED
	};
	State state_;

	// For UNION: track seen records to deduplicate
	std::unordered_set<std::string> seenRecords_;

	// Buffer for right query results (needed for deduplication)
	std::vector<Record> bufferedRightRecords_;
	size_t bufferedRightIndex_;

	/**
	 * @brief Serializes a record to a string for hash comparison.
	 * Used for deduplication in UNION.
	 *
	 * @param record The record to serialize.
	 * @return String representation of the record.
	 */
	std::string serializeRecord(const Record& record) const;
};

} // namespace graph::query::execution::operators
