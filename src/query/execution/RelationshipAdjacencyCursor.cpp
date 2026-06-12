#include "graph/query/execution/RelationshipAdjacencyCursor.hpp"

#include <utility>

namespace graph::query::execution {

	RelationshipAdjacencyCursor::RelationshipAdjacencyCursor(std::shared_ptr<storage::DataManager> dm)
		: dm_(std::move(dm)) {}

	bool RelationshipAdjacencyCursor::matchesTargetLabels(const Node &node, const RelationshipExpandConfig &config) const {
		for (const int64_t labelId : config.targetLabelIds) {
			if (labelId <= 0 || !node.hasLabelId(labelId)) {
				return false;
			}
		}
		return true;
	}

	int64_t RelationshipAdjacencyCursor::targetForSource(const Edge &edge, int64_t sourceId, const std::string &direction) const {
		if (direction == "in") {
			return edge.getSourceNodeId();
		}
		if (direction == "both") {
			return edge.getSourceNodeId() == sourceId ? edge.getTargetNodeId() : edge.getSourceNodeId();
		}
		return edge.getTargetNodeId();
	}

	bool RelationshipAdjacencyCursor::acceptsTarget(int64_t targetId,
	                                                const RelationshipExpandConfig &config,
	                                                const RelationshipExpandRequirements &requirements) const {
		if (!requirements.needsTargetActiveCheck && !requirements.needsTargetLabels) {
			return true;
		}
		if (requirements.needsTargetLabels) {
			for (const int64_t labelId : config.targetLabelIds) {
				if (labelId <= 0) {
					return false;
				}
			}
		}
		Node target = dm_->getNode(targetId);
		if (requirements.needsTargetActiveCheck && !target.isActive()) {
			return false;
		}
		return !requirements.needsTargetLabels || matchesTargetLabels(target, config);
	}

	std::optional<int64_t> RelationshipAdjacencyCursor::acceptedTargetForEdge(
			const Edge &edge,
			int64_t sourceId,
			const RelationshipExpandConfig &config,
			const RelationshipExpandRequirements &requirements) const {
		if (requirements.needsEdgeActiveCheck && !edge.isActive()) {
			return std::nullopt;
		}
		if (config.edgeTypeId != 0 && edge.getTypeId() != config.edgeTypeId) {
			return std::nullopt;
		}

		const int64_t targetId = targetForSource(edge, sourceId, config.direction);
		if (!acceptsTarget(targetId, config, requirements)) {
			return std::nullopt;
		}
		return targetId;
	}

	RelationshipExpandBatch RelationshipAdjacencyCursor::expand(const std::vector<int64_t> &sourceIds,
	                                                          const RelationshipExpandConfig &config,
	                                                          const RelationshipExpandRequirements &requirements) const {
		RelationshipExpandBatch batch;
		if (!dm_ || config.edgeTypeId < 0) {
			return batch;
		}

		(void) forEach(sourceIds, config, requirements, [&](const RelationshipExpandRow &row) {
			batch.rows.push_back(row);
			batch.selected.push_back(1);
			return true;
		});
		return batch;
	}

	int64_t RelationshipAdjacencyCursor::count(const std::vector<int64_t> &sourceIds,
	                                           const RelationshipExpandConfig &config,
	                                           const RelationshipExpandRequirements &requirements) const {
		int64_t total = 0;
		if (!dm_ || config.edgeTypeId < 0) {
			return total;
		}

		for (const int64_t sourceId : sourceIds) {
			dm_->visitEdgesByNode(
					sourceId,
					[&](const Edge &edge) {
						if (acceptedTargetForEdge(edge, sourceId, config, requirements).has_value()) {
							++total;
						}
						return true;
					},
					config.direction);
		}
		return total;
	}

	size_t RelationshipAdjacencyCursor::forEach(const std::vector<int64_t> &sourceIds,
	                                            const RelationshipExpandConfig &config,
	                                            const RelationshipExpandRequirements &requirements,
	                                            const RelationshipExpandVisitor &visitor) const {
		if (!dm_ || !visitor || config.edgeTypeId < 0) {
			return 0;
		}

		size_t emitted = 0;
		bool keepGoing = true;
		for (const int64_t sourceId : sourceIds) {
			if (!keepGoing) {
				break;
			}
			dm_->visitEdgesByNode(
					sourceId,
					[&](const Edge &edge) {
						const auto targetId = acceptedTargetForEdge(edge, sourceId, config, requirements);
						if (!targetId.has_value()) {
							return true;
						}

						++emitted;
						keepGoing = visitor(RelationshipExpandRow{sourceId, edge.getId(), *targetId});
						return keepGoing;
					},
					config.direction);
		}
		return emitted;
	}

} // namespace graph::query::execution
