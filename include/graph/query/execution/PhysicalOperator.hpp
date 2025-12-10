/**
 * @file PhysicalOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <vector>
#include <string>
#include <optional>
#include "Record.hpp"

namespace graph::query::execution {

	class PhysicalOperator {
	public:
		virtual ~PhysicalOperator() = default;

		/**
		 * @brief Prepares the operator for execution.
		 * Allocates resources, opens files, or initializes iterators.
		 */
		virtual void open() = 0;

		/**
		 * @brief Pulls the next batch of records from this operator.
		 *
		 * @return std::optional<RecordBatch> containing data, or std::nullopt if the stream is exhausted.
		 */
		virtual std::optional<RecordBatch> next() = 0;

		/**
		 * @brief Cleans up resources.
		 */
		virtual void close() = 0;

		/**
		 * @brief Returns the schema (variables) produced by this operator.
		 */
		[[nodiscard]] virtual std::vector<std::string> getOutputVariables() const = 0;
	};

} // namespace graph::query::execution