/**
 * @file CsrProjection.hpp
 * @author Nexepic
 * @date 2026/7/3
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

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "graph/concurrent/ParallelOperatorExecutor.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/algorithm/GraphProjection.hpp"

namespace graph::query::algorithm {

	/**
	 * @brief Compact Compressed Sparse Row (CSR) projection of a GraphProjection.
	 *
	 * Replaces the unordered_map<int64_t, vector<ProjectedEdge>> layout (~40-80
	 * bytes/edge) with three contiguous arrays (~16 bytes/edge), cutting memory
	 * by roughly 3-5x and making neighbor iteration cache-friendly.
	 *
	 * Build is parallel: nodes are sharded across worker threads, each collecting
	 * a thread-local edge list, then a single compactification pass assembles the
	 * CSR. Memory peak during build is the per-shard edge lists, not the whole
	 * graph migrated at once.
	 *
	 * Cached in GraphProjectionManager so repeated Leiden runs reuse the CSR
	 * without re-scanning storage.
	 */
	class CsrProjection {
	public:
		/// A weighted undirected edge between two CSR-indexed nodes.
		struct Edge {
			int64_t src;
			int64_t dst;
			float weight;
		};

		static std::shared_ptr<CsrProjection> build(const GraphProjection &proj,
													 concurrent::ThreadPool *pool = nullptr) {
			// new + shared_ptr (not make_shared) so the private default ctor is
			// invoked directly; libc++'s allocate_shared construct_at path does
			// not see past private access.
			auto csr = std::shared_ptr<CsrProjection>(new CsrProjection());
			csr->buildInternal(proj, pool);
			return csr;
		}

		/// Build a CSR from an in-memory undirected edge list over node indices
		/// 0..nodeCount-1. Used by Louvain Level Lifting to rebuild a smaller
		/// "super-node" graph after each local-moving convergence. Each edge in
		/// the list is treated as already undirected (callers pass one entry per
		//  undirected edge); this method mirrors it to both endpoints.
		static std::shared_ptr<CsrProjection> buildFromEdgeList(size_t nodeCount,
																const std::vector<Edge> &edges) {
			auto csr = std::shared_ptr<CsrProjection>(new CsrProjection());
			csr->buildFromEdges(nodeCount, edges);
			return csr;
		}

		[[nodiscard]] size_t nodeCount() const noexcept { return nodeIds_.size(); }
		[[nodiscard]] size_t edgeCount() const noexcept { return colIdx_.size(); }
		[[nodiscard]] bool isWeighted() const noexcept { return isWeighted_; }

		/// Original nodeId at CSR index.
		[[nodiscard]] int64_t nodeIdAt(size_t idx) const { return nodeIds_[idx]; }

		/// CSR index for an original nodeId, or SIZE_MAX if absent.
		[[nodiscard]] size_t indexOf(int64_t nodeId) const {
			auto it = idToIdx_.find(nodeId);
			return it != idToIdx_.end() ? it->second : SIZE_MAX;
		}

		/// Range of neighbor nodeIds for the node at CSR index.
		[[nodiscard]] std::span<const int64_t> neighbors(size_t nodeIdx) const {
			return {colIdx_.data() + rowPtr_[nodeIdx], rowPtr_[nodeIdx + 1] - rowPtr_[nodeIdx]};
		}

		/// Range of edge weights, parallel to neighbors().
		[[nodiscard]] std::span<const float> neighborWeights(size_t nodeIdx) const {
			return {weights_.data() + rowPtr_[nodeIdx], rowPtr_[nodeIdx + 1] - rowPtr_[nodeIdx]};
		}

	private:
		CsrProjection() = default;

		void buildInternal(const GraphProjection &proj, concurrent::ThreadPool *pool) {
			isWeighted_ = proj.isWeighted();
			nodeIds_.assign(proj.getNodeIds().begin(), proj.getNodeIds().end());
			std::sort(nodeIds_.begin(), nodeIds_.end());
			const size_t n = nodeIds_.size();
			idToIdx_.reserve(n * 2);
			for (size_t i = 0; i < n; ++i) idToIdx_.emplace(nodeIds_[i], i);

			// Undirected degree: each out-edge (src,dst) contributes one slot to
			// src's row and one to dst's row. Reciprocal edges (a->b and b->a)
			// yield weight 2 between a,b, which is the correct undirected sum.
			std::vector<uint32_t> degrees(n, 0);
			for (size_t i = 0; i < n; ++i) {
				const auto &edges = proj.getOutNeighbors(nodeIds_[i]);
				degrees[i] += static_cast<uint32_t>(edges.size());
				for (const auto &e : edges) {
					auto it = idToIdx_.find(e.targetId);
					if (it != idToIdx_.end()) ++degrees[it->second];
				}
			}
			rowPtr_.resize(n + 1, 0);
			for (size_t i = 0; i < n; ++i) rowPtr_[i + 1] = rowPtr_[i] + degrees[i];

			colIdx_.resize(rowPtr_[n]);
			weights_.resize(rowPtr_[n], 1.0f);

			// Fill phase via the shared parallel executor: adaptive worker selection
			// (small graphs fall back to serial, large ones parallelise) with
			// profile telemetry under phase "leiden.csrBuild". Each partition
			// fills a disjoint node range; writes go through per-row atomic
			// cursors so mirror writes into a dst row never collide.
			std::vector<std::atomic<uint32_t>> cursors(rowPtr_.size());
			for (size_t i = 0; i < rowPtr_.size(); ++i) cursors[i].store(rowPtr_[i], std::memory_order_relaxed);
			struct CsrFillState {}; // no per-partition state; atomic cursors handle writes
			const concurrent::ParallelOperatorOptions options{
				.phase = std::string_view("leiden.csrBuild"),
				.workloadKind = concurrent::ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL,
				.estimatedItems = n,
				.minPartitions = 2,
				.minItems = 1024,
				.minItemsPerWorker = 256,
			};
			concurrent::ParallelOperatorExecutor::runRangePartitions<CsrFillState>(
				0, n, pool, options,
				[&](const concurrent::ParallelRangePartition &range, CsrFillState &) {
					for (size_t i = range.begin; i < range.end; ++i) {
						int64_t srcId = nodeIds_[i];
						const auto &edges = proj.getOutNeighbors(srcId);
						for (const auto &e : edges) {
							auto dstIt = idToIdx_.find(e.targetId);
							if (dstIt == idToIdx_.end()) continue;
							size_t dst = dstIt->second;
							float w = static_cast<float>(e.weight);
							uint32_t posSrc = cursors[i].fetch_add(1, std::memory_order_relaxed);
							colIdx_[posSrc] = e.targetId;
							weights_[posSrc] = w;
							uint32_t posDst = cursors[dst].fetch_add(1, std::memory_order_relaxed);
							colIdx_[posDst] = srcId;
							weights_[posDst] = w;
						}
					}
				},
				[]([[maybe_unused]] size_t, CsrFillState &) {});
		}

		// Build from an in-memory undirected edge list over indices [0, nodeCount).
		// No filtering needed (callers pre-filter), no storage I/O. Each supplied
		// edge is mirrored to both endpoints so the CSR is undirected.
		void buildFromEdges(size_t nodeCount, const std::vector<Edge> &edges) {
			const size_t n = nodeCount;
			nodeIds_.resize(n);
			isWeighted_ = !edges.empty();
			for (size_t i = 0; i < n; ++i) {
				nodeIds_[i] = static_cast<int64_t>(i);
				idToIdx_.emplace(static_cast<int64_t>(i), i);
			}

			std::vector<uint32_t> degrees(n, 0);
			for (const auto &e : edges) {
				if (e.src == e.dst) continue; // self-loop: contributes to its own row only
				degrees[static_cast<size_t>(e.src)] += 1;
				degrees[static_cast<size_t>(e.dst)] += 1;
			}
			rowPtr_.resize(n + 1, 0);
			for (size_t i = 0; i < n; ++i) rowPtr_[i + 1] = rowPtr_[i] + degrees[i];

			colIdx_.resize(rowPtr_[n]);
			weights_.resize(rowPtr_[n], 1.0f);
			std::vector<std::atomic<uint32_t>> cursors(rowPtr_.size());
			for (size_t i = 0; i < rowPtr_.size(); ++i)
				cursors[i].store(rowPtr_[i], std::memory_order_relaxed);

			for (const auto &e : edges) {
				if (e.src == e.dst) continue;
				size_t s = static_cast<size_t>(e.src);
				size_t d = static_cast<size_t>(e.dst);
				uint32_t ps = cursors[s].fetch_add(1, std::memory_order_relaxed);
				colIdx_[ps] = e.dst;
				weights_[ps] = e.weight;
				uint32_t pd = cursors[d].fetch_add(1, std::memory_order_relaxed);
				colIdx_[pd] = e.src;
				weights_[pd] = e.weight;
			}
		}

		std::vector<int64_t> nodeIds_;
		std::vector<uint32_t> rowPtr_; // size n+1
		std::vector<int64_t> colIdx_;  // stores target nodeId
		std::vector<float> weights_;
		std::unordered_map<int64_t, size_t> idToIdx_;
		bool isWeighted_ = false;
	};

} // namespace graph::query::algorithm