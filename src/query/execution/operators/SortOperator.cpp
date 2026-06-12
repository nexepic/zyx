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
#include <limits>
#include "graph/query/QueryContext.hpp"
#include <string>
#include "graph/concurrent/ParallelExecutionPolicy.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"

namespace graph::query::execution::operators {

namespace {

	std::optional<PropertyValue> evaluateDirectAccess(
			const Record &record,
			const graph::query::expressions::Expression *expression) {
		using namespace graph::query::expressions;
		const auto type = expression->getExpressionType();
		if (type != ExpressionType::VARIABLE_REFERENCE && type != ExpressionType::PROPERTY_ACCESS) {
			return std::nullopt;
	}

	const auto *var = static_cast<const VariableReferenceExpression *>(expression);
	const auto &variableName = var->getVariableName();
	if (!var->hasProperty()) {
		if (auto node = record.getNodeRef(variableName)) {
			return PropertyValue(node->get().getId());
		}
		if (auto edge = record.getEdgeRef(variableName)) {
			return PropertyValue(edge->get().getId());
		}
		if (auto value = record.getValueRef(variableName)) {
			return value->get();
		}
		return PropertyValue();
	}

	const auto &propertyName = var->getPropertyName();
	if (auto node = record.getNodeRef(variableName)) {
		const auto &props = node->get().getProperties();
		auto it = props.find(propertyName);
		return it != props.end() ? std::optional<PropertyValue>(it->second) : std::optional<PropertyValue>(PropertyValue());
	}
	if (auto edge = record.getEdgeRef(variableName)) {
		const auto &props = edge->get().getProperties();
		auto it = props.find(propertyName);
		return it != props.end() ? std::optional<PropertyValue>(it->second) : std::optional<PropertyValue>(PropertyValue());
	}
	if (auto value = record.getValueRef(variableName);
	    value && value->get().getType() == PropertyType::MAP) {
		const auto &map = value->get().getMap();
		auto it = map.find(propertyName);
		return it != map.end() ? std::optional<PropertyValue>(it->second) : std::optional<PropertyValue>(PropertyValue());
	}

	return PropertyValue();
}

} // namespace

void SortOperator::open() {
	if (child_)
		child_->open();
	sortedRecords_.clear();
	currentOutputIndex_ = 0;
	isSorted_ = false;
}

std::optional<RecordBatch> SortOperator::next() {
	if (!isSorted_) {
		if (topNLimit_) {
			performTopN();
		} else {
			// 1. Materialize Phase (Blocking)
			while (true) {
				if (queryContext_) queryContext_->checkGuard();
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
		}
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
	if (topNLimit_) {
		if (!sortItems_.empty()) {
			s += ", ";
		}
		s += "LIMIT " + std::to_string(*topNLimit_);
	}
	s += ")";
	return s;
}

void SortOperator::performSort() {
	using Clock = std::chrono::steady_clock;
	auto sortStart = Clock::now();

	std::vector<DecoratedRecord> decorated;
	decorated.reserve(sortedRecords_.size());
	for (auto &record: sortedRecords_) {
		auto keys = evaluateSortKeys(record);
		decorated.push_back(DecoratedRecord{std::move(record), std::move(keys)});
	}
	sortedRecords_.clear();

	auto comparator = [this](const DecoratedRecord &a, const DecoratedRecord &b) -> bool {
		return compareKeys(a.sortKeys, b.sortKeys);
	};

	static constexpr size_t PARALLEL_SORT_THRESHOLD = 8192;
	const graph::concurrent::ParallelWorkEstimate estimate{
			.workloadKind = graph::concurrent::ParallelWorkloadKind::PWK_MEMORY_INTENSIVE,
			.partitions = decorated.size(),
			.estimatedItems = decorated.size(),
			.minPartitions = 2,
			.minItems = PARALLEL_SORT_THRESHOLD,
			.minItemsPerWorker = PARALLEL_SORT_THRESHOLD};
	const auto parallelDecision = graph::concurrent::decideParallelExecution(threadPool_, estimate);
	graph::concurrent::ScopedParallelExecutionTelemetry telemetry(threadPool_, estimate, parallelDecision);

	if (!parallelDecision.useParallel) {
		// Sequential sort for small datasets
		std::sort(decorated.begin(), decorated.end(), comparator);
		sortedRecords_.reserve(decorated.size());
		for (auto &item: decorated) {
			sortedRecords_.push_back(std::move(item.record));
		}
		telemetry.markCompleted();
		debug::PerfTrace::addDuration(
				"sort", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
														 sortStart)
												  .count()));
		return;
	}

	// Parallel sort: split into chunks, sort each in parallel, then k-way merge
	size_t numChunks = parallelDecision.workerCount;
	size_t total = decorated.size();
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

	threadPool_->parallelFor(0, numChunks, parallelDecision.workerCount, [&](size_t c) {
		std::sort(decorated.begin() + chunks[c].begin,
				  decorated.begin() + chunks[c].end, comparator);
	});

	// Phase 2: Sequential k-way merge (merge pairs bottom-up)
	// This is an iterative merge: merge adjacent sorted chunks pairwise
	const auto mergeStart = Clock::now();
	size_t step = 1;
	while (step < numChunks) {
		size_t numPairs = (numChunks + 2 * step - 1) / (2 * step);
		// Parallel merge of independent pairs
		threadPool_->parallelFor(0, numPairs, std::min(parallelDecision.workerCount, numPairs), [&](size_t p) {
			size_t left = p * 2 * step;
			size_t right = left + step;
			if (right >= numChunks)
				return;

			size_t mergeBegin = chunks[left].begin;
			size_t mergeMid = chunks[right].begin;
			size_t mergeEnd = chunks[std::min(right + step, numChunks) - 1].end;

			std::inplace_merge(decorated.begin() + mergeBegin,
							   decorated.begin() + mergeMid,
							   decorated.begin() + mergeEnd, comparator);
		});
		step *= 2;
	}
	telemetry.setMergeNs(static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - mergeStart).count()));

	sortedRecords_.reserve(decorated.size());
	for (auto &item: decorated) {
		sortedRecords_.push_back(std::move(item.record));
	}
	telemetry.markCompleted();

	debug::PerfTrace::addDuration(
			"sort", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
													 sortStart)
											  .count()));
}

void SortOperator::performTopN() {
	using Clock = std::chrono::steady_clock;
	uint64_t sortNanos = 0;

	sortedRecords_.clear();
	const size_t limit = topNLimit_.value_or(0);
	if (limit == 0) {
		debug::PerfTrace::addDuration("sort", 0);
		return;
	}

	std::vector<DecoratedRecord> heap;
	heap.reserve(limit);
	auto heapComparator = [this](const DecoratedRecord &a, const DecoratedRecord &b) -> bool {
		// compareKeys() defines final output order, so the heap root is the
		// current worst retained row and can be replaced by a better candidate.
		return compareKeys(a.sortKeys, b.sortKeys);
	};

	while (true) {
		if (queryContext_) queryContext_->checkGuard();
		auto batchOpt = child_->next();
		if (!batchOpt)
			break;

		auto sortStart = Clock::now();
		auto &batch = *batchOpt;
		for (auto &record: batch) {
			auto keys = evaluateSortKeys(record);
			DecoratedRecord candidate{std::move(record), std::move(keys)};
			if (heap.size() < limit) {
				heap.push_back(std::move(candidate));
				std::push_heap(heap.begin(), heap.end(), heapComparator);
			} else if (compareKeys(candidate.sortKeys, heap.front().sortKeys)) {
				std::pop_heap(heap.begin(), heap.end(), heapComparator);
				heap.back() = std::move(candidate);
				std::push_heap(heap.begin(), heap.end(), heapComparator);
			}
		}
		sortNanos += static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - sortStart).count());
	}

	auto finalSortStart = Clock::now();
	auto finalComparator = [this](const DecoratedRecord &a, const DecoratedRecord &b) -> bool {
		return compareKeys(a.sortKeys, b.sortKeys);
	};
	std::sort(heap.begin(), heap.end(), finalComparator);

	sortedRecords_.reserve(heap.size());
	for (auto &item: heap) {
		sortedRecords_.push_back(std::move(item.record));
	}
	sortNanos += static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - finalSortStart).count());

	debug::PerfTrace::addDuration("sort", sortNanos);
}

std::vector<PropertyValue> SortOperator::evaluateSortKeys(const Record &record) const {
	std::vector<PropertyValue> keys;
	keys.reserve(sortItems_.size());

	for (const auto &item: sortItems_) {
		PropertyValue value;
		if (item.expression) {
			if (auto directValue = evaluateDirectAccess(record, item.expression.get())) {
				value = std::move(*directValue);
			} else if (queryContext_ && !queryContext_->parameters.empty()) {
				graph::query::expressions::EvaluationContext context(record, queryContext_->parameters);
				graph::query::expressions::ExpressionEvaluator evaluator(context);
				value = evaluator.evaluate(item.expression.get());
			} else {
				graph::query::expressions::EvaluationContext context(record);
				graph::query::expressions::ExpressionEvaluator evaluator(context);
				value = evaluator.evaluate(item.expression.get());
			}
		}
		keys.push_back(std::move(value));
	}

	return keys;
}

bool SortOperator::compareKeys(const std::vector<PropertyValue> &left,
                               const std::vector<PropertyValue> &right) const {
	const size_t count = std::min(left.size(), right.size());
	for (size_t i = 0; i < count; ++i) {
		if (left[i] == right[i]) {
			continue;
		}
		return sortItems_[i].ascending ? left[i] < right[i] : left[i] > right[i];
	}
	return false;
}

size_t SortOperator::normalizeLimit(int64_t limit) {
	if (limit <= 0) {
		return 0;
	}
	const auto unsignedLimit = static_cast<uint64_t>(limit);
	const auto maxSize = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
	return static_cast<size_t>(std::min(unsignedLimit, maxSize));
}

} // namespace graph::query::execution::operators
