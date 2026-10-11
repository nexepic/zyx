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
#include <optional>

#include "graph/storage/LogicalGraphVectorSearch.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"

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

	private:
		struct NodeState {
			LogicalGraphNode node;
			bool created = false;
			bool deleted = false;
		};

		struct EdgeState {
			LogicalGraphEdge edge;
			bool created = false;
			bool deleted = false;
		};

		class Snapshot;
		class VectorSnapshot;

		[[nodiscard]] LogicalGraphReadSnapshot &baseSnapshot() const;
		[[nodiscard]] std::optional<LogicalGraphMutationFence> baseFence() const;
		[[nodiscard]] std::optional<LogicalGraphMutationFence> overlayFence() const;
		[[nodiscard]] NodeState &ensureNode(uint64_t nodeId);
		[[nodiscard]] EdgeState &ensureEdge(uint64_t edgeId);
		void applyBatch(const LogicalGraphMutationBatch &batch, const LogicalGraphMutationBatchResult &result);
		[[nodiscard]] bool hasVisibleIncidentEdge(uint64_t nodeId) const;
		[[nodiscard]] bool isNodeVisible(uint64_t nodeId) const;
		[[nodiscard]] bool isEdgeVisible(const LogicalGraphEdge &edge, uint64_t edgeId) const;

		LogicalGraphTransaction &transaction_;
		std::map<uint64_t, NodeState> nodes_;
		std::map<uint64_t, EdgeState> edges_;
		// Base-snapshot presence is immutable for this transaction. Cache it so a
		// bulk edge batch does not repeat directory probes for shared endpoints.
		mutable std::map<uint64_t, bool> persisted_node_visibility_;
		// This changes only after a successfully staged nonempty batch. It is
		// carried by overlay snapshots so transaction-local derived views can
		// report staleness without affecting the durable write fence.
		uint64_t logical_revision_ = 0U;
	};

} // namespace graph::storage
