/**
 * @file SortOperator.cpp
 * @author Nexepic
 * @date 2025/12/22
 *
 * @copyright Copyright (c) 2025 Nexepic
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

#include "graph/query/execution/operators/SortOperator.hpp"
#include <algorithm>
#include <string>
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"

namespace graph::query::execution::operators {

void SortOperator::open() {
	if (child_)
		child_->open();
	sortedRecords_.clear();
	currentOutputIndex_ = 0;
	isSorted_ = false;
}

std::optional<RecordBatch> SortOperator::next() {
	// 1. Materialize Phase (Blocking)
	if (!isSorted_) {
		while (true) {
			auto batchOpt = child_->next();
			if (!batchOpt)
				break;

			// Accumulate all records
			auto &batch = *batchOpt;
			sortedRecords_.insert(sortedRecords_.end(), std::make_move_iterator(batch.begin()),
								  std::make_move_iterator(batch.end()));
		}

		// 2. Sort Phase
		performSort();
		isSorted_ = true;
	}

	// 3. Output Phase (Stream buffered results)
	if (currentOutputIndex_ >= sortedRecords_.size()) {
		return std::nullopt;
	}

	RecordBatch batch;
	batch.reserve(BATCH_SIZE);

	while (batch.size() < BATCH_SIZE && currentOutputIndex_ < sortedRecords_.size()) {
		batch.push_back(std::move(sortedRecords_[currentOutputIndex_++]));
	}

	return batch;
}

void SortOperator::close() {
	if (child_)
		child_->close();
	sortedRecords_.clear();
}

std::string SortOperator::toString() const {
	std::string s = "Sort(";
	for (size_t i = 0; i < sortItems_.size(); ++i) {
		const auto &item = sortItems_[i];
		if (item.expression) {
			s += item.expression->toString();
		}

		s += (item.ascending ? " ASC" : " DESC");

		if (i < sortItems_.size() - 1) {
			s += ", ";
		}
	}
	s += ")";
	return s;
}

void SortOperator::performSort() {
	std::sort(sortedRecords_.begin(), sortedRecords_.end(), [this](const Record &a, const Record &b) -> bool {
		for (const auto &item: sortItems_) {
			// Evaluate expressions to get comparison values
			PropertyValue valA, valB;

			if (item.expression) {
				graph::query::expressions::EvaluationContext contextA(a);
				graph::query::expressions::ExpressionEvaluator evaluatorA(contextA);
				valA = evaluatorA.evaluate(item.expression.get());

				graph::query::expressions::EvaluationContext contextB(b);
				graph::query::expressions::ExpressionEvaluator evaluatorB(contextB);
				valB = evaluatorB.evaluate(item.expression.get());
			}

			if (valA != valB) {
				if (item.ascending)
					return valA < valB;
				else
					return valA > valB;
			}
			// If equal, continue to next sort key
		}
		return false; // Strictly equal
	});
}

} // namespace graph::query::execution::operators
