#pragma once
#include "graph/storage/relocatable/entity/ScanMembershipMutation.hpp"
#include "graph/storage/relocatable/metadata/NativeGraphDeferredIndexImages.hpp"
#include "graph/storage/relocatable/property/HybridPropertyExtension.hpp"
/**
 * @file NativeGraphWritePlanner.cpp
 * @brief Snapshot-pinned native Entity/Page graph write planning.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/EntityFreeSpaceIndex.hpp"
#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"

#include "graph/utils/OwnedAssociativeContainers.hpp"
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
#include "graph/storage/relocatable/property/HybridPropertyIndexIds.hpp"
#include "graph/storage/relocatable/property/HybridPropertyRecords.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"
#include "graph/storage/relocatable/adjacency/CoveredAdjacencyCandidate.hpp"
#include "graph/storage/relocatable/entity/NativeScalarIndexMutationPlanner.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/index/FixedBTreeMapMutation.hpp"
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
            std::array<bool, 2> complete_kinds{};
			bool valid = false;
		};
		std::mutex insertion_cache_mutex;
		InsertionTargetCache insertion_cache;
	};

	namespace {
        std::shared_ptr<query::execution::MaterializedByteBudget> nativeFieldWorkspace() {
            constexpr size_t bytes = 1024U * 1024U;
            if (!RSS_SCAN_PATH_FORMAT_ENABLED)
                return std::make_shared<query::execution::MaterializedByteBudget>(bytes);
            auto parent = utils::OwnedAllocationScope::owner();
            if (!parent) throw query::execution::MaterializedByteLimitExceeded();
            auto credit = std::make_shared<utils::OwnedHeapReservation>(parent, bytes);
            // The child may outlive the local staging branch inside its writer.
            // Its deleter retains the parent credit through the last child user.
            return {new query::execution::MaterializedByteBudget(bytes - 4096U),
                [credit = std::move(credit)](query::execution::MaterializedByteBudget *value) {
                    static_cast<void>(credit); delete value;
                }};
        }


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
                static_cast<uint8_t>(current.hybrid_property_descriptor_root == base.hybrid_property_descriptor_root) &
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
			return indexId == RSS_LARGE_VALUE_CONTENT_INDEX_ID || indexId == RSS_GLOBAL_ADJACENCY_INDEX_ID ||
                isHybridPropertyIndexId(indexId) || isEntityFreeSpaceIndexId(indexId);
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


} // namespace graph::storage
