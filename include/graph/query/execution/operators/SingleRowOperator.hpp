/**
 * @file SingleRowOperator.hpp
 * @author Nexepic
 * @date 2026/1/9
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include <optional>
#include "graph/query/execution/PhysicalOperator.hpp"

namespace graph::query::execution::operators {

	class SingleRowOperator : public PhysicalOperator {
	public:
		SingleRowOperator() = default;

		void open() override { emitted_ = false; }

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

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }

		[[nodiscard]] std::string toString() const override { return "SingleRow"; }

	private:
		bool emitted_ = false;
	};

} // namespace graph::query::execution::operators
