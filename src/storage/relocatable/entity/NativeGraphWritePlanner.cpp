/**
 * @file NativeGraphWritePlanner.cpp
 * @brief Snapshot-pinned native Entity/Page graph write planning.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/adjacency/GlobalAdjacencyMutationMerger.hpp"
#include "graph/storage/relocatable/index/NativeBulkRun.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"

namespace graph::storage {

	struct NativeGraphWritePlannerContext {
		RelocatableMetadataStore *metadata = nullptr;
		EntityPageStore *entities = nullptr;
		EntityLabelSetStore *labels = nullptr;
		PropertyCollectionRootStore *properties = nullptr;
		BTreePageStore *indexes = nullptr;
		NativeGraphWritePlannerOptions options;
		std::mutex publication_mutex;
		struct InsertionTarget {
			PageId page_id{};
			EntityPageRecordKind kind = EntityPageRecordKind::EPRK_NODE;
			CatalogEntry pool;
			uint16_t remaining = 0U;
		};
		struct InsertionTargetCache {
			PhysicalPageRef node_directory_root{};
			PhysicalPageRef edge_directory_root{};
			std::vector<InsertionTarget> targets;
			bool valid = false;
		};
		std::mutex insertion_cache_mutex;
		InsertionTargetCache insertion_cache;
	};

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] LogSequenceNumber nextCheckpointLsn(const Superblock &base) {
			if (!base.checkpoint_lsn.isValid() || base.checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) { // ZYX_COV_EXCL_LINE: published snapshots reject invalid or exhausted checkpoint state before planning.
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "native graph write cannot advance the checkpoint LSN");
			}
			return LogSequenceNumber(base.checkpoint_lsn.value() + 1U);
		}

		[[nodiscard]] size_t maximumAdjacencyTreeEntries(const NativeGraphWritePlannerOptions &options) {
			return options.maximum_global_adjacency_tree_pages *
				   static_cast<size_t>(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
		}

		[[nodiscard]] NativeBulkRunFragmentLayout planDeferredAdjacencyFragments(
				std::span<const BTreeScalarIndexOperation> operations) {
			if (operations.empty()) { // ZYX_COV_EXCL_LINE: the deferred planner sets this helper only after canonical operations are present.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph deferred adjacency bulk run requires ordered operations");
			}
			const size_t payloadLimit = RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES;
			NativeBulkRunFragmentLayoutBuilder builder(payloadLimit);
			for (const BTreeScalarIndexOperation &operation: operations) {
				if ((operation.kind != BTreeScalarIndexOperationKind::BSIOP_UPSERT && // ZYX_COV_EXCL_LINE: canonicalGlobalAdjacencyOperations emits only key-only upserts or erases.
					 operation.kind != BTreeScalarIndexOperationKind::BSIOP_ERASE) ||
					operation.value != 0U || operation.key.empty()) {
					fail(FormatError::FERR_INVALID_STATE,
							 "native graph deferred adjacency bulk run received a noncanonical operation");
				}
				const size_t entryBytes = RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES +
								  RSS_BTREE_ENTRY_HEADER_BYTES + operation.key.size();
				if (entryBytes > payloadLimit - RSS_NATIVE_BULK_RUN_HEADER_BYTES) { // bounded record sizing rejects oversized keys before fragment encoding.
					fail(FormatError::FERR_INVALID_HEADER,
						 "native graph adjacency key cannot fit in one bounded scratch fragment");
				}
				builder.append(entryBytes);
			}
			NativeBulkRunFragmentLayout layout = builder.finish();
			const size_t manifestCapacity =
					(payloadLimit - RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES) / RSS_NATIVE_BULK_RUN_MANIFEST_ENTRY_BYTES;
			if (layout.fragments.size() > manifestCapacity) { // ZYX_COV_EXCL_LINE: the bounded operation and segment contracts cap fragment fan-out before this helper is called.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph adjacency bulk run exceeds one bounded manifest fan-out");
			}
			return layout;
		}

		[[nodiscard]] NativeBulkRunFragment deferredAdjacencyFragment(
				std::span<const BTreeScalarIndexOperation> operations, const NativeBulkRunFragmentRange &range) {
			if (range.entry_count == 0U || range.first_entry_ordinal > operations.size() ||
				range.entry_count > operations.size() - range.first_entry_ordinal) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph deferred adjacency bulk run received an invalid planned source range");
			}
			NativeBulkRunFragment fragment;
			fragment.entries.reserve(range.entry_count);
			for (const BTreeScalarIndexOperation &operation:
				 operations.subspan(range.first_entry_ordinal, range.entry_count)) {
				const uint64_t operationTag = operation.kind == BTreeScalarIndexOperationKind::BSIOP_UPSERT ? 1U : 2U;
				// Scratch preserves the final-state operation tag. Final adjacency
				// leaves remain key-only; the tag is consumed only by the deferred
				// merge before any public B+Tree page is materialized.
				fragment.entries.push_back({operation.key, operationTag});
			}
			if (fragment.entries.empty()) { // ZYX_COV_EXCL_LINE: the immutable layout contains only nonempty ranges.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph deferred adjacency bulk run lost a planned scratch fragment");
			}
			return fragment;
		}

		[[nodiscard]] bool isLogicalIdLeaseOnlyAdvance(const RelocatableMetadataStore &metadata,
													 const Superblock &base, const Superblock &current) {
			const auto advancedOrExhausted = [](uint64_t before, uint64_t after) {
				const uint8_t valid = static_cast<uint8_t>(before != 0U) &
									  (static_cast<uint8_t>(after == before) | static_cast<uint8_t>(after == 0U) |
									   static_cast<uint8_t>(after > before));
				return valid != 0U;
			};
			const uint8_t advanced =
					static_cast<uint8_t>(current.generation > base.generation) &
					static_cast<uint8_t>(current.checkpoint_lsn > base.checkpoint_lsn) &
					static_cast<uint8_t>(advancedOrExhausted(base.next_logical_node_id, current.next_logical_node_id)) &
					static_cast<uint8_t>(advancedOrExhausted(base.next_logical_edge_id, current.next_logical_edge_id));
		const bool identityOnlyBootstrap =
				(!base.usesPageDirectoryBootstrap() && !current.usesPageDirectoryBootstrap())
						? current.page_directory_root == base.page_directory_root
						: metadata.isIdentityOnlyBootstrapAdvance(base, current);
		const uint8_t unchanged =
				static_cast<uint8_t>(current.database_id == base.database_id) &
				static_cast<uint8_t>(current.flags == base.flags) &
				static_cast<uint8_t>(current.data_region_offset == base.data_region_offset) &
				static_cast<uint8_t>(current.catalog_root == base.catalog_root) &
				static_cast<uint8_t>(identityOnlyBootstrap) &
				static_cast<uint8_t>(current.extent_directory_root == base.extent_directory_root) &
				static_cast<uint8_t>(current.allocator_root == base.allocator_root) &
				static_cast<uint8_t>(current.node_entity_directory_root == base.node_entity_directory_root) &
				static_cast<uint8_t>(current.edge_entity_directory_root == base.edge_entity_directory_root) &
				static_cast<uint8_t>(current.entity_property_directory_root == base.entity_property_directory_root) &
				static_cast<uint8_t>(current.index_root_directory_root == base.index_root_directory_root) &
				static_cast<uint8_t>(current.vector_location_directory_root == base.vector_location_directory_root) &
				static_cast<uint8_t>(current.vector_manifest_directory_root == base.vector_manifest_directory_root) &
				static_cast<uint8_t>(current.large_value_manifest_directory_root == base.large_value_manifest_directory_root) &
				static_cast<uint8_t>(current.state_directory_root == base.state_directory_root) &
				static_cast<uint8_t>(current.retire_queue_root == base.retire_queue_root) &
				static_cast<uint8_t>(current.metadata_emergency_reserve == base.metadata_emergency_reserve) &
				static_cast<uint8_t>(current.next_logical_page_id == base.next_logical_page_id) &
				static_cast<uint8_t>(current.next_logical_segment_id == base.next_logical_segment_id) &
				static_cast<uint8_t>(current.next_logical_vector_id == base.next_logical_vector_id) &
				static_cast<uint8_t>(current.next_large_value_id == base.next_large_value_id) &
				static_cast<uint8_t>(current.next_property_key_token == base.next_property_key_token) &
				static_cast<uint8_t>(current.next_schema_token_id == base.next_schema_token_id);
			if ((advanced & unchanged) == 0U) {
				return false;
			}
			return true;
		}

		void validateLabels(std::span<const uint64_t> labels) {
			if (labels.size() > RSS_ENTITY_MAX_LABELS ||
				std::ranges::any_of(labels, [](uint64_t value) { return value == 0U; }) ||
				!std::ranges::is_sorted(labels) || std::ranges::adjacent_find(labels) != labels.end()) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native graph write requires a bounded, strictly ordered, nonzero label set");
			}
		}

		[[nodiscard]] constexpr bool isReservedInternalIndexId(uint64_t indexId) {
			return indexId == RSS_LARGE_VALUE_CONTENT_INDEX_ID || indexId == RSS_GLOBAL_ADJACENCY_INDEX_ID;
		}

		void validatePlannerOptions(const NativeGraphWritePlannerOptions &options) {
			const uint8_t invalid =
					static_cast<uint8_t>(!options.first_slot.isValid()) |
					static_cast<uint8_t>(!options.last_slot.isValid()) |
					static_cast<uint8_t>(options.first_slot > options.last_slot) |
					static_cast<uint8_t>(options.entity_pool_owner_id == 0U) |
					static_cast<uint8_t>(options.maximum_adjacency_read_edges == 0U) |
					static_cast<uint8_t>(options.maximum_adjacency_read_edges == std::numeric_limits<size_t>::max()) |
					static_cast<uint8_t>(options.maximum_global_adjacency_tree_pages == 0U) |
					static_cast<uint8_t>(options.maximum_global_adjacency_tree_pages ==
														 std::numeric_limits<size_t>::max()) |
					static_cast<uint8_t>(options.maximum_global_adjacency_tree_pages >
														 std::numeric_limits<size_t>::max() / RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
					static_cast<uint8_t>(options.maximum_global_adjacency_write_entries == 0U) |
					static_cast<uint8_t>(options.maximum_global_adjacency_write_entries ==
														 std::numeric_limits<size_t>::max()) |
					static_cast<uint8_t>(options.global_adjacency_bulk_rebuild_entry_threshold == 0U) |
					static_cast<uint8_t>(options.logical_id_lease_block_size == 0U) |
					static_cast<uint8_t>(options.logical_id_lease_block_size == std::numeric_limits<size_t>::max()) |
					static_cast<uint8_t>(options.maximum_logical_id_reservation == 0U) |
					static_cast<uint8_t>(options.maximum_logical_id_reservation == std::numeric_limits<size_t>::max()) |
					static_cast<uint8_t>(!options.property_roots.first_slot.isValid()) |
					static_cast<uint8_t>(!options.property_roots.last_slot.isValid()) |
					static_cast<uint8_t>(options.property_roots.first_slot > options.property_roots.last_slot) |
					static_cast<uint8_t>(options.property_roots.page_owner_id == 0U);
			if (invalid != 0U) {
				throw std::invalid_argument(
						"native graph write planner requires bounded Entity, Property, and adjacency pool options");
			}
		}

		void requireAdjacencyOperationCapacity(size_t stagedOperationCount, size_t additionalOperationCount,
											   size_t maximumOperationCount) {
			if (additionalOperationCount > maximumOperationCount ||
				stagedOperationCount > maximumOperationCount - additionalOperationCount) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph global adjacency mutation exceeds its configured entry bound");
			}
		}

		[[nodiscard]] NativeGraphLogicalIdReservation
		leaseLogicalIds(NativeGraphWritePlannerContext &context, bool nodes, uint64_t count) {
			if (count == 0U) { // ZYX_COV_EXCL_LINE: callers guard zero reservations before entering this private leasing helper.
				return {};
			}
			if (count > static_cast<uint64_t>(context.options.maximum_logical_id_reservation)) { // ZYX_COV_EXCL_LINE: leaseAndAppendLogicalIdRanges applies the same bound before dispatch.
				fail(FormatError::FERR_INVALID_HEADER,
					 "native graph logical-ID reservation exceeds its configured transaction work bound");
			}
			return context.metadata->acquireNativeGraphLogicalIds(nodes ? count : 0U, nodes ? 0U : count,
														   static_cast<uint64_t>(context.options.logical_id_lease_block_size));
		}

		[[nodiscard]] uint64_t logicalIdRangeCount(std::span<const NativeGraphLogicalIdLeaseRange> ranges,
													  size_t maximumCount) {
			uint64_t total = 0U;
			for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
				if (range.first_id == 0U || range.count == 0U || // metadata-issued logical-ID ranges are validated before planner adoption.
					range.count > std::numeric_limits<uint64_t>::max() - total) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph transaction retained an invalid logical-ID lease");
				}
				total += range.count;
			}
			if (total > static_cast<uint64_t>(maximumCount)) { // callers pass the same configured bound used by lease acquisition.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph transaction retained more logical-ID reservations than its configured bound");
			}
			return total;
		}

		void appendLogicalIdRanges(std::vector<NativeGraphLogicalIdLeaseRange> &target,
								   std::span<const NativeGraphLogicalIdLeaseRange> source, size_t maximumCount) {
			const uint64_t existingCount = logicalIdRangeCount(target, maximumCount);
			std::vector<NativeGraphLogicalIdLeaseRange> candidate = target;
			uint64_t total = existingCount;
			for (const NativeGraphLogicalIdLeaseRange &range: source) {
				if (range.first_id == 0U || range.count == 0U ||
					range.count > static_cast<uint64_t>(maximumCount) - total) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph logical-ID reservations exceed their configured transaction work bound");
				}
				// Keep acquisition boundaries exact. The metadata-store capability
				// registry validates one returned range at a time, so coalescing two
				// adjacent acquisitions would erase their ownership boundary.
				candidate.push_back(range);
				total += range.count;
			}
			target.swap(candidate);
		}

		void leaseAndAppendLogicalIdRanges(NativeGraphWritePlannerContext &context,
											  std::vector<NativeGraphLogicalIdLeaseRange> &target, bool nodes,
											  uint64_t count) {
			if (count == 0U) { // ZYX_COV_EXCL_LINE: public reservation entry points return before invoking this helper for zero counts.
				return;
			}
			const size_t maximumCount = context.options.maximum_logical_id_reservation;
			const uint64_t existingCount = logicalIdRangeCount(target, maximumCount);
			if (count > static_cast<uint64_t>(maximumCount) ||
				existingCount > static_cast<uint64_t>(maximumCount) - count) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph logical-ID reservations exceed their configured transaction work bound");
			}

			NativeGraphLogicalIdReservation reservation = leaseLogicalIds(context, nodes, count);
			const std::span<const NativeGraphLogicalIdLeaseRange> ranges =
					nodes ? std::span<const NativeGraphLogicalIdLeaseRange>(reservation.node_ranges)
						  : std::span<const NativeGraphLogicalIdLeaseRange>(reservation.edge_ranges);
			try {
				appendLogicalIdRanges(target, ranges, maximumCount);
			} catch (...) { // ZYX_COV_EXCL_LINE: metadata-issued ranges satisfy appendLogicalIdRanges invariants; only external adoption can be malformed.
				// The durable high-water mark intentionally remains advanced, but the
				// process-local capability must not outlive a rejected acquisition.
				context.metadata->abandonNativeGraphLogicalIdReservation(reservation);
				throw;
			}
		}

		void appendAuxiliary(EntityPageAuxiliaryMutationBatch &target, EntityPageAuxiliaryMutationBatch source) {
			target.replacements.insert(target.replacements.end(), std::make_move_iterator(source.replacements.begin()),
									   std::make_move_iterator(source.replacements.end()));
			target.appends.insert(target.appends.end(), std::make_move_iterator(source.appends.begin()),
								  std::make_move_iterator(source.appends.end()));
			target.new_pools.insert(target.new_pools.end(), std::make_move_iterator(source.new_pools.begin()),
									std::make_move_iterator(source.new_pools.end()));
			target.node_update_bindings.insert(target.node_update_bindings.end(),
											   std::make_move_iterator(source.node_update_bindings.begin()),
											   std::make_move_iterator(source.node_update_bindings.end()));
			target.node_insertion_bindings.insert(target.node_insertion_bindings.end(),
												  std::make_move_iterator(source.node_insertion_bindings.begin()),
												  std::make_move_iterator(source.node_insertion_bindings.end()));
		}

		struct FreshEntityPool {
			SegmentId segment_id{};
			uint64_t owner_id = 0U;
			Generation generation{1U};
			PhysicalSlotId first_slot{};
			PhysicalSlotId last_slot{};
			size_t initial_frame_count = 0U;
			size_t replacement_frame_count = 0U;

			[[nodiscard]] size_t reservedFrameCount() const { return initial_frame_count + replacement_frame_count; }

			[[nodiscard]] EntityPageFreshPoolDestination replacementDestination() const {
				return {segment_id, owner_id, generation, first_slot, last_slot};
			}
		};

		struct EntityPageReplacementDestination {
			CatalogEntry current_pool;
			std::optional<EntityPageFreshPoolDestination> fresh_pool;
		};

		[[nodiscard]] bool isCurrentPool(const CatalogEntry &pool) {
			const uint8_t valid = static_cast<uint8_t>(pool.kind == SegmentKind::SGK_ENTITY_PAGE_POOL) &
								  static_cast<uint8_t>(pool.state == SegmentState::SGS_SEALED) &
								  static_cast<uint8_t>(pool.segment_id.isValid()) &
								  static_cast<uint8_t>(pool.generation.isValid());
			return valid != 0U;
		}

	} // namespace

	NativeGraphWriteTransaction::NativeGraphWriteTransaction(std::shared_ptr<NativeGraphWritePlannerContext> context,
															 MetadataSnapshotPin snapshot) :
			context_(std::move(context)), snapshot_(std::move(snapshot)),
			state_(NativeGraphWriteTransactionState::NGWTS_ACTIVE) {
		next_vector_id_ = snapshot_.superblock().next_logical_vector_id;
	}

	NativeGraphWriteTransaction::~NativeGraphWriteTransaction() { rollback(); }

	NativeGraphWriteTransaction::NativeGraphWriteTransaction(NativeGraphWriteTransaction &&other) noexcept :
		context_(std::move(other.context_)), snapshot_(std::move(other.snapshot_)), state_(other.state_),
		next_vector_id_(other.next_vector_id_), node_id_ranges_(std::move(other.node_id_ranges_)),
		edge_id_ranges_(std::move(other.edge_id_ranges_)), nodes_(std::move(other.nodes_)),
		edges_(std::move(other.edges_)), global_adjacency_operations_(std::move(other.global_adjacency_operations_)),
		global_adjacency_operations_canonical_(other.global_adjacency_operations_canonical_),
		global_adjacency_plan_(std::move(other.global_adjacency_plan_)),
		global_adjacency_retirement_plan_(std::move(other.global_adjacency_retirement_plan_)),
		global_adjacency_initial_plan_(std::move(other.global_adjacency_initial_plan_)),
		global_adjacency_deferred_bulk_(other.global_adjacency_deferred_bulk_),
		properties_(std::move(other.properties_)), scalar_index_mutations_(std::move(other.scalar_index_mutations_)),
		initial_scalar_index_mutations_(std::move(other.initial_scalar_index_mutations_)),
		vector_bootstraps_(std::move(other.vector_bootstraps_)), vector_deltas_(std::move(other.vector_deltas_)) {
		other.state_ = NativeGraphWriteTransactionState::NGWTS_ROLLED_BACK;
		other.next_vector_id_ = 0U;
		other.global_adjacency_operations_canonical_ = false;
		other.global_adjacency_deferred_bulk_ = false;
	}

	NativeGraphWriteTransaction &NativeGraphWriteTransaction::operator=(NativeGraphWriteTransaction &&other) noexcept {
		if (this == &other) {
			return *this;
		}
		// A live target owns snapshot and logical-ID capabilities that must be
		// abandoned before replacement; default move assignment would leak them.
		rollback();
		context_ = std::move(other.context_);
		snapshot_ = std::move(other.snapshot_);
		state_ = other.state_;
		next_vector_id_ = other.next_vector_id_;
		node_id_ranges_ = std::move(other.node_id_ranges_);
		edge_id_ranges_ = std::move(other.edge_id_ranges_);
		nodes_ = std::move(other.nodes_);
		edges_ = std::move(other.edges_);
		global_adjacency_operations_ = std::move(other.global_adjacency_operations_);
		global_adjacency_operations_canonical_ = other.global_adjacency_operations_canonical_;
		global_adjacency_plan_ = std::move(other.global_adjacency_plan_);
		global_adjacency_retirement_plan_ = std::move(other.global_adjacency_retirement_plan_);
		global_adjacency_initial_plan_ = std::move(other.global_adjacency_initial_plan_);
		global_adjacency_deferred_bulk_ = other.global_adjacency_deferred_bulk_;
		properties_ = std::move(other.properties_);
		scalar_index_mutations_ = std::move(other.scalar_index_mutations_);
		initial_scalar_index_mutations_ = std::move(other.initial_scalar_index_mutations_);
		vector_bootstraps_ = std::move(other.vector_bootstraps_);
		vector_deltas_ = std::move(other.vector_deltas_);
		other.state_ = NativeGraphWriteTransactionState::NGWTS_ROLLED_BACK;
		other.next_vector_id_ = 0U;
		other.global_adjacency_operations_canonical_ = false;
		other.global_adjacency_deferred_bulk_ = false;
		return *this;
	}

	bool NativeGraphWriteTransaction::isActive() const {
		return state_ == NativeGraphWriteTransactionState::NGWTS_ACTIVE;
	}

	const Superblock &NativeGraphWriteTransaction::baseSuperblock() const {
		requireActive("inspect its base Superblock");
		return snapshot_.superblock();
	}

	const MetadataSnapshotPin &NativeGraphWriteTransaction::baseSnapshot() const {
		requireActive("inspect its pinned metadata snapshot");
		return snapshot_;
	}

	void NativeGraphWriteTransaction::requireActive(const char *operation) const {
		if (!isActive()) {
			fail(FormatError::FERR_INVALID_STATE, "native graph write transaction is no longer active");
		}
		static_cast<void>(operation);
	}

	NativeGraphWriteTransaction::NodeChange &NativeGraphWriteTransaction::mutableNode(uint64_t nodeId) {
		requireActive("modify a Node");
		if (nodeId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph write requires a nonzero Node ID");
		}
		const auto existing = nodes_.find(nodeId);
		if (existing != nodes_.end()) {
			if (existing->second.erased) {
				fail(FormatError::FERR_INVALID_STATE, "native graph write cannot modify an erased Node");
			}
			return existing->second;
		}
		NodeChange change;
		change.record = context_->entities->readNode(snapshot_, nodeId);
		auto [iterator, inserted] = nodes_.emplace(nodeId, std::move(change));
		static_cast<void>(inserted);
		return iterator->second;
	}

	NativeGraphWriteTransaction::EdgeChange &NativeGraphWriteTransaction::mutableEdge(uint64_t edgeId) {
		requireActive("modify an Edge");
		if (edgeId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph write requires a nonzero Edge ID");
		}
		const auto existing = edges_.find(edgeId);
		if (existing != edges_.end()) {
			if (existing->second.erased) {
				fail(FormatError::FERR_INVALID_STATE, "native graph write cannot modify an erased Edge");
			}
			return existing->second;
		}
		EdgeChange change;
		change.record = context_->entities->readEdge(snapshot_, edgeId);
		auto [iterator, inserted] = edges_.emplace(edgeId, std::move(change));
		static_cast<void>(inserted);
		return iterator->second;
	}

	uint64_t NativeGraphWriteTransaction::createNode(std::span<const uint64_t> labels) {
		requireActive("create a Node");
		validateLabels(labels);
		leaseAndAppendLogicalIdRanges(*context_, node_id_ranges_, true, 1U);
		const uint64_t nodeId = node_id_ranges_.back().first_id;
		NodeChange change;
		change.inserted = true;
		change.labels = std::vector<uint64_t>(labels.begin(), labels.end());
		const bool inserted = nodes_.emplace(nodeId, std::move(change)).second;
		if (!inserted) { // ZYX_COV_EXCL_LINE: each newly leased logical ID is unique and cannot already exist in this transaction map.
			fail(FormatError::FERR_INVALID_STATE, "native graph write allocated a duplicate Node ID");
		}
		return nodeId;
	}

	uint64_t NativeGraphWriteTransaction::createEdge(uint64_t sourceNodeId, uint64_t targetNodeId, uint64_t typeId) {
		requireActive("create an Edge");
		// Reserve both directions before changing any staged Entity state. This keeps
		// a rejected bounded write from leaving a one-direction adjacency mutation.
		requireAdjacencyOperationCapacity(global_adjacency_operations_.size(), 2U,
										  context_->options.maximum_global_adjacency_write_entries);
		static_cast<void>(mutableNode(sourceNodeId));
		if (sourceNodeId != targetNodeId) {
			static_cast<void>(mutableNode(targetNodeId));
		}
		leaseAndAppendLogicalIdRanges(*context_, edge_id_ranges_, false, 1U);
		const uint64_t edgeId = edge_id_ranges_.back().first_id;
		EdgeChange change;
		change.inserted = true;
		change.record.source_node_id = sourceNodeId;
		change.record.target_node_id = targetNodeId;
		change.record.type_id = typeId;
		const bool inserted = edges_.emplace(edgeId, std::move(change)).second;
		if (!inserted) { // ZYX_COV_EXCL_LINE: an adopted lease is validated as unique by the metadata capability registry.
			fail(FormatError::FERR_INVALID_STATE, "native graph write allocated a duplicate Edge ID");
		}
		stageGlobalAdjacencyMutation(GlobalAdjacencyDirection::GAD_OUT, sourceNodeId, edgeId,
									 BTreeScalarIndexOperationKind::BSIOP_UPSERT);
		stageGlobalAdjacencyMutation(GlobalAdjacencyDirection::GAD_IN, targetNodeId, edgeId,
									 BTreeScalarIndexOperationKind::BSIOP_UPSERT);
		return edgeId;
	}

	void NativeGraphWriteTransaction::adoptLogicalIdReservation(NativeGraphLogicalIdReservation reservation) {
		requireActive("adopt logical-ID reservations");
		if (!nodes_.empty() || !edges_.empty() || !node_id_ranges_.empty() || !edge_id_ranges_.empty()) {
			fail(FormatError::FERR_INVALID_STATE,
					 "native graph logical-ID reservations must be adopted before staging Entity mutations");
		}
		// Validate both entity kinds before changing either retained range. An
		// externally issued reservation stays whole when one kind is malformed.
		std::vector<NativeGraphLogicalIdLeaseRange> nextNodeRanges = node_id_ranges_;
		std::vector<NativeGraphLogicalIdLeaseRange> nextEdgeRanges = edge_id_ranges_;
		appendLogicalIdRanges(nextNodeRanges, reservation.node_ranges, context_->options.maximum_logical_id_reservation);
		appendLogicalIdRanges(nextEdgeRanges, reservation.edge_ranges, context_->options.maximum_logical_id_reservation);
		node_id_ranges_.swap(nextNodeRanges);
		edge_id_ranges_.swap(nextEdgeRanges);
	}

	uint64_t NativeGraphWriteTransaction::createNodeWithReservedId(uint64_t nodeId, std::span<const uint64_t> labels) {
		requireActive("create a Node with a logical-ID reservation");
		validateLabels(labels);
		if (nodeId == 0U ||
			!std::ranges::any_of(node_id_ranges_, [nodeId](const NativeGraphLogicalIdLeaseRange &range) {
				return range.contains(nodeId);
			})) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph write attempted to create a Node outside its adopted logical-ID lease");
		}
		NodeChange change;
		change.inserted = true;
		change.labels = std::vector<uint64_t>(labels.begin(), labels.end());
		if (!nodes_.emplace(nodeId, std::move(change)).second) {
			fail(FormatError::FERR_INVALID_STATE, "native graph write adopted a duplicate Node ID");
		}
		return nodeId;
	}

	uint64_t NativeGraphWriteTransaction::createEdgeWithReservedId(uint64_t edgeId, uint64_t sourceNodeId,
																   uint64_t targetNodeId, uint64_t typeId) {
		requireActive("create an Edge with a logical-ID reservation");
		requireAdjacencyOperationCapacity(global_adjacency_operations_.size(), 2U,
										  context_->options.maximum_global_adjacency_write_entries);
		if (edgeId == 0U ||
			!std::ranges::any_of(edge_id_ranges_, [edgeId](const NativeGraphLogicalIdLeaseRange &range) {
				return range.contains(edgeId);
			})) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph write attempted to create an Edge outside its adopted logical-ID lease");
		}
		static_cast<void>(mutableNode(sourceNodeId));
		if (sourceNodeId != targetNodeId) {
			static_cast<void>(mutableNode(targetNodeId));
		}
		EdgeChange change;
		change.inserted = true;
		change.record.source_node_id = sourceNodeId;
		change.record.target_node_id = targetNodeId;
		change.record.type_id = typeId;
		if (!edges_.emplace(edgeId, std::move(change)).second) {
			fail(FormatError::FERR_INVALID_STATE, "native graph write adopted a duplicate Edge ID");
		}
		stageGlobalAdjacencyMutation(GlobalAdjacencyDirection::GAD_OUT, sourceNodeId, edgeId,
									 BTreeScalarIndexOperationKind::BSIOP_UPSERT);
		stageGlobalAdjacencyMutation(GlobalAdjacencyDirection::GAD_IN, targetNodeId, edgeId,
									 BTreeScalarIndexOperationKind::BSIOP_UPSERT);
		return edgeId;
	}

	void NativeGraphWriteTransaction::reserveNodeIds(uint64_t count) {
		requireActive("reserve Node IDs");
		if (count == 0U) {
			return;
		}
		leaseAndAppendLogicalIdRanges(*context_, node_id_ranges_, true, count);
	}

	void NativeGraphWriteTransaction::reserveEdgeIds(uint64_t count) {
		requireActive("reserve Edge IDs");
		if (count == 0U) {
			return;
		}
		leaseAndAppendLogicalIdRanges(*context_, edge_id_ranges_, false, count);
	}

	std::vector<uint64_t> NativeGraphWriteTransaction::collectIncidentEdgeIds(uint64_t nodeId,
																			  size_t maximumEdges) const {
		requireActive("enumerate a Node's incident Edges");
		if (nodeId == 0U || maximumEdges == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph incident Edge enumeration requires a Node ID and positive bound");
		}
		const auto validateNode = [this, nodeId] {
			const auto staged = nodes_.find(nodeId);
			if (staged != nodes_.end()) {
				if (staged->second.erased) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph incident Edge enumeration references an erased Node");
				}
				return;
			}
			static_cast<void>(context_->entities->readNode(snapshot_, nodeId));
		};
		validateNode();
		// The snapshot still contains Edges erased earlier in this write
		// transaction. Account for each final-state tombstone before choosing the
		// bounded raw scan, then apply the complete adjacency overlay before
		// evaluating the logical incident-Edge limit.
		std::set<uint64_t> erasedIncidentEdges;
		for (const BTreeScalarIndexOperation &operation: global_adjacency_operations_) {
			if (operation.kind != BTreeScalarIndexOperationKind::BSIOP_ERASE) {
				continue;
			}
			const GlobalAdjacencyKey key = AdjacencyKeyCodec::decode(operation.key);
			if (key.node_id == nodeId) {
				erasedIncidentEdges.insert(key.edge_id);
			}
		}
		if (erasedIncidentEdges.size() > std::numeric_limits<size_t>::max() - maximumEdges) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph incident Edge enumeration cannot derive its bounded overlay scan");
		}
		const size_t rawScanLimit = maximumEdges + erasedIncidentEdges.size();
		std::set<uint64_t> incident;
		const auto collectDirection = [this, nodeId, rawScanLimit, &incident](GlobalAdjacencyDirection direction) {
			const GlobalAdjacencyRange range = GlobalAdjacencyIndex(*context_->indexes, *context_->metadata)
															 .scan(snapshot_, direction, nodeId, rawScanLimit);
			for (const uint64_t edgeId: range.edge_ids) {
				incident.insert(edgeId);
			}
			return range.continuation.has_value();
		};
		const bool outgoingHasMore = collectDirection(GlobalAdjacencyDirection::GAD_OUT);
		const bool incomingHasMore = collectDirection(GlobalAdjacencyDirection::GAD_IN);
		for (const BTreeScalarIndexOperation &operation: global_adjacency_operations_) {
			const GlobalAdjacencyKey key = AdjacencyKeyCodec::decode(operation.key);
			if (key.node_id != nodeId) {
				continue;
			}
			if (operation.kind == BTreeScalarIndexOperationKind::BSIOP_UPSERT) {
				incident.insert(key.edge_id);
			} else if (operation.kind == BTreeScalarIndexOperationKind::BSIOP_ERASE) {
				incident.erase(key.edge_id);
			} else {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph global adjacency stream has an unknown operation");
			}
		}
		if (outgoingHasMore || incomingHasMore || incident.size() > maximumEdges) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph incident Edge enumeration exceeded its caller-supplied adjacency bound");
		}
		return {incident.begin(), incident.end()};
	}

	bool NativeGraphWriteTransaction::nodeHasIncidentEdges(uint64_t nodeId) const {
		requireActive("inspect a Node's relationship heads");
		if (nodeId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph relationship-head inspection requires a Node ID");
		}
		return !collectIncidentEdgeIds(nodeId, context_->options.maximum_adjacency_read_edges).empty();
	}

	void NativeGraphWriteTransaction::replaceNodeLabels(uint64_t nodeId, std::span<const uint64_t> labels) {
		requireActive("replace Node labels");
		validateLabels(labels);
		NodeChange &change = mutableNode(nodeId);
		change.labels = std::vector<uint64_t>(labels.begin(), labels.end());
	}

	void NativeGraphWriteTransaction::replaceNodeProperty(PropertyCollectionRoot root) {
		requireActive("replace Node Properties");
			if (root.owner_entity_type != Node::typeId || root.owner_entity_id == 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native graph write Node Property roots require a direct owner");
		}
		const auto nodeChange = nodes_.find(root.owner_entity_id);
		if (nodeChange != nodes_.end()) {
			if (nodeChange->second.erased) {
				fail(FormatError::FERR_INVALID_STATE, "native graph write cannot attach Properties to an erased Node");
			}
		} else {
			// Property-only writes do not COW an unchanged fixed Entity record.
			static_cast<void>(context_->entities->readNode(snapshot_, root.owner_entity_id));
		}
		const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, root.owner_entity_id};
		properties_.insert_or_assign(key.directoryId(), std::move(root));
	}

	void NativeGraphWriteTransaction::replaceEdgeProperty(PropertyCollectionRoot root) {
		requireActive("replace Edge Properties");
			if (root.owner_entity_type != Edge::typeId || root.owner_entity_id == 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native graph write Edge Property roots require a direct owner");
		}
		const auto edgeChange = edges_.find(root.owner_entity_id);
		if (edgeChange != edges_.end()) {
			if (edgeChange->second.erased) {
				fail(FormatError::FERR_INVALID_STATE, "native graph write cannot attach Properties to an erased Edge");
			}
		} else {
			// Property-only writes do not COW an unchanged fixed Entity record.
			static_cast<void>(context_->entities->readEdge(snapshot_, root.owner_entity_id));
		}
		const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_EDGE, root.owner_entity_id};
		properties_.insert_or_assign(key.directoryId(), std::move(root));
	}

	void NativeGraphWriteTransaction::stageScalarIndexUpsert(BTreePageStore &index, uint64_t indexId,
															 std::span<const std::byte> key, uint64_t value, BTreeLeafValueLayout leafValueLayout) {
		requireActive("stage a scalar Index upsert");
		if ((leafValueLayout != BTreeLeafValueLayout::BLVL_U64 && leafValueLayout != BTreeLeafValueLayout::BLVL_EMPTY) ||
			(leafValueLayout == BTreeLeafValueLayout::BLVL_EMPTY && value != 0U)) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph staged Index has an invalid leaf value/layout");
		}
		if (indexId == 0U || isReservedInternalIndexId(indexId) || !index.usesMetadataStore(*context_->metadata)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph staged scalar Index upsert requires a non-reserved Index in this metadata domain");
		}
		const auto initial =
				std::lower_bound(initial_scalar_index_mutations_.begin(), initial_scalar_index_mutations_.end(),
								 indexId, [](const StagedInitialScalarIndexMutation &mutation, uint64_t value) {
									 return mutation.index_id < value;
								 });
		if (initial != initial_scalar_index_mutations_.end() && initial->index_id == indexId) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot mutate a scalar Index staged for first publication");
		}
		auto existing = std::lower_bound(
				scalar_index_mutations_.begin(), scalar_index_mutations_.end(), indexId,
				[](const StagedScalarIndexMutation &mutation, uint64_t value) { return mutation.index_id < value; });
		if (existing == scalar_index_mutations_.end() || existing->index_id != indexId) {
			existing = scalar_index_mutations_.insert(existing, {&index, indexId, {}, std::nullopt, std::nullopt, leafValueLayout});
		}
		if (existing->index != &index || existing->leaf_value_layout != leafValueLayout) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph staged scalar Index mutation changed its page-store identity or leaf layout");
		}
		if (existing->retirement_plan.has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot mutate a scalar Index staged for whole-tree retirement");
		}
		existing->operations.push_back(
				{BTreeScalarIndexOperationKind::BSIOP_UPSERT, std::vector<std::byte>(key.begin(), key.end()), value});
		existing->plan.reset();
	}

	void NativeGraphWriteTransaction::stageScalarIndexErase(BTreePageStore &index, uint64_t indexId,
															std::span<const std::byte> key, BTreeLeafValueLayout leafValueLayout) {
		requireActive("stage a scalar Index erase");
		if (leafValueLayout != BTreeLeafValueLayout::BLVL_U64 && leafValueLayout != BTreeLeafValueLayout::BLVL_EMPTY) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph staged Index has an invalid leaf layout");
		}
		if (indexId == 0U || isReservedInternalIndexId(indexId) || !index.usesMetadataStore(*context_->metadata)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph staged scalar Index erase requires a non-reserved Index in this metadata domain");
		}
		const auto initial =
				std::lower_bound(initial_scalar_index_mutations_.begin(), initial_scalar_index_mutations_.end(),
								 indexId, [](const StagedInitialScalarIndexMutation &mutation, uint64_t value) {
									 return mutation.index_id < value;
								 });
		if (initial != initial_scalar_index_mutations_.end() && initial->index_id == indexId) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot mutate a scalar Index staged for first publication");
		}
		auto existing = std::lower_bound(
				scalar_index_mutations_.begin(), scalar_index_mutations_.end(), indexId,
				[](const StagedScalarIndexMutation &mutation, uint64_t value) { return mutation.index_id < value; });
		if (existing == scalar_index_mutations_.end() || existing->index_id != indexId) {
			existing = scalar_index_mutations_.insert(existing, {&index, indexId, {}, std::nullopt, std::nullopt, leafValueLayout});
		}
		if (existing->index != &index || existing->leaf_value_layout != leafValueLayout) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph staged scalar Index mutation changed its page-store identity or leaf layout");
		}
		if (existing->retirement_plan.has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot mutate a scalar Index staged for whole-tree retirement");
		}
		existing->operations.push_back(
				{BTreeScalarIndexOperationKind::BSIOP_ERASE, std::vector<std::byte>(key.begin(), key.end()), 0U});
		existing->plan.reset();
	}

	void NativeGraphWriteTransaction::stageScalarIndexCreate(BTreePageStore &index, uint64_t indexId,
															 std::span<const BTreePageEntry> entries) {
		requireActive("stage a scalar Index creation");
		if (indexId == 0U || isReservedInternalIndexId(indexId) || !index.usesMetadataStore(*context_->metadata)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph staged scalar Index creation requires a non-reserved Index in this metadata domain");
		}
		const auto existingMutation = std::lower_bound(
				scalar_index_mutations_.begin(), scalar_index_mutations_.end(), indexId,
				[](const StagedScalarIndexMutation &mutation, uint64_t value) { return mutation.index_id < value; });
		if (existingMutation != scalar_index_mutations_.end() && existingMutation->index_id == indexId) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot create and mutate the same scalar Index in one transaction");
		}
		auto existingCreation =
				std::lower_bound(initial_scalar_index_mutations_.begin(), initial_scalar_index_mutations_.end(),
								 indexId, [](const StagedInitialScalarIndexMutation &mutation, uint64_t value) {
									 return mutation.index_id < value;
								 });
		if (existingCreation != initial_scalar_index_mutations_.end() && existingCreation->index_id == indexId) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph staged scalar Index creation duplicates one logical Index ID");
		}
		BTreeInitialTreeMutationPlan plan = index.planInitialTreeMutation(snapshot_, indexId, entries);
		stageScalarIndexCreate(index, std::move(plan));
	}

	void NativeGraphWriteTransaction::stageScalarIndexCreate(BTreePageStore &index, BTreeInitialTreeMutationPlan plan) {
		requireActive("stage a scalar Index creation plan");
		if (plan.index_id == 0U || isReservedInternalIndexId(plan.index_id) || !plan.preparer ||
			plan.new_pool_mutations.empty() || !index.usesMetadataStore(*context_->metadata)) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph staged scalar Index creation plan is invalid, "
												   "reserved, or belongs to another metadata domain");
		}
		auto existingMutation = std::lower_bound(
				scalar_index_mutations_.begin(), scalar_index_mutations_.end(), plan.index_id,
				[](const StagedScalarIndexMutation &mutation, uint64_t value) { return mutation.index_id < value; });
		if (existingMutation != scalar_index_mutations_.end() && existingMutation->index_id == plan.index_id) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot create and mutate the same scalar Index in one transaction");
		}
		auto existingCreation =
				std::lower_bound(initial_scalar_index_mutations_.begin(), initial_scalar_index_mutations_.end(),
								 plan.index_id, [](const StagedInitialScalarIndexMutation &mutation, uint64_t value) {
									 return mutation.index_id < value;
								 });
		if (existingCreation != initial_scalar_index_mutations_.end() && existingCreation->index_id == plan.index_id) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph staged scalar Index creation duplicates one logical Index ID");
		}
		initial_scalar_index_mutations_.insert(existingCreation, {&index, plan.index_id, std::move(plan)});
	}

	void NativeGraphWriteTransaction::stageScalarIndexDrop(BTreePageStore &index, uint64_t indexId,
														   size_t maximumPages) {
		requireActive("stage a scalar Index whole-tree retirement");
		if (indexId == 0U || isReservedInternalIndexId(indexId) || maximumPages == 0U ||
			!index.usesMetadataStore(*context_->metadata)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph staged scalar Index retirement requires a non-reserved local Index, ID, and page bound");
		}
		const auto initial =
				std::lower_bound(initial_scalar_index_mutations_.begin(), initial_scalar_index_mutations_.end(),
								 indexId, [](const StagedInitialScalarIndexMutation &mutation, uint64_t value) {
									 return mutation.index_id < value;
								 });
		if (initial != initial_scalar_index_mutations_.end() && initial->index_id == indexId) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot retire a scalar Index staged for first publication");
		}
		auto existing = std::lower_bound(
				scalar_index_mutations_.begin(), scalar_index_mutations_.end(), indexId,
				[](const StagedScalarIndexMutation &mutation, uint64_t value) { return mutation.index_id < value; });
		if (existing != scalar_index_mutations_.end() && existing->index_id == indexId) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot retire and mutate the same scalar Index in one transaction");
		}
		BTreeTreeRetirementPlan plan = index.planTreeRetirement(snapshot_, indexId, maximumPages);
		scalar_index_mutations_.insert(existing, {&index, indexId, {}, std::nullopt, std::move(plan)});
	}

	void NativeGraphWriteTransaction::stageVectorIndexBootstrap(
			BTreePageStore &mappingTrees, VectorPageStore &rawPages, VectorManifestStore &manifests,
			const IndexDefinition &definition, std::span<const NativeGraphVectorBootstrapRecord> records,
			NativeGraphVectorBootstrapOptions options) {
		if (options.checkpoint) options.checkpoint();
		requireActive("stage a Vector Index bootstrap");
		IndexDefinitionCodec::validate(definition);
		const uint8_t invalidInput = static_cast<uint8_t>(definition.kind != IndexDefinitionKind::IDK_VECTOR) |
				static_cast<uint8_t>(definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE) |
				static_cast<uint8_t>(definition.index_id == 0U) | static_cast<uint8_t>(records.empty()) |
				static_cast<uint8_t>(options.maximum_vectors == 0U) |
				static_cast<uint8_t>(records.size() > options.maximum_vectors) |
				static_cast<uint8_t>(options.maximum_pages == 0U) |
				static_cast<uint8_t>(!options.first_slot.isValid()) |
				static_cast<uint8_t>(!options.last_slot.isValid()) |
				static_cast<uint8_t>(options.first_slot > options.last_slot) |
				static_cast<uint8_t>(!mappingTrees.usesMetadataStore(*context_->metadata)) |
				static_cast<uint8_t>(!rawPages.usesMetadataStore(*context_->metadata)) |
				static_cast<uint8_t>(!manifests.usesMetadataStore(*context_->metadata)) |
				static_cast<uint8_t>(definition.vector_dimensions > RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS);
		if (invalidInput != 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph Vector bootstrap requires a bounded complete schema and one metadata domain");
		}
		if (context_->metadata->findIndexRootOptional(snapshot_, definition.index_id).has_value() ||
			context_->metadata->findVectorManifestOptional(snapshot_, definition.index_id).has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph Vector bootstrap refuses to overwrite an existing physical Index state");
		}
		if (next_vector_id_ == 0U || records.size() - 1U > std::numeric_limits<uint64_t>::max() - next_vector_id_) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph Vector bootstrap exhausts the logical Vector-ID namespace");
		}

		const VectorDistanceMetric metric = VectorSemanticCodec::metricFor(definition);
		const size_t expectedEncodedBytes = static_cast<size_t>(definition.vector_dimensions) * sizeof(uint16_t);
		std::vector<const NativeGraphVectorBootstrapRecord *> ordered;
		ordered.reserve(records.size());
		for (const NativeGraphVectorBootstrapRecord &record: records) {
			if (ordered.size() % 256U == 0U && options.checkpoint) options.checkpoint();
			if (record.node_id == 0U || record.encoded_bfloat16.size() != expectedEncodedBytes) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native graph Vector bootstrap record has an invalid Node identity or BF16 width");
			}
			const auto stagedNode = nodes_.find(record.node_id);
			if (stagedNode != nodes_.end()) {
				if (stagedNode->second.erased) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph Vector bootstrap cannot map a Node erased by this transaction");
				}
			} else {
				// A native mapping is part of the graph generation, so it must not
				// introduce a durable reference to a non-existent logical Node.
				static_cast<void>(context_->entities->readNode(snapshot_, record.node_id));
			}
			const bool nonzeroFiniteNorm = VectorSemanticCodec::hasNonZeroFiniteL2Norm(record.encoded_bfloat16);
			if (metric == VectorDistanceMetric::VDM_COSINE && !nonzeroFiniteNorm) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native graph Vector bootstrap rejects a zero-norm cosine Vector record");
			}
			ordered.push_back(&record);
		}
		std::ranges::sort(ordered, {}, [](const NativeGraphVectorBootstrapRecord *record) { return record->node_id; });
		if (std::ranges::adjacent_find(ordered, {}, [](const NativeGraphVectorBootstrapRecord *record) {
				return record->node_id;
			}) != ordered.end()) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph Vector bootstrap has duplicate Node mappings");
		}

		if (expectedEncodedBytes > std::numeric_limits<uint32_t>::max() - sizeof(uint64_t)) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph Vector bootstrap record stride exceeds the persisted width");
		}
		const uint32_t rawRecordBytes = static_cast<uint32_t>(sizeof(uint64_t) + expectedEncodedBytes);
		const uint16_t rawCapacity = VectorPageStore::rawSlotCapacity(rawRecordBytes);
		if (rawCapacity == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph Vector bootstrap record does not fit a Raw Vector page");
		}
		const size_t rawPageCount = (ordered.size() - 1U) / static_cast<size_t>(rawCapacity) + 1U;
		if (rawPageCount > std::numeric_limits<uint32_t>::max()) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph Vector bootstrap Raw-page count exceeds its persisted width");
		}

		const uint64_t mappingIndexId =
				VectorNodeMapping::selectMappingIndexId(snapshot_, *context_->metadata, definition.index_id);
		if (std::ranges::any_of(vector_bootstraps_,
								[&](const StagedVectorBootstrap &staged) {
									return staged.mutation.index_id == definition.index_id ||
										   staged.mutation.vector_node_mapping_index_id == mappingIndexId;
								}) ||
			std::ranges::any_of(initial_scalar_index_mutations_, [&](const StagedInitialScalarIndexMutation &staged) {
				return staged.index_id == definition.index_id || staged.index_id == mappingIndexId;
			})) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph transaction has overlapping Vector bootstrap mapping identities");
		}

		utils::OwnedStdVector<BTreePageEntry> nodeMappingEntries;
		utils::OwnedStdVector<BTreePageEntry> vectorNodeMappingEntries;
		nodeMappingEntries.reserve(ordered.size());
		vectorNodeMappingEntries.reserve(ordered.size());
		StagedVectorBootstrap staged;
		staged.mapping_trees = &mappingTrees;
		staged.raw_pages = &rawPages;
		staged.manifests = &manifests;
		staged.maximum_pages = options.maximum_pages;
		staged.records.reserve(ordered.size());
		const uint64_t firstVectorId = next_vector_id_;
		for (size_t ordinal = 0U; ordinal < ordered.size(); ++ordinal) {
			if (ordinal % 256U == 0U && options.checkpoint) options.checkpoint();
			const NativeGraphVectorBootstrapRecord &record = *ordered[ordinal];
			const uint64_t vectorId = firstVectorId + static_cast<uint64_t>(ordinal);
			nodeMappingEntries.push_back({VectorSemanticCodec::nodeMappingKey(record.node_id), vectorId});
			vectorNodeMappingEntries.push_back({VectorNodeMapping::key(vectorId), record.node_id});
			staged.records.push_back(
					{record.node_id, vectorId, VectorSemanticCodec::rawRecord(vectorId, record.encoded_bfloat16)});
		}
		const BTreeInitialTreeMutationPlan nodeMappingPlan =
				mappingTrees.planInitialTreeMutation(snapshot_, definition.index_id, nodeMappingEntries);
		if (options.checkpoint) options.checkpoint();
		const BTreeInitialTreeMutationPlan vectorNodeMappingPlan =
				mappingTrees.planInitialTreeMutation(snapshot_, mappingIndexId, vectorNodeMappingEntries);
		const auto planPageCount = [](const BTreeInitialTreeMutationPlan &plan) {
			size_t count = 0U;
			for (const BTreeNewIndexTreePoolMutation &pool: plan.new_pool_mutations) {
				if (pool.appended_page_count > std::numeric_limits<size_t>::max() - count) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "native graph Vector bootstrap mapping-page count overflows host address space");
				}
				count += pool.appended_page_count;
			}
			return count;
		};
		const size_t nodeMappingPageCount = planPageCount(nodeMappingPlan);
		const size_t vectorNodeMappingPageCount = planPageCount(vectorNodeMappingPlan);
		const uint8_t invalidPagePlan = static_cast<uint8_t>(nodeMappingPageCount == 0U) |
				static_cast<uint8_t>(vectorNodeMappingPageCount == 0U) |
				static_cast<uint8_t>(nodeMappingPageCount > options.maximum_pages) |
				static_cast<uint8_t>(vectorNodeMappingPageCount > options.maximum_pages - nodeMappingPageCount) |
				static_cast<uint8_t>(rawPageCount >
									  options.maximum_pages - nodeMappingPageCount - vectorNodeMappingPageCount) |
				static_cast<uint8_t>(options.maximum_pages - nodeMappingPageCount - vectorNodeMappingPageCount - rawPageCount <
									  1U);
		if (invalidPagePlan != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph Vector bootstrap exceeds its configured typed-page work bound");
		}

		// All cancellation checks precede mutation of this transaction's staged roots.
		if (options.checkpoint) options.checkpoint();
		stageScalarIndexCreate(mappingTrees, nodeMappingPlan);
		stageScalarIndexCreate(mappingTrees, vectorNodeMappingPlan);
		staged.mutation = {definition.index_id,
						   mappingIndexId,
						   firstVectorId,
						   static_cast<uint64_t>(ordered.size()),
						   definition.vector_dimensions,
						   metric,
						   static_cast<uint32_t>(rawPageCount),
						   options.first_slot,
						   options.last_slot};
		vector_bootstraps_.push_back(std::move(staged));
		const uint64_t lastVectorId = firstVectorId + static_cast<uint64_t>(ordered.size()) - 1U;
		next_vector_id_ = lastVectorId == std::numeric_limits<uint64_t>::max() ? 0U : lastVectorId + 1U;
	}

	void NativeGraphWriteTransaction::stageVectorIndexDelta(BTreePageStore &mappingTrees, VectorPageStore &rawPages,
														VectorManifestStore &manifests,
														const IndexDefinition &definition,
														std::span<const NativeVectorMutation> mutations,
														NativeGraphVectorDeltaOptions options) {
		requireActive("stage a Vector Index delta");
		IndexDefinitionCodec::validate(definition);
		const uint8_t invalidInput = static_cast<uint8_t>(definition.kind != IndexDefinitionKind::IDK_VECTOR) |
				static_cast<uint8_t>(definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE) |
				static_cast<uint8_t>(definition.index_id == 0U) | static_cast<uint8_t>(mutations.empty()) |
				static_cast<uint8_t>(options.maximum_records == 0U) |
				static_cast<uint8_t>(mutations.size() > options.maximum_records) |
				static_cast<uint8_t>(options.maximum_raw_pages == 0U) |
				static_cast<uint8_t>(!options.first_slot.isValid()) |
				static_cast<uint8_t>(!options.last_slot.isValid()) |
				static_cast<uint8_t>(options.rebuild_threshold == 0U) |
				static_cast<uint8_t>(options.rebuild_threshold == std::numeric_limits<size_t>::max()) |
				static_cast<uint8_t>(options.rebuild_threshold > std::numeric_limits<uint32_t>::max()) |
				static_cast<uint8_t>(options.maximum_overlay_entries == 0U) |
				static_cast<uint8_t>(options.maximum_overlay_entries == std::numeric_limits<size_t>::max()) |
				static_cast<uint8_t>(options.maximum_overlay_entries > std::numeric_limits<uint32_t>::max()) |
				static_cast<uint8_t>(options.rebuild_threshold > options.maximum_overlay_entries) |
				static_cast<uint8_t>(options.maximum_delta_pages == 0U) |
				static_cast<uint8_t>(options.maximum_delta_pages == std::numeric_limits<size_t>::max()) |
				static_cast<uint8_t>(options.maximum_overlay_entries >
									  VectorDeltaMapping::maximumEntriesForPageBudget(options.maximum_delta_pages)) |
				static_cast<uint8_t>(options.first_slot > options.last_slot) |
				static_cast<uint8_t>(!mappingTrees.usesMetadataStore(*context_->metadata)) |
				static_cast<uint8_t>(!rawPages.usesMetadataStore(*context_->metadata)) |
				static_cast<uint8_t>(!manifests.usesMetadataStore(*context_->metadata)) |
				static_cast<uint8_t>(definition.vector_dimensions == 0U) |
				static_cast<uint8_t>(definition.vector_dimensions > RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS);
		if (invalidInput != 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph Vector delta requires bounded canonical input in one metadata domain");
		}
		if (mutations.size() > std::numeric_limits<size_t>::max() / 2U) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph Vector delta mapping work exceeds host address space");
		}
		if (std::ranges::any_of(vector_bootstraps_,
								[&](const StagedVectorBootstrap &staged) {
									return staged.mutation.index_id == definition.index_id;
								}) ||
			std::ranges::any_of(vector_deltas_, [&](const StagedVectorDelta &staged) {
				return staged.mutation.index_id == definition.index_id;
			})) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph transaction cannot stage more than one Vector publication shape per Index");
		}

		const VectorDistanceMetric metric = VectorSemanticCodec::metricFor(definition);
		const std::optional<IndexRootLocation> nodeMappingRoot =
				context_->metadata->findIndexRootOptional(snapshot_, definition.index_id);
		const std::optional<VectorManifestLocation> manifestDirectory =
				context_->metadata->findVectorManifestOptional(snapshot_, definition.index_id);
		if (!nodeMappingRoot.has_value() || !manifestDirectory.has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph Vector delta requires an already published mapping root and Manifest");
		}
		const VectorManifest sourceManifest = manifests.read(snapshot_, definition.index_id).manifest();
		const std::optional<IndexRootLocation> reverseMappingRoot =
				sourceManifest.vector_node_mapping_index_id == 0U
						? std::nullopt
						: context_->metadata->findIndexRootOptional(snapshot_,
																	sourceManifest.vector_node_mapping_index_id);
		const bool hasSourceDelta = sourceManifest.delta_mapping_index_id != 0U;
		const std::optional<IndexRootLocation> sourceDeltaRoot =
				hasSourceDelta
						? context_->metadata->findIndexRootOptional(snapshot_, sourceManifest.delta_mapping_index_id)
						: std::nullopt;
		const PageLocation manifestLocation = context_->metadata->findPageLocation(snapshot_, sourceManifest.page_id);
		const CatalogEntry manifestPool = context_->metadata->findCatalog(snapshot_, manifestLocation.segment_id);
		const uint8_t invalidManifest =
				static_cast<uint8_t>(sourceManifest.index_id != definition.index_id) |
				static_cast<uint8_t>(sourceManifest.dimensions != definition.vector_dimensions) |
				static_cast<uint8_t>(sourceManifest.metric != metric) |
				static_cast<uint8_t>(sourceManifest.node_mapping_root_page_id != nodeMappingRoot->root_page_id) |
				static_cast<uint8_t>(sourceManifest.node_mapping_root_generation != nodeMappingRoot->root_generation) |
				static_cast<uint8_t>(
						!VectorNodeMapping::isMappingIndexId(sourceManifest.vector_node_mapping_index_id)) |
				static_cast<uint8_t>(!reverseMappingRoot.has_value()) |
				static_cast<uint8_t>(reverseMappingRoot.has_value() &&
									 sourceManifest.vector_node_mapping_root_page_id !=
											 reverseMappingRoot->root_page_id) |
				static_cast<uint8_t>(reverseMappingRoot.has_value() &&
									 sourceManifest.vector_node_mapping_root_generation !=
											 reverseMappingRoot->root_generation) |
				static_cast<uint8_t>(hasSourceDelta &&
									 !VectorDeltaMapping::isMappingIndexId(sourceManifest.delta_mapping_index_id)) |
				static_cast<uint8_t>(hasSourceDelta != sourceManifest.delta_mapping_root_page_id.isValid()) |
				static_cast<uint8_t>(hasSourceDelta != sourceManifest.delta_mapping_root_generation.isValid()) |
				static_cast<uint8_t>(hasSourceDelta != (sourceManifest.delta_entry_count != 0U)) |
				static_cast<uint8_t>(hasSourceDelta != sourceDeltaRoot.has_value()) |
				static_cast<uint8_t>(sourceDeltaRoot.has_value() &&
									 sourceManifest.delta_mapping_root_page_id != sourceDeltaRoot->root_page_id) |
				static_cast<uint8_t>(sourceDeltaRoot.has_value() &&
									 sourceManifest.delta_mapping_root_generation != sourceDeltaRoot->root_generation) |
				static_cast<uint8_t>(manifestDirectory->manifest_page_id != sourceManifest.page_id) |
				static_cast<uint8_t>(manifestDirectory->generation != sourceManifest.generation) |
				static_cast<uint8_t>(manifestPool.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
				static_cast<uint8_t>(manifestPool.owner_id != definition.index_id) |
				static_cast<uint8_t>(manifestPool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(manifestPool.generation != manifestLocation.generation) |
				static_cast<uint8_t>(!manifestPool.committed_lsn.isValid()) |
				static_cast<uint8_t>(!sourceManifest.committed_lsn.isValid());
		if (invalidManifest != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph Vector delta found an inconsistent mapping or Manifest fence");
		}

		const size_t encodedBytes = static_cast<size_t>(definition.vector_dimensions) * sizeof(uint16_t);
		if (encodedBytes > std::numeric_limits<uint32_t>::max() - sizeof(uint64_t)) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph Vector delta raw record stride exceeds persisted width");
		}
		const uint32_t rawRecordBytes = static_cast<uint32_t>(sizeof(uint64_t) + encodedBytes);
		const uint16_t rawCapacity = VectorPageStore::rawSlotCapacity(rawRecordBytes);
		if (rawCapacity == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph Vector delta record does not fit a Raw Vector page");
		}
		std::map<uint64_t, VectorDeltaEntry> originalDeltaOperations;
		if (hasSourceDelta) {
			// The threshold schedules maintenance, while the independently bounded
			// overlay limit remains a hard safety ceiling for retained exact state.
			const size_t sourceDeltaEntries = static_cast<size_t>(sourceManifest.delta_entry_count);
			if (sourceDeltaEntries > options.maximum_overlay_entries) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph Vector delta exceeds its configured durable overlay bound");
			}
			const size_t scanLimit = std::max(options.rebuild_threshold, sourceDeltaEntries);
			const auto sourceEntries = VectorDeltaMapping::scanOwned(
					snapshot_, mappingTrees, *context_->metadata, sourceManifest.delta_mapping_index_id, scanLimit);
			if (sourceEntries.size() != sourceManifest.delta_entry_count) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph Vector delta Manifest count disagrees with its typed overlay tree");
			}
			for (const VectorDeltaEntry &entry: sourceEntries) {
				if (!originalDeltaOperations.emplace(entry.vector_id, entry).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph Vector delta overlay contains a duplicate Vector-ID entry");
				}
			}
		}
		std::map<uint64_t, VectorDeltaEntry> deltaOperations = originalDeltaOperations;

		std::vector<const NativeVectorMutation *> ordered;
		ordered.reserve(mutations.size());
		for (const NativeVectorMutation &mutation: mutations) {
			const bool insert = mutation.kind == NativeVectorMutationKind::NVMK_INSERT;
			const bool erase = mutation.kind == NativeVectorMutationKind::NVMK_ERASE;
			const uint8_t invalid = static_cast<uint8_t>(!insert && !erase) |
									static_cast<uint8_t>(mutation.index_id != definition.index_id) |
									static_cast<uint8_t>(mutation.node_id == 0U) |
									static_cast<uint8_t>(mutation.dimensions != definition.vector_dimensions) |
									static_cast<uint8_t>(mutation.metric != metric) |
									static_cast<uint8_t>(insert && mutation.encoded_bfloat16.size() != encodedBytes) |
									static_cast<uint8_t>(erase && !mutation.encoded_bfloat16.empty());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "native graph Vector delta has an invalid semantic mutation");
			}
			if (insert) {
				const auto stagedNode = nodes_.find(mutation.node_id);
				if (stagedNode != nodes_.end()) {
					if (stagedNode->second.erased) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph Vector delta cannot insert a mapping for a Node erased by this transaction");
					}
				} else {
					static_cast<void>(context_->entities->readNode(snapshot_, mutation.node_id));
				}
				const bool nonzeroFiniteNorm = VectorSemanticCodec::hasNonZeroFiniteL2Norm(mutation.encoded_bfloat16);
				if (metric == VectorDistanceMetric::VDM_COSINE && !nonzeroFiniteNorm) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native graph Vector delta rejects a zero-norm cosine Vector record");
				}
			}
			ordered.push_back(&mutation);
		}
		std::ranges::sort(ordered, {}, [](const NativeVectorMutation *mutation) {
			return std::tuple<uint64_t, uint8_t>{mutation->node_id, static_cast<uint8_t>(mutation->kind)};
		});
		for (size_t ordinal = 1U; ordinal < ordered.size(); ++ordinal) {
			const NativeVectorMutation &previous = *ordered[ordinal - 1U];
			const NativeVectorMutation &current = *ordered[ordinal];
			if (previous.node_id == current.node_id && previous.kind == current.kind) {
				fail(FormatError::FERR_INVALID_HEADER, "native graph Vector delta has duplicate Node transition kinds");
			}
		}

		StagedVectorDelta staged;
		staged.mapping_trees = &mappingTrees;
		staged.raw_pages = &rawPages;
		staged.manifests = &manifests;
		staged.source_manifest = sourceManifest;
		staged.maximum_raw_pages = options.maximum_raw_pages;
		staged.mutation.index_id = definition.index_id;
		staged.mutation.vector_node_mapping_index_id = sourceManifest.vector_node_mapping_index_id;
		staged.mutation.expected_delta_mapping_index_id = sourceManifest.delta_mapping_index_id;
		staged.mutation.expected_delta_mapping_root = sourceDeltaRoot;
		staged.mutation.expected_node_mapping_root = *nodeMappingRoot;
		staged.mutation.expected_vector_node_mapping_root = *reverseMappingRoot;
		staged.mutation.expected_manifest_pool = manifestPool;
		staged.mutation.expected_manifest_location = manifestLocation;
		staged.mutation.expected_manifest = *manifestDirectory;
		staged.mutation.expected_manifest_owner_lsn = sourceManifest.committed_lsn;
		staged.mutation.dimensions = definition.vector_dimensions;
		staged.mutation.metric = sourceManifest.metric;
		staged.mutation.first_slot = options.first_slot;
		staged.mutation.last_slot = options.last_slot;
		staged.mutation.manifest_in_new_pool = !manifests.hasReplacementFrameCapacity(manifestPool, 1U);

		std::set<uint64_t> erasedNodes;
		std::set<uint64_t> insertedNodes;
		struct MappingOperation {
			bool erase = false;
			uint64_t index_id = 0U;
			std::vector<std::byte> key;
			uint64_t value = 0U;
		};
		std::vector<MappingOperation> mappingOperations;
		mappingOperations.reserve(mutations.size() * 2U);
		for (const NativeVectorMutation *mutation: ordered) {
			if (mutation->kind != NativeVectorMutationKind::NVMK_ERASE) {
				continue;
			}
			const std::vector<std::byte> key = VectorSemanticCodec::nodeMappingKey(mutation->node_id);
			const std::optional<uint64_t> vectorId =
					BTreeIndex(definition.index_id, mappingTrees, *context_->metadata).find(snapshot_, key);
			const std::optional<uint64_t> reverseNode =
					vectorId.has_value()
							? VectorNodeMapping::findNode(snapshot_, mappingTrees, *context_->metadata,
														  sourceManifest.vector_node_mapping_index_id, *vectorId)
							: std::nullopt;
			if (!vectorId.has_value() || *vectorId == 0U || !reverseNode.has_value() || // staged Vector mappings are produced only after mapping-tree validation.
				*reverseNode != mutation->node_id ||
				!context_->metadata->findVectorLocationOptional(snapshot_, *vectorId).has_value() ||
				!erasedNodes.insert(mutation->node_id).second) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph Vector delta erase found an inconsistent semantic mapping");
			}
			mappingOperations.push_back({true, definition.index_id, key, 0U});
			mappingOperations.push_back(
					{true, sourceManifest.vector_node_mapping_index_id, VectorNodeMapping::key(*vectorId), 0U});
			staged.records.push_back({NativeVectorMutationKind::NVMK_ERASE, mutation->node_id, *vectorId, {}});
			staged.mutation.erased_vector_ids.push_back(*vectorId);
			const auto delta = deltaOperations.find(*vectorId);
			if (delta == deltaOperations.end()) {
				deltaOperations.emplace(
						*vectorId, VectorDeltaEntry{*vectorId, mutation->node_id, VectorDeltaOperation::VDO_ERASE});
			} else if (delta->second.operation == VectorDeltaOperation::VDO_INSERT) {
				if (delta->second.node_id != mutation->node_id) { // reverse mapping lookup is authenticated against the staged Node identity.
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph Vector delta erase changed the Node identity of an inserted Vector");
				}
				deltaOperations.erase(delta);
			} else {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph Vector delta found a live semantic mapping hidden by an overlay tombstone");
			}
		}

		const size_t insertCount =
				static_cast<size_t>(std::ranges::count_if(ordered, [](const NativeVectorMutation *mutation) {
					return mutation->kind == NativeVectorMutationKind::NVMK_INSERT;
				}));
		uint64_t nextVectorId = next_vector_id_;
		if (insertCount != 0U &&
			(nextVectorId == 0U || insertCount - 1U > std::numeric_limits<uint64_t>::max() - nextVectorId)) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph Vector delta exhausts the logical Vector-ID namespace");
		}
		staged.mutation.first_vector_id = insertCount == 0U ? 0U : nextVectorId;
		staged.mutation.inserted_vector_count = static_cast<uint64_t>(insertCount);
		for (const NativeVectorMutation *mutation: ordered) {
			if (mutation->kind != NativeVectorMutationKind::NVMK_INSERT) {
				continue;
			}
			const std::vector<std::byte> key = VectorSemanticCodec::nodeMappingKey(mutation->node_id);
			const std::optional<uint64_t> existing =
					BTreeIndex(definition.index_id, mappingTrees, *context_->metadata).find(snapshot_, key);
			if ((existing.has_value() && !erasedNodes.contains(mutation->node_id)) || // duplicate staged Vector identities are rejected during request validation.
				!insertedNodes.insert(mutation->node_id).second) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph Vector delta insert found a duplicate Node mapping");
			}
			const uint64_t vectorId = nextVectorId;
			nextVectorId = vectorId == std::numeric_limits<uint64_t>::max() ? 0U : vectorId + 1U;
			mappingOperations.push_back({false, definition.index_id, key, vectorId});
			mappingOperations.push_back({false, sourceManifest.vector_node_mapping_index_id,
										 VectorNodeMapping::key(vectorId), mutation->node_id});
			staged.records.push_back({NativeVectorMutationKind::NVMK_INSERT, mutation->node_id, vectorId,
									  VectorSemanticCodec::rawRecord(vectorId, mutation->encoded_bfloat16)});
			if (!deltaOperations
						 .emplace(vectorId,
								  VectorDeltaEntry{vectorId, mutation->node_id, VectorDeltaOperation::VDO_INSERT})
						 .second) {
				fail(FormatError::FERR_INVALID_STATE, "native graph Vector delta reused an existing overlay Vector ID");
			}
		}

		const LogSequenceNumber checkpointLsn = nextCheckpointLsn(snapshot_.superblock());
		std::vector<size_t> insertRecordOrdinals;
		for (size_t ordinal = 0U; ordinal < staged.records.size(); ++ordinal) {
			if (staged.records[ordinal].kind == NativeVectorMutationKind::NVMK_INSERT) {
				insertRecordOrdinals.push_back(ordinal);
			}
		}
		if (!insertRecordOrdinals.empty()) {
			if (!sourceManifest.raw_write_tail_page_id.isValid()) { // a delta source is admitted only with a durable Raw tail.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph Vector delta has inserts but no current Raw write tail");
			}
			const PageLocation tailLocation =
					context_->metadata->findPageLocation(snapshot_, sourceManifest.raw_write_tail_page_id);
			const CatalogEntry tailPool = context_->metadata->findCatalog(snapshot_, tailLocation.segment_id);
			const VectorPage tail = rawPages.read(snapshot_, tailLocation.page_id);
			const VectorPageHeader tailHeader = tail.header();
			const uint8_t invalidTail = static_cast<uint8_t>(tailPool.kind != SegmentKind::SGK_VECTOR_RAW_POOL) |
										static_cast<uint8_t>(tailPool.owner_id != definition.index_id) |
										static_cast<uint8_t>(tailPool.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(tailPool.generation != tailLocation.generation) |
										static_cast<uint8_t>(tailHeader.kind != VectorStorageKind::VSK_RAW) |
										static_cast<uint8_t>(tailHeader.index_id != definition.index_id) |
										static_cast<uint8_t>(tailHeader.page_id != tailLocation.page_id) |
										static_cast<uint8_t>(tailHeader.generation != tailLocation.generation) |
										static_cast<uint8_t>(tailHeader.record_bytes != rawRecordBytes) |
										static_cast<uint8_t>(tailHeader.slot_capacity != rawCapacity) |
										static_cast<uint8_t>(!tailHeader.committed_lsn.isValid());
			if (invalidTail != 0U) { // source Manifest page bounds are validated before delta planning.
				fail(FormatError::FERR_INVALID_STATE, "native graph Vector delta found an invalid Raw write tail");
			}
			size_t nextRecord = 0U;
			const size_t tailFree = static_cast<size_t>(tailHeader.slot_capacity - tailHeader.live_count);
			if (tailFree != 0U && rawPages.hasReplacementFrameCapacity(tailPool, 1U)) {
				const PageOwnerHeader owner{tailLocation.page_id,
											definition.index_id,
											PageOwnerKind::POK_VECTOR_RAW,
											0U,
											RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
											tailLocation.generation,
											checkpointLsn};
				VectorPage image = tail.rebindOwner(owner);
				const size_t count = std::min(tailFree, insertRecordOrdinals.size());
				StagedVectorDeltaRawPage rawPage;
				rawPage.mutation = {NativeGraphVectorDeltaRawPlacement::NGVDRP_COW_TAIL,
									tailPool,
									tailLocation,
									tailHeader.committed_lsn,
									0U,
									0U,
									static_cast<uint32_t>(count)};
				rawPage.record_ordinals.insert(rawPage.record_ordinals.end(), insertRecordOrdinals.begin(),
											   insertRecordOrdinals.begin() + static_cast<std::ptrdiff_t>(count));
				for (const size_t recordOrdinal: rawPage.record_ordinals) {
					static_cast<void>(image.insert(staged.records[recordOrdinal].raw_record));
				}
				rawPage.tail_image.emplace(std::move(image));
				staged.raw_page_images.push_back(std::move(rawPage));
				nextRecord = count;
			}
			const size_t remaining = insertRecordOrdinals.size() - nextRecord;
			if (remaining != 0U) {
				const size_t freshPageCount = (remaining - 1U) / static_cast<size_t>(rawCapacity) + 1U;
				if (staged.raw_page_images.size() + freshPageCount > options.maximum_raw_pages) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph Vector delta exceeds its configured Raw-page work bound");
				}
				const bool appendCurrent =
						rawPages.hasReplacementFrameCapacity(tailPool, freshPageCount + (nextRecord != 0U ? 1U : 0U));
				for (size_t pageOrdinal = 0U; pageOrdinal < freshPageCount; ++pageOrdinal) {
					const size_t first = nextRecord + pageOrdinal * static_cast<size_t>(rawCapacity);
					const size_t count =
							std::min(static_cast<size_t>(rawCapacity), insertRecordOrdinals.size() - first);
					StagedVectorDeltaRawPage rawPage;
					rawPage.mutation.placement =
							appendCurrent ? NativeGraphVectorDeltaRawPlacement::NGVDRP_APPEND_TO_CURRENT_POOL
										  : NativeGraphVectorDeltaRawPlacement::NGVDRP_NEW_POOL;
					rawPage.mutation.expected_pool = appendCurrent ? tailPool : CatalogEntry{};
					rawPage.mutation.page_ordinal = static_cast<uint32_t>(staged.raw_page_images.size());
					rawPage.mutation.new_pool_ordinal =
							appendCurrent ? 0U
										  : static_cast<uint32_t>(pageOrdinal / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U);
					rawPage.mutation.inserted_record_count = static_cast<uint32_t>(count);
					rawPage.record_ordinals.insert(rawPage.record_ordinals.end(),
												   insertRecordOrdinals.begin() + static_cast<std::ptrdiff_t>(first),
												   insertRecordOrdinals.begin() +
														   static_cast<std::ptrdiff_t>(first + count));
					staged.raw_page_images.push_back(std::move(rawPage));
				}
			}
		}
		if (staged.raw_page_images.size() > options.maximum_raw_pages) { // raw page planning is bounded before materialization.
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph Vector delta exceeds its configured Raw-page work bound");
		}
		staged.mutation.raw_pages.reserve(staged.raw_page_images.size());
		for (const StagedVectorDeltaRawPage &rawPage: staged.raw_page_images) {
			staged.mutation.raw_pages.push_back(rawPage.mutation);
		}
		// Semantic operations are ordered by Node ID so mapping updates are
		// deterministic; directory erasures instead require Vector-ID order.
		std::ranges::sort(staged.mutation.erased_vector_ids);
		if (std::ranges::adjacent_find(staged.mutation.erased_vector_ids) != staged.mutation.erased_vector_ids.end()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph Vector delta resolved one logical Vector ID more than once");
		}
		// Crossing the configured threshold schedules a base rebuild after this
		// transaction publishes. Keeping the overlay visible until the rebuild
		// completes preserves the `base - tombstones + exact inserts` invariant.
		if (deltaOperations.size() > std::numeric_limits<uint32_t>::max()) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph Vector delta entry count exceeds its persisted width");
		}
		if (deltaOperations.size() > options.maximum_overlay_entries) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph Vector delta exceeds its configured durable overlay bound");
		}
		staged.mutation.maintenance_rebuild_required = deltaOperations.size() > options.rebuild_threshold;
		staged.mutation.delta_entry_count = static_cast<uint32_t>(deltaOperations.size());
		if (!deltaOperations.empty()) {
			const uint64_t deltaMappingIndexId =
					hasSourceDelta ? sourceManifest.delta_mapping_index_id
								   : VectorDeltaMapping::selectMappingIndexId(snapshot_, *context_->metadata,
																			  definition.index_id);
			staged.mutation.delta_mapping_index_id = deltaMappingIndexId;
			if (hasSourceDelta) {
				for (const auto &[vectorId, original]: originalDeltaOperations) {
					const auto updated = deltaOperations.find(vectorId);
					if (updated == deltaOperations.end()) {
						stageScalarIndexErase(mappingTrees, deltaMappingIndexId,
											  VectorDeltaMapping::key(vectorId, original.operation));
					} else if (updated->second != original) {
						stageScalarIndexErase(mappingTrees, deltaMappingIndexId,
											  VectorDeltaMapping::key(vectorId, original.operation));
						stageScalarIndexUpsert(mappingTrees, deltaMappingIndexId,
											   VectorDeltaMapping::key(vectorId, updated->second.operation),
											   updated->second.node_id);
					}
				}
				for (const auto &[vectorId, updated]: deltaOperations) {
					if (!originalDeltaOperations.contains(vectorId)) {
						stageScalarIndexUpsert(mappingTrees, deltaMappingIndexId,
											   VectorDeltaMapping::key(vectorId, updated.operation), updated.node_id);
					}
				}
			} else {
				utils::OwnedStdVector<BTreePageEntry> entries;
				entries.reserve(deltaOperations.size());
				for (const auto &[vectorId, entry]: deltaOperations) {
					entries.push_back({VectorDeltaMapping::key(vectorId, entry.operation), entry.node_id});
				}
				stageScalarIndexCreate(mappingTrees, deltaMappingIndexId, entries);
			}
		} else if (hasSourceDelta) {
			stageScalarIndexDrop(mappingTrees, sourceManifest.delta_mapping_index_id, options.maximum_delta_pages);
		}
		for (const MappingOperation &operation: mappingOperations) {
			if (operation.erase) {
				stageScalarIndexErase(mappingTrees, operation.index_id, operation.key);
			} else {
				stageScalarIndexUpsert(mappingTrees, operation.index_id, operation.key, operation.value);
			}
		}
		next_vector_id_ = nextVectorId;
		vector_deltas_.push_back(std::move(staged));
	}

	void NativeGraphWriteTransaction::stageGlobalAdjacencyMutation(GlobalAdjacencyDirection direction, uint64_t nodeId,
																   uint64_t edgeId,
																   BTreeScalarIndexOperationKind kind) {
		if ((direction != GlobalAdjacencyDirection::GAD_OUT && direction != GlobalAdjacencyDirection::GAD_IN) || // this private helper is called only by Edge create/erase paths with canonical directions, nonzero identities, and key-only operation kinds.
			nodeId == 0U || edgeId == 0U ||
			(kind != BTreeScalarIndexOperationKind::BSIOP_UPSERT &&
			 kind != BTreeScalarIndexOperationKind::BSIOP_ERASE)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph global adjacency mutation requires canonical key and operation identities");
		}
		requireAdjacencyOperationCapacity(global_adjacency_operations_.size(), 1U,
										  context_->options.maximum_global_adjacency_write_entries);
		global_adjacency_operations_.push_back({kind, AdjacencyKeyCodec::encode({direction, nodeId, edgeId}), 0U});
		global_adjacency_operations_canonical_ = false;
		global_adjacency_plan_.reset();
		global_adjacency_retirement_plan_.reset();
		global_adjacency_initial_plan_.reset();
		global_adjacency_deferred_bulk_ = false;
	}

	const std::vector<BTreeScalarIndexOperation> &NativeGraphWriteTransaction::canonicalGlobalAdjacencyOperations() {
		if (global_adjacency_operations_canonical_) {
			return global_adjacency_operations_;
		}
		const auto less = [](const BTreeScalarIndexOperation &left, const BTreeScalarIndexOperation &right) {
			return std::lexicographical_compare(left.key.begin(), left.key.end(), right.key.begin(), right.key.end());
		};
		// stable_sort retains statement order inside an equal-key group, preserving
		// the only cancellable sequence: provisional UPSERT followed by ERASE.
		std::stable_sort(global_adjacency_operations_.begin(), global_adjacency_operations_.end(), less);
		for (const BTreeScalarIndexOperation &operation: global_adjacency_operations_) {
			if (operation.value != 0U || operation.key.empty() || // this private vector is populated only by stageGlobalAdjacencyMutation with a typed key and zero value.
				(operation.kind != BTreeScalarIndexOperationKind::BSIOP_UPSERT &&
				 operation.kind != BTreeScalarIndexOperationKind::BSIOP_ERASE)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph global adjacency operation stream is not key-only and canonical");
			}
			static_cast<void>(AdjacencyKeyCodec::decode(operation.key));
		}
		size_t output = 0U;
		for (size_t input = 0U; input < global_adjacency_operations_.size();) {
			size_t next = input + 1U;
			while (next < global_adjacency_operations_.size() &&
				   !less(global_adjacency_operations_[input], global_adjacency_operations_[next]) &&
				   !less(global_adjacency_operations_[next], global_adjacency_operations_[input])) {
				++next;
			}
			if (next - input == 1U) {
				// A unique group normally stays at the same index. Moving a vector to
				// itself is not a no-op and may empty its key, so compact only when a
				// preceding cancelled group has actually created a gap.
				if (output != input) {
					global_adjacency_operations_[output] = std::move(global_adjacency_operations_[input]);
				}
				++output;
			} else if (next - input == 2U && // erasing a provisional Edge removes its two staged upserts directly, so the public API cannot retain a duplicate-key pair for this defensive canonicalizer.
					   global_adjacency_operations_[input].kind == BTreeScalarIndexOperationKind::BSIOP_UPSERT &&
					   global_adjacency_operations_[input + 1U].kind == BTreeScalarIndexOperationKind::BSIOP_ERASE) {
				// A provisional Edge created and removed in one transaction leaves no
				// durable adjacency key or empty-root publication behind.
			} else { // public Edge mutations emit at most one final-state operation per directional key after provisional cancellation.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph transaction stages contradictory duplicate global adjacency keys");
			}
			input = next;
		}
		global_adjacency_operations_.resize(output);
		global_adjacency_operations_canonical_ = true;
		return global_adjacency_operations_;
	}

	void NativeGraphWriteTransaction::prepareGlobalAdjacencyMutationPlan() {
		global_adjacency_plan_.reset();
		global_adjacency_retirement_plan_.reset();
		global_adjacency_initial_plan_.reset();
		if (global_adjacency_operations_.empty()) {
			return;
		}
		const std::vector<BTreeScalarIndexOperation> &operations = canonicalGlobalAdjacencyOperations();
		if (operations.empty()) { // provisional Edge deletion removes its keys before planning; any other nonempty staged vector remains nonempty after canonicalization.
			return;
		}
		if (operations.size() > context_->options.maximum_global_adjacency_write_entries) { // every addition passes requireAdjacencyOperationCapacity with this same bound.
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph global adjacency mutation exceeds its configured entry bound");
		}
		if (context_->metadata->findIndexRootOptional(snapshot_, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value()) {
			if (operations.size() >= context_->options.global_adjacency_bulk_rebuild_entry_threshold) {
				global_adjacency_deferred_bulk_ = true;
				return;
			}
			global_adjacency_plan_ = context_->indexes->planScalarIndexTransactionMutation(
					snapshot_, RSS_GLOBAL_ADJACENCY_INDEX_ID, operations, true, false,
					BTreeLeafValueLayout::BLVL_EMPTY);
			if (global_adjacency_plan_->final_tree_empty) {
				global_adjacency_retirement_plan_ =
						context_->indexes->planTreeRetirement(snapshot_, RSS_GLOBAL_ADJACENCY_INDEX_ID,
															  context_->options.maximum_global_adjacency_tree_pages);
				global_adjacency_plan_.reset();
			}
			return;
		}
		for (const BTreeScalarIndexOperation &operation: operations) {
			if (operation.kind != BTreeScalarIndexOperationKind::BSIOP_UPSERT) { // a persisted Edge always has an existing global adjacency root; a new tree receives only Edge-create upserts.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph cannot erase a global adjacency key from an absent tree");
			}
		}
		if (operations.size() >= context_->options.global_adjacency_bulk_rebuild_entry_threshold) {
			// A large first tree can be built from a private, same-file run after the
			// coordinator owns the writer. This removes the second B+Tree-entry copy
			// from the commit's planning phase while preserving one atomic publication.
			global_adjacency_deferred_bulk_ = true;
			return;
		}
		// Keep the first-tree bulk source transaction-owned as well. This avoids a
		// second entry/key allocation while preserving the builder's rewindable
		// source contract through the enclosing commit.
		const auto *initialOperations = &global_adjacency_operations_;
		const OrderedBTreeEntrySource source = [initialOperations] {
			auto ordinal = std::make_shared<size_t>(0U);
			return [initialOperations, ordinal]() -> std::optional<BTreePageEntry> {
				if (*ordinal == initialOperations->size()) {
					return std::nullopt;
				}
				return BTreePageEntry{(*initialOperations)[(*ordinal)++].key, 0U};
			};
		};
		BulkBTreeBuildOptions options;
		options.first_slot = context_->options.first_slot;
		options.last_slot = context_->options.last_slot;
		options.pool_owner_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
		options.maximum_entries = maximumAdjacencyTreeEntries(context_->options);
		options.maximum_pages = context_->options.maximum_global_adjacency_tree_pages;
		options.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
		global_adjacency_initial_plan_ = BulkBTreeBuilder::planNativeTree(
				*context_->indexes, *context_->metadata, snapshot_, RSS_GLOBAL_ADJACENCY_INDEX_ID, source, options);
	}

	bool NativeGraphWriteTransaction::rebaseAdjacencyOnly(MetadataSnapshotPin &current) {
		if (!current.isValid() || next_vector_id_ != snapshot_.superblock().next_logical_vector_id ||
			!scalar_index_mutations_.empty() || !initial_scalar_index_mutations_.empty() ||
			!vector_bootstraps_.empty() || !vector_deltas_.empty()) {
			return false;
		}

		std::vector<uint64_t> nodeIds;
		nodeIds.reserve(nodes_.size());
		for (const auto &[nodeId, change]: nodes_) {
			static_cast<void>(change);
			nodeIds.push_back(nodeId);
		}
		const std::vector<std::optional<NodeEntityRecord>> nodeRecords =
				nodeIds.empty() ? std::vector<std::optional<NodeEntityRecord>>()
									: context_->entities->readNodesSorted(current, nodeIds);
		if (nodeRecords.size() != nodeIds.size()) {
			fail(FormatError::FERR_INVALID_STATE, "native graph adjacency rebase changed Node batch cardinality");
		}
		const auto nodeRecordFor = [&](uint64_t nodeId) -> const std::optional<NodeEntityRecord> & {
			const auto iterator = std::ranges::lower_bound(nodeIds, nodeId);
			if (iterator == nodeIds.end() || *iterator != nodeId) {
				fail(FormatError::FERR_INVALID_STATE, "native graph adjacency rebase lost a Node mutation ID");
			}
			return nodeRecords[static_cast<size_t>(iterator - nodeIds.begin())];
		};
		for (const auto &[nodeId, change]: nodes_) {
			if (change.erased || (change.labels.has_value() && !change.inserted)) {
				return false;
			}
			if (change.inserted) {
				if (nodeRecordFor(nodeId).has_value()) {
					return false;
				}
			} else if (!nodeRecordFor(nodeId).has_value() || *nodeRecordFor(nodeId) != change.record) {
				return false;
			}
		}

		size_t erasedEdgeCount = 0U;
		std::vector<uint64_t> edgeIds;
		edgeIds.reserve(edges_.size());
		for (const auto &[edgeId, change]: edges_) {
			static_cast<void>(change);
			edgeIds.push_back(edgeId);
		}
		const std::vector<std::optional<EdgeEntityRecord>> edgeRecords =
				edgeIds.empty() ? std::vector<std::optional<EdgeEntityRecord>>()
										: context_->entities->readEdgesSorted(current, edgeIds);
		if (edgeRecords.size() != edgeIds.size()) {
			fail(FormatError::FERR_INVALID_STATE, "native graph adjacency rebase changed Edge batch cardinality");
		}
		const auto edgeRecordFor = [&](uint64_t edgeId) -> const std::optional<EdgeEntityRecord> & {
			const auto iterator = std::ranges::lower_bound(edgeIds, edgeId);
			if (iterator == edgeIds.end() || *iterator != edgeId) {
				fail(FormatError::FERR_INVALID_STATE, "native graph adjacency rebase lost an Edge mutation ID");
			}
			return edgeRecords[static_cast<size_t>(iterator - edgeIds.begin())];
		};
		for (const auto &[edgeId, change]: edges_) {
			const EntityPropertyKey propertyKey{EntityPropertyOwnerKind::EPOK_EDGE, edgeId};
			const auto property = properties_.find(propertyKey.directoryId());
			if (change.inserted) {
				if (property != properties_.end() || edgeRecordFor(edgeId).has_value()) {
					return false;
				}
				continue;
			}
			if (!change.erased || property == properties_.end() || property->second.owner_entity_id != edgeId ||
				property->second.owner_entity_type != Edge::typeId ||
				!property->second.entries.empty() ||
				!edgeRecordFor(edgeId).has_value() || *edgeRecordFor(edgeId) != change.record) {
				return false;
			}
			++erasedEdgeCount;
		}
		if (properties_.size() != erasedEdgeCount) {
			return false;
		}

		const std::vector<BTreeScalarIndexOperation> &operations = canonicalGlobalAdjacencyOperations();
		if (operations.size() != edges_.size() * 2U ||
			std::ranges::any_of(operations, [](const BTreeScalarIndexOperation &operation) {
				return (operation.kind != BTreeScalarIndexOperationKind::BSIOP_UPSERT &&
						operation.kind != BTreeScalarIndexOperationKind::BSIOP_ERASE) ||
					   operation.value != 0U;
			})) {
			return false;
		}

		// All source fences have been revalidated before changing the transaction's
		// planning snapshot. Subsequent COW plans therefore target the current root.
		snapshot_.release();
		snapshot_ = std::move(current);
		return true;
	}

	void NativeGraphWriteTransaction::eraseEdge(uint64_t edgeId) {
		requireActive("erase an Edge");
		const auto staged = edges_.find(edgeId);
		if (staged != edges_.end() && staged->second.inserted) {
			const EdgeEntityRecord record = staged->second.record;
			const std::array<std::vector<std::byte>, 2U> provisionalKeys = {
					AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, record.source_node_id, edgeId}),
					AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_IN, record.target_node_id, edgeId})};
			const auto eraseProvisionalKey = [this](const std::vector<std::byte> &key) {
				const auto operation = std::ranges::find_if(
						global_adjacency_operations_, [&key](const BTreeScalarIndexOperation &candidate) {
							return candidate.kind == BTreeScalarIndexOperationKind::BSIOP_UPSERT &&
								   candidate.value == 0U && candidate.key == key;
						});
				if (operation == global_adjacency_operations_.end()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph write lost a provisional global adjacency key for a newly created Edge");
				}
				global_adjacency_operations_.erase(operation);
			};
			eraseProvisionalKey(provisionalKeys[0U]);
			eraseProvisionalKey(provisionalKeys[1U]);
			global_adjacency_operations_canonical_ = false;
			global_adjacency_plan_.reset();
			global_adjacency_retirement_plan_.reset();
			global_adjacency_initial_plan_.reset();
			properties_.erase(EntityPropertyKey{EntityPropertyOwnerKind::EPOK_EDGE, edgeId}.directoryId());
			edges_.erase(staged);
			return;
		}
		// Removing an Edge must stage both exact keys or neither key. Check before
		// mutableEdge records an erase candidate in the transaction overlay.
		requireAdjacencyOperationCapacity(global_adjacency_operations_.size(), 2U,
										  context_->options.maximum_global_adjacency_write_entries);
		EdgeChange &change = mutableEdge(edgeId);
		const EdgeEntityRecord record = change.record;
		stageGlobalAdjacencyMutation(GlobalAdjacencyDirection::GAD_OUT, record.source_node_id, edgeId,
									 BTreeScalarIndexOperationKind::BSIOP_ERASE);
		stageGlobalAdjacencyMutation(GlobalAdjacencyDirection::GAD_IN, record.target_node_id, edgeId,
									 BTreeScalarIndexOperationKind::BSIOP_ERASE);
		change.erased = true;
		// Keep deletion in the owner-keyed Property batch. Removing only the
		// directory entry would leave the root slot reachable in its Property page.
		properties_.insert_or_assign(EntityPropertyKey{EntityPropertyOwnerKind::EPOK_EDGE, edgeId}.directoryId(),
									 PropertyCollectionRoot{edgeId, Edge::typeId, {}});
	}

	bool NativeGraphWriteTransaction::hasRemainingIncidentEdge(uint64_t nodeId) const {
		return nodeHasIncidentEdges(nodeId);
	}

	void NativeGraphWriteTransaction::eraseNode(uint64_t nodeId) {
		requireActive("erase a Node");
		NodeChange &change = mutableNode(nodeId);
		if (change.inserted) {
			if (hasRemainingIncidentEdge(nodeId)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph write rejects Node deletion while an incident Edge remains");
			}
			properties_.erase(EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, nodeId}.directoryId());
			nodes_.erase(nodeId);
			return;
		}
		if (hasRemainingIncidentEdge(nodeId)) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph write rejects Node deletion while an incident Edge remains");
		}
		change.erased = true;
		// An empty root is an explicit page-local erase plan, published atomically
		// with the Entity Map deletion rather than leaving an orphaned page slot.
		properties_.insert_or_assign(EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, nodeId}.directoryId(),
									 PropertyCollectionRoot{nodeId, Node::typeId, {}});
	}

	void NativeGraphWriteTransaction::prepareIndexMutationPlans(
			LargeValueContentIndexPublicationPlan *contentIndexPlan,
			const std::function<void()> &forceFreshContentIndexPlan) {
		struct PoolDemand {
			BTreePageStore *reclaimer = nullptr;
			SegmentId segment_id{};
			size_t required_frames = 0U;
		};
		struct Candidate {
			BTreePageStore *reclaimer = nullptr;
			std::function<const std::vector<IndexTreeMutationPoolRequest> &()> requests;
			std::function<void()> force_fresh;
		};
		using PoolKey = uint64_t;
		std::map<PoolKey, PoolDemand> demands;
		std::map<PoolKey, std::vector<std::pair<size_t, size_t>>> contributions;
		std::vector<Candidate> candidates;
		for (size_t stagedOrdinal = 0U; stagedOrdinal < scalar_index_mutations_.size(); ++stagedOrdinal) {
			StagedScalarIndexMutation &staged = scalar_index_mutations_[stagedOrdinal];
			if (staged.index == nullptr || staged.index_id == 0U || // staged scalar mutations retain their validated local index and identity.
				!staged.index->usesMetadataStore(*context_->metadata)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged scalar Index operation stream is incomplete or foreign");
			}
			if (staged.retirement_plan.has_value()) {
				const BTreeTreeRetirementPlan &retirement = *staged.retirement_plan;
				if (!staged.operations.empty() || staged.plan.has_value() || !retirement.preparer || // retirement plans are created from one pinned root and cannot coexist with mutation operations.
					retirement.expected_root.index_id != staged.index_id || retirement.pool_requests.empty()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph staged scalar Index retirement plan is incomplete or contradictory");
				}
				continue;
			}
			if (staged.operations.empty()) {
				fail(FormatError::FERR_INVALID_STATE, "native graph staged scalar Index operation stream is empty");
			}
			staged.plan =
					staged.index->planScalarIndexTransactionMutation(
						snapshot_, staged.index_id, staged.operations, true, false, staged.leaf_value_layout);
			candidates.push_back({staged.index,
					[&staged]() -> const std::vector<IndexTreeMutationPoolRequest> & {
						return staged.plan->pool_requests;
					},
					[this, &staged] {
						staged.plan = staged.index->planScalarIndexTransactionMutation(
								snapshot_, staged.index_id, staged.operations, false, true, staged.leaf_value_layout);
					}});
		}
		if (global_adjacency_plan_.has_value()) {
			candidates.push_back({context_->indexes,
					[this]() -> const std::vector<IndexTreeMutationPoolRequest> & {
						return global_adjacency_plan_->pool_requests;
					},
					[this] {
						global_adjacency_plan_ = context_->indexes->planScalarIndexTransactionMutation(
								snapshot_, RSS_GLOBAL_ADJACENCY_INDEX_ID, canonicalGlobalAdjacencyOperations(), true, true,
								BTreeLeafValueLayout::BLVL_EMPTY);
					}});
		}
		if (contentIndexPlan != nullptr) {
			if (!forceFreshContentIndexPlan) {
				fail(FormatError::FERR_INVALID_STATE,
						"native graph content-index capacity planning lost its fresh-rebuild callback");
			}
			Candidate contentCandidate;
			contentCandidate.reclaimer = &context_->properties->largeValueStore()->contentIndex();
			contentCandidate.requests = [contentIndexPlan]() -> const std::vector<IndexTreeMutationPoolRequest> & {
				return contentIndexPlan->mutation.pool_requests;
			};
			contentCandidate.force_fresh = forceFreshContentIndexPlan;
			candidates.push_back(std::move(contentCandidate));
		}

		const auto addDemand = [&](size_t candidateOrdinal, BTreePageStore *reclaimer, SegmentId segmentId,
								   size_t requiredFrames) {
			if (requiredFrames == 0U) {
				return;
			}
			auto [position, inserted] =
				demands.try_emplace(segmentId.value(), PoolDemand{reclaimer, segmentId, 0U});
			static_cast<void>(inserted);
			if (requiredFrames > std::numeric_limits<size_t>::max() - position->second.required_frames) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						"native graph staged scalar Index frame demand overflows host address space");
			}
			position->second.required_frames += requiredFrames;
			contributions[segmentId.value()].push_back({candidateOrdinal, requiredFrames});
		};
		for (size_t candidateOrdinal = 0U; candidateOrdinal < candidates.size(); ++candidateOrdinal) {
			const Candidate &candidate = candidates[candidateOrdinal];
			for (const IndexTreeMutationPoolRequest &pool: candidate.requests()) {
				// A cross-pool replacement consumes a frame from the destination, while
				// appends and same-pool replacements consume frames from the source.
				// Keep those demands separate so a full source pool does not trigger a
				// needless dense rebuild when a compatible destination has capacity.
				const SegmentId replacementPool = pool.replacement_segment_id.isValid()
						? pool.replacement_segment_id
						: pool.segment_id;
				addDemand(candidateOrdinal, candidate.reclaimer, replacementPool,
						pool.expected_replacements.size());
				addDemand(candidateOrdinal, candidate.reclaimer, pool.segment_id, pool.appended_page_count);
			}
		}

		std::map<PoolKey, size_t> availableFrames;
		for (const auto &[key, demand]: demands) {
			const CatalogEntry currentPool = context_->metadata->findCatalog(snapshot_, demand.segment_id);
			if (demand.reclaimer->hasReplacementFrameCapacity(currentPool, demand.required_frames)) {
				availableFrames.emplace(key, demand.required_frames);
				continue;
			}
			// Reclaim only retired frames invisible to every active snapshot before
			// selecting the immutable fresh-pool fallback.
			demand.reclaimer->reclaimRetiredFrames(demand.segment_id);
			size_t available = 0U;
			while (available < RSS_PAGE_POOL_DATA_FRAME_COUNT &&
				   demand.reclaimer->hasReplacementFrameCapacity(currentPool, available + 1U)) {
				++available;
			}
			availableFrames.emplace(key, available);
		}

		std::set<size_t> forceFreshRebuilds;
		// All candidates are accounted against the same current-pool frame budget;
		// independent trees may safely consume distinct frames in one publication.
		// Only an aggregate shortfall below forces a deterministic fresh rebuild.
		bool selected = true;
		while (selected) {
			selected = false;
			for (const auto &[key, demand]: demands) {
				static_cast<void>(demand);
				const auto available = availableFrames.find(key);
				if (available == availableFrames.end()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph scalar Index capacity scan lost a pool availability result");
				}
				size_t remainingDemand = 0U;
				for (const auto &[candidateOrdinal, requiredFrames]: contributions.at(key)) {
					if (!forceFreshRebuilds.contains(candidateOrdinal)) {
						if (requiredFrames > std::numeric_limits<size_t>::max() - remainingDemand) {
							fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
								 "native graph staged scalar Index shared-pool demand overflows host address space");
						}
						remainingDemand += requiredFrames;
					}
				}
				if (remainingDemand <= available->second) {
					continue;
				}
				std::optional<size_t> selectedOrdinal;
				size_t selectedFrames = 0U;
				for (const auto &[candidateOrdinal, requiredFrames]: contributions.at(key)) {
					if (forceFreshRebuilds.contains(candidateOrdinal)) {
						continue;
					}
					if (!selectedOrdinal.has_value() || requiredFrames > selectedFrames) {
						selectedOrdinal = candidateOrdinal;
						selectedFrames = requiredFrames;
					}
				}
				if (!selectedOrdinal.has_value()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph scalar Index cannot reduce an exhausted shared-pool demand");
				}
				forceFreshRebuilds.insert(*selectedOrdinal);
				selected = true;
			}
		}
		for (const size_t candidateOrdinal: forceFreshRebuilds) {
			if (candidateOrdinal >= candidates.size() || !candidates[candidateOrdinal].force_fresh) { // capacity planning records callbacks from the same candidate vector it replays.
				fail(FormatError::FERR_INVALID_STATE,
						"native graph Index capacity planner lost a fresh-rebuild callback");
			}
			candidates[candidateOrdinal].force_fresh();
			for (const IndexTreeMutationPoolRequest &pool: candidates[candidateOrdinal].requests()) {
				if (!pool.expected_replacements.empty() || pool.appended_page_count != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
							"native graph Index fresh rebuild retained a current-pool frame mutation");
				}
			}
		}
		for (const StagedInitialScalarIndexMutation &staged: initial_scalar_index_mutations_) {
			if (staged.index == nullptr || staged.index_id == 0U || !staged.plan.has_value() || // initial index plans retain their validated local index and preflight output.
				!staged.index->usesMetadataStore(*context_->metadata) || staged.plan->index_id != staged.index_id ||
				staged.plan->new_pool_mutations.empty() || !staged.plan->preparer) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged initial scalar Index plan is incomplete or foreign");
			}
		}
	}

	NativeGraphWriteCommitResult NativeGraphWriteTransaction::commit(const MetadataPublishOptions &publishOptions) {
        debug::PerfTrace::addValue("native_graph.write.graph_commit_calls", 1);
		debug::ScopedPerfTimer timer("native_graph.commit");
		requireActive("commit");
		const bool hasGraphWork = !nodes_.empty() || !edges_.empty() || !properties_.empty() ||
								  !scalar_index_mutations_.empty() || !initial_scalar_index_mutations_.empty() ||
								  !vector_bootstraps_.empty() || !vector_deltas_.empty();
		if (!hasGraphWork && node_id_ranges_.empty() && edge_id_ranges_.empty()) {
			fail(FormatError::FERR_INVALID_STATE, "native graph write transaction cannot commit without staged work");
		}
		NativeGraphLogicalIdReservation logicalIds;
		logicalIds.node_ranges = node_id_ranges_;
		logicalIds.edge_ranges = edge_id_ranges_;
		std::lock_guard publicationLock(context_->publication_mutex);
		if (!hasGraphWork) {
			// Logical-ID leasing is already durable. A reservation-only transaction
			// must not publish a second, empty metadata generation just to acknowledge
			// IDs that are intentionally allowed to remain unused.
			if (!context_->metadata->claimNativeGraphLogicalIdReservation(logicalIds)) {
				snapshot_.release();
				state_ = NativeGraphWriteTransactionState::NGWTS_CONFLICTED;
				return {NativeGraphWriteCommitOutcome::NGWCO_CONFLICTED, {}, {}};
			}
			MetadataSnapshotPin current = context_->metadata->openSnapshot();
			const Superblock committed = current.superblock();
			current.release();
			snapshot_.release();
			state_ = NativeGraphWriteTransactionState::NGWTS_COMMITTED;
			return {NativeGraphWriteCommitOutcome::NGWCO_COMMITTED, committed, {}};
		}
		MetadataSnapshotPin current = context_->metadata->openSnapshot();
		if (current.superblock() != snapshot_.superblock()) {
			if (isLogicalIdLeaseOnlyAdvance(*context_->metadata, snapshot_.superblock(), current.superblock())) {
				snapshot_.release();
				snapshot_ = std::move(current);
			} else if (!rebaseAdjacencyOnly(current)) {
				current.release();
				context_->metadata->abandonNativeGraphLogicalIdReservation(logicalIds);
				snapshot_.release();
				state_ = NativeGraphWriteTransactionState::NGWTS_CONFLICTED;
				return {NativeGraphWriteCommitOutcome::NGWCO_CONFLICTED, {}, {}};
			}
		} else {
			current.release();
		}
		{
			debug::ScopedPerfTimer adjacencyPlanTimer("native_graph.commit.adjacency_plan");
			prepareGlobalAdjacencyMutationPlan();
		}

		const LogSequenceNumber checkpointLsn = nextCheckpointLsn(snapshot_.superblock());
		uint64_t nextSegmentId = snapshot_.superblock().next_logical_segment_id;
		uint64_t nextPageId = snapshot_.superblock().next_logical_page_id;
		if (nextSegmentId == 0U || nextPageId == 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph write received invalid logical Segment or Page high-water marks");
		}
		const auto nextSegment = [&nextSegmentId] {
			if (nextSegmentId == std::numeric_limits<uint64_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "native graph write exhausted logical Segment IDs");
			}
			return SegmentId(nextSegmentId);
		};
		const auto reserveSegment = [&nextSegmentId, &nextSegment] {
			const SegmentId segmentId = nextSegment();
			++nextSegmentId;
			return segmentId;
		};
		const auto nextPage = [&nextPageId] {
			if (nextPageId == std::numeric_limits<uint64_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "native graph write exhausted logical Page IDs");
			}
			return PageId(nextPageId);
		};
		const auto reservePage = [&nextPageId, &nextPage] {
			const PageId pageId = nextPage();
			++nextPageId;
			return pageId;
		};

		const std::vector<CatalogEntry> currentPools = context_->entities->currentPools(snapshot_);
		// Fresh pools can receive either initial logical pages or COW images of
		// existing PageIds. Both uses consume the same bounded frame budget and
		// become visible through this transaction's single metadata publication.
		std::vector<FreshEntityPool> labelPools;
		std::map<uint64_t, uint8_t> freeFrames;
		for (const CatalogEntry &pool: currentPools) {
			freeFrames.emplace(pool.segment_id.value(), context_->entities->availableFrameCount(snapshot_, pool));
		}
		std::map<uint64_t, uint8_t> reservedFrames;
		const auto tryReserveCurrentFrame = [&](const CatalogEntry &pool) {
			const auto available = freeFrames.find(pool.segment_id.value());
			if (available == freeFrames.end()) {
				return false;
			}
			if (reservedFrames[pool.segment_id.value()] >= available->second) {
				// Reclaim only frames absent from every active snapshot before declaring
				// the pool full. Consecutive short writes must not exhaust a finite
				// COW pool merely because their retired frames were never revisited.
				context_->entities->reclaimRetiredFrames(pool.segment_id);
				available->second = context_->entities->availableFrameCount(snapshot_, pool);
				if (reservedFrames[pool.segment_id.value()] >= available->second) {
					return false;
				}
			}
			++reservedFrames[pool.segment_id.value()];
			return true;
		};
		const auto reserveCurrentFrame = [&tryReserveCurrentFrame](const CatalogEntry &pool) {
			if (!tryReserveCurrentFrame(pool)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph write has no COW frame capacity in the selected Entity pool");
			}
		};
		const auto reserveReplacementFrame = [&currentPools, &tryReserveCurrentFrame, &labelPools, &reserveSegment,
											  this](const CatalogEntry &source) {
			const auto compatible = [&source](const CatalogEntry &candidate) {
				return candidate.kind == SegmentKind::SGK_ENTITY_PAGE_POOL &&
					   candidate.state == SegmentState::SGS_SEALED && candidate.owner_id == source.owner_id &&
					   candidate.generation.isValid();
			};
			if (compatible(source) && tryReserveCurrentFrame(source)) {
				return EntityPageReplacementDestination{source, std::nullopt};
			}
			for (const CatalogEntry &candidate: currentPools) {
				if (candidate.segment_id != source.segment_id && compatible(candidate) &&
					tryReserveCurrentFrame(candidate)) {
					return EntityPageReplacementDestination{candidate, std::nullopt};
				}
			}
			for (FreshEntityPool &candidate: labelPools) {
				if (candidate.owner_id == source.owner_id && candidate.generation.isValid() &&
					candidate.reservedFrameCount() < RSS_PAGE_POOL_DATA_FRAME_COUNT) {
					++candidate.replacement_frame_count;
					return EntityPageReplacementDestination{{}, candidate.replacementDestination()};
				}
			}
			FreshEntityPool destination{reserveSegment(),
										source.owner_id,
										Generation(1U),
										context_->options.first_slot,
										context_->options.last_slot,
										0U,
										1U};
			const EntityPageFreshPoolDestination fresh = destination.replacementDestination();
			labelPools.push_back(std::move(destination));
			return EntityPageReplacementDestination{{}, fresh};
		};
		const auto findAppendPool = [&]() -> std::optional<CatalogEntry> {
			for (const CatalogEntry &pool: currentPools) {
				if (reservedFrames[pool.segment_id.value()] < freeFrames[pool.segment_id.value()]) {
					return pool;
				}
			}
			return std::nullopt;
		};

		// A fresh pool is only reserved after a LabelSet plan materializes it. A
		// plan that overwrites an existing slot must not consume an unused logical
		// PageId or SegmentId from the durable high-water marks.
		EntityPageAuxiliaryMutationBatch auxiliary;
		std::set<uint64_t> auxiliaryReplacementPages;
		std::set<uint64_t> auxiliaryAppendPages;
		std::set<uint64_t> auxiliaryNewPages;
		const auto accountAuxiliary = [&](EntityPageAuxiliaryMutationBatch plan) {
			for (EntityPageAuxiliaryReplacement &replacement: plan.replacements) {
				if (!auxiliaryReplacementPages.insert(replacement.expected_location.page_id.value()).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph write cannot replace one auxiliary Entity page twice in one transaction");
				}
				// Retirement drops the old frame through the metadata publication; it
				// does not consume a second COW frame in the source pool.
				if (!replacement.retire_page) {
					const EntityPageReplacementDestination destination =
							reserveReplacementFrame(replacement.expected_pool);
					replacement.replacement_pool = destination.current_pool;
					replacement.replacement_new_pool = destination.fresh_pool;
				}
			}
			for (const EntityPageAuxiliaryAppend &append: plan.appends) {
				if (!auxiliaryAppendPages.insert(append.page_id.value()).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph write allocated a duplicate LabelSet append Page ID");
				}
				reserveCurrentFrame(append.expected_pool);
			}
			for (const EntityPageAuxiliaryNewPool &pool: plan.new_pools) {
				if (!auxiliaryNewPages.insert(pool.page_id.value()).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph write allocated a duplicate LabelSet new-pool Page ID");
				}
			}
			appendAuxiliary(auxiliary, std::move(plan));
		};

		// Build all LabelSet pages before Entity placement. The auxiliary frames
		// retain exact Node bindings and remain unreachable until the final batch.
		for (auto &[nodeId, change]: nodes_) {
			if (change.erased || !change.labels.has_value()) {
				continue;
			}
			const std::span<const uint64_t> labels(*change.labels);
			EntityLabelSetWritePlan labelPlan;
			if (change.inserted) {
				labelPlan.excluded_page_ids.reserve(auxiliaryReplacementPages.size());
				for (const uint64_t pageId: auxiliaryReplacementPages) {
					labelPlan.excluded_page_ids.emplace_back(pageId);
				}
			}
			FreshEntityPool candidateNewPool;
			FreshEntityPool *sharedNewPool = nullptr;
			bool createsNewPool = false;
			PageId candidatePageId{};
			if (labels.size() > RSS_ENTITY_INLINE_LABEL_CAPACITY) {
				candidatePageId = nextPage();
				if (const std::optional<CatalogEntry> appendPool = findAppendPool(); appendPool.has_value()) {
					labelPlan.append_page = EntityLabelSetAppendRequest{appendPool->segment_id, candidatePageId};
				} else {
					for (FreshEntityPool &pool: labelPools) {
						if (pool.reservedFrameCount() < RSS_PAGE_POOL_DATA_FRAME_COUNT) {
							sharedNewPool = &pool;
							break;
						}
					}
					if (sharedNewPool == nullptr) {
						candidateNewPool = {nextSegment(),
											context_->options.entity_pool_owner_id,
											Generation(1U),
											context_->options.first_slot,
											context_->options.last_slot,
											0U,
											0U};
						sharedNewPool = &candidateNewPool;
						createsNewPool = true;
					}
					labelPlan.new_pool = EntityLabelSetNewPoolRequest{
							sharedNewPool->segment_id, sharedNewPool->owner_id,	  sharedNewPool->generation,
							candidatePageId,		   sharedNewPool->first_slot, sharedNewPool->last_slot};
				}
			}

			if (change.inserted) {
				EntityLabelSetInitialNodePlan labelResult = context_->labels->planInitialNodeLabels(
						snapshot_, change.record, labels, labelPlan, checkpointLsn);
				change.record = std::move(labelResult.node);
				if (!labelResult.overflow.isEmpty()) {
					auxiliary.node_insertion_bindings.push_back({nodeId, change.record});
				}
				const bool appended = !labelResult.auxiliary_pages.appends.empty();
				const bool created = !labelResult.auxiliary_pages.new_pools.empty();
				if (appended || created) {
					static_cast<void>(reservePage());
				}
				if (created) {
					if (createsNewPool) {
						static_cast<void>(reserveSegment());
						candidateNewPool.initial_frame_count = labelResult.auxiliary_pages.new_pools.size();
						labelPools.push_back(candidateNewPool);
					} else {
						sharedNewPool->initial_frame_count += labelResult.auxiliary_pages.new_pools.size();
					}
				}
				accountAuxiliary(std::move(labelResult.auxiliary_pages));
			} else {
				EntityLabelSetMutationPlan labelResult =
						context_->labels->planNodeLabels(snapshot_, nodeId, labels, labelPlan, checkpointLsn);
				change.record = std::move(labelResult.replacement_node);
				const bool appended = !labelResult.auxiliary_pages.appends.empty();
				const bool created = !labelResult.auxiliary_pages.new_pools.empty();
				if (appended || created) {
					static_cast<void>(reservePage());
				}
				if (created) {
					if (createsNewPool) {
						static_cast<void>(reserveSegment());
						candidateNewPool.initial_frame_count = labelResult.auxiliary_pages.new_pools.size();
						labelPools.push_back(candidateNewPool);
					} else {
						sharedNewPool->initial_frame_count += labelResult.auxiliary_pages.new_pools.size();
					}
				}
				accountAuxiliary(std::move(labelResult.auxiliary_pages));
			}
		}

		std::vector<EntityPageBatchMutation> mutations;
		std::vector<EntityPageBatchAppend> appends;
		std::vector<EntityPageBatchNewPool> newPools;
		std::map<uint64_t, size_t> mutationOrdinals;
		std::map<uint64_t, uint16_t> remainingSlots;
		struct ExistingInsertCandidate {
			PageId page_id{};
			EntityPageRecordKind kind = EntityPageRecordKind::EPRK_NODE;
			CatalogEntry pool;
			uint16_t remaining = 0U;
			bool planned = false;
			bool cached = false;
		};
		const bool needsNodeInsertSearch = std::ranges::any_of(nodes_, [](const auto &entry) {
			return entry.second.inserted && !entry.second.erased;
		});
		const bool needsEdgeInsertSearch = std::ranges::any_of(edges_, [](const auto &entry) {
			return entry.second.inserted && !entry.second.erased;
		});
		std::map<uint64_t, EntityPageRecordKind> currentEntityPageKinds;
		std::vector<ExistingInsertCandidate> existingInsertCandidates;
		existingInsertCandidates.reserve(currentEntityPageKinds.size());
		const auto targetKindIndex = [](EntityPageRecordKind kind) -> size_t {
			if (kind == EntityPageRecordKind::EPRK_NODE) {
				return 0U;
			}
			if (kind == EntityPageRecordKind::EPRK_EDGE) {
				return 1U;
			}
			fail(FormatError::FERR_INVALID_STATE, "native graph write encountered an unknown Entity record kind");
		};
		// All pages observed in one snapshot belong to a stable Catalog
		// generation. Cache the pool descriptor per segment while checking free
		// insertion slots instead of descending the Catalog once per page.
		std::map<uint64_t, CatalogEntry> currentEntityPools;
		std::vector<NativeGraphWritePlannerContext::InsertionTarget> priorInsertionTargets;
		bool priorInsertionCacheValid = false;
		bool usedInsertionTargetCache = false;
		{
			std::lock_guard cacheLock(context_->insertion_cache_mutex);
			const auto &cache = context_->insertion_cache;
			priorInsertionCacheValid = cache.valid;
			priorInsertionTargets = cache.targets;
			// Directory roots are physical COW identities and can change after an
			// unrelated insertion. Cached logical PageIds remain usable; each selected
			// candidate is re-authenticated lazily against the current snapshot below.
			const auto hasCachedKind = [&](EntityPageRecordKind kind) {
				return std::ranges::any_of(cache.targets, [kind](const auto &target) { return target.kind == kind; });
			};
			usedInsertionTargetCache = cache.valid &&
					(!needsNodeInsertSearch || hasCachedKind(EntityPageRecordKind::EPRK_NODE)) &&
					(!needsEdgeInsertSearch || hasCachedKind(EntityPageRecordKind::EPRK_EDGE));
			if (usedInsertionTargetCache) {
				existingInsertCandidates.reserve(cache.targets.size());
				for (const auto &target : cache.targets) {
					existingInsertCandidates.push_back(
							{target.page_id, target.kind, target.pool, target.remaining, false, true});
					currentEntityPools.emplace(target.pool.segment_id.value(), target.pool);
				}
				debug::PerfTrace::addValue("native_graph.commit.entity_insert_target_cache_hits", 1);
			}
		}
		if (!usedInsertionTargetCache) {
			// Updates/deletes never need an insertion target. Avoid walking the
			// complete Entity Directory in those transactions; mixed writes inspect
			// only the kind that can consume a free slot.
			if (needsNodeInsertSearch) {
				context_->metadata->visitNodePageLocations(snapshot_, [&](const EntityPageLocation &location) {
					debug::PerfTrace::addValue("native_graph.commit.entity_insert_node_directory_pages", 1);
					currentEntityPageKinds.emplace(location.page_id.value(), EntityPageRecordKind::EPRK_NODE);
				});
			}
			if (needsEdgeInsertSearch) {
				context_->metadata->visitEdgePageLocations(snapshot_, [&](const EntityPageLocation &location) {
					debug::PerfTrace::addValue("native_graph.commit.entity_insert_edge_directory_pages", 1);
					currentEntityPageKinds.emplace(location.page_id.value(), EntityPageRecordKind::EPRK_EDGE);
				});
			}
			for (const auto &[pageValue, kind]: currentEntityPageKinds) {
				const PageId pageId(pageValue);
				const PageLocation location = context_->metadata->findPageLocation(snapshot_, pageId);
				auto poolIterator = currentEntityPools.find(location.segment_id.value());
				if (poolIterator == currentEntityPools.end()) {
					debug::PerfTrace::addValue("native_graph.commit.entity_insert_catalog_lookups", 1);
					poolIterator = currentEntityPools.emplace(location.segment_id.value(),
							context_->metadata->findCatalog(snapshot_, location.segment_id)).first;
				}
				const CatalogEntry &pool = poolIterator->second;
				if (!isCurrentPool(pool) || location.generation != pool.generation) {
					fail(FormatError::FERR_INVALID_STATE,
							"native graph write found an Entity Map page outside a current Entity pool");
				}
				const uint16_t remaining = context_->entities->freeSlotCount(snapshot_, pageId, kind);
				if (remaining != 0U) {
					existingInsertCandidates.push_back({pageId, kind, pool, remaining, false, false});
				}
			}
		}
		std::array<std::set<std::pair<uint16_t, size_t>>, 2U> existingInsertOrder;
		for (size_t ordinal = 0U; ordinal < existingInsertCandidates.size(); ++ordinal) {
			existingInsertOrder[targetKindIndex(existingInsertCandidates[ordinal].kind)].emplace(
					existingInsertCandidates[ordinal].remaining, ordinal);
		}

		const auto ensureMutation = [&](PageId pageId, EntityPageRecordKind kind) -> EntityPageBatchMutation & {
			const auto existing = mutationOrdinals.find(pageId.value());
			if (existing != mutationOrdinals.end()) {
				EntityPageBatchMutation &mutation = mutations[existing->second];
				if (mutation.record_kind != kind) {
					fail(FormatError::FERR_INVALID_STATE, "native graph write mixed Entity record kinds in one page");
				}
				return mutation;
			}
			const PageLocation location = context_->metadata->findPageLocation(snapshot_, pageId);
			auto poolIterator = currentEntityPools.find(location.segment_id.value());
			if (poolIterator == currentEntityPools.end()) {
				debug::PerfTrace::addValue("native_graph.commit.entity_mutation_catalog_lookups", 1);
				poolIterator = currentEntityPools.emplace(location.segment_id.value(),
						context_->metadata->findCatalog(snapshot_, location.segment_id)).first;
			}
			const CatalogEntry &pool = poolIterator->second;
			if (!isCurrentPool(pool) || location.generation != pool.generation) {
				fail(FormatError::FERR_INVALID_STATE, "native graph write selected a stale Entity page pool");
			}
			const EntityPageReplacementDestination replacement = reserveReplacementFrame(pool);
			remainingSlots.emplace(pageId.value(), context_->entities->freeSlotCount(snapshot_, pageId, kind));
			const size_t ordinal = mutations.size();
			EntityPageBatchMutation mutation;
			mutation.page_id = pageId;
			mutation.record_kind = kind;
			mutation.replacement_pool = replacement.current_pool;
			mutation.replacement_new_pool = replacement.fresh_pool;
			mutations.push_back(std::move(mutation));
			mutationOrdinals.emplace(pageId.value(), ordinal);
			return mutations.back();
		};
		// Resolve existing entities through one ordered Directory range per record
		// kind. Each selected page is still authenticated by ensureMutation(), but
		// large update/delete batches no longer pay one root-to-leaf descent per ID.
		std::vector<uint64_t> nodeMutationIds;
		std::vector<uint64_t> edgeMutationIds;
		for (const auto &[nodeId, change]: nodes_) {
			if (!change.inserted) {
				nodeMutationIds.push_back(nodeId);
			}
		}
		for (const auto &[edgeId, change]: edges_) {
			if (!change.inserted) {
				edgeMutationIds.push_back(edgeId);
			}
		}
		std::vector<std::optional<EntityPageLocation>> nodeMutationLocations(nodeMutationIds.size());
		std::vector<std::optional<EntityPageLocation>> edgeMutationLocations(edgeMutationIds.size());
		if (!nodeMutationIds.empty()) {
			context_->metadata->findNodePageLocationsSorted(snapshot_, nodeMutationIds, nodeMutationLocations);
		}
		if (!edgeMutationIds.empty()) {
			context_->metadata->findEdgePageLocationsSorted(snapshot_, edgeMutationIds, edgeMutationLocations);
		}
		const auto requireMutationLocation = [](uint64_t entityId, const std::vector<uint64_t> &ids,
				const std::vector<std::optional<EntityPageLocation>> &locations,
				const char *message) -> EntityPageLocation {
			const auto iterator = std::ranges::lower_bound(ids, entityId);
			if (iterator == ids.end() || *iterator != entityId) {
				throw std::logic_error(message);
			}
			const size_t ordinal = static_cast<size_t>(iterator - ids.begin());
			if (!locations[ordinal].has_value()) {
				fail(FormatError::FERR_INVALID_STATE, message);
			}
			return *locations[ordinal];
		};

		for (const auto &[nodeId, change]: nodes_) {
			if (change.inserted) {
				continue;
			}
			const EntityPageLocation location = requireMutationLocation(
				nodeId, nodeMutationIds, nodeMutationLocations,
				"native graph write could not resolve an existing Node mutation location");
			EntityPageBatchMutation &mutation = ensureMutation(location.page_id, EntityPageRecordKind::EPRK_NODE);
			if (change.erased) {
				mutation.erased_node_ids.push_back(nodeId);
			} else {
				mutation.node_updates.push_back({nodeId, change.record});
			}
		}
		for (const auto &[edgeId, change]: edges_) {
			if (change.inserted) {
				continue;
			}
			const EntityPageLocation location = requireMutationLocation(
				edgeId, edgeMutationIds, edgeMutationLocations,
				"native graph write could not resolve an existing Edge mutation location");
			EntityPageBatchMutation &mutation = ensureMutation(location.page_id, EntityPageRecordKind::EPRK_EDGE);
			if (change.erased) {
				mutation.erased_edge_ids.push_back(edgeId);
			} else {
				mutation.edge_updates.push_back({edgeId, change.record});
			}
		}

		// The target kind keeps append and fresh-pool ordinals disjoint. Their
		// vectors both start at zero, so a bool is insufficient to identify one.
		enum class OpenTargetStorage : uint8_t {
			OTS_MUTATION = 1,
			OTS_APPEND = 2,
			OTS_NEW_POOL = 3,
		};
		struct OpenTarget {
			EntityPageRecordKind kind = EntityPageRecordKind::EPRK_NODE;
			OpenTargetStorage storage = OpenTargetStorage::OTS_MUTATION;
			size_t ordinal = 0U;
			uint16_t remaining = 0U;
		};
		std::vector<OpenTarget> targets;
		// Keep open insertion targets ordered by remaining capacity. The previous
		// linear best-fit scan made a large batch O(records * targets); this index
		// keeps page grouping deterministic while reducing selection to O(log N).
		std::array<std::set<std::pair<uint16_t, size_t>>, 2U> targetOrder;
		const auto addTarget = [&](OpenTarget target) {
			const size_t ordinal = targets.size();
			targets.push_back(target);
			targetOrder[targetKindIndex(target.kind)].emplace(target.remaining, ordinal);
		};
		for (const auto &[pageValue, ordinal]: mutationOrdinals) {
			const EntityPageBatchMutation &mutation = mutations[ordinal];
			const uint16_t remaining = remainingSlots.at(pageValue);
			if (remaining != 0U) {
				addTarget({mutation.record_kind, OpenTargetStorage::OTS_MUTATION, ordinal, remaining});
			}
		}
		// LabelSet pages allocated above are initial frames of the same physical
		// Entity pool namespace. New Node/Edge pages may fill their remaining
		// frames instead of allocating a second pool for one transaction.
		std::vector<std::pair<SegmentId, size_t>> freshPoolPageCounts;
		freshPoolPageCounts.reserve(labelPools.size());
		for (const FreshEntityPool &pool: labelPools) {
			if (pool.reservedFrameCount() == 0U || pool.reservedFrameCount() > RSS_PAGE_POOL_DATA_FRAME_COUNT) { // fresh LabelSet pool reservations are bounded when the planner creates them.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph write recorded an invalid fresh LabelSet pool frame count");
			}
			freshPoolPageCounts.emplace_back(pool.segment_id, pool.reservedFrameCount());
		}
		const auto createNewEntityPage = [&](EntityPageRecordKind kind) -> OpenTarget {
			for (const auto &[segmentId, count]: freshPoolPageCounts) {
				if (count < RSS_PAGE_POOL_DATA_FRAME_COUNT) {
					const size_t ordinal = newPools.size();
					EntityPageNewPoolRequest request{
							segmentId,	   context_->options.entity_pool_owner_id, Generation(1U),
							reservePage(), context_->options.first_slot,		   context_->options.last_slot};
					newPools.push_back({request, kind, {}, {}});
					for (auto &entry: freshPoolPageCounts) {
						if (entry.first == segmentId) {
							++entry.second;
							break;
						}
					}
					return {kind, OpenTargetStorage::OTS_NEW_POOL, ordinal, EntityPage::recordCapacity(kind)};
				}
			}
			const SegmentId segmentId = reserveSegment();
			freshPoolPageCounts.push_back({segmentId, 1U});
			const size_t ordinal = newPools.size();
			EntityPageNewPoolRequest request{
					segmentId,	   context_->options.entity_pool_owner_id, Generation(1U),
					reservePage(), context_->options.first_slot,		   context_->options.last_slot};
			newPools.push_back({request, kind, {}, {}});
			return {kind, OpenTargetStorage::OTS_NEW_POOL, ordinal, EntityPage::recordCapacity(kind)};
		};
		const auto createAppendEntityPage = [&](EntityPageRecordKind kind) -> OpenTarget {
			const std::optional<CatalogEntry> pool = findAppendPool();
			if (!pool.has_value()) {
				return createNewEntityPage(kind);
			}
			reserveCurrentFrame(*pool);
			const size_t ordinal = appends.size();
			appends.push_back({{*pool, reservePage()}, kind, {}, {}});
			return {kind, OpenTargetStorage::OTS_APPEND, ordinal, EntityPage::recordCapacity(kind)};
		};
		const auto refreshCachedCandidate = [&](ExistingInsertCandidate &candidate) {
			if (!candidate.cached) {
				return candidate.remaining != 0U;
			}
			const std::optional<PageLocation> location =
					context_->metadata->findPageLocationOptional(snapshot_, candidate.page_id);
			if (!location.has_value()) {
				candidate.remaining = 0U;
				return false;
			}
			const CatalogEntry pool = context_->metadata->findCatalog(snapshot_, location->segment_id);
			if (!isCurrentPool(pool) || location->generation != pool.generation) {
				candidate.remaining = 0U;
				return false;
			}
			candidate.pool = pool;
			currentEntityPools.insert_or_assign(pool.segment_id.value(), pool);
			candidate.remaining = context_->entities->freeSlotCount(snapshot_, candidate.page_id, candidate.kind);
			return candidate.remaining != 0U;
		};
		const auto insertNew = [&](uint64_t entityId, const auto &record, EntityPageRecordKind kind) {
			const size_t kindIndex = targetKindIndex(kind);
			std::optional<size_t> targetOrdinal;
			if (!targetOrder[kindIndex].empty()) {
				targetOrdinal = targetOrder[kindIndex].begin()->second;
			}
			if (!targetOrdinal.has_value()) {
				std::optional<size_t> candidateOrdinal;
				for (auto iterator = existingInsertOrder[kindIndex].begin();
						iterator != existingInsertOrder[kindIndex].end();) {
					const auto [indexedRemaining, ordinal] = *iterator++;
					ExistingInsertCandidate &candidate = existingInsertCandidates[ordinal];
					if (candidate.cached && candidate.remaining != indexedRemaining) {
						existingInsertOrder[kindIndex].erase({indexedRemaining, ordinal});
						if (refreshCachedCandidate(candidate)) {
							existingInsertOrder[kindIndex].emplace(candidate.remaining, ordinal);
						}
						continue;
					}
					if (candidate.cached && !refreshCachedCandidate(candidate)) {
						existingInsertOrder[kindIndex].erase({indexedRemaining, ordinal});
						continue;
					}
					if (!candidate.planned && candidate.remaining != 0U &&
							reservedFrames[candidate.pool.segment_id.value()] < freeFrames[candidate.pool.segment_id.value()]) {
						candidateOrdinal = ordinal;
						break;
					}
				}
				if (candidateOrdinal.has_value()) {
					ExistingInsertCandidate &candidate = existingInsertCandidates[*candidateOrdinal];
					static_cast<void>(ensureMutation(candidate.page_id, kind));
					const size_t ordinal = mutationOrdinals.at(candidate.page_id.value());
					candidate.planned = true;
					addTarget({kind, OpenTargetStorage::OTS_MUTATION, ordinal, candidate.remaining});
					targetOrdinal = targets.size() - 1U;
				} else {
					addTarget(createAppendEntityPage(kind));
					targetOrdinal = targets.size() - 1U;
				}
			}
			OpenTarget &target = targets[*targetOrdinal];
			// The selected target is the smallest remaining page. Re-key it after
			// consuming one slot so the next insert keeps the same best-fit order.
			targetOrder[kindIndex].erase({target.remaining, *targetOrdinal});
			if constexpr (std::is_same_v<std::decay_t<decltype(record)>, NodeEntityRecord>) {
				if (target.storage == OpenTargetStorage::OTS_MUTATION) {
					mutations[target.ordinal].node_insertions.push_back({entityId, record});
				} else if (target.storage == OpenTargetStorage::OTS_APPEND) {
					appends[target.ordinal].node_insertions.push_back({entityId, record});
				} else {
					newPools[target.ordinal].node_insertions.push_back({entityId, record});
				}
			} else {
				if (target.storage == OpenTargetStorage::OTS_MUTATION) {
					mutations[target.ordinal].edge_insertions.push_back({entityId, record});
				} else if (target.storage == OpenTargetStorage::OTS_APPEND) {
					appends[target.ordinal].edge_insertions.push_back({entityId, record});
				} else {
					newPools[target.ordinal].edge_insertions.push_back({entityId, record});
				}
			}
			--target.remaining;
			if (target.remaining != 0U) {
				targetOrder[kindIndex].emplace(target.remaining, *targetOrdinal);
			}
		};

		for (const auto &[nodeId, change]: nodes_) {
			if (change.inserted && !change.erased) {
				insertNew(nodeId, change.record, EntityPageRecordKind::EPRK_NODE);
			}
		}
		for (const auto &[edgeId, change]: edges_) {
			if (change.inserted && !change.erased) {
				insertNew(edgeId, change.record, EntityPageRecordKind::EPRK_EDGE);
			}
		}
		// Record the generic page-grouping result once the bounded target index has
		// assigned every insertion.  These counters attribute write amplification
		// without exposing physical locations to the logical API.
		size_t insertionTargetPages = 0U;
		size_t insertionTargetRecords = 0U;
		const auto accountInsertionTargets = [&](const auto &batches) {
			for (const auto &batch: batches) {
				if (batch.node_insertions.size() > std::numeric_limits<size_t>::max() - batch.edge_insertions.size()) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						"native graph insertion target record count overflowed");
				}
				const size_t records = batch.node_insertions.size() + batch.edge_insertions.size();
				if (records != 0U) {
					if (insertionTargetPages == std::numeric_limits<size_t>::max() ||
						insertionTargetRecords > std::numeric_limits<size_t>::max() - records) {
						fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
							"native graph insertion target accounting overflowed");
					}
					++insertionTargetPages;
					insertionTargetRecords += records;
				}
			}
		};
		accountInsertionTargets(mutations);
		accountInsertionTargets(appends);
		accountInsertionTargets(newPools);
		debug::PerfTrace::addValue("native_graph.commit.entity_insert_target_pages",
				static_cast<int64_t>(insertionTargetPages));
		debug::PerfTrace::addValue("native_graph.commit.entity_insert_target_records",
				static_cast<int64_t>(insertionTargetRecords));

		std::vector<EntityPropertyRootMutationPlan> propertyPlans;
		propertyPlans.reserve(properties_.size());
		std::vector<PropertyCollectionRoot> propertyRoots;
		propertyRoots.reserve(properties_.size());
		for (const auto &[directoryId, root]: properties_) {
			static_cast<void>(directoryId);
			propertyRoots.push_back(root);
		}
		{
			debug::ScopedPerfTimer propertyPlanTimer("native_graph.commit.property_plan");
			propertyPlans = context_->properties->planEntityRootReplacements(snapshot_, propertyRoots,
																						 context_->options.property_roots);
		}

		struct PendingDerivedIndexBinding {
			NativeGraphIndexTreeMutation mutation;
			size_t expected_pool_count = 0U;
			BTreeStructuralMutationPreparer preparer;
		};
		std::vector<PendingDerivedIndexBinding> pendingDerivedBindings;
		pendingDerivedBindings.reserve(scalar_index_mutations_.size() + initial_scalar_index_mutations_.size() + 1U);
		const std::vector<BTreeScalarIndexOperation> *deferredAdjacencyOperations = nullptr;
		std::optional<NativeGraphPrivateSegmentPlan> deferredAdjacencyPrivateSegmentPlan;
		NativeGraphPrivateSegmentImagePreparer privateSegmentImagePreparer;
		NativeGraphDeferredIndexPlanner deferredIndexPlanner;
		if (global_adjacency_deferred_bulk_) {
			const std::vector<BTreeScalarIndexOperation> &operations = canonicalGlobalAdjacencyOperations();
			const NativeBulkRunFragmentLayout fragmentLayout = planDeferredAdjacencyFragments(operations);
			const size_t fragmentCount = fragmentLayout.fragments.size();
			deferredAdjacencyOperations = &operations;
			// Scratch candidates are a private append-only suffix, not allocator
			// destinations for final graph pages. Do not inherit the final-page
			// placement range: a bounded arbitrary range would reintroduce a
			// per-fragment slot map or reject a valid current file tail.
			deferredAdjacencyPrivateSegmentPlan = {SegmentKind::SGK_BULK_SCRATCH,
					RSS_GLOBAL_ADJACENCY_INDEX_ID, PhysicalSlotId(1U),
					PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
					RSS_ENVELOPE_BYTES, {}, fragmentCount + 1U};
			privateSegmentImagePreparer = [operations = deferredAdjacencyOperations, fragmentLayout](
					const NativeGraphPrivateSegmentSet &identities,
					size_t ordinal) -> std::vector<std::byte> {
				const size_t fragmentCount = fragmentLayout.fragments.size();
				if (operations == nullptr || identities.size() != fragmentCount + 1U || ordinal >= identities.size()) { // ZYX_COV_EXCL_LINE: metadata invokes this callback with the immutable partition it just validated.
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph deferred adjacency bulk run lost its scratch identity partition");
				}
				if (ordinal < fragmentCount) {
					std::vector<std::byte> encoded = NativeBulkRunFragmentCodec::encode(
							deferredAdjacencyFragment(*operations, fragmentLayout.fragments[ordinal]));
					if (encoded.size() != fragmentLayout.fragments[ordinal].encoded_bytes) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph deferred adjacency source changed after fragment planning");
					}
					return encoded;
				}
				NativeBulkRunManifest manifest;
				manifest.fragments.reserve(fragmentCount);
				for (size_t fragmentOrdinal = 0U; fragmentOrdinal < fragmentCount; ++fragmentOrdinal) {
					const NativeGraphPrivateSegmentMaterialization identity = identities[fragmentOrdinal];
					manifest.fragments.push_back({identity.segment_id, identity.generation, identity.owner_id});
				}
				manifest.entry_count = operations->size();
				manifest.encoded_bytes = fragmentLayout.encoded_bytes;
				return NativeBulkRunManifestCodec::encode(manifest);
			};
			deferredIndexPlanner = [indexes = context_->indexes, metadata = context_->metadata,
										options = context_->options, checkpointLsn, fragmentCount](
										const NativeGraphDeferredIndexPlanningContext &planning) {
					if (indexes == nullptr || metadata == nullptr || planning.private_segments.size() != fragmentCount + 1U) { // ZYX_COV_EXCL_LINE: the planner captures live stores and metadata supplies the exact scratch partition.
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph deferred adjacency planner lost its storage or scratch partition");
				}
				const NativeGraphPrivateSegmentMaterialization manifestIdentity = planning.private_segments.back();
				const NativeBulkRunFragmentLocation manifestLocation{
						manifestIdentity.segment_id, manifestIdentity.generation, manifestIdentity.owner_id};
				// The returned B+Tree materialization callback outlives this planning
				// context. Copy the scoped handles, rather than retaining a reference to
				// the temporary context object passed by the metadata coordinator.
				const NativeBulkRunPayloadReader reader = [privateSegments = planning.private_segments,
						privateSegmentReader = planning.private_segment_reader](const NativeBulkRunFragmentLocation &location) {
					const NativeGraphPrivateSegmentMaterialization identity{
							location.segment_id, location.generation, location.transaction_id};
					try {
						static_cast<void>(privateSegments.ordinalOf(identity));
					} catch (const FormatException &) {
						fail(FormatError::FERR_INVALID_STATE,
								 "native graph deferred adjacency planner received a foreign scratch reference");
					}
					return privateSegmentReader(identity);
				};
				const OrderedBTreeEntrySource operations = NativeBulkRunStore::orderedSourceFromPayloadReader(
						manifestLocation, 1U, reader);
				BulkBTreeBuildOptions build;
				build.first_slot = options.first_slot;
				build.last_slot = options.last_slot;
				build.pool_owner_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
				build.maximum_entries = maximumAdjacencyTreeEntries(options);
				build.maximum_pages = options.maximum_global_adjacency_tree_pages;
				build.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
				NativeGraphDeferredIndexPlan result;
				result.mutation.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
				const std::optional<IndexRootLocation> existingRoot =
						metadata->findIndexRootOptional(planning.current_snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
				if (!existingRoot.has_value()) {
					const OrderedBTreeEntrySource entries = [operations] {
						OrderedBTreeEntryCursor cursor = operations();
						return [cursor = std::move(cursor)]() mutable -> std::optional<BTreePageEntry> {
							std::optional<BTreePageEntry> operation = cursor();
							if (!operation.has_value()) {
								return std::nullopt;
							}
							if (operation->value_or_child != 1U) {
								fail(FormatError::FERR_INVALID_STATE,
									 "native graph cannot erase an adjacency key from an absent deferred tree");
							}
							return BTreePageEntry{std::move(operation->key), 0U};
						};
					};
					const BTreeInitialTreeMutationPlan plan = BulkBTreeBuilder::planNativeTree(
							*indexes, *metadata, planning.current_snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID, entries, build);
					for (const BTreeNewIndexTreePoolMutation &pool: plan.new_pool_mutations) {
						result.mutation.new_pool_requests.push_back(
								{build.first_slot, build.last_slot, pool.owner_id, pool.appended_page_count});
					}
					result.mutation.replacement_root = {
							plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal, true};
					result.preparer = [preparer = plan.preparer, checkpointLsn](
							std::vector<IndexTreeMutationPoolBatch> &batches) { preparer(batches, checkpointLsn); };
					return result;
				}

				const MetadataSnapshotPin *snapshot = &planning.current_snapshot;
				BTreeIndex existing(RSS_GLOBAL_ADJACENCY_INDEX_ID, *indexes, *metadata,
									BTreeLeafValueLayout::BLVL_EMPTY);
				const OrderedBTreeEntrySource oldEntries =
						[existing = std::move(existing), snapshot,
						 maximumEntries = maximumAdjacencyTreeEntries(options)]() mutable {
							return existing.orderedSource(*snapshot, maximumEntries)();
						};
				const OrderedBTreeEntrySource merged = GlobalAdjacencyMutationMerger::merge(
						oldEntries, operations, maximumAdjacencyTreeEntries(options));
				const BTreeInitialTreeMutationPlan replacement = BulkBTreeBuilder::planNativeReplacementTree(
						*indexes, *metadata, planning.current_snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID, merged, build);
				const BTreeTreeRetirementPlan retirement = indexes->planTreeRetirement(
						planning.current_snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID, options.maximum_global_adjacency_tree_pages);
				if (replacement.entry_count == 0U) {
					result.mutation.expected_root = retirement.expected_root;
					result.mutation.pool_requests = retirement.pool_requests;
					result.mutation.retire_root = true;
					result.preparer = [preparer = retirement.preparer, checkpointLsn](
							std::vector<IndexTreeMutationPoolBatch> &batches) { preparer(batches, checkpointLsn); };
					return result;
				}
				result.mutation.expected_root = retirement.expected_root;
				result.mutation.pool_requests = retirement.pool_requests;
				for (const BTreeNewIndexTreePoolMutation &pool: replacement.new_pool_mutations) {
					result.mutation.new_pool_requests.push_back(
							{build.first_slot, build.last_slot, pool.owner_id, pool.appended_page_count});
				}
				result.mutation.replacement_root = {
						replacement.root_page.pool_ordinal, replacement.root_page.appended_page_ordinal, true};
				const size_t retirementPoolCount = retirement.pool_requests.size();
				result.preparer = [retirementPreparer = retirement.preparer, replacementPreparer = replacement.preparer,
							retirementPoolCount, checkpointLsn](std::vector<IndexTreeMutationPoolBatch> &batches) {
					if (batches.size() < retirementPoolCount) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph deferred adjacency replacement changed its pool partition");
					}
					std::vector<IndexTreeMutationPoolBatch> retired(
							batches.begin(), batches.begin() + static_cast<ptrdiff_t>(retirementPoolCount));
					std::vector<IndexTreeMutationPoolBatch> fresh(
							batches.begin() + static_cast<ptrdiff_t>(retirementPoolCount), batches.end());
					retirementPreparer(retired, checkpointLsn);
					replacementPreparer(fresh, checkpointLsn);
					std::move(retired.begin(), retired.end(), batches.begin());
					std::move(fresh.begin(), fresh.end(),
							batches.begin() + static_cast<ptrdiff_t>(retirementPoolCount));
				};
				return result;
			};
		}
		// A new graph-owned LargeValue cannot become reachable without its full
		// content-identity entry. Plan this reserved tree before binding any
		// derived mutation so its current-pool frames join the shared budget.
		std::vector<NativeGraphLargeValueMutation> contentValuesToPublish;
		std::optional<LargeValueContentIndexPublicationPlan> contentIndexPlan;
		LargeValueStore *contentIndexStore = nullptr;
		for (const EntityPropertyRootMutationPlan &propertyPlan: propertyPlans) {
			contentValuesToPublish.insert(contentValuesToPublish.end(), propertyPlan.large_values.begin(),
														 propertyPlan.large_values.end());
		}
		if (!contentValuesToPublish.empty()) {
			const bool conflictsWithUserMutation =
					std::ranges::any_of(scalar_index_mutations_,
										[](const StagedScalarIndexMutation &mutation) {
											return mutation.index_id == RSS_LARGE_VALUE_CONTENT_INDEX_ID;
										}) ||
					std::ranges::any_of(initial_scalar_index_mutations_,
										[](const StagedInitialScalarIndexMutation &mutation) {
											return mutation.index_id == RSS_LARGE_VALUE_CONTENT_INDEX_ID;
										});
			if (conflictsWithUserMutation) {
				fail(FormatError::FERR_INVALID_STATE,
						"native graph transaction cannot modify the reserved LargeValue content index directly");
			}
			contentIndexStore = context_->properties->largeValueStore();
			if (contentIndexStore == nullptr || !contentIndexStore->usesMetadataStore(*context_->metadata)) { // the attached property store owns a validated local LargeValue store.
				fail(FormatError::FERR_INVALID_STATE,
						"native graph LargeValue materialization has no local persistent content index");
			}
			contentIndexPlan = contentIndexStore->planNativeGraphContentIndexMutation(
					snapshot_, contentValuesToPublish, context_->options.first_slot, context_->options.last_slot);
		}
		// Every current-pool B+Tree mutation now competes in one deterministic
		// frame budget. Fresh rebuilds are selected before any typed preparer runs,
		// so scalar, adjacency, and LargeValue content trees cannot overbook a pool.
		if (contentIndexPlan.has_value()) {
			prepareIndexMutationPlans(&*contentIndexPlan, [this, &contentIndexPlan, &contentValuesToPublish,
													contentIndexStore] {
				*contentIndexPlan = contentIndexStore->planNativeGraphContentIndexMutation(
							snapshot_, contentValuesToPublish, context_->options.first_slot, context_->options.last_slot, true);
			});
		} else {
			prepareIndexMutationPlans();
		}
		for (const StagedScalarIndexMutation &staged: scalar_index_mutations_) {
			if (staged.index == nullptr || staged.index_id == 0U ||
				!staged.index->usesMetadataStore(*context_->metadata)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged scalar Index operation stream has no valid finalized overlay plan");
			}
			if (staged.retirement_plan.has_value()) {
				const BTreeTreeRetirementPlan &plan = *staged.retirement_plan;
				if (staged.plan.has_value() || !staged.operations.empty() || !plan.preparer || // retirement planning retains one pinned root and does not coexist with a structural plan.
					plan.expected_root.index_id != staged.index_id || plan.pool_requests.empty()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph staged scalar Index retirement lost its pinned root or frame preparer");
				}
				NativeGraphIndexTreeMutation mutation;
				mutation.index_id = staged.index_id;
				mutation.expected_root = plan.expected_root;
				mutation.pool_requests = plan.pool_requests;
				mutation.retire_root = true;
				pendingDerivedBindings.push_back({std::move(mutation), plan.pool_requests.size(), plan.preparer});
				continue;
			}
			if (!staged.plan.has_value()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged scalar Index operation stream has no finalized overlay plan");
			}
			const BTreeStructuralMutationPlan &plan = *staged.plan;
			if (plan.expected_root.index_id != staged.index_id || !plan.preparer) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged scalar Index plan lost its root identity or frame preparer");
			}
			NativeGraphIndexTreeMutation mutation;
			mutation.index_id = staged.index_id;
			mutation.expected_root = plan.expected_root;
			mutation.pool_requests = plan.pool_requests;
			if (plan.new_pool_mutation.has_value()) {
				mutation.new_pool_requests.push_back({context_->options.first_slot, context_->options.last_slot,
													  plan.new_pool_mutation->owner_id,
													  plan.new_pool_mutation->appended_page_count});
			}
			for (const BTreeNewIndexTreePoolMutation &fresh: plan.additional_new_pool_mutations) {
				mutation.new_pool_requests.push_back({context_->options.first_slot, context_->options.last_slot,
													  fresh.owner_id, fresh.appended_page_count});
			}
			if (plan.replacement_root.has_value()) {
				const IndexTreeMutationAppendedPageReference reference = *plan.replacement_root;
				const bool newlyReserved = reference.pool_ordinal >= plan.pool_requests.size();
				const size_t poolOrdinal =
						newlyReserved ? reference.pool_ordinal - plan.pool_requests.size() : reference.pool_ordinal;
				const size_t poolCount =
						newlyReserved ? mutation.new_pool_requests.size() : mutation.pool_requests.size();
				if (poolOrdinal >= poolCount) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph staged scalar Index root references a missing appended page pool");
				}
				mutation.replacement_root = {poolOrdinal, reference.appended_page_ordinal, newlyReserved};
			}
			mutation.stable_replacement_root = plan.stable_replacement_root;
			const size_t freshPoolCount =
					static_cast<size_t>(plan.new_pool_mutation.has_value()) + plan.additional_new_pool_mutations.size();
			pendingDerivedBindings.push_back(
					{std::move(mutation), plan.pool_requests.size() + freshPoolCount, plan.preparer});
		}
		for (const StagedInitialScalarIndexMutation &staged: initial_scalar_index_mutations_) {
			if (staged.index == nullptr || staged.index_id == 0U || !staged.plan.has_value() || // finalized initial index plans retain their validated local index and preflight output.
				!staged.index->usesMetadataStore(*context_->metadata)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged initial scalar Index has no valid finalized tree plan");
			}
			const BTreeInitialTreeMutationPlan &plan = *staged.plan;
			if (plan.index_id != staged.index_id || plan.new_pool_mutations.empty() || !plan.preparer || // preflight builds the root location from the same nonempty pool mutation set.
				plan.root_page.pool_ordinal >= plan.new_pool_mutations.size() ||
				plan.root_page.appended_page_ordinal >=
						plan.new_pool_mutations[plan.root_page.pool_ordinal].appended_page_count) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged initial scalar Index plan lost its root or page-preparation identity");
			}
			NativeGraphIndexTreeMutation mutation;
			mutation.index_id = plan.index_id;
			for (const BTreeNewIndexTreePoolMutation &pool: plan.new_pool_mutations) {
				mutation.new_pool_requests.push_back({context_->options.first_slot, context_->options.last_slot,
													  pool.owner_id, pool.appended_page_count});
			}
			mutation.replacement_root = {plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal, true};
			pendingDerivedBindings.push_back({std::move(mutation), plan.new_pool_mutations.size(), plan.preparer});
		}
		if (global_adjacency_plan_.has_value()) {
			const BTreeStructuralMutationPlan &plan = *global_adjacency_plan_;
			if (plan.expected_root.index_id != RSS_GLOBAL_ADJACENCY_INDEX_ID || !plan.preparer) { // canonical adjacency planning pins its own root and preparer.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph global adjacency overlay lost its pinned root or frame preparer");
			}
			NativeGraphIndexTreeMutation mutation;
			mutation.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
			mutation.expected_root = plan.expected_root;
			mutation.pool_requests = plan.pool_requests;
			if (plan.new_pool_mutation.has_value()) {
				mutation.new_pool_requests.push_back({context_->options.first_slot, context_->options.last_slot,
													  plan.new_pool_mutation->owner_id,
													  plan.new_pool_mutation->appended_page_count});
			}
			for (const BTreeNewIndexTreePoolMutation &fresh: plan.additional_new_pool_mutations) {
				mutation.new_pool_requests.push_back({context_->options.first_slot, context_->options.last_slot,
													  fresh.owner_id, fresh.appended_page_count});
			}
			if (plan.replacement_root.has_value()) {
				const IndexTreeMutationAppendedPageReference reference = *plan.replacement_root;
				const bool newlyReserved = reference.pool_ordinal >= plan.pool_requests.size();
				const size_t poolOrdinal =
						newlyReserved ? reference.pool_ordinal - plan.pool_requests.size() : reference.pool_ordinal;
				const size_t poolCount =
						newlyReserved ? mutation.new_pool_requests.size() : mutation.pool_requests.size();
				if (poolOrdinal >= poolCount) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph global adjacency root references a missing appended page pool");
				}
				mutation.replacement_root = {poolOrdinal, reference.appended_page_ordinal, newlyReserved};
			}
			mutation.stable_replacement_root = plan.stable_replacement_root;
			const size_t freshPoolCount =
					static_cast<size_t>(plan.new_pool_mutation.has_value()) + plan.additional_new_pool_mutations.size();
			pendingDerivedBindings.push_back(
					{std::move(mutation), plan.pool_requests.size() + freshPoolCount, plan.preparer});
		}
		if (global_adjacency_retirement_plan_.has_value()) {
			const BTreeTreeRetirementPlan &plan = *global_adjacency_retirement_plan_;
			if (plan.expected_root.index_id != RSS_GLOBAL_ADJACENCY_INDEX_ID || !plan.preparer) { // canonical adjacency retirement pins its own root and preparer.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph global adjacency retirement lost its pinned root or frame preparer");
			}
			NativeGraphIndexTreeMutation mutation;
			mutation.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
			mutation.expected_root = plan.expected_root;
			mutation.pool_requests = plan.pool_requests;
			mutation.retire_root = true;
			pendingDerivedBindings.push_back({std::move(mutation), plan.pool_requests.size(), plan.preparer});
		}
		if (global_adjacency_initial_plan_.has_value()) {
			const BTreeInitialTreeMutationPlan &plan = *global_adjacency_initial_plan_;
			if (plan.index_id != RSS_GLOBAL_ADJACENCY_INDEX_ID || plan.new_pool_mutations.empty() || !plan.preparer || // canonical adjacency initial planning derives its root from one nonempty prepared pool set.
				plan.root_page.pool_ordinal >= plan.new_pool_mutations.size() ||
				plan.root_page.appended_page_ordinal >=
						plan.new_pool_mutations[plan.root_page.pool_ordinal].appended_page_count) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph initial global adjacency tree lost its root or page-preparation identity");
			}
			NativeGraphIndexTreeMutation mutation;
			mutation.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
			for (const BTreeNewIndexTreePoolMutation &pool: plan.new_pool_mutations) {
				mutation.new_pool_requests.push_back({context_->options.first_slot, context_->options.last_slot,
													  pool.owner_id, pool.appended_page_count});
			}
			mutation.replacement_root = {plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal, true};
			pendingDerivedBindings.push_back({std::move(mutation), plan.new_pool_mutations.size(), plan.preparer});
		}
		if (contentIndexPlan.has_value()) {
			const size_t poolCount = contentIndexPlan->mutation.pool_requests.size() +
					contentIndexPlan->mutation.new_pool_requests.size();
			pendingDerivedBindings.push_back(
					{std::move(contentIndexPlan->mutation), poolCount, std::move(contentIndexPlan->preparer)});
		}
		std::ranges::sort(pendingDerivedBindings, {},
						  [](const PendingDerivedIndexBinding &binding) { return binding.mutation.index_id; });
		if (std::ranges::adjacent_find(pendingDerivedBindings, {}, [](const PendingDerivedIndexBinding &binding) {
				return binding.mutation.index_id;
			}) != pendingDerivedBindings.end()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph transaction has overlapping existing and initial scalar Index mutations");
		}
		NativeGraphDerivedStateMutation derivedState;
		derivedState.index_tree_mutations.reserve(pendingDerivedBindings.size());
		derivedState.private_segment_plan = std::move(deferredAdjacencyPrivateSegmentPlan);
		for (const EntityPropertyRootMutationPlan &propertyPlan: propertyPlans) {
			derivedState.reused_large_values.insert(derivedState.reused_large_values.end(),
													propertyPlan.reused_large_values.begin(),
													propertyPlan.reused_large_values.end());
		}
		std::ranges::sort(derivedState.reused_large_values, {}, &LargeValueRef::value_id);
		derivedState.reused_large_values.erase(std::unique(derivedState.reused_large_values.begin(),
														   derivedState.reused_large_values.end(),
														   [](const LargeValueRef &left, const LargeValueRef &right) {
															   return left.value_id == right.value_id && left == right;
														   }),
											   derivedState.reused_large_values.end());
		for (size_t ordinal = 1U; ordinal < derivedState.reused_large_values.size(); ++ordinal) {
			if (derivedState.reused_large_values[ordinal - 1U].value_id ==
				derivedState.reused_large_values[ordinal].value_id) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph Property plans reused one LargeValue ID with conflicting references");
			}
		}
		struct DerivedIndexBinding {
			size_t expected_pool_count = 0U;
			BTreeStructuralMutationPreparer preparer;
		};
		std::vector<DerivedIndexBinding> derivedBindings;
		derivedBindings.reserve(pendingDerivedBindings.size());
		for (PendingDerivedIndexBinding &binding: pendingDerivedBindings) {
			derivedState.index_tree_mutations.push_back(std::move(binding.mutation));
			derivedBindings.push_back({binding.expected_pool_count, std::move(binding.preparer)});
		}
		NativeGraphDerivedStatePreparer derivedStatePreparer;
		if (!derivedBindings.empty()) {
			derivedStatePreparer = [bindings = std::move(derivedBindings),
									checkpointLsn](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				if (batches.size() != bindings.size()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph scalar Index publication changed its immutable tree cardinality");
				}
				for (size_t ordinal = 0U; ordinal < bindings.size(); ++ordinal) {
					const DerivedIndexBinding &binding = bindings[ordinal];
					if (!binding.preparer || batches[ordinal].size() != binding.expected_pool_count) { // each private derived-index binding retains its preparer and exact pool count from the same immutable planning pass.
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph scalar Index publication changed its immutable pool plan");
					}
					binding.preparer(batches[ordinal], checkpointLsn);
				}
			};
		} else if (!derivedState.empty()) {
			derivedStatePreparer = [](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
					if (!batches.empty()) { // a reused LargeValue-only derived state has no scalar Index binding, so the paired coordinator callback receives no Index batches.
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph reused LargeValue publication unexpectedly prepared Index pages");
				}
			};
		}

		std::vector<StagedVectorBootstrap> vectorBootstrapBindings;
		vectorBootstrapBindings.reserve(vector_bootstraps_.size());
		for (StagedVectorBootstrap &staged: vector_bootstraps_) {
			const NativeGraphVectorBootstrapMutation &mutation = staged.mutation;
			const uint8_t invalid = // vector bootstrap captures three stores from one validated planner context.
					static_cast<uint8_t>(staged.mapping_trees == nullptr || staged.raw_pages == nullptr ||
										 staged.manifests == nullptr) |
					static_cast<uint8_t>(!staged.mapping_trees->usesMetadataStore(*context_->metadata)) |
					static_cast<uint8_t>(!staged.raw_pages->usesMetadataStore(*context_->metadata)) |
					static_cast<uint8_t>(!staged.manifests->usesMetadataStore(*context_->metadata)) |
					static_cast<uint8_t>(mutation.index_id == 0U || mutation.vector_node_mapping_index_id == 0U ||
									 mutation.vector_count == 0U || mutation.first_vector_id == 0U ||
									 mutation.dimensions == 0U || staged.records.size() != mutation.vector_count ||
									 staged.maximum_pages == 0U);
			if (invalid != 0U) { // vector bootstrap staging validates these captured stores and canonical records before commit constructs this private binding.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged Vector bootstrap lost its typed stores or immutable record plan");
			}
			for (size_t ordinal = 0U; ordinal < staged.records.size(); ++ordinal) {
				const StagedVectorBootstrapRecord &record = staged.records[ordinal];
				if (record.node_id == 0U || record.vector_id != mutation.first_vector_id + ordinal || // staging emits this immutable record sequence after validating the public bootstrap request.
					record.raw_record.size() !=
							sizeof(uint64_t) + static_cast<size_t>(mutation.dimensions) * sizeof(uint16_t) ||
					LittleEndianCodec::readU64(record.raw_record, 0U) != record.vector_id ||
					(ordinal != 0U && record.node_id <= staged.records[ordinal - 1U].node_id)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph staged Vector bootstrap lost canonical Node or Vector identities");
				}
			}
			vectorBootstrapBindings.push_back(std::move(staged));
		}
		std::ranges::sort(vectorBootstrapBindings, {},
						  [](const StagedVectorBootstrap &staged) { return staged.mutation.index_id; });
		if (std::ranges::adjacent_find(vectorBootstrapBindings, {}, [](const StagedVectorBootstrap &staged) { // public staging rejects a second bootstrap for the same Index before this private sort.
				return staged.mutation.index_id;
			}) != vectorBootstrapBindings.end()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph transaction staged duplicate Vector bootstrap Index IDs");
		}
		derivedState.vector_bootstraps.reserve(vectorBootstrapBindings.size());
		for (const StagedVectorBootstrap &staged: vectorBootstrapBindings) {
			derivedState.vector_bootstraps.push_back(staged.mutation);
		}

		NativeGraphVectorBootstrapPreparer vectorBootstrapPreparer;
		if (!vectorBootstrapBindings.empty()) {
			vectorBootstrapPreparer = [bindings = std::move(vectorBootstrapBindings),
									   checkpointLsn](std::vector<NativeGraphVectorBootstrapMaterialization>
															  &materializations) {
				if (materializations.size() != bindings.size()) { // the metadata coordinator allocates one bootstrap materialization for every submitted derived-state binding.
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph Vector bootstrap publication changed its immutable Index cardinality");
				}
				for (size_t ordinal = 0U; ordinal < bindings.size(); ++ordinal) {
					const StagedVectorBootstrap &staged = bindings[ordinal];
					NativeGraphVectorBootstrapMaterialization &materialization = materializations[ordinal];
					const NativeGraphVectorBootstrapMutation &mutation = staged.mutation;
					const uint32_t recordBytes = static_cast<uint32_t>(
							sizeof(uint64_t) + static_cast<size_t>(mutation.dimensions) * sizeof(uint16_t));
					const uint16_t capacity = VectorPageStore::rawSlotCapacity(recordBytes);
					const uint8_t invalid =
							static_cast<uint8_t>(staged.raw_pages == nullptr || staged.manifests == nullptr) |
							static_cast<uint8_t>(capacity == 0U) |
							static_cast<uint8_t>(materialization.mutation.index_id != mutation.index_id) |
							static_cast<uint8_t>(materialization.mutation.vector_node_mapping_index_id !=
												 mutation.vector_node_mapping_index_id) |
							static_cast<uint8_t>(materialization.mutation.first_vector_id != mutation.first_vector_id) |
							static_cast<uint8_t>(materialization.mutation.vector_count != mutation.vector_count) |
							static_cast<uint8_t>(materialization.mutation.dimensions != mutation.dimensions) |
							static_cast<uint8_t>(materialization.mutation.metric != mutation.metric) |
							static_cast<uint8_t>(materialization.node_mapping_root.index_id != mutation.index_id) |
							static_cast<uint8_t>(materialization.vector_node_mapping_root.index_id !=
												 mutation.vector_node_mapping_index_id) |
							static_cast<uint8_t>(!materialization.node_mapping_root.root_page_id.isValid()) |
							static_cast<uint8_t>(!materialization.vector_node_mapping_root.root_page_id.isValid()) |
							static_cast<uint8_t>(materialization.raw_pages.size() != mutation.raw_page_count) |
							static_cast<uint8_t>(materialization.raw_pools.empty()) |
							static_cast<uint8_t>(staged.records.size() != mutation.vector_count);
					if (invalid != 0U) { // the coordinator returns the same validated bootstrap mutation and mapping roots it received from this binding.
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph Vector bootstrap materialization changed its metadata-owned identity");
					}
					const size_t expectedRawPoolCount =
							(materialization.raw_pages.size() - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
					if (materialization.raw_pools.size() != expectedRawPoolCount) { // the authenticated bootstrap allocator derives Raw-pool cardinality from its immutable page count.
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph Vector bootstrap has a noncanonical Raw-pool cardinality");
					}

					std::vector<VectorPage> rawPages;
					rawPages.reserve(materialization.raw_pages.size());
					materialization.vector_locations.clear();
					materialization.vector_locations.reserve(staged.records.size());
					size_t firstRecord = 0U;
					for (size_t pageOrdinal = 0U; pageOrdinal < materialization.raw_pages.size(); ++pageOrdinal) {
						PageLocation &location = materialization.raw_pages[pageOrdinal];
						const CatalogEntry &pool =
								materialization.raw_pools[pageOrdinal / RSS_PAGE_POOL_DATA_FRAME_COUNT];
						if (location.segment_id != pool.segment_id || location.generation != pool.generation || // materialization locations are allocated from their ordinal-selected Raw pool in the same coordinator pass.
							location.page_index != 0U || location.free_bytes != 0U) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native graph Vector bootstrap assigned a Raw page to the wrong pool");
						}
						VectorPageHeader header;
						header.index_id = mutation.index_id;
						header.page_id = location.page_id;
						header.generation = pool.generation;
						header.kind = VectorStorageKind::VSK_RAW;
						header.record_bytes = recordBytes;
						header.slot_capacity = capacity;
						header.bitmap_bytes = static_cast<uint16_t>((static_cast<uint32_t>(capacity) + 7U) / 8U);
						header.committed_lsn = checkpointLsn;
						const PageOwnerHeader owner{header.page_id,
													mutation.index_id,
													PageOwnerKind::POK_VECTOR_RAW,
													0U,
													RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
													header.generation,
													checkpointLsn};
						VectorPage page = VectorPage::create(owner, header);
						const size_t pageEnd =
								std::min(firstRecord + static_cast<size_t>(capacity), staged.records.size());
						for (size_t recordOrdinal = firstRecord; recordOrdinal < pageEnd; ++recordOrdinal) {
							const StagedVectorBootstrapRecord &record = staged.records[recordOrdinal];
							if (record.vector_id != mutation.first_vector_id + recordOrdinal || // staged records are immutable and their ordinal/vector identity was authenticated before private preparation.
								record.raw_record.size() != recordBytes ||
								LittleEndianCodec::readU64(record.raw_record, 0U) != record.vector_id) {
								fail(FormatError::FERR_INVALID_STATE,
									 "native graph Vector bootstrap changed a canonical Raw record");
							}
							const uint16_t slot = page.insert(record.raw_record);
							materialization.vector_locations.push_back(
									{record.vector_id, location.page_id, slot, page.slotGeneration(slot)});
						}
						firstRecord = pageEnd;
						rawPages.push_back(std::move(page));
					}
					if (firstRecord != staged.records.size()) { // page iteration consumes the exact staged-record count using the same positive Raw capacity used by preflight.
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph Vector bootstrap Raw-page plan does not cover every Vector record");
					}
					for (size_t poolOrdinal = 0U; poolOrdinal < materialization.raw_pools.size(); ++poolOrdinal) {
						const size_t firstPage = poolOrdinal * RSS_PAGE_POOL_DATA_FRAME_COUNT;
						const size_t pageCount = std::min(static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT),
														  rawPages.size() - firstPage);
						staged.raw_pages->prepareRawNewPoolPages(
								materialization.raw_pools[poolOrdinal],
								std::span<const VectorPage>(rawPages).subspan(firstPage, pageCount),
								std::span<PageLocation>(materialization.raw_pages).subspan(firstPage, pageCount),
								checkpointLsn);
					}

					VectorManifest manifest;
					manifest.index_id = mutation.index_id;
					manifest.page_id = materialization.manifest_page.page_id;
					manifest.generation = materialization.manifest_pool.generation;
					manifest.committed_lsn = checkpointLsn;
					manifest.dimensions = mutation.dimensions;
					manifest.metric = mutation.metric;
					manifest.node_mapping_root_page_id = materialization.node_mapping_root.root_page_id;
					manifest.node_mapping_root_generation = materialization.node_mapping_root.root_generation;
					manifest.vector_node_mapping_index_id = mutation.vector_node_mapping_index_id;
					manifest.vector_node_mapping_root_page_id = materialization.vector_node_mapping_root.root_page_id;
					manifest.vector_node_mapping_root_generation =
							materialization.vector_node_mapping_root.root_generation;
					manifest.raw_write_tail_page_id = materialization.raw_pages.back().page_id;
					manifest.entries.reserve(rawPages.size());
					for (const VectorPage &page: rawPages) {
						manifest.entries.push_back(
								{page.header().page_id, VectorStorageKind::VSK_RAW, page.header().live_count});
					}
					const PageOwnerHeader manifestOwner{manifest.page_id,
														mutation.index_id,
														PageOwnerKind::POK_VECTOR_MANIFEST,
														0U,
														RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
														manifest.generation,
														checkpointLsn};
					staged.manifests->prepareNewPoolInitial(materialization.manifest_pool,
															VectorManifestPage::create(manifestOwner, manifest),
															checkpointLsn, materialization.manifest_page);
					materialization.manifest_location = {mutation.index_id, manifest.page_id, manifest.generation};
				}
			};
		}

		std::vector<StagedVectorDelta> vectorDeltaBindings;
		vectorDeltaBindings.reserve(vector_deltas_.size());
		for (StagedVectorDelta &staged: vector_deltas_) {
			const NativeGraphVectorDeltaMutation &mutation = staged.mutation;
			const uint8_t invalid = // vector delta captures three stores from one validated planner context.
					static_cast<uint8_t>(staged.mapping_trees == nullptr || staged.raw_pages == nullptr ||
										 staged.manifests == nullptr) |
					static_cast<uint8_t>(!staged.mapping_trees->usesMetadataStore(*context_->metadata)) |
					static_cast<uint8_t>(!staged.raw_pages->usesMetadataStore(*context_->metadata)) |
					static_cast<uint8_t>(!staged.manifests->usesMetadataStore(*context_->metadata)) |
					static_cast<uint8_t>(mutation.index_id == 0U || mutation.vector_node_mapping_index_id == 0U ||
										 mutation.dimensions == 0U || staged.maximum_raw_pages == 0U);
			if (invalid != 0U) { // vector delta staging validates its captured stores and source fence before commit constructs this private binding.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged Vector delta lost its typed stores or immutable source fence");
			}
			vectorDeltaBindings.push_back(std::move(staged));
		}
		std::ranges::sort(vectorDeltaBindings, {},
						  [](const StagedVectorDelta &staged) { return staged.mutation.index_id; });
		if (std::ranges::adjacent_find(vectorDeltaBindings, {}, [](const StagedVectorDelta &staged) { // public staging rejects a second delta for one Index before this private sort.
				return staged.mutation.index_id;
			}) != vectorDeltaBindings.end()) {
			fail(FormatError::FERR_INVALID_STATE, "native graph transaction staged duplicate Vector delta Index IDs");
		}
		derivedState.vector_deltas.reserve(vectorDeltaBindings.size());
		for (const StagedVectorDelta &staged: vectorDeltaBindings) {
			derivedState.vector_deltas.push_back(staged.mutation);
		}

		NativeGraphVectorDeltaPreparer vectorDeltaPreparer;
		if (!vectorDeltaBindings.empty()) {
			auto bindings = std::make_shared<std::vector<StagedVectorDelta>>(std::move(vectorDeltaBindings));
			vectorDeltaPreparer = [bindings = std::move(bindings), checkpointLsn](
										  std::vector<NativeGraphVectorDeltaMaterialization> &materializations) {
				if (materializations.size() != bindings->size()) { // the metadata coordinator allocates one delta materialization for every submitted derived-state binding.
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph Vector delta publication changed its immutable Index cardinality");
				}
				for (size_t ordinal = 0U; ordinal < bindings->size(); ++ordinal) {
					const StagedVectorDelta &staged = (*bindings)[ordinal];
					NativeGraphVectorDeltaMaterialization &materialization = materializations[ordinal];
					const NativeGraphVectorDeltaMutation &mutation = staged.mutation;
					const uint32_t recordBytes = static_cast<uint32_t>(
							sizeof(uint64_t) + static_cast<size_t>(mutation.dimensions) * sizeof(uint16_t));
					const uint16_t capacity = VectorPageStore::rawSlotCapacity(recordBytes);
					if (capacity == 0U || materialization.mutation.index_id != mutation.index_id || // preflight fixes the Raw capacity and the coordinator returns the same authenticated mutation and mapping roots.
						materialization.mutation.raw_pages.size() != staged.raw_page_images.size() ||
						materialization.node_mapping_root.index_id != mutation.index_id ||
						materialization.vector_node_mapping_root.index_id != mutation.vector_node_mapping_index_id ||
						(materialization.delta_mapping_root.index_id != mutation.delta_mapping_index_id) ||
						(mutation.delta_mapping_index_id != 0U &&
						 (!materialization.delta_mapping_root.root_page_id.isValid() ||
						  !materialization.delta_mapping_root.root_generation.isValid()))) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph Vector delta materialization changed its source or mapping identity");
					}
					materialization.vector_locations.clear();
					materialization.vector_locations.reserve(static_cast<size_t>(mutation.inserted_vector_count));
					VectorManifest manifest = staged.source_manifest;
					manifest.page_id = materialization.manifest_replacement.page_id;
					manifest.generation = materialization.manifest_replacement.generation;
					manifest.committed_lsn = checkpointLsn;
					manifest.node_mapping_root_page_id = materialization.node_mapping_root.root_page_id;
					manifest.node_mapping_root_generation = materialization.node_mapping_root.root_generation;
					manifest.vector_node_mapping_root_page_id = materialization.vector_node_mapping_root.root_page_id;
					manifest.vector_node_mapping_root_generation =
							materialization.vector_node_mapping_root.root_generation;
					manifest.delta_mapping_index_id = mutation.delta_mapping_index_id;
					manifest.delta_mapping_root_page_id = materialization.delta_mapping_root.root_page_id;
					manifest.delta_mapping_root_generation = materialization.delta_mapping_root.root_generation;
					manifest.delta_entry_count = mutation.delta_entry_count;
					std::vector<std::vector<VectorPage>> freshPages(materialization.raw_new_pools.size());
					std::vector<std::vector<size_t>> freshPageOrdinals(materialization.raw_new_pools.size());
					for (size_t rawOrdinal = 0U; rawOrdinal < staged.raw_page_images.size(); ++rawOrdinal) {
						const StagedVectorDeltaRawPage &stagedRaw = staged.raw_page_images[rawOrdinal];
						NativeGraphVectorDeltaRawPageMaterialization &raw = materialization.raw_pages[rawOrdinal];
						if (raw.mutation.placement != stagedRaw.mutation.placement || // the coordinator replays the exact authenticated Raw-page plan retained by this delta binding.
							raw.mutation.inserted_record_count != stagedRaw.mutation.inserted_record_count ||
							raw.mutation.page_ordinal != stagedRaw.mutation.page_ordinal) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native graph Vector delta Raw-page identity changed during materialization");
						}
						const bool cow = raw.mutation.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_COW_TAIL;
						VectorPage page = [&]() {
							if (cow) {
								if (!stagedRaw.tail_image.has_value()) { // COW Raw transitions retain their source tail image during delta preflight.
									fail(FormatError::FERR_INVALID_STATE,
										 "native graph Vector delta COW tail lost its retained page image");
								}
								const PageOwnerHeader owner{raw.location.page_id,
															mutation.index_id,
															PageOwnerKind::POK_VECTOR_RAW,
															0U,
															RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
															raw.location.generation,
															checkpointLsn};
								return stagedRaw.tail_image->rebindOwner(owner);
							}
							VectorPageHeader header;
							header.index_id = mutation.index_id;
							header.page_id = raw.location.page_id;
							header.generation = raw.pool.generation;
							header.kind = VectorStorageKind::VSK_RAW;
							header.record_bytes = recordBytes;
							header.slot_capacity = capacity;
							header.bitmap_bytes = static_cast<uint16_t>((static_cast<uint32_t>(capacity) + 7U) / 8U);
							header.committed_lsn = checkpointLsn;
							const PageOwnerHeader owner{header.page_id,
														mutation.index_id,
														PageOwnerKind::POK_VECTOR_RAW,
														0U,
														RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
														header.generation,
														checkpointLsn};
							VectorPage created = VectorPage::create(owner, header);
							for (const size_t recordOrdinal: stagedRaw.record_ordinals) {
									if (recordOrdinal >= staged.records.size() || // preflight emits Raw record ordinals from the immutable staged insertion sequence.
									staged.records[recordOrdinal].kind != NativeVectorMutationKind::NVMK_INSERT ||
									staged.records[recordOrdinal].raw_record.size() != recordBytes ||
									LittleEndianCodec::readU64(staged.records[recordOrdinal].raw_record, 0U) !=
											staged.records[recordOrdinal].vector_id) {
									fail(FormatError::FERR_INVALID_STATE,
										 "native graph Vector delta lost a canonical inserted Raw record");
								}
								static_cast<void>(created.insert(staged.records[recordOrdinal].raw_record));
							}
							return created;
						}();
						for (const size_t recordOrdinal: stagedRaw.record_ordinals) {
							const StagedVectorDeltaRecord &record = staged.records[recordOrdinal];
							const std::optional<uint16_t> slot = page.findVectorId(record.vector_id);
							if (!slot.has_value()) { // each validated inserted Raw record is inserted into this fresh page before its location is captured.
								fail(FormatError::FERR_INVALID_STATE,
									 "native graph Vector delta Raw page lost an inserted Vector ID");
							}
							materialization.vector_locations.push_back(
									{record.vector_id, raw.location.page_id, *slot, page.slotGeneration(*slot)});
						}
						const VectorManifestEntry updatedEntry{raw.location.page_id, VectorStorageKind::VSK_RAW,
															   static_cast<uint32_t>(page.header().live_count)};
						auto entry = std::lower_bound(
								manifest.entries.begin(), manifest.entries.end(), updatedEntry,
								[](const VectorManifestEntry &left, const VectorManifestEntry &right) {
									return std::tuple<uint16_t, uint64_t>{static_cast<uint16_t>(left.storage_kind),
																		  left.page_id.value()} <
										   std::tuple<uint16_t, uint64_t>{static_cast<uint16_t>(right.storage_kind),
																		  right.page_id.value()};
								});
						const bool matchingEntry = entry != manifest.entries.end() &&
												   entry->page_id == updatedEntry.page_id &&
												   entry->storage_kind == updatedEntry.storage_kind;
						if (cow) {
							if (!matchingEntry) { // a COW transition originates from a Manifest entry authenticated by the source snapshot.
								fail(FormatError::FERR_INVALID_STATE,
									 "native graph Vector delta COW tail is absent from its source Manifest");
							}
							*entry = updatedEntry;
						} else if (matchingEntry) { // a non-COW Raw page receives a newly allocated page identity absent from the source Manifest.
							fail(FormatError::FERR_INVALID_STATE, "native graph Vector delta assigned a new Raw page "
																  "that already exists in its Manifest");
						} else {
							manifest.entries.insert(entry, updatedEntry);
						}
						if (cow) {
							staged.raw_pages->prepareRawReplacement(raw.pool, raw.mutation.expected_location,
																	raw.mutation.expected_owner_lsn, page,
																	checkpointLsn, raw.location);
						} else if (raw.mutation.placement ==
								   NativeGraphVectorDeltaRawPlacement::NGVDRP_APPEND_TO_CURRENT_POOL) {
							staged.raw_pages->prepareRawAppend(raw.pool, page, checkpointLsn, raw.location);
						} else {
							const size_t poolOrdinal = static_cast<size_t>(raw.mutation.new_pool_ordinal - 1U);
								if (poolOrdinal >= freshPages.size() || // new-pool ordinals and pool identities are allocated together by the authenticated delta materialization.
								raw.pool.segment_id != materialization.raw_new_pools[poolOrdinal].segment_id ||
								raw.pool.physical_slot != materialization.raw_new_pools[poolOrdinal].physical_slot ||
								raw.pool.generation != materialization.raw_new_pools[poolOrdinal].generation) {
								fail(FormatError::FERR_INVALID_STATE,
									 "native graph Vector delta fresh Raw page lost its assigned pool identity");
							}
							freshPages[poolOrdinal].push_back(std::move(page));
							freshPageOrdinals[poolOrdinal].push_back(rawOrdinal);
						}
					}
					for (size_t poolOrdinal = 0U; poolOrdinal < freshPages.size(); ++poolOrdinal) {
						if (freshPages[poolOrdinal].empty() || // the delta materializer emits one nonempty bounded Raw-page group for every allocated fresh pool.
							freshPages[poolOrdinal].size() > RSS_PAGE_POOL_DATA_FRAME_COUNT) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native graph Vector delta has an invalid fresh Raw-pool page group");
						}
						std::vector<PageLocation> locations;
						for (const size_t rawOrdinal: freshPageOrdinals[poolOrdinal]) {
							locations.push_back(materialization.raw_pages[rawOrdinal].location);
						}
						staged.raw_pages->prepareRawNewPoolPages(materialization.raw_new_pools[poolOrdinal],
																 freshPages[poolOrdinal], locations, checkpointLsn);
						for (size_t index = 0U; index < locations.size(); ++index) {
							materialization.raw_pages[freshPageOrdinals[poolOrdinal][index]].location =
									locations[index];
						}
					}
					if (!materialization.raw_pages.empty()) {
						manifest.raw_write_tail_page_id = materialization.raw_pages.back().location.page_id;
					}
					const PageOwnerHeader owner{manifest.page_id,
												mutation.index_id,
												PageOwnerKind::POK_VECTOR_MANIFEST,
												0U,
												RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
												manifest.generation,
												checkpointLsn};
					const VectorManifestPage page = VectorManifestPage::create(owner, manifest);
					if (mutation.manifest_in_new_pool) {
						staged.manifests->prepareNewPoolReplacement(materialization.manifest_new_pool, page,
																	checkpointLsn,
																	materialization.manifest_replacement);
					} else {
						staged.manifests->prepareReplacement(mutation.expected_manifest_pool,
															 mutation.expected_manifest_location,
															 mutation.expected_manifest_owner_lsn, page, checkpointLsn,
															 materialization.manifest_replacement);
					}
					materialization.manifest_location = {mutation.index_id, manifest.page_id, manifest.generation};
				}
			};
		}

		const auto refreshInsertionTargetCache = [&](const EntityPageBatchResult &published) noexcept {
			try {
				MetadataSnapshotPin nextSnapshot = context_->metadata->openSnapshot();
				std::map<std::pair<uint64_t, uint16_t>, NativeGraphWritePlannerContext::InsertionTarget> targets;
				// Preserve untouched targets for update/delete-only transactions and for
				// COW publications that changed a Directory root. PageIds are logical and
				// never reused; the next insertion lazily re-authenticates each target.
				if (priorInsertionCacheValid) {
					for (const auto &target: priorInsertionTargets) {
						targets[{target.page_id.value(), static_cast<uint16_t>(target.kind)}] = target;
					}
				}
				for (const ExistingInsertCandidate &candidate: existingInsertCandidates) {
					if (candidate.remaining != 0U) {
						targets[{candidate.page_id.value(), static_cast<uint16_t>(candidate.kind)}] =
								{candidate.page_id, candidate.kind, candidate.pool, candidate.remaining};
					}
				}
				for (size_t ordinal = 0U; ordinal < mutations.size(); ++ordinal) {
					const EntityPageBatchMutation &mutation = mutations[ordinal];
					const uint16_t oldRemaining = remainingSlots.at(mutation.page_id.value());
					const size_t inserted = mutation.record_kind == EntityPageRecordKind::EPRK_NODE
							? mutation.node_insertions.size() : mutation.edge_insertions.size();
					const size_t erased = mutation.record_kind == EntityPageRecordKind::EPRK_NODE
							? mutation.erased_node_ids.size() : mutation.erased_edge_ids.size();
					const auto key = std::make_pair(mutation.page_id.value(), static_cast<uint16_t>(mutation.record_kind));
					if (ordinal >= published.pages.size() || published.pages[ordinal].page_retired ||
							inserted > static_cast<size_t>(oldRemaining) + erased) {
						targets.erase(key);
						continue;
					}
					const uint16_t remaining = static_cast<uint16_t>(static_cast<size_t>(oldRemaining) + erased - inserted);
					if (remaining == 0U) {
						targets.erase(key);
						continue;
					}
					const PageLocation location = context_->metadata->findPageLocation(nextSnapshot, mutation.page_id);
					const CatalogEntry pool = context_->metadata->findCatalog(nextSnapshot, location.segment_id);
					targets[key] = {mutation.page_id, mutation.record_kind, pool, remaining};
				}
				const auto addFreshTarget = [&](PageId pageId, EntityPageRecordKind kind, size_t inserted) {
					const uint16_t capacity = EntityPage::recordCapacity(kind);
					if (inserted >= capacity) return;
					const PageLocation location = context_->metadata->findPageLocation(nextSnapshot, pageId);
					const CatalogEntry pool = context_->metadata->findCatalog(nextSnapshot, location.segment_id);
					targets[{pageId.value(), static_cast<uint16_t>(kind)}] =
							{pageId, kind, pool, static_cast<uint16_t>(capacity - inserted)};
				};
				for (const EntityPageBatchAppend &append: appends) {
					addFreshTarget(append.request.page_id, append.record_kind,
							append.record_kind == EntityPageRecordKind::EPRK_NODE ? append.node_insertions.size()
																								 : append.edge_insertions.size());
				}
				for (const EntityPageBatchNewPool &newPool: newPools) {
					addFreshTarget(newPool.request.page_id, newPool.record_kind,
							newPool.record_kind == EntityPageRecordKind::EPRK_NODE ? newPool.node_insertions.size()
																								 : newPool.edge_insertions.size());
				}
				std::lock_guard cacheLock(context_->insertion_cache_mutex);
				context_->insertion_cache.targets.clear();
				context_->insertion_cache.targets.reserve(targets.size());
				for (const auto &[key, target]: targets) {
					static_cast<void>(key);
					context_->insertion_cache.targets.push_back(target);
				}
				context_->insertion_cache.node_directory_root = nextSnapshot.superblock().node_entity_directory_root;
				context_->insertion_cache.edge_directory_root = nextSnapshot.superblock().edge_entity_directory_root;
				context_->insertion_cache.valid = true;
				nextSnapshot.release();
			} catch (...) {
				std::lock_guard cacheLock(context_->insertion_cache_mutex);
				context_->insertion_cache.valid = false;
				context_->insertion_cache.targets.clear();
			}
		};

		try {
			std::vector<uint64_t> vectorRebuildIndexIds;
			for (const StagedVectorDelta &staged: vector_deltas_) {
				if (staged.mutation.maintenance_rebuild_required) {
					vectorRebuildIndexIds.push_back(staged.mutation.index_id);
				}
			}
			std::ranges::sort(vectorRebuildIndexIds);
			const EntityPageBatchResult published = [&] {
				debug::ScopedPerfTimer publishTimer("native_graph.commit.entity_publish");
				return context_->entities->applyNativeGraphBatchTransaction(
						snapshot_, mutations, appends, newPools, propertyPlans, *context_->properties,
						context_->options.property_roots, auxiliary, derivedState, derivedStatePreparer, checkpointLsn,
						publishOptions, logicalIds, vectorBootstrapPreparer, vectorDeltaPreparer,
						privateSegmentImagePreparer, {}, deferredIndexPlanner, {});
			}();
			refreshInsertionTargetCache(published);
			snapshot_.release();
			state_ = NativeGraphWriteTransactionState::NGWTS_COMMITTED;
			return {NativeGraphWriteCommitOutcome::NGWCO_COMMITTED, published.superblock,
					std::move(vectorRebuildIndexIds)};
		} catch (...) {
			snapshot_.release();
			state_ = NativeGraphWriteTransactionState::NGWTS_INDETERMINATE;
			throw;
		}
	}

	void NativeGraphWriteTransaction::rollback() {
		if (state_ == NativeGraphWriteTransactionState::NGWTS_ACTIVE) {
			// Rollback is also the staging-allocation-failure path. Borrow the
			// already retained ranges instead of allocating a second reservation.
			context_->metadata->abandonNativeGraphLogicalIdReservation(node_id_ranges_, edge_id_ranges_);
			snapshot_.release();
			state_ = NativeGraphWriteTransactionState::NGWTS_ROLLED_BACK;
		}
	}

	NativeGraphWritePlanner::NativeGraphWritePlanner(RelocatableMetadataStore &metadata, EntityPageStore &entities,
													 EntityLabelSetStore &labels,
													 PropertyCollectionRootStore &properties, BTreePageStore &indexes,
													 NativeGraphWritePlannerOptions options) :
		context_(std::make_shared<NativeGraphWritePlannerContext>()) {
		validatePlannerOptions(options);
		const uint8_t foreignDomain = static_cast<uint8_t>(!entities.usesMetadataStore(metadata)) |
									  static_cast<uint8_t>(!labels.usesMetadataStore(metadata)) |
									  static_cast<uint8_t>(!properties.usesMetadataStore(metadata)) |
									  static_cast<uint8_t>(!indexes.usesMetadataStore(metadata));
		if (foreignDomain != 0U) {
			throw std::invalid_argument("native graph write planner requires one metadata storage domain");
		}
		context_->metadata = &metadata;
		context_->entities = &entities;
		context_->labels = &labels;
		context_->properties = &properties;
		context_->indexes = &indexes;
		context_->options = options;
	}

	NativeGraphWriteTransaction NativeGraphWritePlanner::beginTransaction() {
		if (context_ == nullptr) {
			fail(FormatError::FERR_INVALID_STATE, "native graph write planner is detached from storage");
		}
		return NativeGraphWriteTransaction(context_, context_->metadata->openSnapshot());
	}

	NativeGraphLogicalIdReservation NativeGraphWritePlanner::reserveLogicalIds(uint64_t nodeCount, uint64_t edgeCount) {
		if (context_ == nullptr || (nodeCount == 0U && edgeCount == 0U) ||
			nodeCount > static_cast<uint64_t>(context_->options.maximum_logical_id_reservation) ||
			edgeCount > static_cast<uint64_t>(context_->options.maximum_logical_id_reservation)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph logical transaction identity reservation exceeds its configured work bound");
		}
		return context_->metadata->acquireNativeGraphLogicalIds(
				nodeCount, edgeCount, static_cast<uint64_t>(context_->options.logical_id_lease_block_size));
	}

} // namespace graph::storage
