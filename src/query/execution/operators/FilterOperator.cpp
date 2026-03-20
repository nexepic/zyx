/**
 * @file FilterOperator.cpp
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

#include "graph/query/execution/operators/FilterOperator.hpp"

namespace graph::query::execution::operators {

void FilterOperator::open() {
	if (child_)
		child_->open();
}

std::optional<RecordBatch> FilterOperator::next() {
	// Standard Filter Logic:
	// Keep pulling batches from child until we find matching records or child is exhausted.
	while (true) {
		auto batchOpt = child_->next();
		if (!batchOpt)
			return std::nullopt; // End of stream

		RecordBatch &inputBatch = *batchOpt;
		RecordBatch outputBatch;

		// Optimization: Reserve memory to avoid reallocations
		outputBatch.reserve(inputBatch.size());

		for (auto &record: inputBatch) {
			if (predicate_(record)) {
				outputBatch.push_back(std::move(record));
			}
		}

		// If we found valid records, return them.
		if (!outputBatch.empty()) {
			return outputBatch;
		}

		// If outputBatch is empty, it means the entire input batch was filtered out.
		// We loop again immediately to fetch the next batch.
	}
}

void FilterOperator::close() {
	if (child_)
		child_->close();
}

std::string FilterOperator::toString() const { return "Filter(" + predicateStr_ + ")"; }

} // namespace graph::query::execution::operators
