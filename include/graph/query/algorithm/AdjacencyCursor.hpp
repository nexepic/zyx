/**
 * @file AdjacencyCursor.hpp
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
#include <cstddef>
#include <vector>

#include "graph/query/algorithm/GraphProjection.hpp"

namespace graph::query::algorithm {

	/**
	 * @brief Sequential cursor over a GraphProjection's out-edges.
	 *
	 * Iterates nodes in ascending nodeId order (spatial locality: adjacent
	 * nodeIds tend to live in adjacent segments, so the underlying PageBufferPool
	 * stays warm). Edges are yielded one at a time as (src, dst, weight) without
	 * materializing the entire graph — memory footprint is O(current adjacency).
	 *
	 * Used as the streaming entry point for building CSR projections and (later)
	 * for true streaming algorithms on graphs too large to fit in memory.
	 */
	class AdjacencyCursor {
	public:
		explicit AdjacencyCursor(const GraphProjection &proj)
			: proj_(proj), orderedNodeIds_(proj.getNodeIds().begin(), proj.getNodeIds().end()) {
			std::sort(orderedNodeIds_.begin(), orderedNodeIds_.end());
		}

		/// Yield the next out-edge. Returns false when exhausted.
		bool next(int64_t &src, int64_t &dst, double &weight) {
			while (nodeCursor_ < orderedNodeIds_.size()) {
				int64_t curNode = orderedNodeIds_[nodeCursor_];
				const auto &edges = proj_.getOutNeighbors(curNode);
				if (edgeCursor_ < edges.size()) {
					src = curNode;
					dst = edges[edgeCursor_].targetId;
					weight = edges[edgeCursor_].weight;
					++edgeCursor_;
					return true;
				}
				++nodeCursor_;
				edgeCursor_ = 0;
			}
			return false;
		}

		/// Total number of nodes the cursor will visit.
		[[nodiscard]] size_t nodeCount() const noexcept { return orderedNodeIds_.size(); }

	private:
		const GraphProjection &proj_;
		std::vector<int64_t> orderedNodeIds_;
		size_t nodeCursor_ = 0;
		size_t edgeCursor_ = 0;
	};

} // namespace graph::query::algorithm