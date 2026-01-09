/**
 * @file SingleRowOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2026/1/9
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 **/

#pragma once

#include <optional>
#include "graph/query/execution/PhysicalOperator.hpp"

namespace graph::query::execution::operators {

	class SingleRowOperator : public PhysicalOperator {
	public:
		SingleRowOperator() = default;

		void open() override {
			emitted_ = false;
		}

		std::optional<RecordBatch> next() override {
			if (emitted_) {
				return std::nullopt;
			}
			emitted_ = true;

			// Return one empty record
			RecordBatch batch;
			batch.push_back(Record{});
			return batch;
		}

		void close() override {}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {};
		}

		[[nodiscard]] std::string toString() const override {
			return "SingleRow";
		}

	private:
		bool emitted_ = false;
	};

} // namespace graph::query::execution::operators