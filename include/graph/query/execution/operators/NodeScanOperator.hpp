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

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "../PhysicalOperator.hpp"
#include "../ScanConfigs.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

	class NodeScanOperator : public PhysicalOperator {
	public:
		NodeScanOperator(std::shared_ptr<storage::DataManager> dm, std::shared_ptr<indexes::IndexManager> im,
						 NodeScanConfig config) : dm_(std::move(dm)), im_(std::move(im)), config_(std::move(config)) {}

		void open() override {
			currentIdx_ = 0;
			candidateIds_.clear();

			// 1. Determine Candidate IDs
			switch (config_.type) {
				case ScanType::PROPERTY_SCAN:
					// Implicit Filtering: The Index returns ONLY IDs that match key=value.
					candidateIds_ = im_->findNodeIdsByProperty(config_.indexKey, config_.indexValue);
					break;

				case ScanType::LABEL_SCAN:
					// Implicit Filtering: The Index returns IDs with this label.
					candidateIds_ = im_->findNodeIdsByLabel(config_.label);
					break;

				case ScanType::FULL_SCAN:
				default:
					// No Filtering: Get all IDs.
					int64_t maxId = dm_->getIdAllocator()->getCurrentMaxNodeId();
					candidateIds_.reserve(maxId);
					for (int64_t i = 1; i <= maxId; ++i)
						candidateIds_.push_back(i);
					break;
			}

			// Pre-resolve label ID for the scan loop (optimization)
			targetLabelId_ = 0;
			if (!config_.label.empty()) {
				// We use resolveLabelId if available, or just getOrCreateLabelId.
				// For scanning, if the label doesn't exist in registry, ID is 0, so no node will match.
				targetLabelId_ = dm_->getOrCreateLabelId(config_.label);
			}
		}

		std::optional<RecordBatch> next() override {
			if (currentIdx_ >= candidateIds_.size())
				return std::nullopt;

			// When thread pool is available and enough candidates remain,
			// use a larger batch to amortize parallelization overhead.
			static constexpr size_t PARALLEL_SCAN_THRESHOLD = 4096;
			size_t effectiveBatchSize = DEFAULT_BATCH_SIZE;
			bool canParallelize = threadPool_ && !threadPool_->isSingleThreaded();
			size_t remaining = candidateIds_.size() - currentIdx_;
			if (canParallelize && remaining >= PARALLEL_SCAN_THRESHOLD) {
				// Use all remaining candidates as one large parallel batch
				effectiveBatchSize = remaining;
			}

			size_t batchStart = currentIdx_;
			size_t batchEnd = std::min(currentIdx_ + effectiveBatchSize, candidateIds_.size());
			size_t batchCount = batchEnd - batchStart;
			currentIdx_ = batchEnd;

			if (canParallelize && batchCount >= PARALLEL_SCAN_THRESHOLD) {
				return parallelLoadBatch(batchStart, batchEnd);
			}

			// Sequential path
			RecordBatch batch;
			batch.reserve(batchCount);

			for (size_t idx = batchStart; idx < batchEnd; ++idx) {
				int64_t id = candidateIds_[idx];
				Node node = dm_->getNode(id);

				if (!node.isActive())
					continue;

				if (!config_.label.empty()) {
					if (node.getLabelId() != targetLabelId_)
						continue;
				}

				auto props = dm_->getNodeProperties(id);
				node.setProperties(std::move(props));

				Record r;
				r.setNode(config_.variable, std::move(node));
				batch.push_back(std::move(r));
			}

			if (batch.empty() && currentIdx_ >= candidateIds_.size())
				return std::nullopt;
			return batch;
		}

		void close() override { candidateIds_.clear(); }

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {config_.variable}; }

		[[nodiscard]] std::string toString() const override { return config_.toString(); }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
		NodeScanConfig config_;

		std::vector<int64_t> candidateIds_;
		size_t currentIdx_ = 0;
		int64_t targetLabelId_ = 0; // Cached ID

		std::optional<RecordBatch> parallelLoadBatch(size_t batchStart, size_t batchEnd) {
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

			if (relevantSegIdxs.empty()) {
				if (currentIdx_ >= candidateIds_.size())
					return std::nullopt;
				return RecordBatch{};
			}

			// For non-FULL_SCAN, build a set for fast membership testing
			std::unordered_set<int64_t> candidateSet;
			if (config_.type != ScanType::FULL_SCAN) {
				candidateSet.insert(
					candidateIds_.begin() + batchStart,
					candidateIds_.begin() + batchEnd);
			}

			// Parallel phase: each thread processes a chunk of segments.
			// Per segment: one pread to load all entities, then filter + load properties.
			// This minimizes syscalls (one per segment instead of one per entity).
			size_t numSegs = relevantSegIdxs.size();
			std::vector<RecordBatch> threadBatches(numSegs);

			threadPool_->parallelFor(0, numSegs, [&](size_t si) {
				const auto &seg = segIndex[relevantSegIdxs[si]];
				auto header = dm_->getSegmentTracker()->getSegmentHeader(seg.segmentOffset);
				if (header.used == 0)
					return;

				// One pread for the entire segment data area
				constexpr size_t entitySize = Node::getTotalSize();
				size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				std::vector<char> buf(dataBytes);
				auto dataOffset = static_cast<off_t>(seg.segmentOffset + sizeof(storage::SegmentHeader));
				ssize_t n = dm_->preadBytes(buf.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes))
					return;

				RecordBatch &localBatch = threadBatches[si];

				for (uint32_t i = 0; i < header.used; ++i) {
					int64_t entityId = header.start_id + i;
					if (entityId < startId || entityId > endId)
						continue;

					// For index-based scans, check membership
					if (!candidateSet.empty() && !candidateSet.count(entityId))
						continue;

					// Deserialize from buffer (no syscall)
					// Use a stack-local membuf to avoid allocation
					char *entPtr = buf.data() + i * entitySize;
					struct membuf : std::streambuf {
						membuf(char *p, size_t s) { setg(p, p, p + s); }
					} mb(entPtr, entitySize);
					std::istream stream(&mb);
					Node node = Node::deserialize(stream);

					if (!node.isActive())
						continue;

					if (!config_.label.empty() && node.getLabelId() != targetLabelId_)
						continue;

					// Load properties (pread per property entity — still needed)
					auto props = dm_->getNodePropertiesDirect(node);
					node.setProperties(std::move(props));

					Record r;
					r.setNode(config_.variable, std::move(node));
					localBatch.push_back(std::move(r));
				}
			});

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

			if (batch.empty() && currentIdx_ >= candidateIds_.size())
				return std::nullopt;
			return batch;
		}
	};
}