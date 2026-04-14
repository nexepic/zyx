/**
 * @file GraphProjection.cpp
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

#include "graph/query/algorithm/GraphProjection.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::algorithm {

	const std::vector<ProjectedEdge> GraphProjection::EMPTY_EDGES;

	GraphProjection GraphProjection::build(const std::shared_ptr<storage::DataManager> &dm,
										   const std::string &nodeLabel,
										   const std::string &edgeType,
										   const std::string &weightProperty) {
		GraphProjection proj;
		proj.isWeighted_ = !weightProperty.empty();

		int64_t maxNodeId = dm->getIdAllocator()->getCurrentMaxNodeId();

		// Resolve label IDs once
		int64_t nodeLabelId = 0;
		if (!nodeLabel.empty()) {
			nodeLabelId = dm->resolveTokenId(nodeLabel);
			if (nodeLabelId == 0) nodeLabelId = -1; // non-existent label — match nothing
		}
		int64_t edgeTypeId = 0;
		if (!edgeType.empty()) {
			edgeTypeId = dm->resolveTokenId(edgeType);
			if (edgeTypeId == 0) edgeTypeId = -1; // non-existent type — match nothing
		}

		// Phase 1: Collect active nodes matching label filter
		for (int64_t id = 1; id <= maxNodeId; ++id) {
			auto node = dm->getNode(id);
			if (node.getId() == 0 || !node.isActive()) {
				continue;
			}
			if (nodeLabelId != 0 && node.getLabelId() != nodeLabelId) {
				continue;
			}
			proj.nodeIds_.insert(id);
		}

		// Phase 2: Build adjacency lists from edges
		for (int64_t nodeId : proj.nodeIds_) {
			auto edges = dm->findEdgesByNode(nodeId, "out");
			for (const auto &edge : edges) {
				int64_t targetId = edge.getTargetNodeId();

				// Target must be in the projection
				if (!proj.nodeIds_.contains(targetId)) {
					continue;
				}

				// Filter by edge type
				if (edgeTypeId != 0 && edge.getTypeId() != edgeTypeId) {
					continue;
				}

				// Resolve weight
				double weight = 1.0;
				if (proj.isWeighted_) {
					auto props = dm->getEdgeProperties(edge.getId());
					auto it = props.find(weightProperty);
					if (it != props.end()) {
						const auto &val = it->second;
						if (val.getType() == PropertyType::DOUBLE) {
							weight = std::get<double>(val.getVariant());
						} else if (val.getType() == PropertyType::INTEGER) {
							weight = static_cast<double>(std::get<int64_t>(val.getVariant()));
						}
					}
				}

				proj.outAdj_[nodeId].push_back({targetId, weight});
				proj.inAdj_[targetId].push_back({nodeId, weight});
				proj.edgeCount_++;
			}
		}

		return proj;
	}

	const std::vector<ProjectedEdge> &GraphProjection::getOutNeighbors(int64_t nodeId) const {
		auto it = outAdj_.find(nodeId);
		return it != outAdj_.end() ? it->second : EMPTY_EDGES;
	}

	const std::vector<ProjectedEdge> &GraphProjection::getInNeighbors(int64_t nodeId) const {
		auto it = inAdj_.find(nodeId);
		return it != inAdj_.end() ? it->second : EMPTY_EDGES;
	}

} // namespace graph::query::algorithm
