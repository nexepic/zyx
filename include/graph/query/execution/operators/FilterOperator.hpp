/**
 * @file FilterOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "../PhysicalOperator.hpp"
#include <functional>
#include <memory>

namespace graph::query::execution::operators {

	// A predicate function takes a Record and returns true if it should be kept.
	using Predicate = std::function<bool(const Record&)>;

	class FilterOperator : public PhysicalOperator {
	public:
		FilterOperator(std::unique_ptr<PhysicalOperator> child, Predicate predicate);
		~FilterOperator() override = default;

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override;
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override;

	private:
		std::unique_ptr<PhysicalOperator> child_;
		Predicate predicate_;
	};

} // namespace graph::query::execution::operators