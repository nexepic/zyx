/**
 * @file LogicalGraphWrite.hpp
 * @brief Format-independent atomic logical graph creation boundary.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/LogicalGraphRead.hpp"

namespace graph::storage {

	// A vector, rather than an unordered map, keeps write-request validation and
	// token resolution deterministic. Backends reject duplicate keys explicitly.
	struct LogicalGraphWriteProperty {
		std::string key;
		PropertyValue value;

		bool operator==(const LogicalGraphWriteProperty &) const = default;
	};

	struct LogicalGraphNodeCreateRequest {
		std::vector<std::string> labels;
		std::vector<LogicalGraphWriteProperty> properties;

		bool operator==(const LogicalGraphNodeCreateRequest &) const = default;
	};

	struct LogicalGraphEdgeCreateRequest {
		uint64_t source_node_id = 0U;
		uint64_t target_node_id = 0U;
		// Empty is a valid request for an untyped Cypher relationship. Backends
		// encode it without allocating a schema token.
		std::string type;
		std::vector<LogicalGraphWriteProperty> properties;

		bool operator==(const LogicalGraphEdgeCreateRequest &) const = default;
	};

	// Replacing a collection is the primitive mutation: callers that implement
	// SET/REMOVE first read the snapshot-visible collection, then publish its
	// complete replacement. This keeps one Entity->Property-root reference
	// authoritative and avoids in-place variable-length edits.
	struct LogicalGraphPropertyReplaceRequest {
		uint64_t entity_id = 0U;
		std::vector<LogicalGraphWriteProperty> properties;

		bool operator==(const LogicalGraphPropertyReplaceRequest &) const = default;
	};

	// A non-detach deletion succeeds only for an isolated Node. A detach request
	// carries an explicit bound because discovering its incident relationships
	// must never turn a public logical write into unbounded backend work.
	struct LogicalGraphNodeDeleteRequest {
		uint64_t node_id = 0U;
		bool detach = false;
		std::size_t maximum_detach_edges = 0U;

		bool operator==(const LogicalGraphNodeDeleteRequest &) const = default;
	};

	struct LogicalGraphEdgeDeleteRequest {
		uint64_t edge_id = 0U;

		bool operator==(const LogicalGraphEdgeDeleteRequest &) const = default;
	};

	// These enum members are prefixed for Windows macro safety. A mutation
	// reference remains logical across one atomic batch: persisted identities are
	// stable database IDs, while created identities are zero-based batch ordinals.
	enum class LogicalGraphMutationEntityKind : uint8_t {
		LGMK_NODE = 1,
		LGMK_EDGE = 2,
	};

	enum class LogicalGraphMutationReferenceKind : uint8_t {
		LGRK_PERSISTED = 1,
		LGRK_CREATED_NODE = 2,
		LGRK_CREATED_EDGE = 3,
	};

	struct LogicalGraphMutationEntityReference {
		LogicalGraphMutationEntityKind entity_kind = LogicalGraphMutationEntityKind::LGMK_NODE;
		LogicalGraphMutationReferenceKind reference_kind = LogicalGraphMutationReferenceKind::LGRK_PERSISTED;
		uint64_t value = 0U;

		bool operator==(const LogicalGraphMutationEntityReference &) const = default;
	};

	struct LogicalGraphBatchEdgeCreateRequest {
		LogicalGraphMutationEntityReference source;
		LogicalGraphMutationEntityReference target;
		// Empty preserves an omitted relationship type across one atomic batch.
		std::string type;
		std::vector<LogicalGraphWriteProperty> properties;

		bool operator==(const LogicalGraphBatchEdgeCreateRequest &) const = default;
	};

	struct LogicalGraphPropertyMutationRequest {
		LogicalGraphMutationEntityReference entity;
		std::vector<LogicalGraphWriteProperty> properties;

		bool operator==(const LogicalGraphPropertyMutationRequest &) const = default;
	};

	struct LogicalGraphNodeLabelMutationRequest {
		LogicalGraphMutationEntityReference node;
		std::vector<std::string> labels;

		bool operator==(const LogicalGraphNodeLabelMutationRequest &) const = default;
	};

	// A durable creation lease may be split by independent writers. A range is
	// therefore explicit rather than inferred from one snapshot high-water mark.
	struct LogicalGraphCreationIdentityRange {
		uint64_t first_id = 0U;
		uint64_t count = 0U;

		[[nodiscard]] constexpr bool contains(uint64_t id) const {
			return first_id != 0U && count != 0U && id >= first_id && id - first_id < count;
		}

		bool operator==(const LogicalGraphCreationIdentityRange &) const = default;
	};

	// A transaction may cancel a provisional creation before its single durable
	// publication. The surviving requests retain their already leased IDs, while
	// every leased range prevents a later transaction from reviving a cancelled
	// identity. This plan is backend-validated and emitted only by a logical
	// transaction's final-state normalizer.
	struct LogicalGraphCreationIdentityPlan {
		std::vector<LogicalGraphCreationIdentityRange> node_id_ranges;
		std::vector<uint64_t> node_ids;
		std::vector<LogicalGraphCreationIdentityRange> edge_id_ranges;
		std::vector<uint64_t> edge_ids;

		bool operator==(const LogicalGraphCreationIdentityPlan &) const = default;
	};

	// One batch corresponds to exactly one optimistic graph publication. It is
	// intentionally a declarative final-state request: a Property mutation
	// replaces the complete collection, and a Node label mutation replaces the
	// complete label set. This prevents per-row writes from leaking intermediate
	// graph states into readers or derived indexes.
	struct LogicalGraphMutationBatch {
		std::vector<LogicalGraphNodeCreateRequest> node_creates;
		std::vector<LogicalGraphBatchEdgeCreateRequest> edge_creates;
		std::vector<LogicalGraphPropertyMutationRequest> property_replacements;
		std::vector<LogicalGraphNodeLabelMutationRequest> node_label_replacements;
		std::vector<LogicalGraphEdgeDeleteRequest> edge_deletes;
		std::vector<LogicalGraphNodeDeleteRequest> node_deletes;
		std::optional<LogicalGraphCreationIdentityPlan> creation_identity_plan;

		bool operator==(const LogicalGraphMutationBatch &) const = default;
	};

	struct LogicalGraphMutationBatchResult {
		std::vector<uint64_t> created_node_ids;
		std::vector<uint64_t> created_edge_ids;

		bool operator==(const LogicalGraphMutationBatchResult &) const = default;
	};

	// A caller that evaluated a read-dependent mutation against an older logical
	// snapshot must restart planning, never replay the old row set at a newer
	// generation. This distinct error makes that retry contract explicit.
	class LogicalGraphMutationConflict final : public std::runtime_error {
	public:
		explicit LogicalGraphMutationConflict(const std::string &message) : std::runtime_error(message) {}
	};

	// This interface deliberately exposes logical entity requests only. A caller
	// never supplies a Segment, page, physical owner, or durable checkpoint LSN.
	// Each operation commits its entire input batch atomically or throws without
	// returning any durable logical IDs.
	class LogicalGraphWriteBackend {
	public:
		virtual ~LogicalGraphWriteBackend() = default;

		[[nodiscard]] virtual std::vector<uint64_t>
		createLogicalNodes(const std::vector<LogicalGraphNodeCreateRequest> &requests) = 0;
		[[nodiscard]] virtual std::vector<uint64_t>
		createLogicalEdges(const std::vector<LogicalGraphEdgeCreateRequest> &requests) = 0;
		virtual void replaceLogicalNodeProperties(const std::vector<LogicalGraphPropertyReplaceRequest> &requests) = 0;
		virtual void replaceLogicalEdgeProperties(const std::vector<LogicalGraphPropertyReplaceRequest> &requests) = 0;
		virtual void deleteLogicalNodes(const std::vector<LogicalGraphNodeDeleteRequest> &requests) = 0;
		virtual void deleteLogicalEdges(const std::vector<LogicalGraphEdgeDeleteRequest> &requests) = 0;
		[[nodiscard]] virtual LogicalGraphMutationBatchResult
		executeLogicalGraphMutationBatch(const LogicalGraphMutationBatch &batch,
										 std::optional<LogicalGraphMutationFence> expectedFence = std::nullopt) = 0;
	};

	// All enum members are prefixed for Windows macro safety. A logical
	// transaction pins one read snapshot, accumulates logical final-state
	// mutations, and publishes at most one graph generation on commit.
	enum class LogicalGraphTransactionState : uint8_t {
		LGTTS_ACTIVE = 1,
		LGTTS_COMMITTED = 2,
		LGTTS_ROLLED_BACK = 3,
		LGTTS_CONFLICTED = 4,
	};

	class LogicalGraphTransaction {
	public:
		virtual ~LogicalGraphTransaction() = default;

		[[nodiscard]] virtual LogicalGraphTransactionState state() const = 0;
		[[nodiscard]] virtual bool isReadOnly() const = 0;
		[[nodiscard]] virtual LogicalGraphReadSnapshot &readSnapshot() = 0;
		// Computes the commit-success IDs without mutating the transaction. This
		// lets an in-memory read overlay validate a batch before it is staged.
		[[nodiscard]] virtual LogicalGraphMutationBatchResult
		previewMutationBatch(const LogicalGraphMutationBatch &batch) const = 0;
		// Returns stable-on-success provisional IDs for this batch's newly created
		// entities. They become durable only if commit succeeds; a conflict or
		// rollback invalidates them together with every staged observation.
		[[nodiscard]] virtual LogicalGraphMutationBatchResult
		stageMutationBatch(LogicalGraphMutationBatch batch) = 0;
		[[nodiscard]] virtual LogicalGraphMutationBatchResult commit() = 0;
		virtual void rollback() = 0;
	};

	// This opt-in interface is deliberately separate from auto-commit writes.
	// It lets public transaction adapters feature-detect one logical transaction
	// boundary without exposing a physical relocatable write transaction.
	class LogicalGraphTransactionalBackend {
	public:
		virtual ~LogicalGraphTransactionalBackend() = default;
		[[nodiscard]] virtual std::unique_ptr<LogicalGraphTransaction> beginLogicalGraphTransaction(bool readOnly) = 0;
	};

} // namespace graph::storage
