/**
 * @file NodeScanOperator.hpp
 * @author Nexepic
 * @date 2025/12/10
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

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "../NodeCandidateSource.hpp"
#include "../NodeBatchLoader.hpp"
#include "../NodeColumnBatchMaterializer.hpp"
#include "../NodeScanRequirementUtils.hpp"
#include "../NodeScanRequirements.hpp"
#include "../PhysicalOperator.hpp"
#include "../ScanConfigs.hpp"
#include "graph/concurrent/ParallelOperatorExecutor.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/SegmentReadUtils.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

	class NodeScanOperator : public PhysicalOperator {
	public:
		NodeScanOperator(std::shared_ptr<storage::DataManager> dm, std::shared_ptr<indexes::IndexManager> im,
						 NodeScanConfig config) : dm_(std::move(dm)), im_(std::move(im)), config_(std::move(config)) {}

		NodeScanOperator(std::shared_ptr<storage::DataManager> dm, std::shared_ptr<indexes::IndexManager> im,
		                 NodeScanConfig config, NodeScanRequirements requirements) :
			dm_(std::move(dm)), im_(std::move(im)), config_(std::move(config)),
			requirements_(std::move(requirements)) {}

		void open() override {
			currentIdx_ = 0;
			emittedRows_ = 0;
			candidateIds_.clear();

			NodeCandidateSource candidateSource(dm_, im_);
			auto candidateSet = candidateSource.collectWithMetadata(config_);
			candidateIds_ = std::move(candidateSet.ids);
			labelsSatisfied_ = candidateSet.labelsSatisfied;
			activeSatisfied_ = candidateSet.activeOnly;

			// Pre-resolve label IDs for the scan loop (optimization)
			targetLabelIds_.clear();
			if (!labelsSatisfied_ && !config_.labels.empty()) {
				for (const auto &lbl : config_.labels) {
					targetLabelIds_.push_back(dm_->resolveTokenId(lbl));
				}
			}
		}

		void setOutputLimitHint(size_t limit) override {
			outputLimitHint_ = limit;
		}

		std::optional<RecordBatch> next() override {
			using Clock = std::chrono::steady_clock;
			const bool profileEnabled = debug::PerfTrace::isEnabled();
			const auto scanStart = profileEnabled ? Clock::now() : Clock::time_point{};

			if (currentIdx_ >= candidateIds_.size())
				return std::nullopt;
			if (outputLimitHint_.has_value() && emittedRows_ >= *outputLimitHint_) {
				return std::nullopt;
			}

			// When thread pool is available and enough candidates remain,
			// use a larger batch to amortize parallelization overhead.
			static constexpr size_t PARALLEL_SCAN_THRESHOLD = 4096;
			size_t effectiveBatchSize = chooseEffectiveBatchSize(candidateIds_.size() - currentIdx_);
			bool canParallelize = threadPool_ && !threadPool_->isSingleThreaded();
			size_t remaining = candidateIds_.size() - currentIdx_;
			if (canParallelize && remaining >= PARALLEL_SCAN_THRESHOLD) {
				// Use all remaining candidates as one large parallel batch unless a downstream LIMIT
				// asks for bounded source work.
				effectiveBatchSize = outputLimitHint_.has_value() ? std::min(effectiveBatchSize, remaining) : remaining;
			}

			size_t batchStart = currentIdx_;
			size_t batchEnd = std::min(currentIdx_ + effectiveBatchSize, candidateIds_.size());
			size_t batchCount = batchEnd - batchStart;
			currentIdx_ = batchEnd;

			if (!requirements_.needsFullNode()) {
				NodeCandidateSet candidateMetadata;
				candidateMetadata.labelsSatisfied = labelsSatisfied_;
				candidateMetadata.activeOnly = activeSatisfied_;
				const auto requirements = relaxSatisfiedCandidateChecks(requirements_, candidateMetadata);
				NodeBatchLoader loader(dm_, threadPool_);
				auto columnBatch = loader.load(candidateIds_, batchStart, batchEnd, config_, requirements);
				auto batch = materializeNodeRecords(columnBatch, config_.variable, *dm_, requirements);
				trimToOutputLimit(batch);
				if (batch.empty() && currentIdx_ >= candidateIds_.size()) // ZYX_COV_EXCL_LINE
					return std::nullopt;
				if (profileEnabled) {
					debug::PerfTrace::addDuration(
							"scan.sequential",
							static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
															scanStart)
													 .count()));
				}
				return batch;
			}

			if (canParallelize && batchCount >= PARALLEL_SCAN_THRESHOLD) {
				auto batch = parallelLoadBatch(batchStart, batchEnd);
				if (batch.has_value()) {
					trimToOutputLimit(*batch);
				}
				if (profileEnabled) {
					debug::PerfTrace::addDuration(
							"scan.parallel",
							static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
														   Clock::now() - scanStart)
													   .count()));
				}
				return batch;
			}

			// Sequential path
			RecordBatch batch;
			batch.reserve(batchCount);

			for (size_t idx = batchStart; idx < batchEnd; ++idx) {
				if (outputLimitHint_.has_value() && batch.size() >= (*outputLimitHint_ - emittedRows_)) {
					break;
				}
				int64_t id = candidateIds_[idx];
				Node node = dm_->getNode(id);

				if (!node.isActive()) {
					continue;
				}

				if (!targetLabelIds_.empty()) {
					bool allMatch = true;
					for (int64_t tid : targetLabelIds_) {
						if (!node.hasLabelId(tid)) {
							allMatch = false; break;
						}
					}
					if (!allMatch) continue;
				}

				auto props = dm_->getNodeProperties(id);
				node.setProperties(std::move(props));

				Record r;
				r.setNode(config_.variable, std::move(node));
				batch.push_back(std::move(r));
			}
			emittedRows_ += batch.size();

			if (batch.empty() && currentIdx_ >= candidateIds_.size()) // ZYX_COV_EXCL_LINE
				return std::nullopt;

			if (profileEnabled) {
				debug::PerfTrace::addDuration(
						"scan.sequential",
						static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
														scanStart)
												 .count()));
			}
			return batch;
		}

		void close() override { candidateIds_.clear(); }

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {config_.variable}; }

		[[nodiscard]] std::string toString() const override { return config_.toString(); }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
		NodeScanConfig config_;
		NodeScanRequirements requirements_;

		std::vector<int64_t> candidateIds_;
		size_t currentIdx_ = 0;
		std::vector<int64_t> targetLabelIds_; // Cached label IDs (AND semantics)
		bool labelsSatisfied_ = false;
		bool activeSatisfied_ = false;
		std::optional<size_t> outputLimitHint_;
		size_t emittedRows_ = 0;

		static constexpr size_t kParallelNodeScanMinSegments = 32;
		static constexpr size_t kParallelNodeScanMinTasks = 2;

		[[nodiscard]] size_t chooseEffectiveBatchSize(size_t remainingCandidates) const {
			size_t batchSize = DEFAULT_BATCH_SIZE;
			if (outputLimitHint_.has_value()) {
				const size_t remainingRows = *outputLimitHint_ - emittedRows_;
				// Small LIMIT queries should not trigger a full remaining parallel scan.
				const size_t boundedWindow = std::max<size_t>(remainingRows, DEFAULT_BATCH_SIZE / 4);
				batchSize = std::min(batchSize, boundedWindow);
			}
			return std::min(batchSize, remainingCandidates);
		}

		void trimToOutputLimit(RecordBatch &batch) {
			if (!outputLimitHint_.has_value()) {
				emittedRows_ += batch.size();
				return;
			}
			const size_t remainingRows = *outputLimitHint_ - emittedRows_;
			if (batch.size() > remainingRows) {
				batch.resize(remainingRows);
			}
			emittedRows_ += batch.size();
		}

		std::optional<RecordBatch> parallelLoadBatch(size_t batchStart, size_t batchEnd) {
			using Clock = std::chrono::steady_clock;
			const bool profileEnabled = debug::PerfTrace::isEnabled();

			// Get the sorted segment index for Nodes
			const auto &segIndex = dm_->getSegmentIndexManager()->getNodeSegmentIndex();
			int64_t startId = candidateIds_[batchStart];
			int64_t endId = candidateIds_[batchEnd - 1];

			// Filter to segments that overlap our ID range
			std::vector<size_t> relevantSegIdxs;
			relevantSegIdxs.reserve(segIndex.size());
			for (size_t s = 0; s < segIndex.size(); ++s) {
				if (segIndex[s].endId >= startId && segIndex[s].startId <= endId)
					relevantSegIdxs.push_back(s);
			}

			// For non-FULL_SCAN, build a set for fast membership testing
			std::unordered_set<int64_t> candidateSet;
			if (config_.type != ScanType::FULL_SCAN) {
				candidateSet.insert(
					candidateIds_.begin() + batchStart,
					candidateIds_.begin() + batchEnd);
			}

			std::vector<int64_t> dirtyCandidateIds;
			std::unordered_set<int64_t> dirtyCandidateSet;
			if (dm_->hasUnsavedChanges()) {
				const size_t localCandidateCount = batchEnd - batchStart;
				dirtyCandidateIds.reserve(localCandidateCount);
				dirtyCandidateSet.reserve(localCandidateCount);
				for (size_t idx = batchStart; idx < batchEnd; ++idx) {
					const int64_t id = candidateIds_[idx];
					if (dm_->getDirtyInfo<Node>(id).has_value()) {
						dirtyCandidateIds.push_back(id);
						dirtyCandidateSet.insert(id);
					}
				}
			}

			if (relevantSegIdxs.empty() && dirtyCandidateIds.empty()) {
				if (currentIdx_ >= candidateIds_.size()) // ZYX_COV_EXCL_LINE
					return std::nullopt;
				return RecordBatch{};
			}

			// ── Phase 1: Parallel bulk-read Node segments + collect property IDs ──
			// Build coalesced groups to merge consecutive segment reads into single large I/O calls
			auto groups = storage::buildCoalescedGroups(relevantSegIdxs, segIndex);
			size_t numSegs = relevantSegIdxs.size();
			std::vector<std::vector<Node>> segNodes(numSegs);
			std::vector<std::vector<int64_t>> segPropIds(numSegs);

			struct EmptyParallelState {};
			const graph::concurrent::ParallelOperatorOptions readOptions{
					.phase = "scan.parallel.phase1",
					.workloadKind = graph::concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
					.estimatedItems = numSegs,
					.estimatedBytes = numSegs * storage::TOTAL_SEGMENT_SIZE,
					.minPartitions = kParallelNodeScanMinTasks,
					.minItems = kParallelNodeScanMinSegments};
			auto readGroup = [&](size_t gi, EmptyParallelState &) {
				const auto &group = groups[gi];
				// Single pread for the entire coalesced group
				size_t totalBytes = group.segCount * storage::TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				auto groupOffset = static_cast<int64_t>(group.startOffset);
				ssize_t n = dm_->preadBytes(groupBuf.data(), totalBytes, groupOffset);
				if (n < static_cast<ssize_t>(totalBytes)) // ZYX_COV_EXCL_LINE
					return;

				constexpr size_t entitySize = Node::getTotalSize();

				// Process each segment within the coalesced buffer
				for (size_t mi = 0; mi < group.memberIndices.size(); ++mi) {
					size_t si = group.memberIndices[mi];
					size_t bufOffset = mi * storage::TOTAL_SEGMENT_SIZE;

					// Parse the segment header from the buffer
					storage::SegmentHeader header;
					std::memcpy(&header, groupBuf.data() + bufOffset, sizeof(storage::SegmentHeader));
					if (header.used == 0) // ZYX_COV_EXCL_LINE
						continue;

					const char *dataBuf = groupBuf.data() + bufOffset + sizeof(storage::SegmentHeader);
					auto &localNodes = segNodes[si];
					auto &localPropIds = segPropIds[si];

					for (uint32_t i = 0; i < header.used; ++i) {
						int64_t entityId = header.start_id + i;
						if (entityId < startId || entityId > endId)
							continue;
						if (!candidateSet.empty() && !candidateSet.count(entityId)) // ZYX_COV_EXCL_LINE
							continue;
						if (!dirtyCandidateSet.empty() && dirtyCandidateSet.count(entityId))
							continue;

						Node node = Node::deserializeFromBuffer(dataBuf + i * entitySize);

						if (!node.isActive()) // ZYX_COV_EXCL_LINE
							continue;
						if (!targetLabelIds_.empty()) {
							bool allMatch = true;
							for (int64_t tid : targetLabelIds_) {
								if (!node.hasLabelId(tid)) { allMatch = false; break; }
							}
							if (!allMatch) continue;
						}

						if (node.hasPropertyEntity() &&
							node.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) { // ZYX_COV_EXCL_LINE
							localPropIds.push_back(node.getPropertyEntityId());
						}

						localNodes.push_back(std::move(node));
					}
				}
			};
			(void) graph::concurrent::ParallelOperatorExecutor::runIndexedPartitions<EmptyParallelState>(
					groups.size(), threadPool_, readOptions, readGroup, [](size_t, EmptyParallelState &) {});

			// ── Phase 2: Bulk-load all needed Property entities (segment-sequential) ──
			std::vector<int64_t> allPropIds;
			for (const auto &pids : segPropIds) {
				allPropIds.insert(allPropIds.end(), pids.begin(), pids.end());
			}
			const auto phase2Start = profileEnabled ? Clock::now() : Clock::time_point{};
			auto propertyMap = dm_->bulkLoadPropertyEntities(allPropIds, threadPool_);
			if (profileEnabled) {
				debug::PerfTrace::addDuration(
						"scan.parallel.phase2",
						static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
														phase2Start)
												 .count()));
			}

			// ── Phase 3: Parallel property assignment + Record creation ──
			std::vector<RecordBatch> threadBatches(numSegs);

			const graph::concurrent::ParallelOperatorOptions materializeOptions{
					.phase = "scan.parallel.phase3",
					.workloadKind = graph::concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
					.estimatedItems = numSegs,
					.estimatedBytes = numSegs * storage::TOTAL_SEGMENT_SIZE,
					.minPartitions = kParallelNodeScanMinTasks,
					.minItems = kParallelNodeScanMinSegments};
			auto materializeSegment = [&](size_t si, EmptyParallelState &) {
				RecordBatch &localBatch = threadBatches[si];
				localBatch.reserve(segNodes[si].size());

				for (auto &node : segNodes[si]) {
					auto props = dm_->getNodePropertiesFromMap(node, propertyMap);
					node.setProperties(std::move(props));

					Record r;
					r.setNode(config_.variable, std::move(node));
					localBatch.push_back(std::move(r));
				}
			};
			(void) graph::concurrent::ParallelOperatorExecutor::runIndexedPartitions<EmptyParallelState>(
					numSegs, threadPool_, materializeOptions, materializeSegment, [](size_t, EmptyParallelState &) {});

			// Merge thread-local batches
			size_t totalSize = 0;
			for (const auto &tb : threadBatches)
				totalSize += tb.size();

			RecordBatch batch;
			batch.reserve(totalSize);
			for (auto &tb : threadBatches) {
				for (auto &r : tb)
					batch.push_back(std::move(r));
			}

			if (!dirtyCandidateIds.empty()) {
				for (const int64_t id: dirtyCandidateIds) {
					Node node = dm_->getNode(id);
					if (node.getId() == 0 || !node.isActive()) {
						continue;
					}
					if (!targetLabelIds_.empty()) {
						bool allMatch = true;
						for (int64_t tid: targetLabelIds_) {
							if (!node.hasLabelId(tid)) {
								allMatch = false;
								break;
							}
						}
						if (!allMatch) {
							continue;
						}
					}

					auto props = dm_->getNodeProperties(id);
					node.setProperties(std::move(props));

					Record r;
					r.setNode(config_.variable, std::move(node));
					batch.push_back(std::move(r));
				}
				std::ranges::sort(batch, [this](const Record &lhs, const Record &rhs) {
					const auto leftNode = lhs.getNodeRef(config_.variable);
					const auto rightNode = rhs.getNodeRef(config_.variable);
					if (!leftNode.has_value() || !rightNode.has_value()) {
						return false;
					}
					return leftNode->get().getId() < rightNode->get().getId();
				});
			}

			if (batch.empty() && currentIdx_ >= candidateIds_.size()) // ZYX_COV_EXCL_LINE
				return std::nullopt;
			return batch;
		}
	};
}
