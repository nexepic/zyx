#include "graph/query/execution/RelationshipAdjacencyCursor.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "graph/concurrent/ParallelOperatorExecutor.hpp"

namespace graph::query::execution {
	namespace {
		constexpr size_t kParallelFrontierMinItems = 4096;
		constexpr size_t kParallelFrontierSampleSources = 16;
		constexpr size_t kEstimatedFrontierEdgeBytes = 64;

		struct RelationshipCountPartitionState {
			int64_t count = 0;
		};

		struct RelationshipExpandPartitionState {
			RelationshipExpandBatch batch;
		};

		size_t estimateFrontierItems(
				const std::vector<int64_t> &sourceIds,
				const std::shared_ptr<traversal::RelationshipTraversal> &traversal,
				const traversal::RelationshipTraversalOptions &options,
				const concurrent::ThreadPool *threadPool) {
			size_t estimatedItems = sourceIds.size();
			if (!concurrent::hasParallelWorkers(threadPool) || sourceIds.size() < kParallelFrontierSampleSources) {
				return estimatedItems;
			}

			const size_t sampleCount = std::min(kParallelFrontierSampleSources, sourceIds.size());
			size_t sampledEdges = 0;
			for (size_t i = 0; i < sampleCount; ++i) {
				sampledEdges += traversal->countAdjacentEdgeRefs(sourceIds[i], options);
			}
			if (sampleCount != 0) {
				estimatedItems = std::max(estimatedItems, (sampledEdges * sourceIds.size()) / sampleCount);
			}
			return estimatedItems;
		}

		size_t estimateFrontierBytes(size_t estimatedItems) {
			if (estimatedItems > std::numeric_limits<size_t>::max() / kEstimatedFrontierEdgeBytes) {
				return std::numeric_limits<size_t>::max();
			}
			return estimatedItems * kEstimatedFrontierEdgeBytes;
		}
	} // namespace

	RelationshipAdjacencyCursor::RelationshipAdjacencyCursor(std::shared_ptr<storage::DataManager> dm,
	                                                         concurrent::ThreadPool *threadPool)
		: dm_(std::move(dm)), threadPool_(threadPool) {}

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
		if (!dm_ || config.edgeTypeId < 0 || sourceIds.empty()) {
			return batch;
		}
		auto traversal = dm_->getRelationshipTraversal();
		if (!traversal) {
			return batch;
		}
		const auto options = traversalOptions(config, requirements);

		const size_t estimatedItems = estimateFrontierItems(sourceIds, traversal, options, threadPool_);
		const concurrent::ParallelOperatorOptions parallelOptions{
				.phase = "relationship_frontier.expand",
				.workloadKind = concurrent::ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL,
				.estimatedItems = estimatedItems,
				.estimatedBytes = estimateFrontierBytes(estimatedItems),
				.minPartitions = 2,
				.minItems = kParallelFrontierMinItems};
		(void) concurrent::ParallelOperatorExecutor::runRangePartitions<RelationshipExpandPartitionState>(
				0,
				sourceIds.size(),
				threadPool_,
				parallelOptions,
				[&](const concurrent::ParallelRangePartition &range, RelationshipExpandPartitionState &state) {
					for (size_t i = range.begin; i < range.end; ++i) {
						const int64_t sourceId = sourceIds[i];
						(void) traversal->visitAdjacentEdgeRefs(
								sourceId,
								options,
								[&](const traversal::RelationshipEdgeRef &edgeRef) {
									const auto targetId = acceptedTargetForEdgeRef(edgeRef, sourceId, config, requirements);
									if (!targetId.has_value()) {
										return true;
									}
									state.batch.rows.push_back(RelationshipExpandRow{sourceId, edgeRef.edgeId, *targetId});
									state.batch.selected.push_back(1);
									return true;
								});
					}
				},
				[&](size_t, RelationshipExpandPartitionState &state) {
					batch.rows.insert(batch.rows.end(), state.batch.rows.begin(), state.batch.rows.end());
					batch.selected.insert(batch.selected.end(), state.batch.selected.begin(), state.batch.selected.end());
				});
		return batch;
	}

	int64_t RelationshipAdjacencyCursor::count(const std::vector<int64_t> &sourceIds,
	                                           const RelationshipExpandConfig &config,
	                                           const RelationshipExpandRequirements &requirements) const {
		int64_t total = 0;
		if (!dm_ || config.edgeTypeId < 0 || sourceIds.empty()) {
			return total;
		}
		auto traversal = dm_->getRelationshipTraversal();
		if (!traversal) {
			return total;
		}
		const auto options = traversalOptions(config, requirements);

		const size_t estimatedItems = estimateFrontierItems(sourceIds, traversal, options, threadPool_);
		const concurrent::ParallelOperatorOptions parallelOptions{
				.phase = "relationship_frontier.count",
				.workloadKind = concurrent::ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL,
				.estimatedItems = estimatedItems,
				.estimatedBytes = estimateFrontierBytes(estimatedItems),
				.minPartitions = 2,
				.minItems = kParallelFrontierMinItems};
		(void) concurrent::ParallelOperatorExecutor::runRangePartitions<RelationshipCountPartitionState>(
				0,
				sourceIds.size(),
				threadPool_,
				parallelOptions,
				[&](const concurrent::ParallelRangePartition &range, RelationshipCountPartitionState &state) {
					for (size_t i = range.begin; i < range.end; ++i) {
						const int64_t sourceId = sourceIds[i];
						if (!requirements.needsTargetActiveCheck && !requirements.needsTargetLabels) {
							state.count += static_cast<int64_t>(traversal->countAdjacentEdgeRefs(sourceId, options));
							continue;
						}
						(void) traversal->visitAdjacentEdgeRefs(
								sourceId,
								options,
								[&](const traversal::RelationshipEdgeRef &edgeRef) {
									if (acceptedTargetForEdgeRef(edgeRef, sourceId, config, requirements).has_value()) {
										++state.count;
									}
									return true;
								});
					}
				},
				[&](size_t, RelationshipCountPartitionState &state) {
					total += state.count;
				});
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
