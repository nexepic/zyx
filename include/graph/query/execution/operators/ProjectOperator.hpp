/**
 * @file ProjectOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/12
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "../PhysicalOperator.hpp"
#include <vector>
#include <string>
#include <memory>

namespace graph::query::execution::operators {

	class ProjectOperator : public PhysicalOperator {
	public:
		/**
		 * @brief Constructs a ProjectOperator.
		 * @param child Upstream operator.
		 * @param variableNames List of variable names to keep in the result.
		 */
		ProjectOperator(std::unique_ptr<PhysicalOperator> child,
						std::vector<std::string> variableNames);

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override;

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override;

		// --- Visualization ---
		[[nodiscard]] std::string toString() const override;
		[[nodiscard]] std::vector<const PhysicalOperator*> getChildren() const override;

	private:
		std::unique_ptr<PhysicalOperator> child_;
		std::vector<std::string> variableNames_;
	};

} // namespace graph::query::execution::operators