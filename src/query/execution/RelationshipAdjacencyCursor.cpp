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

	int64_t RelationshipAdjacencyCursor::targetForSource(
			const traversal::RelationshipEdgeRef &edgeRef,
			int64_t sourceId,
			const std::string &direction) const {
		if (direction == "in") {
			return edgeRef.sourceNodeId;
		}
		if (direction == "both") {
			return edgeRef.sourceNodeId == sourceId ? edgeRef.targetNodeId : edgeRef.sourceNodeId;
		}
		return edgeRef.targetNodeId;
	}

	traversal::RelationshipTraversalOptions RelationshipAdjacencyCursor::traversalOptions(
			const RelationshipExpandConfig &config,
			const RelationshipExpandRequirements &requirements) const {
		traversal::RelationshipTraversalOptions options;
		options.direction = traversal::RelationshipTraversal::directionFromString(config.direction);
		options.integrity = traversal::RelationshipTraversalIntegrity::RTI_BOUND_BY_EDGE_COUNT;
		options.activeOnly = requirements.needsEdgeActiveCheck;
		options.typeId = config.edgeTypeId;
		return options;
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

	std::optional<int64_t> RelationshipAdjacencyCursor::acceptedTargetForEdgeRef(
			const traversal::RelationshipEdgeRef &edgeRef,
			int64_t sourceId,
			const RelationshipExpandConfig &config,
			const RelationshipExpandRequirements &requirements) const {
		if (requirements.needsEdgeActiveCheck && !edgeRef.active) {
			return std::nullopt;
		}
		if (config.edgeTypeId != 0 && edgeRef.typeId != config.edgeTypeId) {
			return std::nullopt;
		}

		const int64_t targetId = targetForSource(edgeRef, sourceId, config.direction);
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
		auto traversal = dm_->getRelationshipTraversal();
		if (!traversal) {
			return total;
		}
		const auto options = traversalOptions(config, requirements);

		for (const int64_t sourceId : sourceIds) {
			if (!requirements.needsTargetActiveCheck && !requirements.needsTargetLabels) {
				total += static_cast<int64_t>(traversal->countAdjacentEdgeRefs(sourceId, options));
				continue;
			}
			(void) traversal->visitAdjacentEdgeRefs(sourceId, options, [&](const traversal::RelationshipEdgeRef &edgeRef) {
				if (acceptedTargetForEdgeRef(edgeRef, sourceId, config, requirements).has_value()) {
					++total;
				}
				return true;
			});
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
		auto traversal = dm_->getRelationshipTraversal();
		if (!traversal) {
			return emitted;
		}
		const auto options = traversalOptions(config, requirements);
		for (const int64_t sourceId : sourceIds) {
			if (!keepGoing) {
				break;
			}
			(void) traversal->visitAdjacentEdgeRefs(
					sourceId,
					options,
					[&](const traversal::RelationshipEdgeRef &edgeRef) {
						const auto targetId = acceptedTargetForEdgeRef(edgeRef, sourceId, config, requirements);
						if (!targetId.has_value()) {
							return true;
						}

						++emitted;
						keepGoing = visitor(RelationshipExpandRow{sourceId, edgeRef.edgeId, *targetId});
						return keepGoing;
					});
		}
		return emitted;
	}

} // namespace graph::query::execution
