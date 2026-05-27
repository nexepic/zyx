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

	RelationshipExpandBatch RelationshipAdjacencyCursor::expand(const std::vector<int64_t> &sourceIds,
	                                                          const RelationshipExpandConfig &config,
	                                                          const RelationshipExpandRequirements &requirements) const {
		RelationshipExpandBatch batch;
		if (!dm_ || config.edgeTypeId < 0) {
			return batch;
		}

		for (const int64_t sourceId : sourceIds) {
			const auto edges = dm_->findEdgesByNode(sourceId, config.direction);
			for (const auto &edge : edges) {
				if (requirements.needsEdgeActiveCheck && !edge.isActive()) {
					continue;
				}
				if (config.edgeTypeId != 0 && edge.getTypeId() != config.edgeTypeId) {
					continue;
				}

				const int64_t targetId = targetForSource(edge, sourceId, config.direction);
				Node target = dm_->getNode(targetId);
				if (requirements.needsTargetActiveCheck && !target.isActive()) {
					continue;
				}
				if (requirements.needsTargetLabels && !matchesTargetLabels(target, config)) {
					continue;
				}

				batch.rows.push_back(RelationshipExpandRow{sourceId, edge.getId(), targetId});
				batch.selected.push_back(1);
			}
		}
		return batch;
	}

} // namespace graph::query::execution
