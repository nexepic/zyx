/**
 * @file LeidenEngine.hpp
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

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/algorithm/CsrProjection.hpp"

namespace graph::query::algorithm {
	struct LeidenEngineTestAccess;

	struct LeidenOptions {
		int maxIterations = 20;          ///< max local-moving iterations per level
		int maxLevels = 10;              ///< max level-lifting levels (1 = single level)
		double resolution = 1.0;
		/// Refinement threshold (theta). Positive values require a disconnected
		/// component split to clear this modularity-gain gate; 0 disables refinement.
		double refinementThreshold = 0.01;
	};

	struct NodeCommunity {
		int64_t nodeId;
		int64_t communityId;
	};

	/**
	 * @brief Leiden community detection over a CsrProjection.
	 *
	 * Implements the Leiden method (Traag, Waltman, van Eck 2019), which extends
	 * Louvain with a refinement phase that guarantees the communities found at
	 * each level are internally connected (no "broken" communities split by other
	 * communities). The per-level pipeline is:
	 *
	 *   local moving  →  refinement  →  aggregation (level lifting)
	 *
	 * Local moving optimises modularity greedily; refinement splits each
	 * community into its connected components under a permissive (negative)
	 * gain threshold so that only genuinely connected nodes stay together;
	 * aggregation collapses communities into super-nodes and the next level
	 * repeats on the shrunken graph. Community ids from each level are chained
	 * back to the original node ids.
	 *
	 * Design notes:
	 *  - Local moving uses atomic community labels internally so parallel workers
	 *    can read fresh neighbor assignments without data races.
	 *  - `sigmaTotal` (per-community degree sum) is the other contention point
	 *    in the parallel path and uses atomic updates there.
	 *  - Super-node graphs rebuild a fresh CsrProjection per level via
	 *    CsrProjection::buildFromEdgeList — no storage I/O.
	 *  - Refinement finds connected components inside each community via a
	 *    single-edge-band Union-Find pass, then re-labels so each (community,
	 *    component) pair becomes a distinct community id.
	 *
	 * Invariant: every community in the final assignment is internally connected
	 * on the level-0 graph (Leiden connectivity guarantee).
	 */
	class LeidenEngine {
	public:
		/// Run Leiden. Single-threaded when pool is null.
		static std::vector<NodeCommunity> run(const CsrProjection &csr,
											  const LeidenOptions &opts = {},
											  concurrent::ThreadPool *pool = nullptr);

		/// Compute modularity of the current community assignment (Q in [-0.5, 1]).
		static double modularity(const CsrProjection &csr,
								 const std::vector<int64_t> &communityOf,
								 double resolution = 1.0);

	private:
		friend struct LeidenEngineTestAccess;

		LeidenEngine() = delete;

		/// One level of local moving. Mutates communityOf/sigmaTot in place.
		/// Returns the number of communities after convergence (relabeled dense).
		static size_t localMoveOnce(const CsrProjection &csr,
									const LeidenOptions &opts,
									std::vector<double> &ki,
									double m2,
									std::vector<int64_t> &communityOf,
									std::vector<std::atomic<double>> &sigmaTot,
									concurrent::ThreadPool *pool);

		/// Leiden refinement: split communities that are internally disconnected
		/// into their connected components. Under theta, a community is reduced to
		/// its core so each resulting community is connected. Re-labels communityOf
		/// to dense 0..C'-1 and returns the new community count plus the rebuilt
		/// per-community degree sums (plain doubles, sized to the new label count).
		static std::pair<size_t, std::vector<double>>
		refineCommunities(const CsrProjection &csr,
						  const LeidenOptions &opts,
						  double m2,
						  const std::vector<double> &ki,
						  std::vector<int64_t> &communityOf,
						  const std::vector<std::atomic<double>> &sigmaTot);

		/// Collapse one level's community assignment into a super-node CSR.
		static std::shared_ptr<CsrProjection>
		buildSuperGraph(const CsrProjection &csr,
						const std::vector<int64_t> &communityOf,
						size_t communityCount,
						concurrent::ThreadPool *pool);
	};

} // namespace graph::query::algorithm
