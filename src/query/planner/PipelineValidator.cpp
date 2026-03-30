/**
 * @file PipelineValidator.cpp
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

#include "graph/query/planner/PipelineValidator.hpp"
#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"

namespace graph::query {

std::unique_ptr<execution::PhysicalOperator> PipelineValidator::ensureValidPipeline(
    std::unique_ptr<execution::PhysicalOperator> rootOp,
    const std::shared_ptr<QueryPlanner> &planner,
    const std::string &clauseName,
    ValidationMode mode) {

	if (rootOp) {
		// Pipeline already exists, return as-is
		return rootOp;
	}

	// Pipeline is empty - handle according to mode
	switch (mode) {
		case ValidationMode::ALLOW_EMPTY:
			// Auto-inject singleRowOp for standalone queries
			return planner->singleRowOp();

		case ValidationMode::REQUIRE_PRECEDING:
			// Throw descriptive error
			throw std::runtime_error(errorMessage(clauseName));

		default:
			// Should never happen
			throw std::runtime_error("Invalid validation mode for clause: " + clauseName);
	}
}

std::unique_ptr<logical::LogicalOperator> PipelineValidator::ensureValidLogicalPipeline(
    std::unique_ptr<logical::LogicalOperator> rootOp,
    const std::string &clauseName,
    ValidationMode mode) {

	if (rootOp) {
		// Pipeline already exists, return as-is
		return rootOp;
	}

	// Pipeline is empty - handle according to mode
	switch (mode) {
		case ValidationMode::ALLOW_EMPTY:
			// Auto-inject LogicalSingleRow for standalone queries
			return std::make_unique<logical::LogicalSingleRow>();

		case ValidationMode::REQUIRE_PRECEDING:
			// Throw descriptive error
			throw std::runtime_error(errorMessage(clauseName));

		default:
			// Should never happen
			throw std::runtime_error("Invalid validation mode for clause: " + clauseName);
	}
}

std::string PipelineValidator::errorMessage(const std::string &clauseName,
                                            const std::string &validPreceding) {
	return clauseName + " clause must follow a " + validPreceding + " clause.";
}

} // namespace graph::query
