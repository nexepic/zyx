/**
 * @file LogicalGraphTransactionOverlay.hpp
 * @brief Read-your-writes overlay for one logical graph transaction.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <map>
#include <memory>
#include <memory_resource>
#include <optional>

#include "graph/storage/LogicalGraphVectorSearch.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/storage/detail/OverlayPayloadBudget.hpp"

namespace graph::storage {

	// This adapter owns no physical state. It applies declarative batches to an
	// in-memory final-state overlay while LogicalGraphTransaction retains the
	// one pinned base snapshot and performs the sole durable publication.
	class LogicalGraphTransactionOverlay final : public LogicalGraphReadBackend,
														public LogicalGraphWriteBackend,
														public LogicalGraphVectorSearchBackend {
	public:
		explicit LogicalGraphTransactionOverlay(LogicalGraphTransaction &transaction);
		~LogicalGraphTransactionOverlay() override;
		LogicalGraphTransactionOverlay(const LogicalGraphTransactionOverlay &) = delete;
		LogicalGraphTransactionOverlay &operator=(const LogicalGraphTransactionOverlay &) = delete;

		[[nodiscard]] std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override;
		[[nodiscard]] std::unique_ptr<LogicalGraphVectorSearchSnapshot>
		openLogicalGraphVectorSearchSnapshot() const override;
		[[nodiscard]] std::vector<uint64_t>
		createLogicalNodes(const std::vector<LogicalGraphNodeCreateRequest> &requests) override;
		[[nodiscard]] std::vector<uint64_t>
		createLogicalEdges(const std::vector<LogicalGraphEdgeCreateRequest> &requests) override;
		void replaceLogicalNodeProperties(const std::vector<LogicalGraphPropertyReplaceRequest> &requests) override;
		void replaceLogicalEdgeProperties(const std::vector<LogicalGraphPropertyReplaceRequest> &requests) override;
		void deleteLogicalNodes(const std::vector<LogicalGraphNodeDeleteRequest> &requests) override;
		void deleteLogicalEdges(const std::vector<LogicalGraphEdgeDeleteRequest> &requests) override;
		[[nodiscard]] LogicalGraphMutationBatchResult
		executeLogicalGraphMutationBatch(const LogicalGraphMutationBatch &batch,
									std::optional<LogicalGraphMutationFence> expectedFence = std::nullopt) override;
		[[nodiscard]] LogicalGraphMutationBatchResult executeLogicalGraphMutationBatchWithPropertyPatches(
			LogicalGraphReadSnapshot &snapshot, const LogicalGraphMutationBatch &companionBatch,
			std::span<const LogicalGraphPropertyPatchRequest> patches, size_t maximumPropertyEntries,
			const LogicalGraphMutationFence &expectedFence, const LogicalGraphMutationReadControl &control = {}) override;

	private:
        struct NodeState {
            detail::OverlayPayloadBudget payload_budget;
            LogicalGraphNode node;
            bool created = false, deleted = false;
            explicit NodeState(std::pmr::memory_resource *memory)
                : payload_budget(memory, 2U * query::execution::detail::standardContainerProxyBytes()) {}
            NodeState(std::pmr::memory_resource *memory, const LogicalGraphNode &source)
                : payload_budget(memory, source), node(source) {}
            NodeState(const NodeState &source)
                : payload_budget(source.payload_budget.memory(), source.node), node(source.node),
                  created(source.created), deleted(source.deleted) {}
            friend void swap(NodeState &left, NodeState &right) noexcept {
                using std::swap;
                left.payload_budget.swap(right.payload_budget);
                swap(left.node.node_id, right.node.node_id);
                left.node.labels.swap(right.node.labels); left.node.properties.swap(right.node.properties);
                swap(left.created, right.created); swap(left.deleted, right.deleted);
            }
        };
        struct EdgeState {
            detail::OverlayPayloadBudget payload_budget;
            LogicalGraphEdge edge;
            bool created = false, deleted = false;
            uint64_t relationship_type_token = 0U;
            explicit EdgeState(std::pmr::memory_resource *memory)
                : payload_budget(memory, 2U * query::execution::detail::standardContainerProxyBytes()) {}
            EdgeState(std::pmr::memory_resource *memory, const LogicalGraphEdge &source)
                : payload_budget(memory, source), edge(source) {}
            EdgeState(const EdgeState &source)
                : payload_budget(source.payload_budget.memory(), source.edge), edge(source.edge),
                  created(source.created), deleted(source.deleted), relationship_type_token(source.relationship_type_token) {}
            friend void swap(EdgeState &left, EdgeState &right) noexcept {
                using std::swap;
                left.payload_budget.swap(right.payload_budget);
                swap(left.edge.edge_id, right.edge.edge_id);
                swap(left.edge.source_node_id, right.edge.source_node_id);
                swap(left.edge.target_node_id, right.edge.target_node_id);
                left.edge.type.swap(right.edge.type); left.edge.properties.swap(right.edge.properties);
                swap(left.created, right.created); swap(left.deleted, right.deleted);
                swap(left.relationship_type_token, right.relationship_type_token);
            }
        };

		class Snapshot;
		class VectorSnapshot;
		// Save only owners touched by one statement, once per owner. An empty
		// entry records absence so rollback removes a newly materialized owner.
		struct StatementUndo {
			std::pmr::map<uint64_t, std::optional<NodeState>> nodes;
			std::pmr::map<uint64_t, std::optional<EdgeState>> edges;
            explicit StatementUndo(std::pmr::memory_resource *memory) : nodes(memory), edges(memory) {}
		};
		void rememberNode(StatementUndo &undo, uint64_t nodeId) const;
		void rememberEdge(StatementUndo &undo, uint64_t edgeId) const;
		void restoreStatement(StatementUndo &undo) noexcept;
		[[nodiscard]] LogicalGraphMutationBatchResult executeStagedBatch(
			const LogicalGraphMutationBatch &visibleBatch, std::optional<LogicalGraphMutationFence> expectedFence,
			const LogicalGraphMutationBatch *patchCompanion = nullptr,
			std::vector<LogicalGraphPropertyPatchRequest> patches = {}, size_t entryWork = 0U,
			const LogicalGraphMutationReadControl &control = {});

		[[nodiscard]] LogicalGraphReadSnapshot &baseSnapshot() const;
		[[nodiscard]] std::optional<LogicalGraphMutationFence> baseFence() const;
		[[nodiscard]] std::optional<LogicalGraphMutationFence> overlayFence() const;
		[[nodiscard]] std::optional<LogicalGraphSchemaIdentity> overlaySchemaIdentity() const;
		[[nodiscard]] NodeState &ensureNode(uint64_t nodeId);
		[[nodiscard]] EdgeState &ensureEdge(uint64_t edgeId);
		void applyBatch(const LogicalGraphMutationBatch &batch, const LogicalGraphMutationBatchResult &result,
			StatementUndo &undo);
		[[nodiscard]] bool hasVisibleIncidentEdge(uint64_t nodeId) const;
		[[nodiscard]] bool isNodeVisible(uint64_t nodeId) const;
		[[nodiscard]] bool isEdgeVisible(const LogicalGraphEdge &edge, uint64_t edgeId) const;

		LogicalGraphTransaction &transaction_;
		const uint64_t view_token_;
		// Declare the owner before all containers that allocate from it.
        std::shared_ptr<std::pmr::memory_resource> memory_owner_;
        std::pmr::map<uint64_t, NodeState> nodes_;
		std::pmr::map<uint64_t, EdgeState> edges_;
		// Base-snapshot presence is immutable for this transaction. Cache it so a
		// bulk edge batch does not repeat directory probes for shared endpoints.
		mutable std::pmr::map<uint64_t, bool> persisted_node_visibility_;
		// This changes only after a successfully staged nonempty batch. It is
		// carried by overlay snapshots so transaction-local derived views can
		// report staleness without affecting the durable write fence.
		uint64_t logical_revision_ = 0U;
	};

} // namespace graph::storage
