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

#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace graph::query::algorithm {

	const std::vector<ProjectedEdge> GraphProjection::EMPTY_EDGES;

	namespace {
		bool nodeMatchesLabels(const Node &node,
							   const std::unordered_set<int64_t> &labelIds,
							   bool includeAllLabels) {
			if (includeAllLabels) return true;
			for (const int64_t labelId : node.getLabelIds()) {
				if (labelIds.contains(labelId)) return true;
			}
			return false;
		}

		double readNumericPropertyWeight(const PropertyValue &value, const std::string &propertyName) {
			double weight = 0.0;
			if (value.getType() == PropertyType::DOUBLE) {
				weight = std::get<double>(value.getVariant());
			} else if (value.getType() == PropertyType::INTEGER) {
				weight = static_cast<double>(std::get<int64_t>(value.getVariant()));
			} else {
				throw std::runtime_error("Projection weight property '" + propertyName + "' must be numeric");
			}
			if (!std::isfinite(weight) || weight < 0.0) {
				throw std::runtime_error("Projection weight property '" + propertyName +
										 "' must be a non-negative finite number");
			}
			return weight;
		}

		double resolveWeight(const std::shared_ptr<storage::DataManager> &dm,
							 const Edge &edge,
							 const ProjectionWeightSpec &weightSpec) {
			auto validateWeight = [](double weight, const std::string &context) {
				if (!std::isfinite(weight) || weight < 0.0) {
					throw std::runtime_error(context + " must be a non-negative finite number");
				}
				return weight;
			};
			switch (weightSpec.kind) {
				case ProjectionWeightKind::GPWK_NONE:
					return 1.0;
				case ProjectionWeightKind::GPWK_CONSTANT:
					return validateWeight(weightSpec.constantWeight, "Projection constant weight");
				case ProjectionWeightKind::GPWK_PROPERTY: {
					if (weightSpec.propertyName.empty()) {
						throw std::runtime_error("Projection weight property name must not be empty");
					}
					validateWeight(weightSpec.defaultWeight, "Projection default weight");
					auto props = dm->getEdgeProperties(edge.getId());
					auto it = props.find(weightSpec.propertyName);
					if (it == props.end() || it->second.getType() == PropertyType::NULL_TYPE) {
						return weightSpec.defaultWeight;
					}
					return readNumericPropertyWeight(it->second, weightSpec.propertyName);
				}
			}
			return 1.0;
		}

	} // namespace

	void GraphProjection::addArc(GraphProjection &proj,
								 int64_t sourceId,
								 int64_t targetId,
								 double weight,
								 int64_t relationshipId,
								 bool syntheticReverse) {
		proj.outAdj_[sourceId].push_back({targetId, weight, relationshipId, syntheticReverse});
		proj.inAdj_[targetId].push_back({sourceId, weight, relationshipId, syntheticReverse});
		++proj.edgeCount_;
	}

	void GraphProjection::addProjectedRelationship(GraphProjection &proj,
												  const Edge &edge,
												  const RelationshipProjectionSpec &spec,
												  double weight) {
		const int64_t sourceId = edge.getSourceNodeId();
		const int64_t targetId = edge.getTargetNodeId();
		switch (spec.orientation) {
			case ProjectionOrientation::GPO_NATURAL:
				addArc(proj, sourceId, targetId, weight, edge.getId(), false);
				break;
			case ProjectionOrientation::GPO_REVERSE:
				addArc(proj, targetId, sourceId, weight, edge.getId(), false);
				break;
			case ProjectionOrientation::GPO_UNDIRECTED:
				addArc(proj, sourceId, targetId, weight, edge.getId(), false);
				if (sourceId != targetId) {
					addArc(proj, targetId, sourceId, weight, edge.getId(), true);
				}
				break;
		}
	}

	GraphProjection GraphProjection::build(const std::shared_ptr<storage::DataManager> &dm,
										   const std::string &nodeLabel,
										   const std::string &edgeType,
										   const std::string &weightProperty) {
		return build(dm, ProjectionSpec::legacy("", nodeLabel, edgeType, weightProperty));
	}

	GraphProjection GraphProjection::build(const std::shared_ptr<storage::DataManager> &dm,
										   const ProjectionSpec &spec) {
		GraphProjection proj;
		proj.isWeighted_ = spec.usesWeights();

		int64_t maxNodeId = dm->getIdAllocator(EntityType::Node)->getCurrentMaxId();

		const bool includeAllLabels = spec.nodeLabels.empty();
		std::unordered_set<int64_t> nodeLabelIds;
		for (const auto &label : spec.nodeLabels) {
			const int64_t id = dm->resolveTokenId(label);
			if (id != 0) nodeLabelIds.insert(id);
		}

		std::vector<RelationshipProjectionSpec> defaultRelationships;
		const std::vector<RelationshipProjectionSpec> *relationships = &spec.relationships;
		if (spec.relationships.empty()) {
			RelationshipProjectionSpec allTypes;
			allTypes.orientation = spec.defaultOrientation;
			defaultRelationships.push_back(std::move(allTypes));
			relationships = &defaultRelationships;
		}

		std::unordered_map<int64_t, const RelationshipProjectionSpec *> relationshipsByType;
		const RelationshipProjectionSpec *allRelationshipTypes = nullptr;
		for (const auto &relationship : *relationships) {
			if (relationship.type.empty()) {
				allRelationshipTypes = &relationship;
				continue;
			}
			const int64_t typeId = dm->resolveTokenId(relationship.type);
			if (typeId != 0) {
				relationshipsByType.emplace(typeId, &relationship);
			}
		}

		// Phase 1: Collect active nodes matching label filter
		for (int64_t id = 1; id <= maxNodeId; ++id) {
			auto node = dm->getNode(id);
			if (node.getId() == 0 || !node.isActive()) {
				continue;
			}
			if (!nodeMatchesLabels(node, nodeLabelIds, includeAllLabels)) {
				continue;
			}
			proj.nodeIds_.insert(id);
		}

		// Phase 2: Build adjacency lists from edges
		for (int64_t nodeId : proj.nodeIds_) {
			auto edges = dm->findEdgesByNode(nodeId, "out");
			for (const auto &edge : edges) {
				if (!proj.nodeIds_.contains(edge.getTargetNodeId())) {
					continue;
				}

				const RelationshipProjectionSpec *relationship = allRelationshipTypes;
				if (const auto it = relationshipsByType.find(edge.getTypeId());
					it != relationshipsByType.end()) {
					relationship = it->second;
				}
				if (!relationship) {
					continue;
				}

				const double weight = resolveWeight(dm, edge, relationship->weight);
				addProjectedRelationship(proj, edge, *relationship, weight);
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

	size_t GraphProjection::estimatedMemoryBytes() const {
		constexpr size_t kHashNodeOverhead = 32;
		constexpr size_t kAdjacencyEntryOverhead = sizeof(std::pair<const int64_t, std::vector<ProjectedEdge>>) +
			kHashNodeOverhead;

		auto estimateAdjacency = [&](const std::unordered_map<int64_t, std::vector<ProjectedEdge>> &adjacency) {
			size_t bytes = adjacency.bucket_count() * sizeof(void *);
			bytes += adjacency.size() * kAdjacencyEntryOverhead;
			for (const auto &[_, edges] : adjacency) {
				bytes += edges.capacity() * sizeof(ProjectedEdge);
			}
			return bytes;
		};

		size_t bytes = sizeof(*this);
		bytes += nodeIds_.bucket_count() * sizeof(void *);
		bytes += nodeIds_.size() * (sizeof(int64_t) + kHashNodeOverhead);
		bytes += estimateAdjacency(outAdj_);
		bytes += estimateAdjacency(inAdj_);
		return bytes;
	}

} // namespace graph::query::algorithm
