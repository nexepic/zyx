/**
 * @file PipelineValidator.hpp
 * @author Metrix Contributors
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

#include <memory>
#include <string>

namespace graph::query {

// Forward declarations
class QueryPlanner;
namespace execution {
class PhysicalOperator;
}

/**
 * @class PipelineValidator
 * @brief Unified pipeline validation for Cypher clauses.
 *
 * This class provides consistent validation behavior across all clause handlers,
 * eliminating code duplication and ensuring uniform error messages.
 *
 * **Validation Modes:**
 * - ALLOW_EMPTY: Automatically injects singleRowOp if pipeline is empty
 * - REQUIRE_PRECEDING: Throws error if pipeline is empty (requires preceding clause)
 *
 * **Usage:**
 * @code
 * // ALLOW_EMPTY mode (for RETURN, UNWIND)
 * rootOp = PipelineValidator::ensureValidPipeline(
 *     std::move(rootOp), planner, "RETURN",
 *     PipelineValidator::ValidationMode::ALLOW_EMPTY
 * );
 *
 * // REQUIRE_PRECEDING mode (for SET, DELETE, WITH)
 * rootOp = PipelineValidator::ensureValidPipeline(
 *     std::move(rootOp), planner, "SET",
 *     PipelineValidator::ValidationMode::REQUIRE_PRECEDING
 * );
 * @endcode
 */
class PipelineValidator {
public:
	/**
	 * @brief Validation mode for pipeline checks.
	 */
	enum class ValidationMode {
		/**
		 * Allow empty pipeline.
		 * If rootOp is null, automatically injects singleRowOp.
		 * Used by: RETURN, UNWIND
		 */
		ALLOW_EMPTY,

		/**
		 * Require preceding clause.
		 * If rootOp is null, throw descriptive error.
		 * Used by: SET, DELETE, REMOVE, WITH
		 */
		REQUIRE_PRECEDING
	};

	/**
	 * @brief Ensure pipeline is valid according to the specified mode.
	 *
	 * @param rootOp The current pipeline root (may be null)
	 * @param planner The query planner (for singleRowOp injection)
	 * @param clauseName The name of the clause (for error messages)
	 * @param mode The validation mode to apply
	 * @return Validated (possibly injected) pipeline root
	 * @throws std::runtime_error if validation fails in REQUIRE_PRECEDING mode
	 */
	[[nodiscard]] static std::unique_ptr<execution::PhysicalOperator> ensureValidPipeline(
	    std::unique_ptr<execution::PhysicalOperator> rootOp,
	    const std::shared_ptr<QueryPlanner> &planner,
	    const std::string &clauseName,
	    ValidationMode mode = ValidationMode::REQUIRE_PRECEDING);

private:
	// Default preceding clause suggestions for error messages
	static constexpr const char *DEFAULT_PRECEDING = "MATCH or CREATE";

	/**
	 * @brief Generate error message for empty pipeline.
	 */
	[[nodiscard]] static std::string errorMessage(const std::string &clauseName,
	                                               const std::string &validPreceding = DEFAULT_PRECEDING);
};

} // namespace graph::query
