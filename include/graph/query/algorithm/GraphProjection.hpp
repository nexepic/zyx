/**
 * @file GraphProjection.hpp
 * @author Nexepic
 * @date 2026/4/9
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

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace graph::storage {
	class DataManager;
}

namespace graph::query::algorithm {

	struct ProjectedEdge {
		int64_t targetId;
		double weight;
	};

	/**
	 * @brief In-memory graph projection for efficient algorithm execution.
	 *
	 * Builds a lightweight adjacency list from a filtered subset of nodes/edges.
	 * Algorithms operate on this projection instead of hitting DataManager directly,
	 * providing both safety (bounded subgraph) and performance (zero I/O during computation).
	 */
	class GraphProjection {
	public:
		/**
		 * @brief Build a projection from DataManager with optional label and weight filters.
		 *
		 * @param dm The DataManager to read from
		 * @param nodeLabel Only include nodes with this label (empty = all nodes)
		 * @param edgeType Only include edges with this type (empty = all edges)
		 * @param weightProperty Edge property to use as weight (empty = unweighted, all edges get weight 1.0)
		 */
		static GraphProjection build(const std::shared_ptr<storage::DataManager> &dm,
									 const std::string &nodeLabel = "",
									 const std::string &edgeType = "",
									 const std::string &weightProperty = "");

		[[nodiscard]] const std::unordered_set<int64_t> &getNodeIds() const { return nodeIds_; }

		[[nodiscard]] const std::vector<ProjectedEdge> &getOutNeighbors(int64_t nodeId) const;
		[[nodiscard]] const std::vector<ProjectedEdge> &getInNeighbors(int64_t nodeId) const;

		[[nodiscard]] size_t nodeCount() const { return nodeIds_.size(); }
		[[nodiscard]] size_t edgeCount() const { return edgeCount_; }
		[[nodiscard]] bool isWeighted() const { return isWeighted_; }

	private:
		std::unordered_set<int64_t> nodeIds_;
		std::unordered_map<int64_t, std::vector<ProjectedEdge>> outAdj_;
		std::unordered_map<int64_t, std::vector<ProjectedEdge>> inAdj_;
		size_t edgeCount_ = 0;
		bool isWeighted_ = false;

		static const std::vector<ProjectedEdge> EMPTY_EDGES;
	};

} // namespace graph::query::algorithm
