/**
 * @file FilterOperator.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/execution/operators/FilterOperator.hpp"

namespace graph::query::execution::operators {

	FilterOperator::FilterOperator(std::unique_ptr<PhysicalOperator> child, Predicate predicate)
		: child_(std::move(child)), predicate_(std::move(predicate)) {}

	void FilterOperator::open() {
		if (child_) child_->open();
	}

	std::optional<RecordBatch> FilterOperator::next() {
		while (true) {
			// Pull batch from child (Upstream)
			auto inputBatchOpt = child_->next();

			// Check for End of Stream
			if (!inputBatchOpt.has_value()) {
				return std::nullopt;
			}

			RecordBatch& inputBatch = inputBatchOpt.value();
			RecordBatch outputBatch;
			outputBatch.reserve(inputBatch.size());

			// Apply Predicate
			for (auto& record : inputBatch) {
				if (predicate_(record)) {
					outputBatch.push_back(std::move(record));
				}
			}

			// If we found matching records, return them.
			// If outputBatch is empty, it means the entire child batch was filtered out.
			// We must loop again to get the next batch from the child.
			if (!outputBatch.empty()) {
				return outputBatch;
			}
		}
	}

	void FilterOperator::close() {
		if (child_) child_->close();
	}

	std::vector<std::string> FilterOperator::getOutputVariables() const {
		return child_->getOutputVariables();
	}

} // namespace graph::query::execution::operators