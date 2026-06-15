/**
 * @file RelationshipFrontierExecutor.hpp
 * @brief Generic frontier expansion kernel for relationship traversal operators.
 *
 * Frontier execution keeps traversal state as lightweight node/path references
 * and delegates worker selection to ParallelOperatorExecutor. It is intentionally
 * query-shape agnostic: callers provide traversal options, while the executor
 * handles fanout estimation, partition-local expansion, and ordered merge.
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "graph/concurrent/ParallelOperatorExecutor.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Node.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/traversal/RelationshipTraversal.hpp"

namespace graph::query::execution {

	struct RelationshipFrontierEntry {
		static constexpr size_t kNoParent = std::numeric_limits<size_t>::max();

		int64_t nodeId = 0;
		int depth = 0;
		size_t sourceIndex = 0;
		size_t parentIndex = kNoParent;
	};

	struct RelationshipFrontierPath {
		Record baseRecord;
		int64_t nodeId = 0;
		int depth = 0;
		std::vector<int64_t> visitedNodeIds;
	};

	class RelationshipFrontierState {
	public:
		void clear() {
			sourceRecords_.clear();
			entries_.clear();
			frontierIndices_.clear();
		}

		void reserveSources(size_t count) { sourceRecords_.reserve(count); }

		void addSource(Record record, int64_t nodeId) {
			const size_t sourceIndex = sourceRecords_.size();
			sourceRecords_.push_back(std::move(record));
			entries_.push_back({.nodeId = nodeId,
								.depth = 0,
								.sourceIndex = sourceIndex,
								.parentIndex = RelationshipFrontierEntry::kNoParent});
			frontierIndices_.push_back(entries_.size() - 1);
		}

		void addPath(Record record, int64_t nodeId, int depth, const std::vector<int64_t> &visitedNodeIds) {
			const size_t sourceIndex = sourceRecords_.size();
			sourceRecords_.push_back(std::move(record));

			size_t parentIndex = RelationshipFrontierEntry::kNoParent;
			if (!visitedNodeIds.empty()) {
				const int firstDepth = std::max(0, depth - static_cast<int>(visitedNodeIds.size()) + 1);
				for (size_t i = 0; i < visitedNodeIds.size(); ++i) {
					const int entryDepth = firstDepth + static_cast<int>(i);
					entries_.push_back({.nodeId = visitedNodeIds[i],
										.depth = entryDepth,
										.sourceIndex = sourceIndex,
										.parentIndex = parentIndex});
					parentIndex = entries_.size() - 1;
				}
				if (entries_[parentIndex].nodeId != nodeId) {
					entries_.push_back({.nodeId = nodeId,
										.depth = depth,
										.sourceIndex = sourceIndex,
										.parentIndex = parentIndex});
					parentIndex = entries_.size() - 1;
				}
			} else {
				entries_.push_back({.nodeId = nodeId,
									.depth = depth,
									.sourceIndex = sourceIndex,
									.parentIndex = RelationshipFrontierEntry::kNoParent});
				parentIndex = entries_.size() - 1;
			}
			frontierIndices_.push_back(parentIndex);
		}

		[[nodiscard]] bool empty() const noexcept { return frontierIndices_.empty(); }
		[[nodiscard]] size_t size() const noexcept { return frontierIndices_.size(); }
		[[nodiscard]] size_t entryCount() const noexcept { return entries_.size(); }
		[[nodiscard]] size_t sourceCount() const noexcept { return sourceRecords_.size(); }

		[[nodiscard]] int currentDepth() const noexcept {
			return frontierIndices_.empty() ? 0 : entries_[frontierIndices_.front()].depth;
		}

		[[nodiscard]] const RelationshipFrontierEntry &entry(size_t entryIndex) const {
			return entries_[entryIndex];
		}

		[[nodiscard]] const RelationshipFrontierEntry &frontierEntry(size_t frontierOffset) const {
			return entries_[frontierIndices_[frontierOffset]];
		}

		[[nodiscard]] size_t frontierEntryIndex(size_t frontierOffset) const {
			return frontierIndices_[frontierOffset];
		}

		[[nodiscard]] const Record &sourceRecord(size_t sourceIndex) const {
			return sourceRecords_[sourceIndex];
		}

		void replaceFrontier(std::vector<RelationshipFrontierEntry> nextEntries) {
			frontierIndices_.clear();
			frontierIndices_.reserve(nextEntries.size());
			entries_.reserve(entries_.size() + nextEntries.size());
			for (auto &entry: nextEntries) {
				entries_.push_back(entry);
				frontierIndices_.push_back(entries_.size() - 1);
			}
		}

		template<typename Predicate>
		void filterFrontier(Predicate &&predicate) {
			std::vector<size_t> filtered;
			filtered.reserve(frontierIndices_.size());
			for (const size_t entryIndex: frontierIndices_) {
				if (predicate(entries_[entryIndex])) {
					filtered.push_back(entryIndex);
				}
			}
			frontierIndices_ = std::move(filtered);
		}

		[[nodiscard]] bool pathContains(size_t entryIndex, int64_t nodeId) const {
			size_t current = entryIndex;
			while (current != RelationshipFrontierEntry::kNoParent && current < entries_.size()) {
				const auto &entry = entries_[current];
				if (entry.nodeId == nodeId) {
					return true;
				}
				current = entry.parentIndex;
			}
			return false;
		}

		[[nodiscard]] RelationshipFrontierPath materializePath(size_t frontierOffset) const {
			const size_t entryIndex = frontierEntryIndex(frontierOffset);
			const auto &entry = entries_[entryIndex];
			RelationshipFrontierPath path;
			path.baseRecord = sourceRecords_[entry.sourceIndex];
			path.nodeId = entry.nodeId;
			path.depth = entry.depth;

			for (size_t current = entryIndex;
				 current != RelationshipFrontierEntry::kNoParent && current < entries_.size();
				 current = entries_[current].parentIndex) {
				path.visitedNodeIds.push_back(entries_[current].nodeId);
			}
			std::reverse(path.visitedNodeIds.begin(), path.visitedNodeIds.end());
			return path;
		}

	private:
		std::vector<Record> sourceRecords_;
		std::vector<RelationshipFrontierEntry> entries_;
		std::vector<size_t> frontierIndices_;
	};

	class RelationshipFrontierExecutor {
	public:
		RelationshipFrontierExecutor(std::shared_ptr<storage::DataManager> dm,
									 concurrent::ThreadPool *threadPool) :
			dm_(std::move(dm)), threadPool_(threadPool) {}

		void expandInPlace(
				RelationshipFrontierState &state,
				const traversal::RelationshipTraversalOptions &options,
				std::string_view phase = "relationship_frontier.expand_paths") const {
			if (!dm_ || state.empty()) {
				state.replaceFrontier({});
				return;
			}
			auto traversal = dm_->getRelationshipTraversal();
			if (!traversal) {
				state.replaceFrontier({});
				return;
			}

			if (state.size() == 1 && concurrent::hasParallelWorkers(threadPool_)) {
				state.replaceFrontier(expandSingleEntry(
						state, state.frontierEntryIndex(0), *traversal, options, phase));
				return;
			}

			const size_t estimatedEdges = estimateExpansionItems(state, traversal, options);
			emitFrontierEstimateProfile(phase, state, estimatedEdges);

			std::vector<RelationshipFrontierEntry> nextEntries;
			const concurrent::ParallelOperatorOptions parallelOptions{
					.phase = phase,
					.workloadKind = concurrent::ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL,
					.estimatedItems = estimatedEdges,
					.estimatedBytes = estimateFrontierBytes(estimatedEdges),
					.minPartitions = 2,
					.minItems = kMinParallelFrontierEdges,
					.estimatedStateBytesPerItem = kEstimatedFrontierEntryBytes,
					.frontierWidth = state.size(),
					.traversalDepth = static_cast<size_t>(std::max(0, state.currentDepth()))};

			(void) concurrent::ParallelOperatorExecutor::runRangePartitions<ExpansionPartitionState>(
					0,
					state.size(),
					threadPool_,
					parallelOptions,
					[&](const concurrent::ParallelRangePartition &range, ExpansionPartitionState &partitionState) {
						const size_t reservePerPath = estimatedEdges == 0 ? size_t{1} :
								std::max<size_t>(1, estimatedEdges / state.size());
						const size_t rangeSize = range.size();
						const size_t reserveCount = rangeSize == 0 ||
													 reservePerPath > std::numeric_limits<size_t>::max() / rangeSize ?
								range.size() :
								rangeSize * reservePerPath;
						partitionState.next.reserve(reserveCount);
						for (size_t i = range.begin; i < range.end; ++i) {
							expandOneEntry(state, state.frontierEntryIndex(i), *traversal, options, partitionState.next);
						}
					},
					[&](size_t, ExpansionPartitionState &partitionState) {
						nextEntries.insert(nextEntries.end(),
										   std::make_move_iterator(partitionState.next.begin()),
										   std::make_move_iterator(partitionState.next.end()));
					});
			state.replaceFrontier(std::move(nextEntries));
		}

		[[nodiscard]] std::vector<RelationshipFrontierPath> expand(
				const std::vector<RelationshipFrontierPath> &frontier,
				const traversal::RelationshipTraversalOptions &options,
				std::string_view phase = "relationship_frontier.expand_paths") const {
			RelationshipFrontierState state;
			state.reserveSources(frontier.size());
			for (const auto &path: frontier) {
				state.addPath(path.baseRecord, path.nodeId, path.depth, path.visitedNodeIds);
			}
			expandInPlace(state, options, phase);

			std::vector<RelationshipFrontierPath> nextFrontier;
			nextFrontier.reserve(state.size());
			for (size_t i = 0; i < state.size(); ++i) {
				nextFrontier.push_back(state.materializePath(i));
			}
			return nextFrontier;
		}

	private:
		struct ExpansionPartitionState {
			std::vector<RelationshipFrontierEntry> next;
		};

		struct EdgeRefPartitionState {
			std::vector<RelationshipFrontierEntry> next;
		};

		static constexpr size_t kFrontierSamplePaths = 16;
		static constexpr size_t kEstimatedFrontierEntryBytes = sizeof(RelationshipFrontierEntry);
		static constexpr size_t kMinParallelFrontierEdges = 2048;

		[[nodiscard]] size_t estimateExpansionItems(
				const RelationshipFrontierState &state,
				const std::shared_ptr<traversal::RelationshipTraversal> &traversal,
				const traversal::RelationshipTraversalOptions &options) const {
			if (!concurrent::hasParallelWorkers(threadPool_) || state.size() < 2) {
				return state.size();
			}

			const size_t sampleCount = std::min(kFrontierSamplePaths, state.size());
			size_t sampledEdges = 0;
			for (size_t i = 0; i < sampleCount; ++i) {
				sampledEdges += traversal->countAdjacentEdgeRefs(state.frontierEntry(i).nodeId, options);
			}
			if (sampleCount == 0 || sampledEdges == 0) {
				return state.size();
			}
			return std::max(state.size(), (sampledEdges * state.size()) / sampleCount);
		}

		[[nodiscard]] static size_t estimateFrontierBytes(size_t estimatedItems) {
			if (estimatedItems > std::numeric_limits<size_t>::max() / kEstimatedFrontierEntryBytes) {
				return std::numeric_limits<size_t>::max();
			}
			return estimatedItems * kEstimatedFrontierEntryBytes;
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

		static void emitIntValue(std::string_view phase, std::string_view suffix, size_t value) {
			if (!debug::PerfTrace::isEnabled()) {
				return;
			}
			const int64_t clamped = static_cast<int64_t>(
					std::min<size_t>(value, static_cast<size_t>(std::numeric_limits<int64_t>::max())));
			if (!phase.empty()) {
				std::string phaseName;
				phaseName.reserve(phase.size() + suffix.size());
				phaseName.append(phase.data(), phase.size());
				phaseName.append(suffix.data(), suffix.size());
				debug::PerfTrace::addValue(phaseName, clamped);
			}
		}

		static void emitFrontierEstimateProfile(
				std::string_view phase,
				const RelationshipFrontierState &state,
				size_t estimatedEdges) {
			debug::PerfTrace::addValue("relationship_frontier.estimated_edges", static_cast<int64_t>(
					std::min<size_t>(estimatedEdges, static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
			emitIntValue(phase, ".estimated_edges", estimatedEdges);
			emitIntValue(phase, ".frontier_width", state.size());
			emitIntValue(phase, ".frontier_entry_bytes", kEstimatedFrontierEntryBytes);
			emitIntValue(phase, ".frontier_depth", static_cast<size_t>(std::max(0, state.currentDepth())));
		}

		[[nodiscard]] std::optional<RelationshipFrontierEntry> makeNextEntry(
				const RelationshipFrontierState &state,
				size_t entryIndex,
				const traversal::RelationshipEdgeRef &edgeRef,
				traversal::RelationshipDirectionKind direction) const {
			const auto &entry = state.entry(entryIndex);
			const int64_t targetId = targetForEdgeRef(edgeRef, entry.nodeId, direction);
			if (state.pathContains(entryIndex, targetId)) {
				return std::nullopt;
			}
			const Node target = dm_->getNode(targetId);
			if (!target.isActive()) {
				return std::nullopt;
			}

			return RelationshipFrontierEntry{.nodeId = targetId,
											 .depth = entry.depth + 1,
											 .sourceIndex = entry.sourceIndex,
											 .parentIndex = entryIndex};
		}

		[[nodiscard]] std::vector<traversal::RelationshipEdgeRef> collectExpandableEdgeRefs(
				const RelationshipFrontierState &state,
				size_t entryIndex,
				const traversal::RelationshipTraversal &traversal,
				const traversal::RelationshipTraversalOptions &options) const {
			const auto &entry = state.entry(entryIndex);
			std::vector<traversal::RelationshipEdgeRef> edgeRefs;
			(void) traversal.visitAdjacentEdgeRefs(
					entry.nodeId,
					options,
					[&](const traversal::RelationshipEdgeRef &edgeRef) {
						const int64_t targetId = targetForEdgeRef(edgeRef, entry.nodeId, options.direction);
						if (!state.pathContains(entryIndex, targetId)) {
							edgeRefs.push_back(edgeRef);
						}
						return true;
					});
			return edgeRefs;
		}

		[[nodiscard]] std::vector<RelationshipFrontierEntry> expandSingleEntry(
				const RelationshipFrontierState &state,
				size_t entryIndex,
				const traversal::RelationshipTraversal &traversal,
				const traversal::RelationshipTraversalOptions &options,
				std::string_view phase) const {
			std::vector<RelationshipFrontierEntry> nextFrontier;

			const auto &entry = state.entry(entryIndex);
			const size_t estimatedEdges = traversal.countAdjacentEdgeRefs(entry.nodeId, options);
			emitFrontierEstimateProfile(phase, state, estimatedEdges);
			if (estimatedEdges == 0) {
				return nextFrontier;
			}
			if (estimatedEdges < kMinParallelFrontierEdges) {
				nextFrontier.reserve(estimatedEdges);
				expandOneEntry(state, entryIndex, traversal, options, nextFrontier);
				return nextFrontier;
			}

			auto edgeRefs = collectExpandableEdgeRefs(state, entryIndex, traversal, options);
			if (edgeRefs.empty()) {
				return nextFrontier;
			}

			const concurrent::ParallelOperatorOptions parallelOptions{
					.phase = phase,
					.workloadKind = concurrent::ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL,
					.estimatedItems = edgeRefs.size(),
					.estimatedBytes = estimateFrontierBytes(edgeRefs.size()),
					.minPartitions = 2,
					.minItems = kMinParallelFrontierEdges,
					.estimatedStateBytesPerItem = kEstimatedFrontierEntryBytes,
					.frontierWidth = state.size(),
					.traversalDepth = static_cast<size_t>(std::max(0, state.currentDepth()))};
			(void) concurrent::ParallelOperatorExecutor::runRangePartitions<EdgeRefPartitionState>(
					0,
					edgeRefs.size(),
					threadPool_,
					parallelOptions,
					[&](const concurrent::ParallelRangePartition &range, EdgeRefPartitionState &partitionState) {
						partitionState.next.reserve(range.size());
						for (size_t i = range.begin; i < range.end; ++i) {
							if (auto next = makeNextEntry(state, entryIndex, edgeRefs[i], options.direction)) {
								partitionState.next.push_back(std::move(*next));
							}
						}
					},
					[&](size_t, EdgeRefPartitionState &partitionState) {
						nextFrontier.insert(nextFrontier.end(),
										   std::make_move_iterator(partitionState.next.begin()),
										   std::make_move_iterator(partitionState.next.end()));
					});
			return nextFrontier;
		}

		void expandOneEntry(const RelationshipFrontierState &state,
						   size_t entryIndex,
						   const traversal::RelationshipTraversal &traversal,
						   const traversal::RelationshipTraversalOptions &options,
						   std::vector<RelationshipFrontierEntry> &out) const {
			const auto &entry = state.entry(entryIndex);
			(void) traversal.visitAdjacentEdgeRefs(
					entry.nodeId,
					options,
					[&](const traversal::RelationshipEdgeRef &edgeRef) {
						if (auto next = makeNextEntry(state, entryIndex, edgeRef, options.direction)) {
							out.push_back(std::move(*next));
						}
						return true;
					});
		}

		std::shared_ptr<storage::DataManager> dm_;
		concurrent::ThreadPool *threadPool_ = nullptr;
	};

} // namespace graph::query::execution
