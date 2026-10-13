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
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

	// A patch preserves unspecified fields. NULL assignment is normalized into
	// removals by the caller; set/remove keys must be disjoint and unique.
	struct LogicalGraphPropertyPatchRequest {
		LogicalGraphMutationEntityReference entity;
		std::vector<LogicalGraphWriteProperty> sets;
		std::vector<std::string> removals;
		bool operator==(const LogicalGraphPropertyPatchRequest &) const = default;
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

	// Synchronous read controls for backend validation before graph publication.
	// Property admission reports absolute encoded selected-source bytes per batch.
	struct LogicalGraphMutationReadControl {
		std::function<void()> checkpoint;
		std::function<void(size_t)> property_admission;
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
		// Transitional old-layout adapter. Persisted targets only; the supplied
		// snapshot/fence owns all reads and the resulting batch publishes once.
		// The entry bound covers cumulative input operations plus source fields,
		// not an entity schema limit or decoded allocation capacity.
		[[nodiscard]] LogicalGraphMutationBatchResult executeLogicalGraphPropertyPatches(
			LogicalGraphReadSnapshot &snapshot, std::span<const LogicalGraphPropertyPatchRequest> patches,
			size_t maximumPropertyEntries, const LogicalGraphMutationFence &expectedFence,
			const LogicalGraphMutationReadControl &control = {});

		// Companion mutations and normalized patches share one publication.
		// Created ordinals resolve only within companionBatch and patch its initial
		// creation fields. Complete replacement and patch targets must be disjoint.
		[[nodiscard]] virtual LogicalGraphMutationBatchResult executeLogicalGraphMutationBatchWithPropertyPatches(
			LogicalGraphReadSnapshot &snapshot, const LogicalGraphMutationBatch &companionBatch,
			std::span<const LogicalGraphPropertyPatchRequest> patches, size_t maximumPropertyEntries,
			const LogicalGraphMutationFence &expectedFence, const LogicalGraphMutationReadControl &control = {});

		[[nodiscard]] virtual LogicalGraphMutationBatchResult executeLogicalGraphMutationBatchWithReadControl(
			const LogicalGraphMutationBatch &batch, std::optional<LogicalGraphMutationFence> expectedFence,
			const LogicalGraphMutationReadControl &control) {
			// Legacy backends retain entry checks; native backends additionally
			// admit constraint sources and check before durable publication.
			if (control.checkpoint) control.checkpoint();
			return executeLogicalGraphMutationBatch(batch, expectedFence);
		}

	};

	// All enum members are prefixed for Windows macro safety. A logical
	// transaction pins one read snapshot, accumulates logical final-state
	// mutations, and publishes at most one graph generation on commit.
	enum class LogicalGraphTransactionState : uint8_t {
		LGTTS_ACTIVE = 1,
		LGTTS_COMMITTED = 2,
		LGTTS_ROLLED_BACK = 3,
		LGTTS_CONFLICTED = 4,
		// The final durability fence reported an error after publication may have
		// become visible. The caller must close and reopen before deciding whether
		// the graph is old or new; this state must never be presented as rollback.
		LGTTS_INDETERMINATE = 5,
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

	// A borrowed columnar view is intentionally separate from
	// LogicalGraphMutationBatch. A bounded ingest backend consumes one caller
	// batch synchronously and must not retain a logical request for every Edge.
	struct LogicalGraphBulkEdgePropertyColumn {
		std::string_view key;
		std::span<const PropertyValue> values;
	};

	struct LogicalGraphPersistedBulkEdgeAppendRequest {
		LogicalGraphPersistedBulkEdgeAppendRequest(
				std::string_view edgeType, std::span<const uint64_t> sourceNodeIds,
				std::span<const uint64_t> targetNodeIds,
				std::span<const LogicalGraphBulkEdgePropertyColumn> propertyColumns) :
			type(edgeType), source_node_ids(sourceNodeIds), target_node_ids(targetNodeIds),
			property_columns(propertyColumns) {}
		// Public C++ and Driver ABI columns use signed identifiers. Borrow them
		// directly so the streamed path never creates a transaction-sized unsigned
		// endpoint copy merely to cross this storage-neutral boundary.
		LogicalGraphPersistedBulkEdgeAppendRequest(
				std::string_view edgeType, std::span<const int64_t> sourceNodeIds,
				std::span<const int64_t> targetNodeIds,
				std::span<const LogicalGraphBulkEdgePropertyColumn> propertyColumns) :
			type(edgeType), signed_source_node_ids(sourceNodeIds), signed_target_node_ids(targetNodeIds),
			property_columns(propertyColumns) {}

		[[nodiscard]] size_t sourceCount() const noexcept {
			return signed_source_node_ids.empty() ? source_node_ids.size() : signed_source_node_ids.size();
		}
		[[nodiscard]] size_t targetCount() const noexcept {
			return signed_target_node_ids.empty() ? target_node_ids.size() : signed_target_node_ids.size();
		}
		[[nodiscard]] uint64_t sourceNodeIdAt(size_t index) const noexcept {
			if (!signed_source_node_ids.empty()) {
				const int64_t value = signed_source_node_ids[index];
				return value > 0 ? static_cast<uint64_t>(value) : 0U;
			}
			return source_node_ids[index];
		}
		[[nodiscard]] uint64_t targetNodeIdAt(size_t index) const noexcept {
			if (!signed_target_node_ids.empty()) {
				const int64_t value = signed_target_node_ids[index];
				return value > 0 ? static_cast<uint64_t>(value) : 0U;
			}
			return target_node_ids[index];
		}

		std::string_view type;
		std::span<const uint64_t> source_node_ids;
		std::span<const uint64_t> target_node_ids;
		std::span<const int64_t> signed_source_node_ids;
		std::span<const int64_t> signed_target_node_ids;
		std::span<const LogicalGraphBulkEdgePropertyColumn> property_columns;
	};

	// Optional capability for an Edge-only transaction that stages persisted
	// endpoints in bounded private storage. It is deliberately distinct from the
	// ordinary overlay: callers needing created endpoints or read-your-writes
	// retain normal logical transaction semantics rather than silently mixing two
	// representations.
	class LogicalGraphBoundedBulkEdgeTransaction {
	public:
		virtual ~LogicalGraphBoundedBulkEdgeTransaction() = default;
		[[nodiscard]] virtual std::vector<uint64_t>
		appendPersistedBulkEdges(const LogicalGraphPersistedBulkEdgeAppendRequest &request) = 0;
		[[nodiscard]] virtual bool hasStreamedBulkEdges() const = 0;
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
