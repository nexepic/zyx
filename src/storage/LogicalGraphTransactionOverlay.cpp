/**
 * @file LogicalGraphTransactionOverlay.cpp
 * @brief In-memory final-state view for one logical graph transaction.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
#include "graph/storage/LogicalGraphTransactionMemoryProvider.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"
#include "graph/storage/detail/ScopedMutationCopies.hpp"
#include "graph/storage/detail/ScopedPropertyBuffers.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "graph/core/Types.hpp"
#include "graph/core/GuardedPropertyCopy.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"

namespace graph::storage {

	namespace {
        std::shared_ptr<std::pmr::memory_resource> transactionWriteMemory(const LogicalGraphTransaction &transaction) {
            const auto *provider = dynamic_cast<const LogicalGraphTransactionMemoryProvider *>(&transaction);
            return provider ? provider->retainedWriteMemory() : nullptr;
        }

		uint64_t nextOverlayViewToken() {
			static std::atomic<uint64_t> next{1U};
			uint64_t value = next.load(std::memory_order_relaxed);
			for (;;) {
				if (value == (std::numeric_limits<uint64_t>::max)())
					throw std::overflow_error("logical transaction overlay view identity exhausted");
				if (next.compare_exchange_weak(value, value + 1U, std::memory_order_relaxed)) return value;
			}
		}

		template<typename Edge>
		void ensureAdjacencyCapacity(std::vector<Edge> &edges, size_t maximumRecords) {
			if (edges.size() >= maximumRecords)
				throw std::runtime_error("logical transaction adjacency exceeded its record bound");
			if (edges.size() < edges.capacity()) return;
			const size_t capacity = edges.capacity();
			const size_t next = capacity == 0U ? std::min<size_t>(maximumRecords, 16U)
				: capacity + std::min(capacity, maximumRecords - capacity);
			edges.reserve(next);
		}

		size_t checkedTopologySum(size_t left, size_t right) {
			if (right > std::numeric_limits<size_t>::max() - left)
				throw std::overflow_error("overlay topology buffer size overflow");
			return left + right;
		}

		size_t topologyStorageBytes(size_t capacity, size_t typeBytes) {
			if (capacity > std::numeric_limits<size_t>::max() / sizeof(LogicalGraphEdgeTopology))
				throw std::overflow_error("overlay topology slot size overflow");
			return checkedTopologySum(capacity * sizeof(LogicalGraphEdgeTopology), typeBytes);
		}

		size_t topologyTypeBytes(const std::vector<LogicalGraphEdgeTopology> &edges) {
			size_t bytes = 0U;
			for (const auto &edge: edges) bytes = checkedTopologySum(bytes, edge.type.capacity());
			return bytes;
		}

		void admitTopologyStorage(const std::function<void(size_t)> &admission,
			const std::vector<LogicalGraphEdgeTopology> &edges, size_t typeBytes, size_t otherBytes = 0U) {
			if (admission) admission(checkedTopologySum(topologyStorageBytes(edges.capacity(), typeBytes), otherBytes));
		}

		void reserveTopologySlot(std::vector<LogicalGraphEdgeTopology> &edges, size_t maximumRecords,
			size_t typeBytes, const std::function<void(size_t)> &admission, size_t otherBytes = 0U) {
			if (edges.size() >= maximumRecords)
				throw std::runtime_error("overlay topology exceeded its record bound");
			if (edges.size() == edges.capacity()) {
				const size_t oldCapacity = edges.capacity();
				const size_t next = oldCapacity == 0U ? std::min<size_t>(maximumRecords, 16U)
					: oldCapacity + std::min(oldCapacity, maximumRecords - oldCapacity);
				// Both allocations coexist during vector relocation.
				if (admission) admission(checkedTopologySum(
					topologyStorageBytes(checkedTopologySum(oldCapacity, next), typeBytes), otherBytes));
				edges.reserve(next);
			}
			admitTopologyStorage(admission, edges, typeBytes, otherBytes);
		}

		void admitStagedProperty(const PropertyValue &value, const std::function<void(size_t)> *admission) {
			if (admission == nullptr) return;
			const size_t bytes = CanonicalPropertyValueCodec::encodedSize(value);
			if (bytes > std::numeric_limits<size_t>::max() - sizeof(uint32_t)) {
				throw std::overflow_error("transaction Property entry size overflowed");
			}
			(*admission)(bytes + sizeof(uint32_t));
		}

        void admitGuardedStagedValue(const PropertyValue &source) {
            auto *scope = property_decode::AllocationScope::current();
            if (!scope) return;
            const auto checkpoint = [scope] { scope->checkpoint(); };
            const std::function<void()> notify = std::ref(checkpoint);
            scope->admit(CanonicalPropertyValueCodec::guardedCopyHeapUpper(source, notify));
        }

		PropertyValue copyStagedValue(const PropertyValue &source, const std::function<void(size_t)> *admission) {
			if (admission == nullptr) {
                detail::sizeScopedMutationCopy([&](detail::WriteCopySizer &meter) { meter.property(source); });
                return source;
            }
			admitGuardedStagedValue(source);
            return graph::property_copy::copyPropertyValueGuarded(source, [&] { (*admission)(0U); });
		}

		LogicalGraphPropertyEntry copyStagedEntry(const LogicalGraphPropertyEntry &entry,
			const std::function<void(size_t)> *admission) {
			if (admission == nullptr) {
                detail::sizeScopedMutationCopy([&](detail::WriteCopySizer &meter) {
                    meter.add(detail::writeStringCopyHeapUpper(entry.key)); meter.property(entry.value);
                });
                return entry;
            }
			auto checkpoint = [&] { (*admission)(0U); };
            detail::sizeScopedMutationCopy([&](detail::WriteCopySizer &meter) { meter.add(detail::writeStringCopyHeapUpper(entry.key)); });
			auto key = graph::property_copy::copyPropertyStringGuarded(entry.key, checkpoint);
			return {std::move(key), copyStagedValue(entry.value, admission)};
		}

		std::vector<LogicalGraphPropertyEntry> copyStagedProperties(
			const std::vector<LogicalGraphPropertyEntry> &source, const std::function<void(size_t)> *admission) {
			if (admission == nullptr) {
                detail::admitScopedPropertyCopies<LogicalGraphPropertyEntry>(source);
                return source;
            }
			// Zero-byte admission is a cancellation checkpoint, not a new charge.
			const auto checkpoint = [&] { (*admission)(0U); };
            detail::admitScopedMutationBytes(query::execution::detail::standardContainerProxyBytes());
            detail::admitScopedPropertyVectorStorage<LogicalGraphPropertyEntry>(source.size());
			std::vector<LogicalGraphPropertyEntry> result;
			result.reserve(source.size());
			for (const auto &entry : source) {
				auto guard = checkpoint;
                detail::sizeScopedMutationCopy([&](detail::WriteCopySizer &meter) { meter.add(detail::writeStringCopyHeapUpper(entry.key)); });
                auto key = graph::property_copy::copyPropertyStringGuarded(entry.key, guard);
                admitGuardedStagedValue(entry.value);
                auto value = graph::property_copy::copyPropertyValueGuarded(entry.value, guard);
                detail::admitScopedMutationBytes(query::execution::detail::addStandardHeapBytes(
                    detail::movedPropertyHeaderHeapUpper(value), query::execution::detail::standardContainerProxyBytes()));
                result.emplace_back(std::move(key), std::move(value));
			}
			return result;
		}

		size_t adjacencyBufferAdd(size_t left, size_t right) {
			if (right > std::numeric_limits<size_t>::max() - left) throw std::overflow_error("overlay adjacency buffers overflowed");
			return left + right;
		}
		size_t adjacencySlots(size_t count, size_t width) {
			if (count > std::numeric_limits<size_t>::max() / width) throw std::overflow_error("overlay adjacency slots overflowed");
			return count * width;
		}
		size_t adjacencyEdgeMetadata(const LogicalGraphEdge &edge) {
			size_t bytes = adjacencySlots(edge.properties.capacity(), sizeof(LogicalGraphPropertyEntry));
			if (!edge.type.empty()) bytes = adjacencyBufferAdd(bytes, edge.type.capacity());
			for (const auto &entry : edge.properties) bytes = adjacencyBufferAdd(bytes, entry.key.capacity());
			return bytes;
		}
		size_t adjacencyMetadata(const std::vector<LogicalGraphEdge> &edges) {
			size_t bytes = 0U;
			for (const auto &edge : edges) bytes = adjacencyBufferAdd(bytes, adjacencyEdgeMetadata(edge));
			return bytes;
		}
		size_t adjacencyBuffers(const std::vector<LogicalGraphEdge> &edges, size_t metadataBytes) {
			return adjacencyBufferAdd(adjacencySlots(edges.capacity(), sizeof(LogicalGraphEdge)), metadataBytes);
		}
		void growAdmittedAdjacency(std::vector<LogicalGraphEdge> &edges, size_t bound,
			const std::function<void(size_t)> *admission, size_t metadataBytes, size_t sourceBytes = 0U) {
			if (admission == nullptr) { ensureAdjacencyCapacity(edges, bound); return; }
			if (edges.size() >= bound) throw std::runtime_error("overlay adjacency exceeded its record bound");
			if (edges.size() == edges.capacity()) {
				const size_t old = edges.capacity();
				const size_t next = old == 0U ? std::min<size_t>(bound, 16U) : old + std::min(old, bound - old);
				(*admission)(adjacencyBufferAdd(adjacencyBufferAdd(sourceBytes, adjacencyBuffers(edges, metadataBytes)), adjacencySlots(next, sizeof(LogicalGraphEdge))));
				edges.reserve(next);
			}
			(*admission)(adjacencyBufferAdd(sourceBytes, adjacencyBuffers(edges, metadataBytes)));
		}

		using PropertyMap = std::map<std::string, PropertyValue>;

		[[nodiscard]] EntityReadShape completeNodeReadShape() {
			return {.include_labels = true, .include_type = false, .all_properties = true};
		}

		[[nodiscard]] EntityReadShape completeEdgeReadShape() {
			return {.include_labels = false, .include_type = true, .all_properties = true};
		}

		[[nodiscard]] bool isEmpty(const LogicalGraphMutationBatch &batch) {
			return batch.node_creates.empty() && batch.edge_creates.empty() && batch.property_replacements.empty() &&
				   batch.node_label_replacements.empty() && batch.edge_deletes.empty() && batch.node_deletes.empty() &&
				   !batch.creation_identity_plan.has_value();
		}

		[[nodiscard]] PropertyMap makePropertyMap(const std::vector<LogicalGraphPropertyEntry> &properties,
														 const char *context) {
			PropertyMap result;
			for (const LogicalGraphPropertyEntry &property: properties) {
				if (property.key.empty() || !result.emplace(property.key, property.value).second) {
					throw std::runtime_error(std::string(context) + " has invalid owner-keyed Property entries");
				}
			}
			return result;
		}

		[[nodiscard]] LogicalGraphMutationEntityReference persistedNodeReference(uint64_t id) {
			return {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id};
		}

		[[nodiscard]] LogicalGraphMutationEntityReference persistedEdgeReference(uint64_t id) {
			return {LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id};
		}

	} // namespace

#include "transaction_overlay/ReadSnapshot.inc"
#include "transaction_overlay/VectorSnapshot.inc"
	LogicalGraphTransactionOverlay::LogicalGraphTransactionOverlay(LogicalGraphTransaction &transaction) :
			transaction_(transaction), view_token_(nextOverlayViewToken()), memory_owner_(transactionWriteMemory(transaction)),
            nodes_(memory_owner_ ? memory_owner_.get() : std::pmr::new_delete_resource()),
            edges_(memory_owner_ ? memory_owner_.get() : std::pmr::new_delete_resource()),
            persisted_node_visibility_(memory_owner_ ? memory_owner_.get() : std::pmr::new_delete_resource()) {
		if (transaction_.state() != LogicalGraphTransactionState::LGTTS_ACTIVE) {
			throw std::logic_error("logical transaction overlay requires an active transaction");
		}
		const std::optional<LogicalGraphMutationFence> fence = baseFence();
		if (!fence.has_value() || !fence->isValid()) {
			throw std::logic_error("logical transaction overlay requires an optimistic graph-state fence");
		}
	}

	LogicalGraphTransactionOverlay::~LogicalGraphTransactionOverlay() = default;

	std::unique_ptr<LogicalGraphReadSnapshot> LogicalGraphTransactionOverlay::openLogicalGraphReadSnapshot() const {
		if (transaction_.state() != LogicalGraphTransactionState::LGTTS_ACTIVE) {
			throw std::logic_error("logical transaction overlay cannot open a snapshot after transaction completion");
		}
		return std::make_unique<Snapshot>(*this);
	}

	std::unique_ptr<LogicalGraphVectorSearchSnapshot>
	LogicalGraphTransactionOverlay::openLogicalGraphVectorSearchSnapshot() const {
		if (transaction_.state() != LogicalGraphTransactionState::LGTTS_ACTIVE) {
			throw std::logic_error("logical transaction overlay cannot open a Vector snapshot after transaction completion");
		}
		auto *provider = dynamic_cast<LogicalGraphTransactionVectorSnapshotProvider *>(&transaction_);
		if (provider == nullptr) {
			throw std::runtime_error("logical transaction does not own a pinned Vector search snapshot");
		}
		return std::make_unique<VectorSnapshot>(*this, provider->pinnedLogicalGraphVectorSearchSnapshot());
	}

	std::vector<uint64_t>
	LogicalGraphTransactionOverlay::createLogicalNodes(const std::vector<LogicalGraphNodeCreateRequest> &requests) {
		LogicalGraphMutationBatch batch;
		batch.node_creates = requests;
		return executeLogicalGraphMutationBatch(batch).created_node_ids;
	}

	std::vector<uint64_t>
	LogicalGraphTransactionOverlay::createLogicalEdges(const std::vector<LogicalGraphEdgeCreateRequest> &requests) {
		LogicalGraphMutationBatch batch;
		batch.edge_creates.reserve(requests.size());
		for (const LogicalGraphEdgeCreateRequest &request: requests) {
			batch.edge_creates.push_back(
					{persistedNodeReference(request.source_node_id), persistedNodeReference(request.target_node_id), request.type,
					 request.properties});
		}
		return executeLogicalGraphMutationBatch(batch).created_edge_ids;
	}

	void LogicalGraphTransactionOverlay::replaceLogicalNodeProperties(
			const std::vector<LogicalGraphPropertyReplaceRequest> &requests) {
		LogicalGraphMutationBatch batch;
		batch.property_replacements.reserve(requests.size());
		for (const LogicalGraphPropertyReplaceRequest &request: requests) {
			batch.property_replacements.push_back({persistedNodeReference(request.entity_id), request.properties});
		}
		static_cast<void>(executeLogicalGraphMutationBatch(batch));
	}

	void LogicalGraphTransactionOverlay::replaceLogicalEdgeProperties(
			const std::vector<LogicalGraphPropertyReplaceRequest> &requests) {
		LogicalGraphMutationBatch batch;
		batch.property_replacements.reserve(requests.size());
		for (const LogicalGraphPropertyReplaceRequest &request: requests) {
			batch.property_replacements.push_back({persistedEdgeReference(request.entity_id), request.properties});
		}
		static_cast<void>(executeLogicalGraphMutationBatch(batch));
	}

	void LogicalGraphTransactionOverlay::deleteLogicalNodes(const std::vector<LogicalGraphNodeDeleteRequest> &requests) {
		LogicalGraphMutationBatch batch;
		batch.node_deletes = requests;
		static_cast<void>(executeLogicalGraphMutationBatch(batch));
	}

	void LogicalGraphTransactionOverlay::deleteLogicalEdges(const std::vector<LogicalGraphEdgeDeleteRequest> &requests) {
		LogicalGraphMutationBatch batch;
		batch.edge_deletes = requests;
		static_cast<void>(executeLogicalGraphMutationBatch(batch));
	}

	LogicalGraphMutationBatchResult LogicalGraphTransactionOverlay::executeLogicalGraphMutationBatch(
			const LogicalGraphMutationBatch &batch, std::optional<LogicalGraphMutationFence> expectedFence) {
		return executeStagedBatch(batch, expectedFence);
	}

	LogicalGraphMutationBatchResult LogicalGraphTransactionOverlay::executeLogicalGraphMutationBatchWithPropertyPatches(
		LogicalGraphReadSnapshot &snapshot, const LogicalGraphMutationBatch &companionBatch,
		std::span<const LogicalGraphPropertyPatchRequest> patches, size_t maximumPropertyEntries,
		const LogicalGraphMutationFence &expectedFence, const LogicalGraphMutationReadControl &control) {
        std::optional<utils::OwnedAllocationScope> allocationScope;
        if (memory_owner_) allocationScope.emplace(memory_owner_);
        detail::PropertyDecodeWorkspace normalizedPayload(memory_owner_, &control.checkpoint);
		auto normalized = normalizeLogicalGraphPropertyPatchBatch(snapshot, companionBatch, patches,
			maximumPropertyEntries, expectedFence, control);
		if (!transaction_.supportsPropertyPatchStaging() || normalized.persisted_patches.empty())
			return executeStagedBatch(normalized.batch, expectedFence, nullptr, {}, 0U, control);
		// Copy only companion requests into durable staging. Full patch outputs
		// serve statement visibility and are released after this call.
		detail::admitScopedMutationBatchCopy(normalized.batch, false);
        LogicalGraphMutationBatch staged;
        staged.property_replacements.reserve(normalized.batch.property_replacements.size());
		staged.node_creates = normalized.batch.node_creates;
		staged.edge_creates = normalized.batch.edge_creates;
		staged.node_label_replacements = normalized.batch.node_label_replacements;
		staged.node_deletes = normalized.batch.node_deletes;
		staged.edge_deletes = normalized.batch.edge_deletes;
		staged.creation_identity_plan = normalized.batch.creation_identity_plan;
		std::pmr::set<std::tuple<uint8_t, uint8_t, uint64_t>> targets(
            memory_owner_ ? memory_owner_.get() : std::pmr::new_delete_resource());
		for (const auto &patch : normalized.persisted_patches)
			targets.emplace(static_cast<uint8_t>(patch.entity.entity_kind),
				static_cast<uint8_t>(patch.entity.reference_kind), patch.entity.value);
		for (const auto &replacement : normalized.batch.property_replacements)
			if (!targets.contains({static_cast<uint8_t>(replacement.entity.entity_kind),
				static_cast<uint8_t>(replacement.entity.reference_kind), replacement.entity.value})) {
                detail::admitScopedPropertyCopies<LogicalGraphWriteProperty>(replacement.properties);
				staged.property_replacements.push_back(replacement);
            }
		return executeStagedBatch(normalized.batch, expectedFence, &staged,
			std::move(normalized.persisted_patches), normalized.entry_work, control);
	}

	LogicalGraphMutationBatchResult LogicalGraphTransactionOverlay::executeStagedBatch(
		const LogicalGraphMutationBatch &batch, std::optional<LogicalGraphMutationFence> expectedFence,
		const LogicalGraphMutationBatch *patchCompanion, std::vector<LogicalGraphPropertyPatchRequest> patches,
		size_t entryWork, const LogicalGraphMutationReadControl &control) {
        std::optional<utils::OwnedAllocationScope> writeAllocation;
        if (memory_owner_) writeAllocation.emplace(memory_owner_);
		if (control.checkpoint) control.checkpoint();
		if (transaction_.state() != LogicalGraphTransactionState::LGTTS_ACTIVE) {
			throw std::logic_error("logical transaction overlay cannot stage a mutation after transaction completion");
		}
		// A plan built from this overlay observes both the durable base fence and
		// the overlay revision. Comparing it to the base fence alone rejects every
		// statement after the first successful staged mutation.
		if (expectedFence.has_value() && (!expectedFence->isValid() || overlayFence() != expectedFence)) {
			throw LogicalGraphMutationConflict("logical transaction overlay mutation does not match its current read fence");
		}
		if (isEmpty(batch)) {
			return {};
		}
		if (logical_revision_ == std::numeric_limits<uint64_t>::max()) {
			throw std::overflow_error("logical transaction overlay revision overflowed");
		}

		const LogicalGraphMutationBatchResult preview = transaction_.previewMutationBatch(batch);
		StatementUndo undo(memory_owner_ ? memory_owner_.get() : std::pmr::new_delete_resource());
		try {
			applyBatch(batch, preview, undo);
			debug::PerfTrace::addValue("logical_graph.overlay.statement_undo_nodes", static_cast<int64_t>(
				std::min(undo.nodes.size(), static_cast<size_t>((std::numeric_limits<int64_t>::max)()))));
			debug::PerfTrace::addValue("logical_graph.overlay.statement_undo_edges", static_cast<int64_t>(
				std::min(undo.edges.size(), static_cast<size_t>((std::numeric_limits<int64_t>::max)()))));
			if (control.checkpoint) control.checkpoint();
			LogicalGraphMutationBatchResult staged = patchCompanion
				? transaction_.stageMutationBatchWithPropertyPatches(*patchCompanion, std::move(patches), entryWork, control)
				: transaction_.stageMutationBatch(batch);
			if (staged != preview) {
				throw std::logic_error("logical transaction changed a batch's provisional identity after validation");
			}
			static_assert(std::is_nothrow_move_constructible_v<LogicalGraphMutationBatchResult>);
			++logical_revision_;
			return staged;
		} catch (...) {
			restoreStatement(undo);
			throw;
		}
	}

	LogicalGraphReadSnapshot &LogicalGraphTransactionOverlay::baseSnapshot() const { return transaction_.readSnapshot(); }

	std::optional<LogicalGraphMutationFence> LogicalGraphTransactionOverlay::baseFence() const {
		return baseSnapshot().mutationFence();
	}

	std::optional<LogicalGraphSchemaIdentity> LogicalGraphTransactionOverlay::overlaySchemaIdentity() const {
		auto identity = baseSnapshot().bindingSchemaIdentity();
		// A mutable overlay cannot be treated as this transaction's immutable base.
		if (!identity) return std::nullopt;
		if (!identity->isValid()) throw std::logic_error("logical transaction base returned an invalid schema identity");
		if (identity->view_token != 0U) return std::nullopt;
		identity->view_token = view_token_;
		identity->logical_revision = logical_revision_;
		return identity;
	}

	std::optional<LogicalGraphMutationFence> LogicalGraphTransactionOverlay::overlayFence() const {
		std::optional<LogicalGraphMutationFence> fence = baseFence();
		if (fence.has_value()) {
			fence->logical_revision = logical_revision_;
		}
		return fence;
	}

	LogicalGraphTransactionOverlay::NodeState &LogicalGraphTransactionOverlay::ensureNode(uint64_t nodeId) {
		if (nodeId == 0U) {
			throw std::invalid_argument("logical transaction overlay requires a nonzero Node ID");
		}
		if (const auto existing = nodes_.find(nodeId); existing != nodes_.end()) {
			if (existing->second.deleted) {
				throw std::runtime_error("logical transaction overlay cannot mutate a deleted Node");
			}
			return existing->second;
		}
        detail::PropertyDecodeWorkspace decodedSource(memory_owner_);
        const auto source = baseSnapshot().readNodeShaped(nodeId, completeNodeReadShape());
        auto [inserted, success] = nodes_.try_emplace(nodeId,
                memory_owner_ ? memory_owner_.get() : std::pmr::new_delete_resource(), source);
		static_cast<void>(success);
		return inserted->second;
	}

	LogicalGraphTransactionOverlay::EdgeState &LogicalGraphTransactionOverlay::ensureEdge(uint64_t edgeId) {
		if (edgeId == 0U) {
			throw std::invalid_argument("logical transaction overlay requires a nonzero Edge ID");
		}
		if (const auto existing = edges_.find(edgeId); existing != edges_.end()) {
			if (existing->second.deleted) {
				throw std::runtime_error("logical transaction overlay cannot mutate a deleted Edge");
			}
			return existing->second;
		}
        detail::PropertyDecodeWorkspace decodedSource(memory_owner_);
        const auto source = baseSnapshot().readEdgeShaped(edgeId, completeEdgeReadShape());
        auto [inserted, success] = edges_.try_emplace(edgeId,
                memory_owner_ ? memory_owner_.get() : std::pmr::new_delete_resource(), source);
		static_cast<void>(success);
		return inserted->second;
	}

	void LogicalGraphTransactionOverlay::rememberNode(StatementUndo &undo, uint64_t nodeId) const {
		if (undo.nodes.contains(nodeId)) return;
		const auto found = nodes_.find(nodeId);
		if (found == nodes_.end()) undo.nodes.try_emplace(nodeId, std::nullopt);
        else undo.nodes.try_emplace(nodeId, std::in_place, found->second);
	}

	void LogicalGraphTransactionOverlay::rememberEdge(StatementUndo &undo, uint64_t edgeId) const {
		if (undo.edges.contains(edgeId)) return;
		const auto found = edges_.find(edgeId);
		if (found == edges_.end()) undo.edges.try_emplace(edgeId, std::nullopt);
        else undo.edges.try_emplace(edgeId, std::in_place, found->second);
	}

	void LogicalGraphTransactionOverlay::restoreStatement(StatementUndo &undo) noexcept {
		static_assert(std::is_nothrow_swappable_v<NodeState>);
		static_assert(std::is_nothrow_swappable_v<EdgeState>);
		// applyBatch never erases existing owners. Restore by swapping in their
		// saved values; rollback needs neither payload copying nor allocation.
		for (auto &[id, before] : undo.nodes) {
			if (!before) nodes_.erase(id);
			else { using std::swap; swap(nodes_.find(id)->second, *before); }
		}
		for (auto &[id, before] : undo.edges) {
			if (!before) edges_.erase(id);
			else { using std::swap; swap(edges_.find(id)->second, *before); }
		}
	}

	void LogicalGraphTransactionOverlay::applyBatch(const LogicalGraphMutationBatch &batch,
		const LogicalGraphMutationBatchResult &result, StatementUndo &undo) {
		if (result.created_node_ids.size() != batch.node_creates.size() ||
			result.created_edge_ids.size() != batch.edge_creates.size()) {
			throw std::logic_error("logical transaction overlay received an incomplete provisional identity result");
		}
		const auto resolveReference = [&result](const LogicalGraphMutationEntityReference &reference) {
			switch (reference.reference_kind) {
				case LogicalGraphMutationReferenceKind::LGRK_PERSISTED:
					return reference.value;
				case LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE:
					if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
						reference.value >= result.created_node_ids.size()) {
						throw std::invalid_argument("logical transaction overlay has an invalid created Node reference");
					}
					return result.created_node_ids[static_cast<size_t>(reference.value)];
				case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE:
					if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE ||
						reference.value >= result.created_edge_ids.size()) {
						throw std::invalid_argument("logical transaction overlay has an invalid created Edge reference");
					}
					return result.created_edge_ids[static_cast<size_t>(reference.value)];
			}
			throw std::invalid_argument("logical transaction overlay has an unknown entity reference kind");
		};
		for (size_t index = 0U; index < batch.node_creates.size(); ++index) {
			const uint64_t nodeId = result.created_node_ids[index];
			const LogicalGraphNodeCreateRequest &request = batch.node_creates[index];
            rememberNode(undo, nodeId);
            auto [entry, inserted] = nodes_.try_emplace(nodeId,
                memory_owner_ ? memory_owner_.get() : std::pmr::new_delete_resource());
            if (!inserted) throw std::logic_error("logical transaction overlay created a duplicate Node identity");
            auto &state = entry->second;
            state.node.node_id = nodeId; state.created = true;
            state.payload_budget.replaceLabels(state.node, request.labels);
            state.payload_budget.replaceProperties(state.node, request.properties);
		}

		for (const LogicalGraphNodeLabelMutationRequest &mutation: batch.node_label_replacements) {
			if (mutation.node.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
				throw std::invalid_argument("logical transaction overlay label mutation requires a Node");
			}
			const uint64_t nodeId = resolveReference(mutation.node);
			rememberNode(undo, nodeId);
			auto &state = ensureNode(nodeId);
            state.payload_budget.replaceLabels(state.node, mutation.labels);
		}

		for (size_t index = 0U; index < batch.edge_creates.size(); ++index) {
			const LogicalGraphBatchEdgeCreateRequest &request = batch.edge_creates[index];
			if (request.source.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
				request.target.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
				throw std::invalid_argument("logical transaction overlay Edge creation has invalid endpoints");
			}
			const uint64_t sourceId = resolveReference(request.source);
			const uint64_t targetId = resolveReference(request.target);
			if (!isNodeVisible(sourceId) || !isNodeVisible(targetId)) {
				throw std::runtime_error("logical transaction overlay Edge creation targets a deleted Node");
			}
			const uint64_t edgeId = result.created_edge_ids[index];
            rememberEdge(undo, edgeId);
            auto [entry, inserted] = edges_.try_emplace(edgeId,
                memory_owner_ ? memory_owner_.get() : std::pmr::new_delete_resource());
            if (!inserted) throw std::logic_error("logical transaction overlay created a duplicate Edge identity");
            auto &state = entry->second;
            state.edge.edge_id = edgeId; state.edge.source_node_id = sourceId; state.edge.target_node_id = targetId;
            state.created = true;
            state.payload_budget.replaceType(state.edge, request.type);
			if (baseSnapshot().supportsNativeRelationshipTypeTokens()) {
				state.relationship_type_token = baseSnapshot().findRelationshipTypeToken(request.type).value_or(0U);
			}
            state.payload_budget.replaceProperties(state.edge, request.properties);
		}

		for (const LogicalGraphPropertyMutationRequest &mutation: batch.property_replacements) {
			const uint64_t entityId = resolveReference(mutation.entity);
			if (mutation.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
				rememberNode(undo, entityId);
				NodeState &state = ensureNode(entityId);
				state.payload_budget.replaceProperties(state.node, mutation.properties);
			} else if (mutation.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE) {
				rememberEdge(undo, entityId);
				EdgeState &state = ensureEdge(entityId);
				state.payload_budget.replaceProperties(state.edge, mutation.properties);
			} else {
				throw std::invalid_argument("logical transaction overlay Property mutation has an unknown entity kind");
			}
		}

		for (const LogicalGraphEdgeDeleteRequest &request: batch.edge_deletes) {
			rememberEdge(undo, request.edge_id);
			EdgeState &edge = ensureEdge(request.edge_id);
			edge.deleted = true;
		}
		for (const LogicalGraphNodeDeleteRequest &request: batch.node_deletes) {
			rememberNode(undo, request.node_id);
			NodeState &node = ensureNode(request.node_id);
			if (request.detach) {
				std::pmr::set<uint64_t> detachedIds(
                    memory_owner_ ? memory_owner_.get() : std::pmr::new_delete_resource());
				const auto markIncident = [this, &undo, &detachedIds, &request](const LogicalGraphEdge &edge) {
					if (const auto existing = edges_.find(edge.edge_id);
						existing != edges_.end() && existing->second.deleted) {
						return true;
					}
					if (detachedIds.insert(edge.edge_id).second) {
						if (detachedIds.size() > request.maximum_detach_edges) {
							throw std::runtime_error("logical transaction overlay detach delete exceeded its explicit Edge bound");
						}
						rememberEdge(undo, edge.edge_id);
						ensureEdge(edge.edge_id).deleted = true;
					}
					return true;
				};
				const auto markIncidentTopology = [&markIncident](const LogicalGraphEdgeTopology &topology) {
					const LogicalGraphEdge edge{topology.edge_id, topology.source_node_id, topology.target_node_id, topology.type};
					return markIncident(edge);
				};
				baseSnapshot().visitAdjacentEdgeTopologies(request.node_id, LogicalGraphReadDirection::LGRD_OUTGOING,
														 request.maximum_detach_edges, markIncidentTopology);
				baseSnapshot().visitAdjacentEdgeTopologies(request.node_id, LogicalGraphReadDirection::LGRD_INCOMING,
														 request.maximum_detach_edges, markIncidentTopology);
				for (auto &[edgeId, edge]: edges_) {
					if (edge.created && !edge.deleted &&
						(edge.edge.source_node_id == request.node_id || edge.edge.target_node_id == request.node_id)) {
						if (!detachedIds.insert(edgeId).second) {
							continue;
						}
						if (detachedIds.size() > request.maximum_detach_edges) {
							throw std::runtime_error("logical transaction overlay detach delete exceeded its explicit Edge bound");
						}
						rememberEdge(undo, edgeId);
						edge.deleted = true;
					}
				}
			} else if (hasVisibleIncidentEdge(request.node_id)) {
				throw std::runtime_error(
						"logical transaction overlay non-detach delete requires an isolated Node");
			}
			node.deleted = true;
		}
	}

	bool LogicalGraphTransactionOverlay::hasVisibleIncidentEdge(uint64_t nodeId) const {
		if (nodeId == 0U || edges_.size() == std::numeric_limits<size_t>::max()) {
			throw std::invalid_argument("logical transaction overlay cannot bound an incident Edge inspection");
		}
		for (const auto &[edgeId, edge]: edges_) {
			if (edge.created && !edge.deleted &&
				(edge.edge.source_node_id == nodeId || edge.edge.target_node_id == nodeId)) {
				return true;
			}
		}
		if (const auto node = nodes_.find(nodeId); node != nodes_.end() && node->second.created) {
			// A transaction-created Node has no base-snapshot adjacency. Every
			// possible incident Edge is already represented in this overlay.
			return false;
		}
		// Every persisted Edge already hidden by the overlay has an entry in
		// `edges_`. Scanning one more record than that set either finds a visible
		// relationship or proves the remaining adjacency is entirely deleted.
		const size_t maximumEdges = edges_.size() + 1U;
		for (const LogicalGraphReadDirection direction:
				{LogicalGraphReadDirection::LGRD_OUTGOING, LogicalGraphReadDirection::LGRD_INCOMING}) {
			bool found = false;
			baseSnapshot().visitAdjacentEdgeTopologies(nodeId, direction, maximumEdges,
					[this, &found](const LogicalGraphEdgeTopology &topology) {
				const LogicalGraphEdge edge{topology.edge_id, topology.source_node_id, topology.target_node_id, topology.type};
				if (isEdgeVisible(edge, edge.edge_id)) {
					found = true;
					return false;
				}
				return true;
				});
			if (found) {
				return true;
			}
		}
		return false;
	}

	bool LogicalGraphTransactionOverlay::isNodeVisible(uint64_t nodeId) const {
		if (const auto state = nodes_.find(nodeId); state != nodes_.end()) {
			return !state->second.deleted;
		}
		if (const auto cached = persisted_node_visibility_.find(nodeId); cached != persisted_node_visibility_.end()) {
			return cached->second;
		}
		const bool visible = baseSnapshot().containsNode(nodeId);
		persisted_node_visibility_.emplace(nodeId, visible);
		return visible;
	}

	bool LogicalGraphTransactionOverlay::isEdgeVisible(const LogicalGraphEdge &edge, uint64_t edgeId) const {
		if (const auto state = edges_.find(edgeId); state != edges_.end() && state->second.deleted) {
			return false;
		}
		return isNodeVisible(edge.source_node_id) && isNodeVisible(edge.target_node_id);
	}

} // namespace graph::storage
