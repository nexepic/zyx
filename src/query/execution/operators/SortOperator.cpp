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
#include <chrono>
#include <cstdint>
#include "graph/query/QueryContext.hpp"
#include <string>
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/debug/PerfTrace.hpp"
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
	using Clock = std::chrono::steady_clock;
	auto sortStart = Clock::now();

	auto comparator = [this](const Record &a, const Record &b) -> bool {
		for (const auto &item: sortItems_) {
			PropertyValue valA, valB;

			if (item.expression) {
				auto makeCtx = [this](const Record &r) {
					if (queryContext_ && !queryContext_->parameters.empty())
						return graph::query::expressions::EvaluationContext(r, queryContext_->parameters);
					return graph::query::expressions::EvaluationContext(r);
				};
				auto contextA = makeCtx(a);
				graph::query::expressions::ExpressionEvaluator evaluatorA(contextA);
				valA = evaluatorA.evaluate(item.expression.get());

				auto contextB = makeCtx(b);
				graph::query::expressions::ExpressionEvaluator evaluatorB(contextB);
				valB = evaluatorB.evaluate(item.expression.get());
			}

			if (valA != valB) {
				if (item.ascending)
					return valA < valB;
				else
					return valA > valB;
			}
		}
		return false;
	};

	static constexpr size_t PARALLEL_SORT_THRESHOLD = 8192;

	if (!threadPool_ || threadPool_->isSingleThreaded() ||
		sortedRecords_.size() < PARALLEL_SORT_THRESHOLD) {
		// Sequential sort for small datasets
		std::sort(sortedRecords_.begin(), sortedRecords_.end(), comparator);
		debug::PerfTrace::addDuration(
				"sort", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
														 sortStart)
												  .count()));
		return;
	}

	// Parallel sort: split into chunks, sort each in parallel, then k-way merge
	size_t numChunks = threadPool_->getThreadCount();
	size_t total = sortedRecords_.size();
	size_t chunkSize = total / numChunks;
	size_t remainder = total % numChunks;

	// Phase 1: Parallel sort of chunks
	struct ChunkRange {
		size_t begin, end;
	};
	std::vector<ChunkRange> chunks;
	chunks.reserve(numChunks);
	size_t pos = 0;
	for (size_t c = 0; c < numChunks; ++c) {
		size_t sz = chunkSize + (c < remainder ? 1 : 0);
		chunks.push_back({pos, pos + sz});
		pos += sz;
	}

	threadPool_->parallelFor(0, numChunks, [&](size_t c) {
		std::sort(sortedRecords_.begin() + chunks[c].begin,
				  sortedRecords_.begin() + chunks[c].end, comparator);
	});

	// Phase 2: Sequential k-way merge (merge pairs bottom-up)
	// This is an iterative merge: merge adjacent sorted chunks pairwise
	size_t step = 1;
	while (step < numChunks) {
		size_t numPairs = (numChunks + 2 * step - 1) / (2 * step);
		// Parallel merge of independent pairs
		threadPool_->parallelFor(0, numPairs, [&](size_t p) {
			size_t left = p * 2 * step;
			size_t right = left + step;
			if (right >= numChunks)
				return;

			size_t mergeBegin = chunks[left].begin;
			size_t mergeMid = chunks[right].begin;
			size_t mergeEnd = std::min(right + step, numChunks) <= numChunks
								 ? chunks[std::min(right + step, numChunks) - 1].end
								 : chunks[numChunks - 1].end;

			std::inplace_merge(sortedRecords_.begin() + mergeBegin,
							   sortedRecords_.begin() + mergeMid,
							   sortedRecords_.begin() + mergeEnd, comparator);
		});
		step *= 2;
	}

	debug::PerfTrace::addDuration(
			"sort", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
													 sortStart)
											  .count()));
}

} // namespace graph::query::execution::operators
