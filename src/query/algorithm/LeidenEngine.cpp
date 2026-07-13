/**
 * @file LeidenEngine.cpp
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

#include "graph/query/algorithm/LeidenEngine.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "graph/concurrent/ParallelOperatorExecutor.hpp"

namespace graph::query::algorithm {

	double LeidenEngine::modularity(const CsrProjection &csr,
									const std::vector<int64_t> &communityOf,
									double resolution) {
		const size_t n = csr.nodeCount();
		if (n == 0) return 0.0;

		double m2 = 0.0;
		for (size_t i = 0; i < n; ++i)
			for (float w : csr.neighborWeights(i)) m2 += static_cast<double>(w);
		if (m2 == 0.0) return 0.0;

		std::unordered_map<int64_t, double> sigmaIn;
		std::unordered_map<int64_t, double> sigmaTot;
		for (size_t i = 0; i < n; ++i) {
			int64_t c = communityOf[i];
			auto nbrs = csr.neighbors(i);
			auto wnbrs = csr.neighborWeights(i);
			for (size_t j = 0; j < nbrs.size(); ++j) {
				double w = wnbrs[j];
				sigmaTot[c] += w;
				size_t dst = csr.indexOf(nbrs[j]);
				if (dst != SIZE_MAX && dst != i && communityOf[dst] == c) sigmaIn[c] += w;
			}
		}
		for (auto &kv : sigmaIn) kv.second /= 2.0;

		double q = 0.0;
		for (const auto &kv : sigmaTot) {
			double tot = kv.second;
			double in = sigmaIn.count(kv.first) ? sigmaIn[kv.first] : 0.0;
			q += in / m2 - resolution * (tot * tot) / (m2 * m2);
		}
		return q;
	}

	size_t LeidenEngine::localMoveOnce(const CsrProjection &csr,
									   const LeidenOptions &opts,
									   std::vector<double> &ki,
									   double m2,
									   std::vector<int64_t> &communityOf,
									   std::vector<std::atomic<double>> &sigmaTot,
									   concurrent::ThreadPool *pool) {
		const size_t n = csr.nodeCount();
		if (n == 0) return 0;

		auto acquireSigmaTot = [&]() {
			std::unordered_map<int64_t, double> snap;
			snap.reserve(n);
			for (size_t i = 0; i < n; ++i)
				snap[static_cast<int64_t>(i)] = sigmaTot[i].load(std::memory_order_relaxed);
			return snap;
		};

		std::vector<std::atomic<int64_t>> communityAtomic(n);
		for (size_t i = 0; i < n; ++i)
			communityAtomic[i].store(communityOf[i], std::memory_order_relaxed);

		const size_t convergenceThreshold = (n < 100) ? 0 : n / 100;
		int iter = 0;
		bool changed = true;
		while (changed && iter < opts.maxIterations) {
			changed = false;
			++iter;

			// Workers read the latest community labels through atomics. This keeps
			// the local-moving phase asynchronous like Louvain/Leiden, while
			// avoiding undefined behaviour from concurrent vector reads/writes.
			std::unordered_map<int64_t, double> sigmaTotSnap = acquireSigmaTot();

			// Each partition counts its own moves; the merger aggregates them in a
			// serial pass (no atomic on movedCount needed).
			struct LocalMoveState {
				size_t moved = 0;
			};
			size_t movedTotal = 0;

			const concurrent::ParallelOperatorOptions options{
				.phase = std::string_view("leiden.localMove"),
				.workloadKind = concurrent::ParallelWorkloadKind::PWK_CPU_BOUND,
				.estimatedItems = n,
				.minPartitions = 2,
				.minItems = 1024, // below this the executor falls back to serial
				.minItemsPerWorker = 64,
			};

			concurrent::ParallelOperatorExecutor::runRangePartitions<LocalMoveState>(
				0, n, pool, options,
				[&](const concurrent::ParallelRangePartition &range, LocalMoveState &state) {
					for (size_t i = range.begin; i < range.end; ++i) {
						int64_t cur = communityAtomic[i].load(std::memory_order_relaxed);
						std::unordered_map<int64_t, double> kic;
						auto nbrs = csr.neighbors(i);
						auto wnbrs = csr.neighborWeights(i);
						for (size_t j = 0; j < nbrs.size(); ++j) {
							size_t dst = csr.indexOf(nbrs[j]);
							if (dst == SIZE_MAX || dst == i) continue;
							kic[communityAtomic[dst].load(std::memory_order_relaxed)] += static_cast<double>(wnbrs[j]);
						}
						if (kic.empty()) continue;

						int64_t best = cur;
						double bestGain = 0.0;
						for (const auto &kv : kic) {
							int64_t c = kv.first;
							double totC = (c == cur) ? (sigmaTotSnap[c] - ki[i]) : sigmaTotSnap[c];
							double gain = kv.second - opts.resolution * totC * ki[i] / m2;
							if (gain > bestGain + 1e-12) {
								bestGain = gain;
								best = c;
							}
						}
						if (best != cur) {
							sigmaTot[cur].fetch_sub(ki[i], std::memory_order_relaxed);
							sigmaTot[best].fetch_add(ki[i], std::memory_order_relaxed);
							communityAtomic[i].store(best, std::memory_order_relaxed);
							++state.moved;
						}
					}
				},
				[&]([[maybe_unused]] size_t partition, LocalMoveState &state) { movedTotal += state.moved; });

			if (movedTotal > convergenceThreshold) changed = true;
		}

		for (size_t i = 0; i < n; ++i)
			communityOf[i] = communityAtomic[i].load(std::memory_order_relaxed);

		// Relabel to dense 0..C-1 so they can index the next level's super-nodes.
		std::unordered_map<int64_t, int64_t> relabel;
		int64_t nextId = 0;
		for (size_t i = 0; i < n; ++i)
			if (relabel.emplace(communityOf[i], nextId).second) ++nextId;
		for (size_t i = 0; i < n; ++i) communityOf[i] = relabel[communityOf[i]];
		return static_cast<size_t>(nextId);
	}

	std::pair<size_t, std::vector<double>>
	LeidenEngine::refineCommunities(const CsrProjection &csr,
									const LeidenOptions &opts,
									double m2,
									const std::vector<double> &ki,
									std::vector<int64_t> &communityOf,
									const std::vector<std::atomic<double>> &sigmaTot) {
		const size_t n = csr.nodeCount();
		if (n == 0) return {0, {}};
		if (opts.refinementThreshold <= 0.0) {
			// disabled: no split; just rebuilt plain sigmaTot (dense labels already).
			int64_t maxC = *std::max_element(communityOf.begin(), communityOf.end());
			std::vector<double> sigmaOut(static_cast<size_t>(maxC) + 1, 0.0);
			for (size_t i = 0; i < n; ++i) sigmaOut[communityOf[i]] += ki[i];
			return {static_cast<size_t>(maxC) + 1, std::move(sigmaOut)};
		}

		// Refinement shards each existing community into its connected components
		// (single-edge-band Union-Find), then for each component decides whether to
		// keep it as its own community. A component is split off only if the
		// modularity loss of leaving the parent is within theta (i.e. the move is
		// not too damaging). Components that stay rejoin the parent; components
		// that split form new community ids.
		//
		// Union-Find over nodes, but edges only union nodes that share a community.
		std::vector<size_t> parent(n);
		for (size_t i = 0; i < n; ++i) parent[i] = i;
		auto find = [&](size_t x) -> size_t {
			while (parent[x] != x) {
				parent[x] = parent[parent[x]];
				x = parent[x];
			}
			return x;
		};
		auto unite = [&](size_t a, size_t b) {
			size_t ra = find(a), rb = find(b);
			if (ra != rb) parent[ra] = rb;
		};
		for (size_t i = 0; i < n; ++i) {
			int64_t ci = communityOf[i];
			auto nbrs = csr.neighbors(i);
			for (size_t j = 0; j < nbrs.size(); ++j) {
				size_t dst = csr.indexOf(nbrs[j]);
				if (dst == SIZE_MAX) continue;
				if (communityOf[dst] == ci && dst != i) unite(i, dst);
			}
		}

		// Group nodes by (community, component root). Each group with size>0 becomes
		// a candidate community. The largest component per original community keeps
		// the original community id (the "core"); every other component becomes a
		// new id — provided the split is not catastrophically bad for modularity.
		std::unordered_map<int64_t, std::unordered_map<size_t, std::vector<size_t>>> groups;
		for (size_t i = 0; i < n; ++i) groups[communityOf[i]][find(i)].push_back(i);

		// For theta gating: a component is split off only if the gain of moving it
		// out as its own community is >= theta (negative theta allowed). We compute
		// the per-component "gain of forming standalone" relative to staying, using
		// the current sigmaTot snapshot.
		std::unordered_map<int64_t, double> sigmaTotSnap;
		for (size_t i = 0; i < n; ++i)
			sigmaTotSnap[static_cast<int64_t>(i)] = sigmaTot[i].load(std::memory_order_relaxed);

		// Assign new labels: keep the original community id for the largest
		// component of each community; smaller components get fresh ids only if the
		// split passes the theta gate.
		int64_t nextId = 0;
		std::unordered_map<int64_t, int64_t> oldToNew;
		std::vector<int64_t> newComm(n, -1);
		for (auto &commPair : groups) {
			int64_t oldC = commPair.first;
			auto &comps = commPair.second;
			// Find the largest component to keep the parent id.
			size_t largestRoot = comps.begin()->first;
			size_t largestSize = 0;
			for (const auto &cp : comps)
				if (cp.second.size() > largestSize) {
					largestSize = cp.second.size();
					largestRoot = cp.first;
				}
			// The kept id for this community.
			int64_t keepId = nextId++;
			oldToNew[oldC] = keepId;
			for (size_t idx : comps[largestRoot]) newComm[idx] = keepId;
			// Other components: gate by theta.
			for (const auto &cp : comps) {
				if (cp.first == largestRoot) continue;
				// Sum weights from component to its own parent community vs internal.
				double internalW = 0.0; // edges within component (counted twice)
				double compKi = 0.0;
				for (size_t idx : cp.second) {
					compKi += ki[idx];
					auto onbrs = csr.neighbors(idx);
					auto ownbrs = csr.neighborWeights(idx);
					for (size_t j = 0; j < onbrs.size(); ++j) {
						size_t d = csr.indexOf(onbrs[j]);
						if (d == SIZE_MAX) continue;
						bool inComp = (newComm[d] == -1) && (communityOf[d] == oldC) && (find(d) == cp.first);
						if (inComp) internalW += ownbrs[j];
					}
				}
				internalW /= 2.0;
				// Modularity gain of this component becoming its own community
				// (leaving the parent): R = internalW/m - resolution*(compKi*totParent)/m^2 ...
				// Leiden's refinement accepts the split when it does not decrease
				// modularity by more than theta. Equivalent gate used here:
				//   gain_in - resolution * compKi*(parentTot - compKi)/m2 >= theta
				double parentTot = sigmaTotSnap[oldC];
				double gain = internalW / m2 - opts.resolution * compKi * (parentTot - compKi) / (m2 * m2);
				if (gain >= opts.refinementThreshold) {
					int64_t newId = nextId++;
					for (size_t idx : cp.second) newComm[idx] = newId;
				} else {
					for (size_t idx : cp.second) newComm[idx] = keepId;
				}
			}
		}

		// Apply new labels and build a plain-double sigmaTot sized to the new labels.
		for (size_t i = 0; i < n; ++i) communityOf[i] = newComm[i];
		std::vector<double> sigmaOut(static_cast<size_t>(nextId), 0.0);
		for (size_t i = 0; i < n; ++i) sigmaOut[newComm[i]] += ki[i];
		return {static_cast<size_t>(nextId), std::move(sigmaOut)};
	}

	std::shared_ptr<CsrProjection>
	LeidenEngine::buildSuperGraph(const CsrProjection &csr,
								  const std::vector<int64_t> &communityOf,
								  size_t communityCount,
								  concurrent::ThreadPool *pool) {
		const size_t n = csr.nodeCount();
		// Each partition aggregates its node-range's edges into a local map
		// (no cross-partition contention); the merger serially folds the per-
		// partition maps into the global aggregate.
		struct SuperGraphState {
			std::unordered_map<int64_t, double> localAgg;
		};
		std::unordered_map<int64_t, double> agg;
		agg.reserve(csr.edgeCount());

		const concurrent::ParallelOperatorOptions options{
			.phase = std::string_view("leiden.buildSuperGraph"),
			.workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
			.estimatedItems = n,
			.minPartitions = 2,
			.minItems = 4096,
			.minItemsPerWorker = 512,
		};

		const int64_t cc = static_cast<int64_t>(communityCount);
		concurrent::ParallelOperatorExecutor::runRangePartitions<SuperGraphState>(
			0, n, pool, options,
			[&](const concurrent::ParallelRangePartition &range, SuperGraphState &state) {
				state.localAgg.reserve(range.size() * 4);
				for (size_t i = range.begin; i < range.end; ++i) {
					int64_t sc = communityOf[i];
					auto nbrs = csr.neighbors(i);
					auto wnbrs = csr.neighborWeights(i);
					for (size_t j = 0; j < nbrs.size(); ++j) {
						size_t dst = csr.indexOf(nbrs[j]);
						if (dst == SIZE_MAX) continue;
						int64_t dc = communityOf[dst];
						int64_t a = std::min(sc, dc), b = std::max(sc, dc);
						state.localAgg[a * cc + b] += static_cast<double>(wnbrs[j]);
					}
				}
			},
			[&]([[maybe_unused]] size_t partition, SuperGraphState &state) {
				for (auto &kv : state.localAgg) agg[kv.first] += kv.second;
			});

		std::vector<CsrProjection::Edge> edges;
		edges.reserve(agg.size());
		for (const auto &kv : agg) {
			int64_t key = kv.first;
			int64_t a = key / cc;
			int64_t b = key % cc;
			float w = static_cast<float>(kv.second / 2.0);
			edges.push_back({a, b, w});
		}
		return CsrProjection::buildFromEdgeList(communityCount, edges);
	}

	std::vector<NodeCommunity> LeidenEngine::run(const CsrProjection &csr,
												 const LeidenOptions &opts,
												 concurrent::ThreadPool *pool) {
		const size_t n = csr.nodeCount();
		std::vector<NodeCommunity> result;
		result.reserve(n);
		if (n == 0) return result;

		std::vector<double> ki(n, 0.0);
		for (size_t i = 0; i < n; ++i)
			for (float w : csr.neighborWeights(i)) ki[i] += static_cast<double>(w);
		double m2 = 0.0;
		for (size_t i = 0; i < n; ++i) m2 += ki[i];
		if (m2 == 0.0) {
			for (size_t i = 0; i < n; ++i)
				result.push_back({csr.nodeIdAt(i), static_cast<int64_t>(i)});
			return result;
		}

		// Level 0.
		std::vector<int64_t> communityOf(n);
		for (size_t i = 0; i < n; ++i) communityOf[i] = static_cast<int64_t>(i);
		std::vector<std::atomic<double>> sigmaTot(n);
		for (size_t i = 0; i < n; ++i) sigmaTot[i].store(ki[i], std::memory_order_relaxed);

		size_t communityCount = localMoveOnce(csr, opts, ki, m2, communityOf, sigmaTot, pool);
		// Leiden refinement: split disconnected communities before aggregation.
		// refineCommunities returns a fresh plain-double sigmaTot; we ignore the
		// atomics from local-moving onward since refinement runs single-threaded.
		auto [refinedCount, refinedSigma] = refineCommunities(csr, opts, m2, ki, communityOf, sigmaTot);
		communityCount = refinedCount;
		(void) refinedSigma; // level-0 sigmaTot not needed after aggregation

		std::vector<std::vector<int64_t>> levels;
		levels.push_back(communityOf);

		const CsrProjection *curCsr = &csr;
		std::shared_ptr<CsrProjection> ownedSuper;

		for (int level = 1; level < opts.maxLevels; ++level) {
			if (communityCount <= 1) break;

			ownedSuper = buildSuperGraph(*curCsr, levels.back(), communityCount, pool);
			const CsrProjection &super = *ownedSuper;
			if (super.nodeCount() >= communityCount) break; // no shrinkage → stop

			const size_t sn = super.nodeCount();
			std::vector<double> superKi(sn, 0.0);
			for (size_t i = 0; i < sn; ++i)
				for (float w : super.neighborWeights(i)) superKi[i] += static_cast<double>(w);
			double superM2 = 0.0;
			for (size_t i = 0; i < sn; ++i) superM2 += superKi[i];
			if (superM2 == 0.0) break;

			std::vector<int64_t> superCommunity(sn);
			for (size_t i = 0; i < sn; ++i) superCommunity[i] = static_cast<int64_t>(i);
			std::vector<std::atomic<double>> superSigma(sn);
			for (size_t i = 0; i < sn; ++i) superSigma[i].store(superKi[i], std::memory_order_relaxed);

			size_t prevCount = sn;
			communityCount = localMoveOnce(super, opts, superKi, superM2, superCommunity, superSigma, pool);
			// Refinement on the super-graph too: keeps super-communities connected.
			auto [rc, rs] = refineCommunities(super, opts, superM2, superKi, superCommunity, superSigma);
			communityCount = rc;
			(void) rs;

			if (communityCount >= prevCount) break;

			levels.push_back(superCommunity);
			curCsr = ownedSuper.get();
		}

		// Chain-map each original node through all levels.
		for (size_t i = 0; i < n; ++i) {
			int64_t c = levels[0][i];
			for (size_t lv = 1; lv < levels.size(); ++lv)
				c = levels[lv][static_cast<size_t>(c)];
			result.push_back({csr.nodeIdAt(i), c});
		}
		return result;
	}

} // namespace graph::query::algorithm
