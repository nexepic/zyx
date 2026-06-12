/**
 * @file RelationshipTraversal.cpp
 * @author Nexepic
 * @date 2025/6/11
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

#include "graph/traversal/RelationshipTraversal.hpp"
#include <algorithm>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/EdgeManager.hpp"

namespace graph::traversal {
	namespace {
		constexpr size_t kNoBatchEdgeIndex = std::numeric_limits<size_t>::max();

		struct LinkListBuildState {
			int64_t currentHeadId = 0;
			size_t currentHeadIndex = kNoBatchEdgeIndex;
			bool nodeLoaded = false;
		};

		struct NodeLinkUpdate {
			Node before;
			Node after;
		};

		struct EdgeLinkUpdate {
			Edge before;
			Edge after;
		};

		template<typename T>
		class HashIdMap {
		public:
			explicit HashIdMap(size_t expectedSize = 0) {
				map_.max_load_factor(0.7F);
				map_.reserve(expectedSize);
			}

			std::pair<T &, bool> getOrCreate(int64_t id) {
				auto [it, inserted] = map_.try_emplace(id);
				return {it->second, inserted};
			}

			template<typename Visitor>
			void forEach(Visitor &&visitor) {
				for (auto &[id, value]: map_) {
					visitor(id, value);
				}
			}

			[[nodiscard]] size_t size() const { return map_.size(); }

		private:
			std::unordered_map<int64_t, T> map_;
		};

		template<typename T>
		class DenseIdMap {
		public:
			DenseIdMap(int64_t minId, int64_t maxId, size_t expectedSize) : baseId_(minId) {
				const auto span =
						static_cast<size_t>(static_cast<uint64_t>(maxId) - static_cast<uint64_t>(minId) + 1U);
				slots_.assign(span, kMissingSlot);
				constexpr size_t kMaxInitialValueReserve = 65'536;
				const size_t reserveCount = (std::min)((std::min)(expectedSize, span), kMaxInitialValueReserve);
				keys_.reserve(reserveCount);
				values_.reserve(reserveCount);
			}

			std::pair<T &, bool> getOrCreate(int64_t id) {
				const auto slot = static_cast<size_t>(id - baseId_);
				int32_t valueIndex = slots_[slot];
				if (valueIndex != kMissingSlot) {
					return {values_[static_cast<size_t>(valueIndex)], false};
				}

				valueIndex = static_cast<int32_t>(values_.size());
				slots_[slot] = valueIndex;
				keys_.push_back(id);
				values_.emplace_back();
				return {values_.back(), true};
			}

			template<typename Visitor>
			void forEach(Visitor &&visitor) {
				for (size_t index = 0; index < values_.size(); ++index) {
					visitor(keys_[index], values_[index]);
				}
			}

			[[nodiscard]] size_t size() const { return values_.size(); }

		private:
			static constexpr int32_t kMissingSlot = -1;

			int64_t baseId_ = 0;
			std::vector<int32_t> slots_;
			std::vector<int64_t> keys_;
			std::vector<T> values_;
		};

		struct DenseNodeIdRange {
			bool enabled = false;
			int64_t minId = 0;
			int64_t maxId = 0;
		};

		DenseNodeIdRange chooseDenseNodeIdRange(const std::vector<Edge> &edges) {
			if (edges.size() < 1024) {
				return {};
			}

			int64_t minId = std::numeric_limits<int64_t>::max();
			int64_t maxId = 0;
			for (const auto &edge: edges) {
				const int64_t sourceId = edge.getSourceNodeId();
				const int64_t targetId = edge.getTargetNodeId();
				if (sourceId <= 0 || targetId <= 0) {
					return {};
				}
				if (sourceId > 0) {
					minId = (std::min)(minId, sourceId);
					maxId = (std::max)(maxId, sourceId);
				}
				if (targetId > 0) {
					minId = (std::min)(minId, targetId);
					maxId = (std::max)(maxId, targetId);
				}
			}
			if (minId == std::numeric_limits<int64_t>::max() || maxId < minId) {
				return {};
			}

			const auto span = static_cast<uint64_t>(maxId) - static_cast<uint64_t>(minId) + 1U;
			const auto edgeCount = static_cast<uint64_t>(edges.size());
			constexpr uint64_t kMaxDenseSlots = 1'000'000;
			const bool denseEnough = span <= edgeCount * 2;
			if (span > kMaxDenseSlots || !denseEnough) {
				return {};
			}
			return {true, minId, maxId};
		}

		RelationshipEdgeRef makeEdgeRef(const Edge &edge) {
			return RelationshipEdgeRef{
					.edgeId = edge.getId(),
					.sourceNodeId = edge.getSourceNodeId(),
					.targetNodeId = edge.getTargetNodeId(),
					.nextOutEdgeId = edge.getNextOutEdgeId(),
					.nextInEdgeId = edge.getNextInEdgeId(),
					.typeId = edge.getTypeId(),
					.active = edge.isActive()};
		}

		int64_t nextEdgeIdForDirection(const RelationshipEdgeRef &edgeRef, RelationshipDirectionKind direction) {
			return direction == RelationshipDirectionKind::RDK_IN ? edgeRef.nextInEdgeId : edgeRef.nextOutEdgeId;
		}

		const char *directionName(RelationshipDirectionKind direction) {
			switch (direction) {
				case RelationshipDirectionKind::RDK_OUT:
					return "outgoing";
				case RelationshipDirectionKind::RDK_IN:
					return "incoming";
				case RelationshipDirectionKind::RDK_BOTH:
					return "connected";
			}
			return "connected";
		}

		bool acceptsEdgeRef(const RelationshipEdgeRef &edgeRef, const RelationshipTraversalOptions &options) {
			if (options.activeOnly && !edgeRef.active) {
				return false;
			}
			return options.typeId == 0 || edgeRef.typeId == options.typeId;
		}

		size_t traversalBound(const storage::DataManager &dataManager) {
			const auto allocator = dataManager.getIdAllocator(EntityType::Edge);
			if (!allocator) {
				return 1;
			}
			const int64_t maxId = allocator->getCurrentMaxId();
			if (maxId <= 0) {
				return 1;
			}
			return static_cast<size_t>(maxId) + 1U;
		}

		template<typename Visitor>
		size_t visitAdjacentEdgeRefsImpl(
				const std::weak_ptr<storage::DataManager> &dataManagerWeak,
				int64_t nodeId,
				const RelationshipTraversalOptions &options,
				Visitor &&visitor) {
			const auto dataManager = dataManagerWeak.lock();
			if (!dataManager) {
				return 0;
			}

			const Node node = dataManager->getNode(nodeId);
			if (node.getId() == 0 || !node.isActive()) {
				return 0;
			}

			bool keepGoing = true;
			const auto visitOneDirection = [&](RelationshipDirectionKind direction, int64_t headEdgeId) {
				size_t visitedCount = 0;
				int64_t currentEdgeId = headEdgeId;
				std::unordered_set<int64_t> visitedEdgeIds;
				size_t remaining = traversalBound(*dataManager);

				while (currentEdgeId != 0 && keepGoing) {
					if (options.integrity == RelationshipTraversalIntegrity::RTI_DETECT_CYCLES) {
						if (visitedEdgeIds.contains(currentEdgeId)) {
							throw std::runtime_error("Cycle detected in " + std::string(directionName(direction)) +
													 " edges linked-list for node " + std::to_string(nodeId));
						}
						visitedEdgeIds.insert(currentEdgeId);
					} else {
						if (remaining == 0) {
							throw std::runtime_error("Cycle detected in " + std::string(directionName(direction)) +
													 " edges linked-list for node " + std::to_string(nodeId));
						}
						--remaining;
					}

					const Edge edge = dataManager->getEdgeForAdjacency(currentEdgeId);
					const auto edgeRef = makeEdgeRef(edge);
					currentEdgeId = nextEdgeIdForDirection(edgeRef, direction);
					if (edgeRef.edgeId == 0) {
						break;
					}
					if (acceptsEdgeRef(edgeRef, options)) {
						++visitedCount;
						keepGoing = visitor(edgeRef);
					}
				}
				return visitedCount;
			};

			size_t visitedCount = 0;
			if (options.direction == RelationshipDirectionKind::RDK_OUT ||
				options.direction == RelationshipDirectionKind::RDK_BOTH) {
				visitedCount += visitOneDirection(RelationshipDirectionKind::RDK_OUT, node.getFirstOutEdgeId());
			}
			if (keepGoing &&
				(options.direction == RelationshipDirectionKind::RDK_IN ||
				 options.direction == RelationshipDirectionKind::RDK_BOTH)) {
				visitedCount += visitOneDirection(RelationshipDirectionKind::RDK_IN, node.getFirstInEdgeId());
			}
			return visitedCount;
		}

		template<typename StateMap, typename NodeUpdateMap, typename OldHeadMap>
		RelationshipBatchLinkUpdates buildBatchLinksWithMaps(
				storage::DataManager &dataManager,
				std::vector<Edge> &edges,
				StateMap &outStates,
				StateMap &inStates,
				NodeUpdateMap &nodeUpdates,
				OldHeadMap &oldHeadUpdates) {
			RelationshipBatchLinkUpdates updates;

			auto getNodeForUpdate = [&](int64_t nodeId) -> std::optional<std::reference_wrapper<Node>> {
				auto [update, inserted] = nodeUpdates.getOrCreate(nodeId);
				if (inserted) {
					update.before = dataManager.getNode(nodeId);
					update.after = update.before;
				}
				if (update.before.getId() == 0) {
					return std::nullopt;
				}
				return update.after;
			};

			auto getOldHeadForUpdate = [&](int64_t edgeId) -> std::optional<std::reference_wrapper<Edge>> {
				if (edgeId == 0) {
					return std::nullopt;
				}
				auto [update, inserted] = oldHeadUpdates.getOrCreate(edgeId);
				if (inserted) {
					update.before = dataManager.getEdge(edgeId);
					update.after = update.before;
				}
				if (update.before.getId() == 0) {
					return std::nullopt;
				}
				return update.after;
			};

			auto appendToOutgoingList = [&](size_t index) {
				Edge &edge = edges[index];
				auto nodeRef = getNodeForUpdate(edge.getSourceNodeId());
				auto [state, inserted] = outStates.getOrCreate(edge.getSourceNodeId());
				(void) inserted;
				if (!state.nodeLoaded) {
					state.currentHeadId = nodeRef.has_value() ? nodeRef->get().getFirstOutEdgeId() : int64_t{0};
					state.currentHeadIndex = kNoBatchEdgeIndex;
					state.nodeLoaded = true;
				}

				edge.setPrevOutEdgeId(0);
				edge.setNextOutEdgeId(state.currentHeadId);
				if (state.currentHeadIndex != kNoBatchEdgeIndex) {
					edges[state.currentHeadIndex].setPrevOutEdgeId(edge.getId());
				} else if (auto oldHead = getOldHeadForUpdate(state.currentHeadId)) {
					oldHead->get().setPrevOutEdgeId(edge.getId());
				}

				state.currentHeadId = edge.getId();
				state.currentHeadIndex = index;
			};

			auto appendToIncomingList = [&](size_t index) {
				Edge &edge = edges[index];
				auto nodeRef = getNodeForUpdate(edge.getTargetNodeId());
				auto [state, inserted] = inStates.getOrCreate(edge.getTargetNodeId());
				(void) inserted;
				if (!state.nodeLoaded) {
					state.currentHeadId = nodeRef.has_value() ? nodeRef->get().getFirstInEdgeId() : int64_t{0};
					state.currentHeadIndex = kNoBatchEdgeIndex;
					state.nodeLoaded = true;
				}

				edge.setPrevInEdgeId(0);
				edge.setNextInEdgeId(state.currentHeadId);
				if (state.currentHeadIndex != kNoBatchEdgeIndex) {
					edges[state.currentHeadIndex].setPrevInEdgeId(edge.getId());
				} else if (auto oldHead = getOldHeadForUpdate(state.currentHeadId)) {
					oldHead->get().setPrevInEdgeId(edge.getId());
				}

				state.currentHeadId = edge.getId();
				state.currentHeadIndex = index;
			};

			{
				debug::ScopedPerfTimer timer("relationship_traversal.link_edges_batch.prepare");
				for (size_t index = 0; index < edges.size(); ++index) {
					if (edges[index].getId() == 0) {
						continue;
					}
					appendToOutgoingList(index);
					appendToIncomingList(index);
				}

				outStates.forEach([&](int64_t nodeId, LinkListBuildState &state) {
					if (auto node = getNodeForUpdate(nodeId)) {
						node->get().setFirstOutEdgeId(state.currentHeadId);
					}
				});
				inStates.forEach([&](int64_t nodeId, LinkListBuildState &state) {
					if (auto node = getNodeForUpdate(nodeId)) {
						node->get().setFirstInEdgeId(state.currentHeadId);
					}
				});

				updates.nodes.reserve(nodeUpdates.size());
				updates.oldNodes.reserve(nodeUpdates.size());
				nodeUpdates.forEach([&](int64_t, NodeLinkUpdate &update) {
					if (update.before.getId() != 0) {
						updates.oldNodes.push_back(std::move(update.before));
						updates.nodes.push_back(std::move(update.after));
					}
				});

				updates.oldHeadEdges.reserve(oldHeadUpdates.size());
				updates.oldHeadEdgesBefore.reserve(oldHeadUpdates.size());
				oldHeadUpdates.forEach([&](int64_t, EdgeLinkUpdate &update) {
					if (update.before.getId() != 0) {
						updates.oldHeadEdgesBefore.push_back(std::move(update.before));
						updates.oldHeadEdges.push_back(std::move(update.after));
					}
				});
			}

			return updates;
		}
	} // namespace

	RelationshipTraversal::RelationshipTraversal(const std::shared_ptr<storage::DataManager> &dataManager) :
		dataManager_(dataManager) {}

	std::vector<Edge> RelationshipTraversal::getOutgoingEdges(int64_t nodeId) const {
		std::vector<Edge> outEdges;
		visitOutgoingEdges(nodeId, [&](const Edge &edge) {
			outEdges.push_back(edge);
			return true;
		});
		return outEdges;
	}

	size_t RelationshipTraversal::visitOutgoingEdges(int64_t nodeId, const EdgeVisitor &visitor) const {
		size_t visitedCount = 0;
		const auto dataManager = dataManager_.lock();
		if (!dataManager || !visitor) {
			return visitedCount;
		}

		const Node node = dataManager->getNode(nodeId);
		int64_t currentEdgeId = node.getFirstOutEdgeId();

		// Keep track of visited edge IDs to detect cycles in the linked list.
		std::unordered_set<int64_t> visitedEdgeIds;

		while (currentEdgeId != 0) {
			// Cycle detected in the edge linked-list. Abort traversal.
			if (visitedEdgeIds.contains(currentEdgeId)) {
				throw std::runtime_error("Cycle detected in outgoing edges linked-list for node " +
										 std::to_string(nodeId));
			}
			visitedEdgeIds.insert(currentEdgeId);

			Edge edge = dataManager->getEdgeForAdjacency(currentEdgeId);
			const int64_t nextEdgeId = edge.getNextOutEdgeId();
			if (edge.isActive()) {
				++visitedCount;
				if (!visitor(edge)) {
					break;
				}
			}
			currentEdgeId = nextEdgeId;
		}
		return visitedCount;
	}

	std::vector<Edge> RelationshipTraversal::getIncomingEdges(int64_t nodeId) const {
		std::vector<Edge> inEdges;
		visitIncomingEdges(nodeId, [&](const Edge &edge) {
			inEdges.push_back(edge);
			return true;
		});
		return inEdges;
	}

	size_t RelationshipTraversal::visitIncomingEdges(int64_t nodeId, const EdgeVisitor &visitor) const {
		size_t visitedCount = 0;
		const auto dataManager = dataManager_.lock();
		if (!dataManager || !visitor) {
			return visitedCount;
		}

		const Node node = dataManager->getNode(nodeId);
		int64_t currentEdgeId = node.getFirstInEdgeId();

		// Keep track of visited edge IDs to detect cycles in the linked list.
		std::unordered_set<int64_t> visitedEdgeIds;

		while (currentEdgeId != 0) {
			// Cycle detected in the edge linked-list. Abort traversal.
			if (visitedEdgeIds.contains(currentEdgeId)) {
				throw std::runtime_error("Cycle detected in incoming edges linked-list for node " +
										 std::to_string(nodeId));
			}
			visitedEdgeIds.insert(currentEdgeId);

			Edge edge = dataManager->getEdgeForAdjacency(currentEdgeId);
			const int64_t nextEdgeId = edge.getNextInEdgeId();
			if (edge.isActive()) {
				++visitedCount;
				if (!visitor(edge)) {
					break;
				}
			}
			currentEdgeId = nextEdgeId;
		}
		return visitedCount;
	}

	std::vector<Edge> RelationshipTraversal::getAllConnectedEdges(int64_t nodeId) const {
		std::vector<Edge> outEdges = getOutgoingEdges(nodeId);
		std::vector<Edge> inEdges = getIncomingEdges(nodeId);

		// Combine the two vectors
		outEdges.insert(outEdges.end(), inEdges.begin(), inEdges.end());
		return outEdges;
	}

	size_t RelationshipTraversal::visitAllConnectedEdges(int64_t nodeId, const EdgeVisitor &visitor) const {
		if (!visitor) {
			return 0;
		}
		bool keepGoing = true;
		size_t visitedCount = visitOutgoingEdges(nodeId, [&](const Edge &edge) {
			keepGoing = visitor(edge);
			return keepGoing;
		});
		if (keepGoing) {
			visitedCount += visitIncomingEdges(nodeId, visitor);
		}
		return visitedCount;
	}

	RelationshipDirectionKind RelationshipTraversal::directionFromString(const std::string &direction) {
		if (direction == "out") {
			return RelationshipDirectionKind::RDK_OUT;
		}
		if (direction == "in") {
			return RelationshipDirectionKind::RDK_IN;
		}
		return RelationshipDirectionKind::RDK_BOTH;
	}

	size_t RelationshipTraversal::visitAdjacentEdgeRefs(
			int64_t nodeId,
			const RelationshipTraversalOptions &options,
			const RelationshipEdgeRefVisitor &visitor) const {
		if (!visitor) {
			return 0;
		}
		return visitAdjacentEdgeRefsImpl(dataManager_, nodeId, options, visitor);
	}

	size_t RelationshipTraversal::countAdjacentEdgeRefs(int64_t nodeId, const RelationshipTraversalOptions &options) const {
		return visitAdjacentEdgeRefsImpl(dataManager_, nodeId, options, [](const RelationshipEdgeRef &) {
			return true;
		});
	}

	std::vector<Node> RelationshipTraversal::getConnectedTargetNodes(int64_t nodeId) const {
		std::vector<Node> targetNodes;
		std::vector<Edge> outEdges = getOutgoingEdges(nodeId);

		targetNodes.reserve(outEdges.size());
		const auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return targetNodes;
		}

		for (const auto &edge: outEdges) {
			targetNodes.push_back(dataManager->getNode(edge.getTargetNodeId()));
		}
		return targetNodes;
	}

	std::vector<Node> RelationshipTraversal::getConnectedSourceNodes(int64_t nodeId) const {
		std::vector<Node> sourceNodes;
		std::vector<Edge> inEdges = getIncomingEdges(nodeId);

		sourceNodes.reserve(inEdges.size());
		const auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return sourceNodes;
		}

		for (const auto &edge: inEdges) {
			sourceNodes.push_back(dataManager->getNode(edge.getSourceNodeId()));
		}
		return sourceNodes;
	}

	std::vector<Node> RelationshipTraversal::getAllConnectedNodes(int64_t nodeId) const {
		std::vector<Node> connectedNodes;

		const auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return connectedNodes;
		}

		std::unordered_set<int64_t> nodeIds;
		for (const auto &edge: getOutgoingEdges(nodeId)) {
			int64_t targetId = edge.getTargetNodeId();
			if (nodeIds.insert(targetId).second) {
				connectedNodes.push_back(dataManager->getNode(targetId));
			}
		}

		for (const auto &edge: getIncomingEdges(nodeId)) {
			int64_t sourceId = edge.getSourceNodeId();
			if (nodeIds.insert(sourceId).second) {
				connectedNodes.push_back(dataManager->getNode(sourceId));
			}
		}
		return connectedNodes;
	}

	void RelationshipTraversal::linkEdge(Edge &edge) const {
		int64_t edgeId = edge.getId();
		int64_t sourceNodeId = edge.getSourceNodeId();
		int64_t targetNodeId = edge.getTargetNodeId();

		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return;
		}

		Node sourceNode = dataManager->getNode(sourceNodeId);
		int64_t firstOutEdgeId = sourceNode.getFirstOutEdgeId();

		if (firstOutEdgeId == 0) {
			sourceNode.setFirstOutEdgeId(edgeId);
			dataManager->updateNode(sourceNode);
		} else {
			Edge firstOutEdge = dataManager->getEdge(firstOutEdgeId);
			edge.setNextOutEdgeId(firstOutEdgeId);
			firstOutEdge.setPrevOutEdgeId(edgeId);

			sourceNode.setFirstOutEdgeId(edgeId);

			dataManager->updateEdge(firstOutEdge);
			dataManager->updateNode(sourceNode);
		}

		Node targetNode = dataManager->getNode(targetNodeId);
		int64_t firstInEdgeId = targetNode.getFirstInEdgeId();

		if (firstInEdgeId == 0) {
			targetNode.setFirstInEdgeId(edgeId);
			dataManager->updateNode(targetNode);
		} else {
			Edge firstInEdge = dataManager->getEdge(firstInEdgeId);
			edge.setNextInEdgeId(firstInEdgeId);
			firstInEdge.setPrevInEdgeId(edgeId);

			targetNode.setFirstInEdgeId(edgeId);

			dataManager->updateEdge(firstInEdge);
			dataManager->updateNode(targetNode);
		}

		dataManager->updateEdge(edge);
	}

	RelationshipBatchLinkUpdates RelationshipTraversal::buildBatchLinks(std::vector<Edge> &edges) const {
		RelationshipBatchLinkUpdates updates;
		if (edges.empty()) {
			return updates;
		}

		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return updates;
		}

		const DenseNodeIdRange denseRange = chooseDenseNodeIdRange(edges);
		if (denseRange.enabled) {
			DenseIdMap<LinkListBuildState> outStates(denseRange.minId, denseRange.maxId, edges.size());
			DenseIdMap<LinkListBuildState> inStates(denseRange.minId, denseRange.maxId, edges.size());
			DenseIdMap<NodeLinkUpdate> nodeUpdates(denseRange.minId, denseRange.maxId, edges.size());
			HashIdMap<EdgeLinkUpdate> oldHeadUpdates((std::min)(edges.size(), size_t{4096}));
			return buildBatchLinksWithMaps(*dataManager, edges, outStates, inStates, nodeUpdates, oldHeadUpdates);
		}

		HashIdMap<LinkListBuildState> outStates(edges.size());
		HashIdMap<LinkListBuildState> inStates(edges.size());
		HashIdMap<NodeLinkUpdate> nodeUpdates(edges.size());
		HashIdMap<EdgeLinkUpdate> oldHeadUpdates((std::min)(edges.size(), size_t{4096}));
		return buildBatchLinksWithMaps(*dataManager, edges, outStates, inStates, nodeUpdates, oldHeadUpdates);
	}

	void RelationshipTraversal::applyBatchLinkUpdates(const RelationshipBatchLinkUpdates &updates) const {
		if (updates.empty()) {
			return;
		}

		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return;
		}

		{
			debug::ScopedPerfTimer timer("relationship_traversal.link_edges_batch.update_nodes");
			dataManager->updateNodesWithBeforeImages(updates.nodes, updates.oldNodes);
		}

		{
			debug::ScopedPerfTimer timer("relationship_traversal.link_edges_batch.update_old_heads");
			dataManager->updateEdgesWithBeforeImages(updates.oldHeadEdges, updates.oldHeadEdgesBefore);
		}
	}

	void RelationshipTraversal::linkEdgesBatch(std::vector<Edge> &edges) const {
		if (edges.empty()) {
			return;
		}

		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return;
		}

		debug::ScopedPerfTimer totalTimer("relationship_traversal.link_edges_batch");
		const auto updates = buildBatchLinks(edges);
		applyBatchLinkUpdates(updates);
		{
			debug::ScopedPerfTimer timer("relationship_traversal.link_edges_batch.update_new_edges");
			dataManager->getEdgeManager()->updateBatch(edges);
		}
	}

	void RelationshipTraversal::unlinkEdge(Edge &edge) const {
		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return;
		}

		int64_t sourceNodeId = edge.getSourceNodeId();
		int64_t targetNodeId = edge.getTargetNodeId();

		int64_t prevOutEdgeId = edge.getPrevOutEdgeId();
		int64_t nextOutEdgeId = edge.getNextOutEdgeId();

		if (prevOutEdgeId == 0) {
			Node sourceNode = dataManager->getNode(sourceNodeId);
			sourceNode.setFirstOutEdgeId(nextOutEdgeId);
			dataManager->updateNode(sourceNode);
		} else {
			Edge prevOutEdge = dataManager->getEdge(prevOutEdgeId);
			prevOutEdge.setNextOutEdgeId(nextOutEdgeId);
			dataManager->updateEdge(prevOutEdge);
		}

		if (nextOutEdgeId != 0) {
			Edge nextOutEdge = dataManager->getEdge(nextOutEdgeId);
			nextOutEdge.setPrevOutEdgeId(prevOutEdgeId);
			dataManager->updateEdge(nextOutEdge);
		}

		int64_t prevInEdgeId = edge.getPrevInEdgeId();
		int64_t nextInEdgeId = edge.getNextInEdgeId();

		if (prevInEdgeId == 0) {
			Node targetNode = dataManager->getNode(targetNodeId);
			targetNode.setFirstInEdgeId(nextInEdgeId);
			dataManager->updateNode(targetNode);
		} else {
			Edge prevInEdge = dataManager->getEdge(prevInEdgeId);
			prevInEdge.setNextInEdgeId(nextInEdgeId);
			dataManager->updateEdge(prevInEdge);
		}

		if (nextInEdgeId != 0) {
			Edge nextInEdge = dataManager->getEdge(nextInEdgeId);
			nextInEdge.setPrevInEdgeId(prevInEdgeId);
			dataManager->updateEdge(nextInEdge);
		}

		edge.setNextOutEdgeId(0);
		edge.setPrevOutEdgeId(0);
		edge.setNextInEdgeId(0);
		edge.setPrevInEdgeId(0);
	}

} // namespace graph::traversal
