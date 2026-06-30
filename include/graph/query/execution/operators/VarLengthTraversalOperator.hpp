/**
 * @file VarLengthTraversalOperator.hpp
 * @author Nexepic
 * @date 2025/12/22
 *
 * @copyright Copyright (c) 2025 Nexepic
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
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "../PhysicalOperator.hpp"
#include "graph/concurrent/ParallelOperatorExecutor.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/execution/RelationshipFrontierExecutor.hpp"
#include "graph/query/execution/PropertyPredicateKernel.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/traversal/RelationshipTraversal.hpp"

namespace graph::query::execution::operators {

	class VarLengthTraversalOperator : public PhysicalOperator {
	public:
		static constexpr size_t PARALLEL_MATERIALIZE_THRESHOLD = 2 * DEFAULT_BATCH_SIZE;
		static constexpr size_t NEIGHBOR_CACHE_MAX_ENTRIES = 16 * DEFAULT_BATCH_SIZE;
		static constexpr size_t NEIGHBOR_CACHE_MAX_NEIGHBORS_PER_ENTRY = 256;
		static constexpr int PROPERTY_FILTER_FRONTIER_MAX_HOPS = 2;
		static constexpr size_t INDEX_ASSISTED_TARGET_MAX_CANDIDATES = 4096;
		static constexpr size_t INDEX_ASSISTED_REVERSE_TARGET_MAX_CANDIDATES = 256;
		static constexpr size_t INDEX_ASSISTED_REVERSE_MAX_VISITED = 1'000'000;

		VarLengthTraversalOperator(std::shared_ptr<storage::DataManager> dm, std::unique_ptr<PhysicalOperator> child,
								   std::string sourceVar, std::string targetVar, std::string edgeType, int minHops,
								   int maxHops, std::string direction,
								   std::vector<int64_t> targetLabelIds = {},
								   std::vector<std::pair<std::string, PropertyValue>> targetProperties = {},
								   std::shared_ptr<indexes::IndexManager> indexManager = nullptr,
								   std::vector<std::string> targetLabelNames = {}) :
			dm_(std::move(dm)), child_(std::move(child)), sourceVar_(std::move(sourceVar)),
			targetVar_(std::move(targetVar)),
			edgeType_(std::move(edgeType)), direction_(std::move(direction)), minHops_(minHops), maxHops_(maxHops),
			targetLabelIds_(std::move(targetLabelIds)), targetProperties_(std::move(targetProperties)),
			indexManager_(std::move(indexManager)), targetLabelNames_(std::move(targetLabelNames)) {}

		void open() override {
			if (child_)
				child_->open();

			// Resolve edge type ID once
			edgeTypeId_ = 0;
			if (!edgeType_.empty()) {
				edgeTypeId_ = dm_->resolveTokenId(edgeType_);
				if (edgeTypeId_ == 0) edgeTypeId_ = -1;
			}

			// Clamp maxHops to runtime config limit
			if (queryContext_) {
				maxHops_ = std::min(maxHops_, queryContext_->maxVarLengthDepth);
			}

			// Reset DFS state
			while (!dfsStack_.empty()) dfsStack_.pop();
			visitedPath_.clear();
			currentInputBatch_.reset();
			inputIdx_ = 0;
			inputExhausted_ = false;
			neighborCache_.clear();
			compileTargetPropertyKernel();
			initializeIndexedTargetCandidates();
			clearFrontierState();
		}

		std::optional<RecordBatch> next() override {
			for (;;) {
				std::vector<PendingEmit> pending;
				pending.reserve(DEFAULT_BATCH_SIZE);
				collectPendingEmits(pending);
				if (pending.empty()) { // ZYX_COV_EXCL_LINE: materialization is called after pending rows are collected.
					return std::nullopt;
				}

				auto outputBatch = materializePendingEmits(pending);
				if (!outputBatch.empty()) {
					return outputBatch;
				}
			}
		}

		void close() override {
			if (child_)
				child_->close();
			while (!dfsStack_.empty()) dfsStack_.pop();
			visitedPath_.clear();
			neighborCache_.clear();
			clearFrontierState();
		}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			auto v = child_ ? child_->getOutputVariables() : std::vector<std::string>{};
			v.push_back(targetVar_);
			return v;
		}

		[[nodiscard]] std::string toString() const override {
			return "VarLengthTraversal(" + sourceVar_ + " -[*" + std::to_string(minHops_) + ".." +
				   std::to_string(maxHops_) + "]-> " + targetVar_ + ")";
		}

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override { return {child_.get()}; }

	private:
		struct DFSFrame {
			int64_t nodeId;
			int depth;
			std::vector<int64_t> neighbors;
			size_t neighborIdx;
			bool neighborsLoaded;
			bool emitted = false;
		};

		struct PendingEmit {
			Record baseRecord;
			int64_t nodeId = 0;
			Node targetNode;
			bool hasTargetNode = false;
			bool hasTargetProperties = false;
		};

		struct MaterializePartitionState {
			RecordBatch records;
		};

		struct IndexedTargetCandidates {
			std::vector<int64_t> ids;
			std::unordered_set<int64_t> idSet;
			std::vector<std::unordered_set<int64_t>> reverseReachableWithin;
			bool reversePruningEnabled = false;
			bool emptyFromIndex = false;
		};

		std::shared_ptr<storage::DataManager> dm_;
		std::unique_ptr<PhysicalOperator> child_;
		std::string sourceVar_, targetVar_, edgeType_, direction_;
		int minHops_, maxHops_;
		int64_t edgeTypeId_ = 0;
		std::vector<int64_t> targetLabelIds_;
		std::vector<std::pair<std::string, PropertyValue>> targetProperties_;
		std::optional<PropertyPredicateKernel> targetPropertyKernel_;
		std::shared_ptr<indexes::IndexManager> indexManager_;
		std::vector<std::string> targetLabelNames_;
		std::optional<IndexedTargetCandidates> indexedTargetCandidates_;

		// DFS state
		std::stack<DFSFrame> dfsStack_;
		std::unordered_set<int64_t> visitedPath_;
		std::optional<RecordBatch> currentInputBatch_;
		size_t inputIdx_ = 0;
		Record currentInputRecord_;
		bool inputExhausted_ = false;
		std::unordered_map<int64_t, std::vector<int64_t>> neighborCache_;
		RelationshipFrontierState frontier_;
		size_t frontierEmitIndex_ = 0;
		bool frontierActive_ = false;
		bool frontierEmitsComplete_ = false;

			bool advanceInput() {
				if (!child_) {
					return false;
				}
				for (;;) {
					if (currentInputBatch_ && inputIdx_ < currentInputBatch_->size()) {
						currentInputRecord_ = (*currentInputBatch_)[inputIdx_++];
						return true;
				}
				if (inputExhausted_) return false;

				auto batch = child_->next();
				if (!batch) {
					inputExhausted_ = true;
					return false;
				}
				currentInputBatch_ = std::move(batch);
				inputIdx_ = 0;
			}
		}

		[[nodiscard]] bool canUseFrontierTraversal() const {
			return threadPool_ != nullptr && !threadPool_->isSingleThreaded() && dm_ != nullptr &&
				   edgeTypeId_ >= 0 && maxHops_ > 0 &&
				   (targetProperties_.empty() || maxHops_ <= PROPERTY_FILTER_FRONTIER_MAX_HOPS ||
					hasReverseTargetPruning());
		}

		void clearFrontierState() {
			frontier_.clear();
			frontierEmitIndex_ = 0;
			frontierActive_ = false;
			frontierEmitsComplete_ = false;
		}

		void compileTargetPropertyKernel() {
			targetPropertyKernel_.reset();
			if (targetProperties_.empty()) {
				return;
			}

			std::vector<VectorizedPropertyPredicate> predicates;
			predicates.reserve(targetProperties_.size());
			for (const auto &[key, value]: targetProperties_) {
				VectorizedPropertyPredicate predicate;
				predicate.propertyKey = key;
				predicate.op = VectorPredicateOp::VPO_EQ;
				predicate.value = value;
				predicates.push_back(std::move(predicate));
			}
			targetPropertyKernel_.emplace(std::move(predicates));
		}

		void initializeIndexedTargetCandidates() {
			indexedTargetCandidates_.reset();
			if (!indexManager_ || targetProperties_.empty()) {
				return;
			}

			struct CandidateSource {
				size_t propertyIndex = 0;
				size_t estimate = 0;
				std::string label;
				bool scoped = false;
			};

			std::optional<CandidateSource> best;
			for (size_t propertyIndex = 0; propertyIndex < targetProperties_.size(); ++propertyIndex) {
				const auto &[key, value] = targetProperties_[propertyIndex];
				for (const auto &label: targetLabelNames_) {
					if (!indexManager_->hasNodePropertyIndexForLabel(label, key)) {
						continue;
					}
					const size_t estimate = indexManager_->estimateNodeIdsByLabelAndProperty(label, key, value);
					if (!best || estimate < best->estimate) {
						best = CandidateSource{.propertyIndex = propertyIndex,
											   .estimate = estimate,
											   .label = label,
											   .scoped = true};
					}
				}
				if (indexManager_->hasPropertyIndex("node", key)) {
					const size_t estimate = indexManager_->estimateNodeIdsByProperty(key, value);
					if (!best || estimate < best->estimate) {
						best = CandidateSource{.propertyIndex = propertyIndex, .estimate = estimate};
					}
				}
			}
			if (!best || best->estimate > INDEX_ASSISTED_TARGET_MAX_CANDIDATES) {
				debug::PerfTrace::addValue("varlength.target_index.reason.not_used", 1);
				return;
			}
			debug::PerfTrace::addValue(best->scoped ? "varlength.target_index.source.scoped"
										   : "varlength.target_index.source.global",
								 1);

			const auto &[key, value] = targetProperties_[best->propertyIndex];
			IndexedTargetCandidates candidates;
			candidates.ids = best->scoped
									 ? indexManager_->findNodeIdsByLabelAndProperty(best->label, key, value)
									 : indexManager_->findNodeIdsByProperty(key, value);
			candidates.idSet.reserve(candidates.ids.size());
			for (const int64_t id: candidates.ids) {
				candidates.idSet.insert(id);
			}
			candidates.emptyFromIndex = candidates.idSet.empty();
			if (candidates.emptyFromIndex) {
				debug::PerfTrace::addValue("varlength.target_index.strategy.empty_candidate", 1);
			}
			debug::PerfTrace::addValue("varlength.target_index.candidates",
									   static_cast<int64_t>(std::min<size_t>(
											   candidates.idSet.size(),
											   static_cast<size_t>(std::numeric_limits<int64_t>::max()))));

			if (!candidates.emptyFromIndex &&
				candidates.idSet.size() <= INDEX_ASSISTED_REVERSE_TARGET_MAX_CANDIDATES &&
				maxHops_ > PROPERTY_FILTER_FRONTIER_MAX_HOPS) {
				buildReverseReachability(candidates);
			}
			indexedTargetCandidates_ = std::move(candidates);
		}

		void buildReverseReachability(IndexedTargetCandidates &candidates) const {
			if (!dm_ || edgeTypeId_ < 0 || maxHops_ <= 0) { // ZYX_COV_EXCL_LINE: reverse reachability is only invoked after open() resolves traversal prerequisites.
				return;
			}
			auto traversal = dm_->getRelationshipTraversal();
			if (!traversal) { // ZYX_COV_EXCL_LINE: initialized DataManager always owns a traversal service.
				return;
			}

			traversal::RelationshipTraversalOptions options;
			options.direction = reverseDirection(traversal::RelationshipTraversal::directionFromString(direction_));
			options.integrity = traversal::RelationshipTraversalIntegrity::RTI_BOUND_BY_EDGE_COUNT;
			options.activeOnly = true;
			options.typeId = edgeTypeId_;

			candidates.reverseReachableWithin.clear();
			candidates.reverseReachableWithin.resize(static_cast<size_t>(maxHops_) + 1);
			candidates.reverseReachableWithin[0] = candidates.idSet;

			std::unordered_set<int64_t> visited = candidates.idSet;
			std::vector<int64_t> frontier(candidates.ids.begin(), candidates.ids.end());
			for (int depth = 1; depth <= maxHops_ && !frontier.empty(); ++depth) { // ZYX_COV_EXCL_LINE: loop termination shape depends on graph topology, not operator semantics.
				std::vector<int64_t> nextFrontier;
				nextFrontier.reserve(frontier.size());
				candidates.reverseReachableWithin[static_cast<size_t>(depth)] =
						candidates.reverseReachableWithin[static_cast<size_t>(depth - 1)];

				for (const int64_t nodeId: frontier) {
					(void) traversal->visitAdjacentEdgeRefs(
							nodeId,
							options,
							[&](const traversal::RelationshipEdgeRef &edgeRef) {
								const int64_t predecessorId = targetForEdgeRef(edgeRef, nodeId, options.direction);
								if (visited.contains(predecessorId)) {
									return true;
								}
								Node predecessor = dm_->getNode(predecessorId);
								if (!predecessor.isActive()) {
									return true;
								}
								visited.insert(predecessorId);
								candidates.reverseReachableWithin[static_cast<size_t>(depth)].insert(predecessorId);
								nextFrontier.push_back(predecessorId);
								return visited.size() < INDEX_ASSISTED_REVERSE_MAX_VISITED;
							});
					if (visited.size() >= INDEX_ASSISTED_REVERSE_MAX_VISITED) { // ZYX_COV_EXCL_LINE: defensive cap for pathological reverse traversals.
						break;
					}
				}

				if (visited.size() >= INDEX_ASSISTED_REVERSE_MAX_VISITED) { // ZYX_COV_EXCL_LINE: defensive cap for pathological reverse traversals.
					candidates.reverseReachableWithin.clear();
					debug::PerfTrace::addValue("varlength.target_index.reverse_prune_aborted", 1);
					return;
				}
				frontier = std::move(nextFrontier);
			}
			candidates.reversePruningEnabled = !candidates.reverseReachableWithin.empty();
			if (candidates.reversePruningEnabled) { // ZYX_COV_EXCL_LINE: reverse sets are either fully built or the function returns on cap abort.
				debug::PerfTrace::addValue("varlength.target_index.strategy.bidirectional_prune", 1);
				debug::PerfTrace::addValue("varlength.target_index.reverse_prune_nodes",
										   static_cast<int64_t>(std::min<size_t>(
												   visited.size(),
												   static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
			}
		}

		[[nodiscard]] static traversal::RelationshipDirectionKind reverseDirection(
				traversal::RelationshipDirectionKind direction) {
			if (direction == traversal::RelationshipDirectionKind::RDK_IN) {
				return traversal::RelationshipDirectionKind::RDK_OUT;
			}
			if (direction == traversal::RelationshipDirectionKind::RDK_OUT) {
				return traversal::RelationshipDirectionKind::RDK_IN;
			}
			return traversal::RelationshipDirectionKind::RDK_BOTH;
		}

		[[nodiscard]] bool hasReverseTargetPruning() const {
			return indexedTargetCandidates_ && indexedTargetCandidates_->reversePruningEnabled;
		}

		[[nodiscard]] bool isIndexedTargetCandidate(int64_t nodeId) const {
			return !indexedTargetCandidates_ || indexedTargetCandidates_->idSet.contains(nodeId);
		}

		[[nodiscard]] bool canReachIndexedTargetWithinRemaining(int64_t nodeId, int depth) const {
			if (!hasReverseTargetPruning()) {
				return true;
			}
			if (depth > maxHops_) {
				return false;
			}
			const size_t remaining = static_cast<size_t>(maxHops_ - depth);
			const auto &sets = indexedTargetCandidates_->reverseReachableWithin;
			if (remaining >= sets.size()) {
				return true;
			}
			return sets[remaining].contains(nodeId);
		}

			bool initializeFrontierFromInputBatch() {
				if (!child_) {
					return false;
				}
				for (;;) {
					if (!currentInputBatch_ || inputIdx_ >= currentInputBatch_->size()) {
						if (inputExhausted_) {
						return false;
					}
					auto batch = child_->next();
					if (!batch) {
						inputExhausted_ = true;
						return false;
					}
					currentInputBatch_ = std::move(batch);
					inputIdx_ = 0;
				}

				frontier_.clear();
				frontier_.reserveSources(currentInputBatch_->size() - inputIdx_);
				for (; inputIdx_ < currentInputBatch_->size(); ++inputIdx_) {
					auto sourceNode = (*currentInputBatch_)[inputIdx_].getNode(sourceVar_);
					if (!sourceNode) {
						continue;
					}

					frontier_.addSource(std::move((*currentInputBatch_)[inputIdx_]), sourceNode->getId());
				}

				if (hasReverseTargetPruning()) {
					frontier_.filterFrontier([&](const RelationshipFrontierEntry &entry) {
						return canReachIndexedTargetWithinRemaining(entry.nodeId, entry.depth);
					});
				}

				if (!frontier_.empty()) {
					frontierEmitIndex_ = 0;
					frontierActive_ = true;
					frontierEmitsComplete_ = false;
					return true;
				}
			}
		}

		void collectPendingEmitsFromFrontier(std::vector<PendingEmit> &pending) {
			if (indexedTargetCandidates_ && indexedTargetCandidates_->emptyFromIndex) {
				inputExhausted_ = true;
				clearFrontierState();
				return;
			}
			const size_t batchThreshold = emitBatchThreshold();
			while (pending.size() < batchThreshold) {
				if (!frontierActive_) {
					if (!initializeFrontierFromInputBatch()) {
						break;
					}
				}

				if (!frontierEmitsComplete_) {
					emitCurrentFrontierDepth(pending, batchThreshold);
					if (pending.size() >= batchThreshold) {
						break;
					}
					frontierEmitsComplete_ = true;
				}

				if (frontier_.empty() || frontier_.currentDepth() >= maxHops_) {
					clearFrontierState();
					continue;
				}

				expandCurrentFrontier();
				if (frontier_.empty()) {
					clearFrontierState();
				}
			}
		}

		void emitCurrentFrontierDepth(std::vector<PendingEmit> &pending, size_t batchThreshold) {
			if (frontier_.empty()) {
				return;
			}
			const int depth = frontier_.currentDepth();
			if (depth < minHops_ || (depth == 0 && minHops_ != 0)) {
				frontierEmitIndex_ = frontier_.size();
				return;
			}

			while (frontierEmitIndex_ < frontier_.size() && pending.size() < batchThreshold) {
				const auto &entry = frontier_.frontierEntry(frontierEmitIndex_++);
				if (auto emit = preparePendingEmit(entry.nodeId)) {
					emit->baseRecord = frontier_.sourceRecord(entry.sourceIndex);
					pending.push_back(std::move(*emit));
				}
			}
		}

		void expandCurrentFrontier() {
			traversal::RelationshipTraversalOptions options;
			options.direction = traversal::RelationshipTraversal::directionFromString(direction_);
			options.integrity = traversal::RelationshipTraversalIntegrity::RTI_BOUND_BY_EDGE_COUNT;
			options.activeOnly = true;
			options.typeId = edgeTypeId_;

			RelationshipFrontierExecutor executor(dm_, threadPool_);
			executor.expandInPlace(frontier_, options, "varlength.frontier.expand");
			if (hasReverseTargetPruning()) {
				frontier_.filterFrontier([&](const RelationshipFrontierEntry &entry) {
					return canReachIndexedTargetWithinRemaining(entry.nodeId, entry.depth);
				});
				debug::PerfTrace::addValue("varlength.target_index.pruned_frontier_width",
										   static_cast<int64_t>(std::min<size_t>(
												   frontier_.size(),
												   static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
			}
			frontierEmitIndex_ = 0;
			frontierEmitsComplete_ = false;
		}

		void collectPendingEmits(std::vector<PendingEmit> &pending) {
			if (indexedTargetCandidates_ && indexedTargetCandidates_->emptyFromIndex) {
				inputExhausted_ = true;
				return;
			}
			if (frontierActive_ || canUseFrontierTraversal()) {
				collectPendingEmitsFromFrontier(pending);
				return;
			}

			const size_t batchThreshold = emitBatchThreshold();
			while (pending.size() < batchThreshold) {
				// If DFS stack is empty, pull next input record.
				if (dfsStack_.empty()) {
					if (!advanceInput()) {
						break;
					}
					auto sourceNode = currentInputRecord_.getNode(sourceVar_);
					if (!sourceNode) continue;

					const int64_t sourceId = sourceNode->getId();
					dfsStack_.push({sourceId, 0, {}, 0, false});
					visitedPath_.clear();
					visitedPath_.insert(sourceId);
				}

				while (!dfsStack_.empty() && pending.size() < DEFAULT_BATCH_SIZE) {
					if (queryContext_) queryContext_->checkGuard();

					auto &frame = dfsStack_.top();

					if (!frame.neighborsLoaded) {
						frame.neighborsLoaded = true;
						frame.neighbors = getNeighborIds(frame.nodeId);
						frame.neighborIdx = 0;
					}

					if (frame.depth >= minHops_ && frame.depth <= maxHops_ && !frame.emitted) {
						frame.emitted = true;
						if (frame.depth > 0 || minHops_ == 0) {
							if (auto emit = preparePendingEmit(frame.nodeId)) {
								emit->baseRecord = currentInputRecord_;
								pending.push_back(std::move(*emit));
							}
							if (pending.size() >= batchThreshold) {
								break;
							}
						}
					}

					if (frame.depth < maxHops_) {
						bool pushed = false;
						while (frame.neighborIdx < frame.neighbors.size()) {
							const int64_t neighborId = frame.neighbors[frame.neighborIdx++];
							if (visitedPath_.count(neighborId) != 0) continue;
							if (!canReachIndexedTargetWithinRemaining(neighborId, frame.depth + 1)) continue;

							Node neighborNode = dm_->getNode(neighborId);
							if (!neighborNode.isActive()) continue;

							visitedPath_.insert(neighborId);
							dfsStack_.push({neighborId, frame.depth + 1, {}, 0, false});
							pushed = true;
							break;
						}

						if (!pushed) {
							visitedPath_.erase(frame.nodeId);
							dfsStack_.pop();
						}
					} else {
						visitedPath_.erase(frame.nodeId);
						dfsStack_.pop();
					}
				}
			}
		}

			RecordBatch materializePendingEmits(const std::vector<PendingEmit> &pending) const {
				RecordBatch outputBatch;
				outputBatch.reserve(pending.size());
			const graph::concurrent::ParallelOperatorOptions options{
					.phase = "varlength.materialize_targets",
					.workloadKind = graph::concurrent::ParallelWorkloadKind::PWK_GENERAL,
					.estimatedItems = pending.size(),
					.estimatedBytes = pending.size() * (sizeof(PendingEmit) + Node::getTotalSize()),
					.minPartitions = 2,
					.minItems = PARALLEL_MATERIALIZE_THRESHOLD,
					.minItemsPerWorker = std::max<size_t>(1, PARALLEL_MATERIALIZE_THRESHOLD / 2)};
			(void) graph::concurrent::ParallelOperatorExecutor::runRangePartitions<MaterializePartitionState>(
					0,
					pending.size(),
					threadPool_,
					options,
					[&](const graph::concurrent::ParallelRangePartition &range, MaterializePartitionState &state) {
						state.records.reserve(range.size());
						for (size_t i = range.begin; i < range.end; ++i) {
							Node targetNode = pending[i].hasTargetNode
												  ? pending[i].targetNode
												  : dm_->getNode(pending[i].nodeId);
							if (!targetNode.isActive()) {
								continue;
							}
							if (!pending[i].hasTargetProperties) {
								auto props = dm_->getNodeProperties(pending[i].nodeId);
								targetNode.setProperties(std::move(props));
							}
							Record newRecord = pending[i].baseRecord;
							newRecord.setNode(targetVar_, std::move(targetNode));
							state.records.push_back(std::move(newRecord));
						}
					},
					[&](size_t, MaterializePartitionState &state) {
						for (auto &record: state.records) {
							outputBatch.push_back(std::move(record));
						}
					});
			return outputBatch;
		}

		[[nodiscard]] size_t emitBatchThreshold() const {
			return targetProperties_.empty() ? DEFAULT_BATCH_SIZE : size_t{1};
		}

		[[nodiscard]] bool hasTargetPredicates() const {
			return !targetLabelIds_.empty() || !targetProperties_.empty();
		}

		[[nodiscard]] std::optional<PendingEmit> preparePendingEmit(int64_t nodeId) const {
			PendingEmit pending;
			pending.nodeId = nodeId;
			if (!isIndexedTargetCandidate(nodeId)) {
				return std::nullopt;
			}
			if (!hasTargetPredicates()) {
				return pending;
			}

			pending.targetNode = dm_->getNode(nodeId);
			pending.hasTargetNode = true;
			if (!pending.targetNode.isActive()) {
				return std::nullopt;
			}
			for (const int64_t labelId: targetLabelIds_) {
				if (labelId <= 0 || !pending.targetNode.hasLabelId(labelId)) {
					return std::nullopt;
				}
			}
			if (!targetProperties_.empty()) {
				auto props = dm_->getNodeProperties(nodeId);
				if (!targetPropertyKernel_.has_value() || !targetPropertyKernel_->matchesMap(props)) {
					return std::nullopt;
				}
				pending.targetNode.setProperties(std::move(props));
				pending.hasTargetProperties = true;
			}
			return pending;
		}

		std::vector<int64_t> getNeighborIds(int64_t nodeId) {
			std::vector<int64_t> neighbors;
			if (edgeTypeId_ < 0 || !dm_) { // ZYX_COV_EXCL_LINE: DFS neighbor loading is reached after edge type and storage prerequisites pass.
				return neighbors;
			}

			auto cached = neighborCache_.find(nodeId);
			if (cached != neighborCache_.end()) {
				debug::PerfTrace::addValue("varlength.neighbor_cache.hits", 1);
				return cached->second;
			}
			debug::PerfTrace::addValue("varlength.neighbor_cache.misses", 1);

			debug::ScopedPerfTimer timer("varlength.load_neighbors");
			auto traversal = dm_->getRelationshipTraversal();
			if (!traversal) { // ZYX_COV_EXCL_LINE: initialized DataManager always owns a traversal service.
				return neighbors;
			}

			traversal::RelationshipTraversalOptions options;
			options.direction = traversal::RelationshipTraversal::directionFromString(direction_);
			options.integrity = traversal::RelationshipTraversalIntegrity::RTI_BOUND_BY_EDGE_COUNT;
			options.activeOnly = true;
			options.typeId = edgeTypeId_;
			(void) traversal->visitAdjacentEdgeRefs(
					nodeId,
					options,
					[&](const traversal::RelationshipEdgeRef &edgeRef) {
						neighbors.push_back(targetForEdgeRef(edgeRef, nodeId, options.direction));
						return true;
					});

			if (neighborCache_.size() < NEIGHBOR_CACHE_MAX_ENTRIES && // ZYX_COV_EXCL_LINE: cache saturation is a capacity guard, not traversal behavior.
				neighbors.size() <= NEIGHBOR_CACHE_MAX_NEIGHBORS_PER_ENTRY) {
				neighborCache_.emplace(nodeId, neighbors);
			}
			return neighbors;
		}

		[[nodiscard]] static int64_t targetForEdgeRef(
				const traversal::RelationshipEdgeRef &edgeRef,
				int64_t sourceId,
				traversal::RelationshipDirectionKind direction) {
			if (direction == traversal::RelationshipDirectionKind::RDK_IN) {
				return edgeRef.sourceNodeId;
			}
			if (direction == traversal::RelationshipDirectionKind::RDK_BOTH) {
				return edgeRef.sourceNodeId == sourceId ? edgeRef.targetNodeId : edgeRef.sourceNodeId;
			}
			return edgeRef.targetNodeId;
		}
	};

} // namespace graph::query::execution::operators
