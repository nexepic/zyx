/**
 * @file RelocatableMetadataStore.hpp
 * @brief Serialized facade for the experimental relocatable Catalog metadata stack.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/metadata/CatalogRetireQueueReclaimer.hpp"
#include "graph/storage/relocatable/metadata/CatalogSegmentRelocationCoordinator.hpp"
#include "graph/storage/relocatable/metadata/CatalogSegmentRetirementCoordinator.hpp"
#include "graph/storage/relocatable/metadata/CatalogSlotRelocation.hpp"
#include "graph/storage/relocatable/metadata/CatalogSlotReservation.hpp"
#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataPoolOwnership.hpp"
#include "graph/storage/relocatable/metadata/MetadataReservationCoordinator.hpp"
#include "graph/storage/relocatable/metadata/MetadataRetirement.hpp"
#include "graph/storage/relocatable/metadata/MetadataSnapshotRegistry.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/storage/relocatable/property/LargeValue.hpp"
#include "graph/storage/relocatable/vector/VectorManifestPage.hpp"

namespace graph::storage {

	class BTreePageStore;
	class SystemCatalog;
	class DerivedObjectReachabilityReclaimer;
	class VectorCodebookReachabilityReclaimer;
	class VectorAdjacencyPageStore;
	class VectorPageStore;
	class LargeValueStore;
	class RelocatableIntegrityScanner;
	class NativeStreamedGraphDeferredPublication;
	struct NativeStreamedGraphFinalPublicationRequest;
	struct LargeValueMaterializationResult;

	struct MetadataPagePoolReclamation {
		Superblock superblock;
		uint64_t reclaimed_pool_slots = 0;
		std::vector<PhysicalSlotId> released_pool_slots;
	};

	// A COW image can seed a previously unallocated Entity pool. The stable
	// PageId remains unchanged; only its Page Directory location moves when the
	// enclosing graph generation is published.
	struct EntityPageFreshPoolDestination {
		SegmentId segment_id{};
		uint64_t pool_owner_id = 0U;
		Generation generation{};
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};

		constexpr bool operator==(const EntityPageFreshPoolDestination &) const = default;
	};

	// A sparse metadata pool is evacuated by publishing fresh COW directory
	// pages outside the source slot. The pool remains Catalog-owned until the
	// ordinary snapshot-safe reclaimer observes its now-empty frame bitmap.
	struct MetadataPagePoolEvacuation {
		Superblock superblock;
		PhysicalSlotId source_pool_slot{};
		uint64_t relocated_metadata_pages = 0;
	};

	// Replaces a consumed bootstrap-addressed reserve with a new tail pool and
	// makes the former reserve an ordinary Catalog-owned metadata pool through
	// the same A/B Superblock publication.
	struct MetadataEmergencyReserveRotation {
		Superblock superblock;
		MetadataEmergencyReserveDescriptor previous_reserve;
		MetadataEmergencyReserveDescriptor current_reserve;
		bool rotated = false;

		constexpr bool operator==(const MetadataEmergencyReserveRotation &) const = default;
	};

	// A single graph transaction can COW or retire several Node and Edge pages,
	// including pages in different shared Entity pools. Every request binds one
	// stable PageId to its current Catalog and Page Directory location. A retired
	// page has no replacement frame and removes all of its supplied Entity Map
	// entries in the same metadata generation.
	struct EntityPageReplacementRequest {
		CatalogEntry expected_pool;
		// When supplied, the replacement frame is allocated from this compatible
		// current Entity pool rather than the source pool. The logical PageId stays
		// stable and the Page Directory publishes the cross-pool move atomically.
		// An absent entry preserves the same-pool COW default for direct callers.
		CatalogEntry replacement_pool;
		// If every compatible current pool is full, the graph publication may
		// reserve this pool and use it as the COW destination. It is mutually
		// exclusive with `replacement_pool` and never creates a new logical page.
		std::optional<EntityPageFreshPoolDestination> replacement_new_pool;
		PageLocation expected_location;
		std::vector<EntityPageLocation> inserted_nodes;
		std::vector<uint64_t> erased_node_ids;
		std::vector<EntityPageLocation> inserted_edges;
		std::vector<uint64_t> erased_edge_ids;
		bool retire_page = false;
		// Entity pools contain both fixed Entity and slotted LabelSet pages. Only
		// fixed Entity pages may carry Entity Map deltas.
		PageOwnerKind owner_kind = PageOwnerKind::POK_ENTITY;
	};

	// Metadata creates these candidate locations while holding its writer guard.
	// The typed Entity store fills the replacement frames before the matching
	// Page Directory and Entity Map roots can be published.
	struct EntityPageReplacementMaterialization {
		CatalogEntry pool;
		PageLocation expected_location;
		// `replacement` is intentionally invalid for a retired page. The callback
		// must instead durably mark the expected source frame retired before the
		// Page Directory entry is removed.
		PageLocation replacement;
		bool retire_page = false;
		bool destination_is_new_pool = false;
		PageOwnerKind owner_kind = PageOwnerKind::POK_ENTITY;
	};

	// A new page is appended to an already sealed Entity pool in the same
	// generation as replacement/retirement requests. Its logical PageId and all
	// initial Entity Map entries are absent from the pinned root.
	struct EntityPageAppendBatchRequest {
		CatalogEntry expected_pool;
		PageId page_id{};
		std::vector<EntityPageLocation> inserted_nodes;
		std::vector<EntityPageLocation> inserted_edges;
		PageOwnerKind owner_kind = PageOwnerKind::POK_ENTITY;
	};

	struct EntityPageAppendBatchMaterialization {
		CatalogEntry pool;
		PageLocation appended_location;
		PageOwnerKind owner_kind = PageOwnerKind::POK_ENTITY;
	};

	// A fresh shared Entity pool receives one or more initial typed logical pages
	// in the same metadata generation as any existing-pool Entity page changes.
	// Requests with the same complete pool identity share one Catalog entry and
	// physical slot, while each PageId receives its own initial frame. The caller
	// supplies a bounded physical allocation range so reservation remains part of
	// the single-file allocator transaction rather than an out-of-band step.
	struct EntityPageNewPoolBatchRequest {
		CatalogSlotReservationRequest reservation;
		PageId page_id{};
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		std::vector<EntityPageLocation> inserted_nodes;
		std::vector<EntityPageLocation> inserted_edges;
		PageOwnerKind owner_kind = PageOwnerKind::POK_ENTITY;
	};

	struct EntityPageNewPoolBatchMaterialization {
		CatalogEntry pool;
		PageLocation initial_location;
		PageOwnerKind owner_kind = PageOwnerKind::POK_ENTITY;
	};

	// A durable lease range is allocated from a Superblock-published logical-ID
	// high-water mark. It is intentionally a runtime capability rather than a
	// persisted object: a process crash abandons unused IDs, but can never make
	// them reusable.
	struct NativeGraphLogicalIdLeaseRange {
		uint64_t first_id = 0U;
		uint64_t count = 0U;

		[[nodiscard]] constexpr bool empty() const { return count == 0U; }
		[[nodiscard]] constexpr bool contains(uint64_t id) const {
			return first_id != 0U && count != 0U && id >= first_id && id - first_id < count;
		}
		constexpr bool operator==(const NativeGraphLogicalIdLeaseRange &) const = default;
	};

	struct NativeGraphLogicalIdLease {
		NativeGraphLogicalIdLeaseRange nodes;
		NativeGraphLogicalIdLeaseRange edges;
		Superblock superblock{};
	};

	// A graph transaction passes only the exact durable ranges from which it
	// issued IDs. This lets a later commit use an ID below the current high-water
	// mark without treating arbitrary stale IDs as valid.
	struct NativeGraphLogicalIdReservation {
		std::vector<NativeGraphLogicalIdLeaseRange> node_ranges;
		std::vector<NativeGraphLogicalIdLeaseRange> edge_ranges;

		[[nodiscard]] bool empty() const { return node_ranges.empty() && edge_ranges.empty(); }
		bool operator==(const NativeGraphLogicalIdReservation &) const = default;
	};

	// A native graph transaction may change a Property root while changing the
	// owning Node or Edge. The source frame and its owner LSN fence a slotted
	// Property-page COW against a concurrent writer; the owner-keyed directory
	// entries become visible with the matching Entity Map transition.
	struct NativeGraphPropertyPageReplacementRequest {
		CatalogEntry expected_pool;
		PageLocation expected_location;
		uint64_t expected_page_owner_id = 0U;
		LogSequenceNumber expected_owner_lsn{};
		// A full source pool may be pinned by old snapshots. These bounds authorize
		// the metadata writer to reserve a fresh compatible destination pool while
		// preserving the stable PageId through the Page Directory publication.
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		std::vector<EntityPropertyLocation> upserted_properties;
		std::vector<EntityPropertyKey> erased_properties;
		// A relocation removes these slots from the source page but keeps their
		// directory identities alive through matching destination upserts in the
		// same metadata transaction. Keeping relocation separate from erase avoids
		// transient deletion semantics and makes unreachable source bytes impossible.
		std::vector<EntityPropertyKey> relocated_properties;
		// The last root was removed from this slotted page. Its source frame is
		// retired before the Page Directory drops the PageId. Property Directory
		// entries are erased or replaced according to the two vectors above.
		bool retire_page = false;
	};

	struct NativeGraphPropertyPageReplacementMaterialization {
		CatalogEntry pool;
		PageLocation expected_location;
		PageLocation replacement;
		uint64_t page_owner_id = 0U;
		LogSequenceNumber expected_owner_lsn{};
		// A retirement deliberately has no replacement frame. The data preparer
		// must mark the source frame retired while the metadata writer is held.
		bool retire_page = false;
		// The destination pool is newly reserved by this metadata generation. Its
		// candidate frame must be initialized before the Catalog/Page Directory
		// roots become recoverable.
		bool destination_is_new_pool = false;
		// Exactly one replacement initializes each newly reserved pool. Later
		// replacements append through the checked cross-pool COW path so a batch
		// packs up to the physical pool capacity instead of wasting fresh pools.
		bool destination_initializes_new_pool = false;
	};

	// Property-page creation is pool-batched. One request either appends several
	// pages to an exact current pool or creates one fresh pool containing several
	// pages. Keeping page packing separate from physical Segment allocation lets
	// all usable frames participate without weakening the metadata publication.
	enum class NativeGraphPropertyPageBatchKind : uint8_t {
		NGPBK_APPEND_EXISTING_POOL = 1,
		NGPBK_CREATE_POOL = 2,
	};

	struct NativeGraphPropertyPageBatchRequest {
		NativeGraphPropertyPageBatchKind kind = NativeGraphPropertyPageBatchKind::NGPBK_CREATE_POOL;
		// Required only for append. The complete identity is a stale-placement
		// fence and is revalidated while the metadata writer is held.
		CatalogEntry expected_pool;
		uint64_t page_owner_id = 0U;
		// Required only for fresh-pool allocation.
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		// Each inner vector is one sorted slotted page. A batch never exceeds the
		// codec-derived data-frame capacity of one physical pool.
		std::vector<std::vector<EntityPropertyKey>> page_property_keys;
	};

	struct NativeGraphPropertyPageInitialMaterialization {
		PageLocation location;
		std::vector<EntityPropertyLocation> upserted_properties;
	};

	struct NativeGraphPropertyPageBatchMaterialization {
		NativeGraphPropertyPageBatchKind kind = NativeGraphPropertyPageBatchKind::NGPBK_CREATE_POOL;
		CatalogEntry pool;
		uint64_t page_owner_id = 0U;
		std::vector<NativeGraphPropertyPageInitialMaterialization> pages;
	};

	struct RetiredPhysicalSlotReclamation {
		Superblock superblock;
		uint64_t reclaimed_physical_slots = 0;
	};

	// Prefixes prevent collisions with Windows platform macros.
	enum class RelocatableTailTruncationStatus : uint8_t {
		VTTS_TRUNCATED = 1,
		VTTS_NOTHING_TO_TRUNCATE = 2,
		VTTS_AWAITING_SNAPSHOTS = 3,
		VTTS_NATIVE_IO_REQUIRED = 4,
		VTTS_IO_ERROR = 5,
	};

	struct RelocatableTailTruncationResult {
		RelocatableTailTruncationStatus status = RelocatableTailTruncationStatus::VTTS_NOTHING_TO_TRUNCATE;
		uint64_t old_file_bytes = 0;
		uint64_t new_file_bytes = 0;
		uint64_t reclaimed_slots = 0;

		constexpr bool operator==(const RelocatableTailTruncationResult &) const = default;
	};

	// A read-only metadata sample used for compaction admission and operator
	// observability. It does not retain a snapshot after this method returns.
	struct RelocatableCompactionObservation {
		Generation generation{};
		LogSequenceNumber checkpoint_lsn{};
		uint64_t active_snapshot_count = 0;
		uint64_t sealed_segments = 0;
		uint64_t retired_segments = 0;
		uint64_t obsolete_segments = 0;
		uint64_t pending_relocation_sources = 0;

		[[nodiscard]] constexpr uint64_t retainedGenerationCount() const {
			return retired_segments + pending_relocation_sources;
		}

		constexpr bool operator==(const RelocatableCompactionObservation &) const = default;
	};

	// Runs after replacement location-directory pages have been prepared but
	// before their candidate roots are Superblock-recoverable. It lets an existing data
	// pool make its owner header and allocation bitmap durable first.
	using LocationPublicationPreparer = std::function<void()>;
	// Runs under the metadata writer after the expected pool generation and the
	// new Page ID have been checked against the current roots. The callback must
	// durably write its page bytes before the replacement Page Directory root is
	// allowed to become Superblock-recoverable.
	using ExistingPoolPagePreparer = std::function<void(const CatalogEntry &, PageLocation &)>;
	// Publishes one replacement Page Directory location while preserving the
	// old frame for snapshots. The callback must mark the old frame retired and
	// make the new frame durable before the COW root switch.
	using ExistingPoolPageReplacementPreparer =
			std::function<void(const CatalogEntry &, const PageLocation &, PageLocation &)>;
	// Materializes the first slotted Property page of a new pool together with
	// the owner-keyed Property Directory entry that makes its record reachable.
	// The callback owns the typed page codec; this metadata layer owns logical
	// identities, physical reservation, and the single publication boundary.
	struct EntityPropertyPagePoolMaterialization {
		Superblock superblock;
		CatalogEntry pool;
		PageLocation page;
		EntityPropertyLocation property;
	};
	using EntityPropertyPagePoolPreparer =
			std::function<void(const CatalogEntry &, PageId, PageLocation &, EntityPropertyLocation &)>;
	// State records use the same slotted-page COW protocol as Properties, but
	// their State Directory entry must become reachable in that exact generation.
	struct StatePagePoolMaterialization {
		Superblock superblock;
		CatalogEntry pool;
		PageLocation page;
		StateLocation state;
	};
	using StatePagePoolPreparer = std::function<void(const CatalogEntry &, PageId, PageLocation &, StateLocation &)>;
	using StatePageReplacementPreparer =
			std::function<void(const CatalogEntry &, const PageLocation &, PageLocation &, StateLocation &)>;
	using EntityPageReplacementBatchPreparer = std::function<void(std::span<EntityPageReplacementMaterialization>)>;
	using EntityPageMutationBatchPreparer = std::function<void(std::span<EntityPageReplacementMaterialization>,
															   std::span<EntityPageAppendBatchMaterialization>)>;
	using EntityPageTransactionBatchPreparer = std::function<void(std::span<EntityPageReplacementMaterialization>,
																  std::span<EntityPageAppendBatchMaterialization>,
																  std::span<EntityPageNewPoolBatchMaterialization>)>;
	using NativeGraphPageTransactionPreparer = std::function<void(
			std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			std::span<EntityPageNewPoolBatchMaterialization>,
			std::span<NativeGraphPropertyPageReplacementMaterialization>,
			std::span<NativeGraphPropertyPageBatchMaterialization>)>;
	using NativeGraphPageTransactionWithLargeValuesPreparer = std::function<void(
			std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			std::span<EntityPageNewPoolBatchMaterialization>,
			std::span<NativeGraphPropertyPageReplacementMaterialization>,
			std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>)>;
	// Retires one logical page while the metadata writer still owns its current
	// Page Directory mapping. The callback must mark the physical source frame
	// retired before the matching directory entries become unreachable.
	using ExistingPoolPageRetirementPreparer = std::function<void(const CatalogEntry &, const PageLocation &)>;
	// A retirement plan binds a physical frame to the exact page-owner identity
	// observed by a reachability scan. It prevents a stale reclaimer from
	// retiring a page whose logical or physical identity changed before publish.
	struct PageRetirement {
		PageLocation location;
		uint64_t expected_owner_id = 0;
		PageOwnerKind expected_owner_kind = PageOwnerKind::POK_NONE;
		LogSequenceNumber expected_owner_lsn{};
	};

	struct ExistingPoolPageRetirementBatch {
		CatalogEntry pool;
		std::vector<PageRetirement> retirements;
	};

	// Binds a whole immutable Large Object extent to the logical location and
	// Catalog owner tuple observed during a reachability scan.
	struct ExtentRetirement {
		ExtentLocation location;
		uint64_t expected_owner_id = 0;
	};

	// Runs under the metadata writer after the expected metadata generation and
	// every Page Directory entry have been checked. The callback must mark each
	// exact physical frame retired before the directory root is made durable.
	using ExistingPoolPagesRetirementPreparer = std::function<void(std::span<const ExistingPoolPageRetirementBatch>)>;
	// Moves one Page ID between two current sealed pools. The callback must make
	// the destination frame durable and retire the source frame before the one
	// replacement Page Directory root can become Superblock-recoverable.
	using CrossPoolPageEvacuationPreparer =
			std::function<void(const CatalogEntry &, const PageLocation &, const CatalogEntry &, PageLocation &)>;
	// Receives durable logical Page IDs allocated by one metadata batch and the
	// corresponding mutable Page Directory locations. It must materialize every
	// page before the batch makes either those locations or the new Index root
	// recoverable.
	using NewIndexTreePagesPreparer =
			std::function<void(const CatalogEntry &, std::span<const PageId>, std::span<PageLocation>)>;
	// One pool may contribute only replacements or only new pages. The complete
	// collection is prepared while one metadata writer owns all directory roots,
	// then becomes reachable through one Page Directory publication.
	struct IndexTreePageRetirement {
		PageLocation location;
		LogSequenceNumber expected_owner_lsn{};

		constexpr bool operator==(const IndexTreePageRetirement &) const = default;
	};

	struct IndexTreeMutationPoolRequest {
		SegmentId segment_id{};
		std::vector<PageLocation> expected_replacements;
		size_t appended_page_count = 0;
		std::vector<IndexTreePageRetirement> expected_retirements;
		// When valid, replacements are written to this compatible current pool while
		// retirements and appended pages remain owned by `segment_id`.  The logical
		// PageId is unchanged; only its Page Directory location moves at publication.
		SegmentId replacement_segment_id{};
	};

	// Physical capacity remains a database-level policy. This request gives the
	// metadata transaction an explicit admissible slot range while it allocates
	// a durable logical Segment ID itself; callers cannot derive identities from
	// offsets or process-local counters.
	struct NewIndexTreePoolRequest {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		uint64_t owner_id = 0;
		size_t appended_page_count = 0;
	};

	// Initial bulk materialization reserves every pool and publishes the full
	// Page Directory/Index Root set together. Each pool is bounded by one
	// physical shared-page segment, while the complete tree may span many pools.
	struct NewIndexTreeMaterializationPoolRequest {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		uint64_t owner_id = 0;
		size_t page_count = 0;
	};

	struct IndexTreeMaterializationPageReference {
		size_t pool_ordinal = 0;
		size_t page_ordinal = 0;
	};

	struct IndexTreeMaterializationPoolBatch {
		CatalogEntry pool;
		std::vector<PageId> page_ids;
		std::vector<PageLocation> locations;
	};

	using IndexTreeMultiPoolMaterializationPreparer =
			std::function<void(std::vector<IndexTreeMaterializationPoolBatch> &)>;

	// Raw Vector records receive both their Page Directory and Vector Location
	// Directory entries in one metadata generation. This prevents a durable
	// frame from becoming reachable without its stable Vector ID mapping.
	struct VectorRawMaterializationPoolRequest {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		size_t page_count = 0;
	};

	struct VectorRawMaterializationPoolBatch {
		CatalogEntry pool;
		std::vector<PageId> page_ids;
		std::vector<PageLocation> locations;
	};

	using VectorRawMultiPoolMaterializationPreparer =
			std::function<void(std::vector<VectorRawMaterializationPoolBatch> &, std::vector<VectorLocation> &)>;

	// Derived PQ frames are page-addressable only through their future immutable
	// mapping tree. They deliberately do not populate the raw Vector Location
	// Directory, whose entries are the authoritative exact-value route.
	struct VectorPqMaterializationPoolRequest {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		size_t page_count = 0;
	};

	struct VectorPqMaterializationPoolBatch {
		CatalogEntry pool;
		std::vector<PageId> page_ids;
		std::vector<PageLocation> locations;
	};

	using VectorPqMultiPoolMaterializationPreparer =
			std::function<void(std::vector<VectorPqMaterializationPoolBatch> &)>;

	// The graph transaction owns physical reservation and publication. This batch
	// is the shared hand-off to typed LargeValue frame materialization.
	struct LargeValueDataMaterializationPoolBatch {
		CatalogEntry pool;
		std::vector<PageId> page_ids;
		std::vector<PageLocation> locations;
		bool is_new_pool = true;
		// Multiple values in one publication may fill disjoint suffixes of the
		// same fresh pool. The typed allocator reconstructs and validates this
		// already-written prefix before extending the pool control bitmap.
		size_t prepared_prefix_count = 0U;
	};

	using LargeValueManifestMaterializationPoolBatch = LargeValueDataMaterializationPoolBatch;

	struct LargeValueMaterializationResult {
		Superblock superblock;
		LargeValueId value_id{};
		std::vector<LargeValueDataMaterializationPoolBatch> data_pools;
		std::vector<LargeValueManifestMaterializationPoolBatch> manifest_pools;
		// Kept as the root pool shorthand for callers that materialize a
		// single-page manifest. It is always equal to manifest_pools.front().pool.
		CatalogEntry manifest_pool;
		PageId manifest_page_id{};
		PageLocation manifest_location;
	};

	// A graph-owned immutable LargeValue is planned from a pinned snapshot. Its
	// preassigned logical ID makes the owning Property reference deterministic;
	// the metadata writer still verifies that the ID is the next durable value
	// before it reserves any physical pool or publishes either side.
	struct NativeGraphLargeValueMutation {
		LargeValueRef reference;
		std::vector<std::byte> logical_bytes;
		PhysicalSlotId first_data_slot{};
		PhysicalSlotId last_data_slot{};
		PhysicalSlotId first_manifest_slot{};
		PhysicalSlotId last_manifest_slot{};
		LargeValueCompressionOptions compression{};
		LargeValueExtentClass extent_class = LargeValueExtentClass::LVEC_32K;
		// Computed from the exact deterministic chunk policy before metadata
		// reserves frames. It prevents a compressed native value from reserving
		// raw-size pages and exposing a partial materialization.
		size_t planned_data_page_count = 0U;
		// Includes the directory-routed root and every immutable manifest tree
		// node, allowing one publication to materialize values beyond one page.
		size_t planned_manifest_page_count = 1U;
	};

	using NativeGraphLargeValuePreparer = std::function<void(std::span<LargeValueMaterializationResult>)>;

	// Derived adjacency frames use the same Page Directory publication rule as
	// PQ frames, but an independent pool kind prevents a snapshot from treating
	// a graph generation as a product-quantized Vector frame.
	struct VectorAdjacencyMaterializationPoolRequest {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		size_t page_count = 0;
	};

	struct VectorAdjacencyMaterializationPoolBatch {
		CatalogEntry pool;
		std::vector<PageId> page_ids;
		std::vector<PageLocation> locations;
	};

	using VectorAdjacencyMultiPoolMaterializationPreparer =
			std::function<void(std::vector<VectorAdjacencyMaterializationPoolBatch> &)>;

	// A Vector Manifest page is itself immutable, but its logical Page ID and
	// pool Segment ID must be assigned by the same metadata batch that publishes
	// its directory entry.
	using NewVectorManifestPagePreparer = std::function<void(const CatalogEntry &, PageId, PageLocation &)>;

	struct IndexTreeMutationAppendedPageReference {
		size_t pool_ordinal = 0;
		size_t appended_page_ordinal = 0;
	};

	struct IndexTreeMutationPoolBatch {
		CatalogEntry pool;
		std::vector<PageLocation> expected_replacements;
		std::vector<PageId> appended_page_ids;
		std::vector<PageLocation> replacement_locations;
		std::vector<PageLocation> appended_locations;
		bool newly_reserved = false;
		std::vector<IndexTreePageRetirement> expected_retirements;
		// The number of contiguous initial frames already materialized in this
		// fresh pool by earlier plans in the same writer-owned compound batch.
		size_t prepared_new_pool_prefix_count = 0U;
		// Destination for replacement frames. It equals `pool` for the normal
		// same-pool COW path and is distinct only for a bounded cross-pool move.
		CatalogEntry replacement_pool;
	};

	using IndexTreeMultiPoolMutationPreparer = std::function<void(std::vector<IndexTreeMutationPoolBatch> &)>;

	// One native graph commit can also publish scalar-index and constraint
	// support-tree changes. This representation is deliberately independent of
	// fixed Entity records: the only durable identities are an Index root, its
	// typed page pools, and the Page Directory locations fenced by the planner.
	struct NativeGraphIndexTreeAppendedPageReference {
		size_t pool_ordinal = 0U;
		size_t appended_page_ordinal = 0U;
		// Existing and newly reserved pools have independent ordinal spaces. A
		// tag keeps zero a valid ordinal in both spaces without a sentinel value.
		bool newly_reserved = false;
	};

	struct NativeGraphIndexTreeMutation {
		// `index_id` is the stable logical identity of this derived tree. An
		// existing root fences an update to one pinned metadata generation; no
		// root expresses first publication and requires the writer to prove the
		// directory has no mapping for this IndexId. Keeping absence explicit
		// avoids overloading an invalid PageId as a creation sentinel.
		uint64_t index_id = 0U;
		std::optional<IndexRootLocation> expected_root;
		std::vector<IndexTreeMutationPoolRequest> pool_requests;
		std::vector<NewIndexTreePoolRequest> new_pool_requests;
		std::optional<NativeGraphIndexTreeAppendedPageReference> replacement_root;
		std::optional<PageId> stable_replacement_root;
		// Root retirement is distinct from an empty-tree update: it removes the
		// Index Root Directory entry and retires every pinned reachable page in the
		// same native graph publication.
		bool retire_root = false;
	};

	// A first-use Vector index has two immutable mapping trees plus typed Raw
	// and Manifest pages. Keeping the complete shape here lets the metadata
	// writer reserve every identity before any Vector byte is written, then make
	// all four directories visible through one Superblock publication.
	struct NativeGraphVectorBootstrapMutation {
		uint64_t index_id = 0U;
		uint64_t vector_node_mapping_index_id = 0U;
		uint64_t first_vector_id = 0U;
		uint64_t vector_count = 0U;
		uint32_t dimensions = 0U;
		VectorDistanceMetric metric = VectorDistanceMetric::VDM_L2;
		uint32_t raw_page_count = 0U;
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
	};

	struct NativeGraphVectorBootstrapMaterialization {
		NativeGraphVectorBootstrapMutation mutation;
		IndexRootLocation node_mapping_root;
		IndexRootLocation vector_node_mapping_root;
		std::vector<CatalogEntry> raw_pools;
		CatalogEntry manifest_pool;
		std::vector<PageLocation> raw_pages;
		PageLocation manifest_page;
		std::vector<VectorLocation> vector_locations;
		VectorManifestLocation manifest_location;
	};

	// A native Vector delta never rewrites an old raw record in place. Inserts
	// either COW the current writable tail or append an immutable packed page;
	// erases remove semantic mappings and leave former bytes unreachable until
	// generation-safe maintenance reclaims their frame.
	enum class NativeGraphVectorDeltaRawPlacement : uint8_t {
		NGVDRP_COW_TAIL = 1,
		NGVDRP_APPEND_TO_CURRENT_POOL = 2,
		NGVDRP_NEW_POOL = 3,
	};

	struct NativeGraphVectorDeltaRawPageMutation {
		NativeGraphVectorDeltaRawPlacement placement = NativeGraphVectorDeltaRawPlacement::NGVDRP_COW_TAIL;
		CatalogEntry expected_pool;
		// Only a COW tail has a source PageId. Append and fresh-pool pages receive
		// a new logical PageId under the metadata writer's generation fence.
		PageLocation expected_location;
		LogSequenceNumber expected_owner_lsn{};
		uint32_t page_ordinal = 0U;
		uint32_t new_pool_ordinal = 0U;
		uint32_t inserted_record_count = 0U;
	};

	// One entry describes an entire same-index logical write batch. The semantic
	// mapping roots and the typed Vector delta root are represented by ordinary
	// NativeGraphIndexTreeMutation entries; this record binds their replacements,
	// raw-page work, manifest COW, and Vector Location Directory delta to one
	// publication.
	struct NativeGraphVectorDeltaMutation {
		uint64_t index_id = 0U;
		uint64_t vector_node_mapping_index_id = 0U;
		uint64_t expected_delta_mapping_index_id = 0U;
		uint64_t delta_mapping_index_id = 0U;
		IndexRootLocation expected_node_mapping_root;
		IndexRootLocation expected_vector_node_mapping_root;
		std::optional<IndexRootLocation> expected_delta_mapping_root;
		CatalogEntry expected_manifest_pool;
		PageLocation expected_manifest_location;
		VectorManifestLocation expected_manifest;
		LogSequenceNumber expected_manifest_owner_lsn{};
		uint32_t dimensions = 0U;
		VectorDistanceMetric metric = VectorDistanceMetric::VDM_L2;
		uint64_t first_vector_id = 0U;
		uint64_t inserted_vector_count = 0U;
		uint32_t delta_entry_count = 0U;
		// The overlay remains query-correct after crossing the soft maintenance
		// threshold, but the next maintenance cycle must fold it into a new base.
		bool maintenance_rebuild_required = false;
		std::vector<uint64_t> erased_vector_ids;
		std::vector<NativeGraphVectorDeltaRawPageMutation> raw_pages;
		bool manifest_in_new_pool = false;
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
	};

	struct NativeGraphVectorDeltaRawPageMaterialization {
		NativeGraphVectorDeltaRawPageMutation mutation;
		CatalogEntry pool;
		PageLocation location;
	};

	struct NativeGraphVectorDeltaMaterialization {
		NativeGraphVectorDeltaMutation mutation;
		IndexRootLocation node_mapping_root;
		IndexRootLocation vector_node_mapping_root;
		IndexRootLocation delta_mapping_root;
		std::vector<CatalogEntry> raw_new_pools;
		CatalogEntry manifest_new_pool;
		std::vector<NativeGraphVectorDeltaRawPageMaterialization> raw_pages;
		PageLocation manifest_replacement;
		std::vector<VectorLocation> vector_locations;
		VectorManifestLocation manifest_location;
	};

	// A private full-segment artifact belongs to one native graph transaction,
	// but deliberately has no published Catalog, Allocator, or graph-directory
	// route. The metadata writer assigns a candidate-only SegmentId and physical
	// slot while it owns the graph publication; once its typed consumer has
	// materialized final pages, the slot remains free in the published allocator.
	// Thus a successful transaction leaves neither a temporary Catalog record nor
	// a second retirement publication behind.
	// SGK_BULK_SCRATCH is the first consumer; the representation remains typed so
	// future bounded builders do not need an adjacency-specific publication path.
	struct NativeGraphPrivateSegmentRequest {
		SegmentKind kind = SegmentKind::SGK_NONE;
		uint64_t owner_id = 0U;
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		uint16_t header_bytes = RSS_ENVELOPE_BYTES;
		std::vector<std::byte> header_extension;
		std::vector<std::byte> payload;

		constexpr bool operator==(const NativeGraphPrivateSegmentRequest &) const = default;
	};

	// A homogeneous private candidate set can be described without allocating one
	// request object per scratch fragment. Its identities remain callback-scoped;
	// no public route may retain them after publication returns.
	struct NativeGraphPrivateSegmentPlan {
		SegmentKind kind = SegmentKind::SGK_NONE;
		uint64_t owner_id = 0U;
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		uint16_t header_bytes = RSS_ENVELOPE_BYTES;
		std::vector<std::byte> header_extension;
		size_t segment_count = 0U;
	};

	// This identity is valid only while the enclosing metadata writer owns its
	// unpublished candidate. It intentionally contains no physical location, so
	// graph builders cannot persist a raw-slot reference into public state.
	struct NativeGraphPrivateSegmentMaterialization {
		SegmentId segment_id{};
		Generation generation{};
		uint64_t owner_id = 0U;

		[[nodiscard]] constexpr bool isValid() const {
			return segment_id.isValid() && generation.isValid() && owner_id != 0U;
		}
		constexpr bool operator==(const NativeGraphPrivateSegmentMaterialization &) const = default;
	};

	// Candidate SegmentIds are allocated contiguously by one metadata batch. The
	// set exposes ordinal access instead of materializing a second identity vector
	// solely so a manifest can refer to its sibling scratch fragments.
	class NativeGraphPrivateSegmentSet final {
	public:
		NativeGraphPrivateSegmentSet(SegmentId firstSegmentId, Generation generation, size_t count, uint64_t ownerId);

		[[nodiscard]] size_t size() const { return count_; }
		[[nodiscard]] NativeGraphPrivateSegmentMaterialization at(size_t ordinal) const;
		[[nodiscard]] NativeGraphPrivateSegmentMaterialization operator[](size_t ordinal) const { return at(ordinal); }
		[[nodiscard]] NativeGraphPrivateSegmentMaterialization front() const { return at(0U); }
		[[nodiscard]] NativeGraphPrivateSegmentMaterialization back() const { return at(count_ - 1U); }
		[[nodiscard]] size_t ordinalOf(const NativeGraphPrivateSegmentMaterialization &identity) const;

	private:
		SegmentId first_segment_id_{};
		Generation generation_{};
		size_t count_ = 0U;
		uint64_t owner_id_ = 0U;
	};

	// The reader is deliberately scoped to the metadata publication callback.
	// It resolves only candidate SGK_BULK_SCRATCH identities and never opens a
	// public snapshot or exposes a physical slot.
	using NativeGraphPrivateSegmentReader =
			std::function<std::vector<std::byte>(const NativeGraphPrivateSegmentMaterialization &)>;
	// This writer is valid only during early publication planning. It can fill an
	// already reserved private candidate exactly once; it cannot allocate an
	// identity, choose a physical slot, or publish metadata. A later candidate
	// reader becomes available only after this phase has completed and synced.
	using NativeGraphPrivateSegmentWriter =
			std::function<void(const NativeGraphPrivateSegmentMaterialization &, std::vector<std::byte>)>;
	// Orders a private multi-pass planner's writes before it reopens them through
	// the scoped reader. It has no metadata, allocation, or publication authority.
	using NativeGraphPrivateSegmentFence = std::function<void()>;
	// A compact private reservation marker lets an immutable candidate layout
	// reserve output identities before a bounded planner can construct their
	// bytes. It is never a public format and must be replaced through the scoped
	// writer before a typed consumer interprets that candidate.
	class NativeGraphPrivateSegmentReservation final {
	public:
		[[nodiscard]] static std::vector<std::byte> payload();
	};
	// Runs after every private SegmentId is assigned but before any candidate
	// image is written. It is the only supported way for a manifest payload to
	// name sibling private segments without guessing future logical IDs.
	using NativeGraphPrivateSegmentImagePreparer =
			std::function<std::vector<std::byte>(const NativeGraphPrivateSegmentSet &, size_t)>;
	using NativeGraphPrivateSegmentPreparer =
			std::function<void(const NativeGraphPrivateSegmentSet &, const NativeGraphPrivateSegmentReader &)>;
	struct NativeGraphDerivedStateMutation {
		// Sorted by IndexId and free of duplicates. This gives one native graph
		// generation a deterministic, fail-closed derived-state publication order.
		std::vector<NativeGraphIndexTreeMutation> index_tree_mutations;
		// A LargeValue reference selected through a pinned content-index lookup is
		// immutable, but its Manifest route can be retired by concurrent
		// maintenance before this graph write owns the metadata writer. The
		// publication path revalidates each reference against the current route.
		std::vector<LargeValueRef> reused_large_values;
		// Sorted by public IndexId. Each entry refers to first-use mapping roots in
		// `index_tree_mutations`; an existing Vector generation is never silently
		// repurposed as a bootstrap destination.
		std::vector<NativeGraphVectorBootstrapMutation> vector_bootstraps;
		// Sorted by public IndexId and disjoint from bootstrap entries. Each delta
		// is bound to roots and a manifest already visible in the pinned snapshot.
		std::vector<NativeGraphVectorDeltaMutation> vector_deltas;
		// Private artifacts are candidate-only. No published Catalog, Allocator,
		// Entity, Page, Extent, Index, or Manifest directory may route to them.
		std::vector<NativeGraphPrivateSegmentRequest> private_segments;
		// Use this compact form when every private candidate has the same physical
		// allocation and envelope contract. It prevents bulk builders from growing
		// transaction state merely to repeat one request per fragment.
		std::optional<NativeGraphPrivateSegmentPlan> private_segment_plan;
		// An append-only transaction-private source tail is never made public. A
		// streamed coordinator reserves this exact range for the lifetime of one
		// metadata batch so final pool allocation cannot overwrite source bytes
		// before they have been copied into candidate-private segments.
		std::optional<PhysicalSlotRange> external_private_tail_reservation;

		[[nodiscard]] constexpr bool empty() const {
			return index_tree_mutations.empty() && reused_large_values.empty() && vector_bootstraps.empty() &&
				   vector_deltas.empty() && private_segments.empty() && !private_segment_plan.has_value() &&
				   !external_private_tail_reservation.has_value();
		}
	};

	// Offline Vector rebuild is not a sequence of independently visible derived
	// writes. PQ-only callers leave the adjacency and delta-retirement fields
	// empty. A base rebuild fills all fields so Codebook extents, PQ pages,
	// adjacency pages, mapping roots, delta retirement, and Manifest replacement
	// become reachable through exactly one A/B Superblock switch.
	struct OfflineVectorDerivedRebuildMetadataMutation {
		uint64_t index_id = 0U;
		CatalogEntry expected_manifest_pool;
		PageLocation expected_manifest_location;
		VectorManifestLocation expected_manifest;
		LogSequenceNumber expected_manifest_owner_lsn{};
		// A new Codebook may be absent when a rebuild has already validated an
		// immutable matching Codebook in the source snapshot.
		PhysicalSlotId codebook_first_slot{};
		PhysicalSlotId codebook_last_slot{};
		std::span<const CatalogSlotReservationRequest> codebook_reservations;
		std::span<const ExtentLocation> codebook_extents;
		std::span<const VectorPqMaterializationPoolRequest> pq_pools;
		// Existing derived PQ frames and an old mapping tree retire in this same
		// generation. Their typed preparers mark source frames retired before the
		// Page and Index Root directories make the replacement reachable.
		std::span<const PageRetirement> pq_retirements;
		std::optional<IndexRootLocation> expected_mapping_root;
		std::span<const IndexTreeMutationPoolRequest> mapping_retirement_pools;
		// The replacement mapping tree is always fresh. Its leaf values are bound
		// to metadata-assigned PQ PageIds by the B+Tree preparer.
		NativeGraphIndexTreeMutation mapping_tree;
		// A complete base rebuild has a second, independently typed immutable
		// mapping tree for adjacency records. Its publication is coupled to PQ so
		// readers cannot observe a new codebook/PQ generation with an old graph.
		std::span<const VectorAdjacencyMaterializationPoolRequest> adjacency_pools;
		std::span<const PageRetirement> adjacency_retirements;
		std::optional<IndexRootLocation> expected_adjacency_mapping_root;
		std::span<const IndexTreeMutationPoolRequest> adjacency_mapping_retirement_pools;
		NativeGraphIndexTreeMutation adjacency_mapping_tree;
		// A nonempty delta root is retired in the same publication that clears it
		// from the Manifest. Its tree carries no replacement root.
		std::optional<IndexRootLocation> expected_delta_mapping_root;
		std::span<const IndexTreeMutationPoolRequest> delta_mapping_retirement_pools;
	};

	struct OfflineVectorDerivedRebuildMetadataMaterialization {
		std::vector<CatalogEntry> codebook_extents;
		std::vector<VectorPqMaterializationPoolBatch> pq_pools;
		std::vector<ExistingPoolPageRetirementBatch> pq_retirement_pools;
		std::vector<IndexTreeMutationPoolBatch> mapping_retirement_pools;
		std::vector<IndexTreeMutationPoolBatch> mapping_tree_pools;
		IndexRootLocation mapping_root;
		std::vector<VectorAdjacencyMaterializationPoolBatch> adjacency_pools;
		std::vector<ExistingPoolPageRetirementBatch> adjacency_retirement_pools;
		std::vector<IndexTreeMutationPoolBatch> adjacency_mapping_retirement_pools;
		std::vector<IndexTreeMutationPoolBatch> adjacency_mapping_tree_pools;
		IndexRootLocation adjacency_mapping_root;
		std::vector<IndexTreeMutationPoolBatch> delta_mapping_retirement_pools;
		// The Manifest retains its stable PageId. The typed callback must fill a
		// distinct COW frame in the expected Manifest pool.
		PageLocation manifest_replacement;
	};

	using OfflineVectorDerivedRebuildMetadataPreparer =
			std::function<void(OfflineVectorDerivedRebuildMetadataMaterialization &)>;

	// The typed B+Tree store receives only metadata-assigned identities. It must
	// materialize every frame before the enclosing native graph transaction can
	// publish its single A/B Superblock generation.
	using NativeGraphDerivedStatePreparer = std::function<void(std::vector<std::vector<IndexTreeMutationPoolBatch>> &)>;

	// A deferred Index plan is intentionally narrower than a general metadata
	// callback. It can add one internally validated tree transition after its
	// private scratch source has been durably staged, but before any Index pool
	// or PageId is assigned. This keeps the two bulk-builder passes inside one
	// writer-owned candidate without exposing mutable metadata state.
	struct NativeGraphDeferredIndexPlan {
		NativeGraphIndexTreeMutation mutation;
		IndexTreeMultiPoolMutationPreparer preparer;
	};
	struct NativeGraphDeferredIndexPlans {
		std::vector<NativeGraphDeferredIndexPlan> plans;
	};
	// An early streamed coordinator derives an immutable Index plan while its
	// callback-scoped private resolver remains valid. The metadata transaction
	// owns this sink and validates every submitted transition before allocating a
	// public Index page or publishing the enclosing generation.
	using NativeGraphDeferredIndexPlanSink = std::function<void(NativeGraphDeferredIndexPlan)>;

	// Planning runs while the metadata writer has fenced the current durable
	// generation. The snapshot is intentionally read-only and callback-scoped:
	// it lets a bulk builder inspect an existing tree without recursively taking
	// the reader lock or receiving any metadata mutation capability.
	struct NativeGraphDeferredIndexPlanningContext {
		const NativeGraphPrivateSegmentSet &private_segments;
		const NativeGraphPrivateSegmentReader &private_segment_reader;
		const MetadataSnapshotPin &current_snapshot;
	};

	// This is the first phase of a streamed graph publication. Its source is
	// already durable and private. Only metadata-owned coordinators may use the
	// writer-scoped batch to reserve logical identities and prepare frames; graph
	// transactions never receive this context or physical-slot authority.
	struct NativeGraphEarlyPublicationPlanningContext {
		const NativeGraphPrivateSegmentSet &private_segments;
		const NativeGraphPrivateSegmentReader &private_segment_reader;
		const NativeGraphPrivateSegmentWriter &private_segment_writer;
		const NativeGraphPrivateSegmentFence &private_segment_fence;
		const MetadataSnapshotPin &current_snapshot;
		MetadataWriteBatch &metadata_batch;
		const Superblock &current_superblock;
		LogSequenceNumber checkpoint_lsn{};
		const NativeGraphDeferredIndexPlanSink &deferred_index_plan_sink;
	};

	using NativeGraphEarlyPublicationPlanner = std::function<void(const NativeGraphEarlyPublicationPlanningContext &)>;

	using NativeGraphDeferredIndexPlanner =
			std::function<NativeGraphDeferredIndexPlan(const NativeGraphDeferredIndexPlanningContext &)>;
	using NativeGraphDeferredIndexPlansPlanner =
			std::function<NativeGraphDeferredIndexPlans(const NativeGraphDeferredIndexPlanningContext &)>;
	// Runs only after the metadata writer has assigned both mapping roots and all
	// Vector pool/Page identities. It must fill the raw pages and immutable
	// manifest without changing any metadata-owned identity in the plan.
	using NativeGraphVectorBootstrapPreparer =
			std::function<void(std::vector<NativeGraphVectorBootstrapMaterialization> &)>;
	// Like bootstrap preparation, delta preparation receives only
	// metadata-assigned Page/Segment identities. It must materialize raw and
	// Manifest frames without changing any planned logical identity.
	using NativeGraphVectorDeltaPreparer = std::function<void(std::vector<NativeGraphVectorDeltaMaterialization> &)>;
	// Runs under the metadata writer against one current, sealed pool. The Page
	// Directory lookup is bound to the writer's recovered root, preventing a
	// recovery action from deciding reachability through a stale snapshot.
	using ExistingPoolFrameVisibility = std::function<bool(PageId, SegmentId, uint16_t, Generation)>;
	using ExistingPoolReconciler =
			std::function<void(const CatalogEntry &, const std::function<std::optional<PageLocation>(PageId)> &,
							   const std::function<CatalogEntry(SegmentId)> &, const ExistingPoolFrameVisibility &)>;
	// Startup recovery receives every current Page Directory entry for one pool
	// in a single pass. This permits reverse frame-to-Page-ID validation without
	// making steady-state append and replacement scan the whole directory.
	using CurrentPoolReconciler = std::function<void(const CatalogEntry &, std::span<const PageLocation>)>;

	class RelocatableMetadataStore final {
		friend class BTreePageStore;
		friend class DerivedObjectReachabilityReclaimer;
		friend class VectorCodebookReachabilityReclaimer;
		friend class VectorAdjacencyPageStore;
		friend class VectorPageStore;
		friend class VectorManifestStore;
		friend class OfflineVectorBaseRebuild;
		friend class OfflineVectorPqRebuild;
		friend class LargeValueStore;
		friend class RelocatableIntegrityScanner;
		friend class RelocatableMetadataStoreVectorTestAccess;
		friend class RelocatableStorageBackend;

	public:
		[[nodiscard]] static std::unique_ptr<RelocatableMetadataStore> initializeNew(std::shared_ptr<StorageIO> io,
																					 const Superblock &initial);
		// Completes the creation-only bootstrap convergence before the caller
		// exposes the file to a later open. Internal tools that own their StorageIO
		// use this instead of publishing the bootstrap-only intermediate state.
		[[nodiscard]] static std::unique_ptr<RelocatableMetadataStore>
		initializeNewComplete(std::shared_ptr<StorageIO> io, const Superblock &initial);
		[[nodiscard]] static std::unique_ptr<RelocatableMetadataStore> open(std::shared_ptr<StorageIO> io);
		[[nodiscard]] static std::unique_ptr<RelocatableMetadataStore> openReadOnly(std::shared_ptr<StorageIO> io);

		RelocatableMetadataStore(const RelocatableMetadataStore &) = delete;
		RelocatableMetadataStore &operator=(const RelocatableMetadataStore &) = delete;
		// Keep destruction in one translation unit. The store owns several
		// layout-bearing storage handles, so callers must not instantiate an
		// implicit destructor against a stale header during incremental builds.
		~RelocatableMetadataStore();

		[[nodiscard]] MetadataSnapshotPin openSnapshot() const;
		// A sequential writer using another database handle may advance the durable
		// root. Rebuild local metadata-pool ownership before this handle publishes
		// more storage. Local snapshots would protect the old pool map, so callers
		// must release them first.
		void refreshAfterExternalPublication();
		// Reserves non-reusable Node/Edge identities in this database file. The
		// operation changes no graph roots and uses a single A/B publication, so
		// callers amortize it with bounded ranges rather than one publication per ID.
		[[nodiscard]] NativeGraphLogicalIdLease
		reserveNativeGraphLogicalIds(uint64_t nodeCount, uint64_t edgeCount,
									 const MetadataPublishOptions &options = {});
		// Allocates from one metadata-store-owned lease stream shared by every
		// planner. A caller receives only the consumed durable ranges, while the
		// bounded remainder stays private to this store for later requests.
		[[nodiscard]] NativeGraphLogicalIdReservation
		acquireNativeGraphLogicalIds(uint64_t nodeCount, uint64_t edgeCount, uint64_t minimumLeaseBlockSize);
		// Claims the process-local capabilities returned by one acquisition. A
		// claimed or abandoned range is never reissued, while an arbitrary range
		// below the durable high-water mark cannot be presented as a lease.
		[[nodiscard]] bool claimNativeGraphLogicalIdReservation(const NativeGraphLogicalIdReservation &reservation);
		void abandonNativeGraphLogicalIdReservation(const NativeGraphLogicalIdReservation &reservation) noexcept;
		// The sole streamed Edge coordinator owns one metadata batch for all
		// Entity/Property/LargeValue, Catalog, adjacency, and index roots.
		[[nodiscard]] Superblock publishStreamedEdgeGraph(const NativeStreamedGraphFinalPublicationRequest &request,
														  SystemCatalog &catalog, BTreePageStore &adjacencyPages,
														  LargeValueStore *largeValueStore,
														  LogSequenceNumber checkpointLsn,
														  const MetadataPublishOptions &options = {});
		// Snapshot-local cursors use this zero-I/O ownership check before retaining
		// a generation fence. It does not interpret any untrusted persisted bytes.
		[[nodiscard]] bool ownsSnapshot(const MetadataSnapshotPin &snapshot) const noexcept;
		// Logical-ID leasing advances a bootstrap header to preserve the route
		// generation invariant without changing either route branch or typed root.
		[[nodiscard]] bool isIdentityOnlyBootstrapAdvance(const Superblock &before, const Superblock &after) const;
		[[nodiscard]] Superblock upsertCatalog(std::span<const CatalogEntry> entries, LogSequenceNumber checkpointLsn,
											   const MetadataPublishOptions &options = {});
		// Publishes Page and Extent directory mutations in one metadata generation.
		// Either span may be empty, but the operation requires at least one mutation.
		[[nodiscard]] Superblock upsertLocations(std::span<const PageLocation> pages,
												 std::span<const ExtentLocation> extents,
												 LogSequenceNumber checkpointLsn,
												 const MetadataPublishOptions &options = {});
		[[nodiscard]] Superblock upsertLocations(std::span<const PageLocation> pages,
												 std::span<const ExtentLocation> extents,
												 LogSequenceNumber checkpointLsn,
												 const LocationPublicationPreparer &preparer,
												 const MetadataPublishOptions &options = {});
		// The native Entity Map resolves an Entity ID to a stable Page ID and a
		// generation-protected slot.
		[[nodiscard]] Superblock
		upsertEntityPageLocations(std::span<const EntityPageLocation> nodes, std::span<const uint64_t> erasedNodeIds,
								  std::span<const EntityPageLocation> edges, std::span<const uint64_t> erasedEdgeIds,
								  LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});
		// Native Stage 5 Property roots are keyed by EntityKind + EntityId. The
		// typed key is encoded only inside this directory and is never exposed as a
		// physical location to graph/query callers.
		[[nodiscard]] Superblock upsertEntityPropertyLocations(std::span<const EntityPropertyLocation> locations,
															   std::span<const EntityPropertyKey> erasedKeys,
															   LogSequenceNumber checkpointLsn,
															   const MetadataPublishOptions &options = {});
		// Creates one Property-page pool and its first Entity Property root in one
		// COW generation. No Property ID is allocated or published on this path.
		[[nodiscard]] std::optional<EntityPropertyPagePoolMaterialization>
		materializeEntityPropertyPagePool(uint64_t pageOwnerId, EntityPropertyKey property, PhysicalSlotId firstSlot,
										  PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
										  const EntityPropertyPagePoolPreparer &preparer,
										  const MetadataPublishOptions &options = {});
		// Creates the first State page and State Directory location in one COW
		// generation. The caller makes the typed page durable inside `preparer`.
		[[nodiscard]] std::optional<StatePagePoolMaterialization>
		materializeStatePagePool(uint64_t pageOwnerId, uint64_t stateId, PhysicalSlotId firstSlot,
								 PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
								 const StatePagePoolPreparer &preparer, const MetadataPublishOptions &options = {});
		// Replaces a State slotted page and publishes its new State Directory
		// location through one COW root switch.
		[[nodiscard]] Superblock replaceStatePageAndUpsertLocation(const PageLocation &expectedPage,
																   StateLocation state, LogSequenceNumber checkpointLsn,
																   const StatePageReplacementPreparer &preparer,
																   const MetadataPublishOptions &options = {});
		[[nodiscard]] Superblock upsertStateLocations(std::span<const StateLocation> locations,
													  std::span<const uint64_t> erasedStateIds,
													  LogSequenceNumber checkpointLsn,
													  const MetadataPublishOptions &options = {});
		// Publishes stable Vector ID to raw/PQ page-slot mappings. Callers must
		// make the referenced VectorPage durable before this root can become
		// recoverable; the checked pool validation prevents cross-kind aliases.
		[[nodiscard]] Superblock upsertVectorLocations(std::span<const VectorLocation> locations,
													   std::span<const uint64_t> erasedVectorIds,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &options = {});
		[[nodiscard]] Superblock upsertVectorManifests(std::span<const VectorManifestLocation> manifests,
													   std::span<const uint64_t> erasedIndexIds,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &options = {});
		// Reserves one never-reused logical Page ID through a durable Superblock
		// transaction. Callers materialize its page before publishing a directory
		// reference to it, which preserves the data-before-metadata invariant.
		[[nodiscard]] PageId allocateLogicalPageId(LogSequenceNumber checkpointLsn,
												   const MetadataPublishOptions &options = {});
		// Reserves several otherwise-unmaterialized Page identities in one durable
		// publication. Derived immutable objects use this to avoid inferring IDs
		// from a mutable directory or requiring one metadata publication per identity.
		[[nodiscard]] std::vector<PageId> allocateLogicalPageIds(size_t count, LogSequenceNumber checkpointLsn,
																 const MetadataPublishOptions &options = {});
		// Segment IDs share one global Catalog namespace. Reserving a group before
		// writing immutable extents prevents metadata-pool growth from racing a
		// caller that would otherwise predict the next high-water value.
		[[nodiscard]] std::vector<SegmentId> allocateLogicalSegmentIds(size_t count, LogSequenceNumber checkpointLsn,
																	   const MetadataPublishOptions &options = {});
		// These allocation-only operations make an issued graph ID durable before a
		// future CRUD batch materializes it. A crash may leave a gap, but IDs are
		// never reused or inferred from mutable directory contents.
		[[nodiscard]] uint64_t allocateLogicalNodeId(LogSequenceNumber checkpointLsn,
													 const MetadataPublishOptions &options = {});
		[[nodiscard]] uint64_t allocateLogicalEdgeId(LogSequenceNumber checkpointLsn,
													 const MetadataPublishOptions &options = {});
		[[nodiscard]] uint64_t allocateLogicalVectorId(LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &options = {});
		[[nodiscard]] uint32_t allocatePropertyKeyToken(LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options = {});
		// Advances the durable online Property-key Token high-water mark past an
		// immutable migration namespace. The operation never decreases or revives
		// an exhausted counter.
		void advancePropertyKeyTokenFloor(uint32_t token, LogSequenceNumber checkpointLsn,
										  const MetadataPublishOptions &options = {});
		[[nodiscard]] uint64_t allocateSchemaTokenId(LogSequenceNumber checkpointLsn,
													 const MetadataPublishOptions &options = {});
		// The sealed schema registry advances this high-water mark during
		// migration. Online schema Token creation can then reserve a new identity
		// without scanning the immutable registry on its hot path.
		void advanceSchemaTokenIdFloor(uint64_t tokenId, LogSequenceNumber checkpointLsn,
									   const MetadataPublishOptions &options = {});
		[[nodiscard]] RetiredPhysicalSlotReclamation
		reclaimRetiredPhysicalSlots(LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});
		// Appends one new logical Page to a sealed existing pool. `expectedPool`
		// makes the operation fail closed if relocation or replacement changed the
		// visible Catalog generation before this writer acquired the lock.
		[[nodiscard]] Superblock appendPageToExistingPool(const CatalogEntry &expectedPool, PageLocation page,
														  LogSequenceNumber checkpointLsn,
														  const ExistingPoolPagePreparer &preparer,
														  const MetadataPublishOptions &options = {});
		// Creates a new fixed Entity page and its initial Entity Map entries in
		// the same publication. The callback makes the complete page image durable
		// before the Page Directory and Entity Map roots become recoverable.
		[[nodiscard]] Superblock appendEntityPageToExistingPool(const CatalogEntry &expectedPool, PageLocation page,
																std::span<const EntityPageLocation> nodes,
																std::span<const EntityPageLocation> edges,
																LogSequenceNumber checkpointLsn,
																const ExistingPoolPagePreparer &preparer,
																const MetadataPublishOptions &options = {});
		// Replaces one existing Page ID with a different frame in the same sealed
		// pool. The prior frame remains allocated until no metadata snapshot can
		// resolve the previous Page Directory root.
		[[nodiscard]] Superblock replacePageInExistingPool(const CatalogEntry &expectedPool,
														   const PageLocation &expectedLocation,
														   PageLocation replacement, LogSequenceNumber checkpointLsn,
														   const ExistingPoolPageReplacementPreparer &preparer,
														   const MetadataPublishOptions &options = {});
		// Replaces one fixed Entity page through COW and publishes its new Entity
		// Map insertions/deletions with the Page Directory switch. New Entity IDs
		// must be absent; erased IDs must currently resolve to the replaced Page.
		[[nodiscard]] Superblock replaceEntityPageInExistingPool(
				const CatalogEntry &expectedPool, const PageLocation &expectedLocation, PageLocation replacement,
				std::span<const EntityPageLocation> nodes, std::span<const uint64_t> erasedNodeIds,
				std::span<const EntityPageLocation> edges, std::span<const uint64_t> erasedEdgeIds,
				LogSequenceNumber checkpointLsn, const ExistingPoolPageReplacementPreparer &preparer,
				const MetadataPublishOptions &options = {});
		// The batched counterpart publishes arbitrary independent Entity-page COW
		// replacements and final-page retirements as one generation. This is the
		// graph-write primitive for a Node/Edge mutation that changes several
		// records without exposing an intermediate topology or Entity Map state.
		[[nodiscard]] Superblock replaceEntityPagesInExistingPools(
				std::span<const EntityPageReplacementRequest> requests, LogSequenceNumber checkpointLsn,
				const EntityPageReplacementBatchPreparer &preparer, const MetadataPublishOptions &options = {});
		// General existing-pool Entity page mutation: COW replacements, final-page
		// retirements, and newly appended pages share one Page Directory and Entity
		// Map publication. Data callbacks must make every candidate durable first.
		[[nodiscard]] Superblock mutateEntityPagesInExistingPools(
				std::span<const EntityPageReplacementRequest> replacements,
				std::span<const EntityPageAppendBatchRequest> appends, LogSequenceNumber checkpointLsn,
				const EntityPageMutationBatchPreparer &preparer, const MetadataPublishOptions &options = {});
		// The complete Entity-page transaction primitive. It performs existing-page
		// COW replacements/retirements, existing-pool appends, and first pages in
		// freshly allocated Entity pools in one Page Directory, Entity Map, Catalog,
		// and Allocator publication.
		[[nodiscard]] Superblock mutateEntityPages(std::span<const EntityPageReplacementRequest> replacements,
												   std::span<const EntityPageAppendBatchRequest> appends,
												   std::span<const EntityPageNewPoolBatchRequest> newPools,
												   LogSequenceNumber checkpointLsn,
												   const EntityPageTransactionBatchPreparer &preparer,
												   const MetadataPublishOptions &options = {});
		// Extends the Entity-page transaction with COW replacements of existing
		// owner-keyed Property pages. All page images and Entity/Property directory
		// deltas are prepared before one A/B Superblock publication.
		[[nodiscard]] Superblock
		mutateNativeGraphPages(std::span<const EntityPageReplacementRequest> replacements,
							   std::span<const EntityPageAppendBatchRequest> appends,
							   std::span<const EntityPageNewPoolBatchRequest> newPools,
							   std::span<const NativeGraphPropertyPageReplacementRequest> propertyReplacements,
							   std::span<const NativeGraphPropertyPageBatchRequest> propertyPageBatches,
							   LogSequenceNumber checkpointLsn, const NativeGraphPageTransactionPreparer &preparer,
							   const MetadataPublishOptions &options = {});
		// The derived-state form extends the Entity/Property publication boundary
		// with scalar-index or constraint-support B+Tree changes. All typed frames
		// are durable before one shared Catalog/Page/Entity/Property/Index-root
		// metadata generation becomes visible.
		[[nodiscard]] Superblock
		mutateNativeGraphPages(std::span<const EntityPageReplacementRequest> replacements,
							   std::span<const EntityPageAppendBatchRequest> appends,
							   std::span<const EntityPageNewPoolBatchRequest> newPools,
							   std::span<const NativeGraphPropertyPageReplacementRequest> propertyReplacements,
							   std::span<const NativeGraphPropertyPageBatchRequest> propertyPageBatches,
							   const NativeGraphDerivedStateMutation &derivedState, LogSequenceNumber checkpointLsn,
							   const NativeGraphPageTransactionPreparer &preparer,
							   const NativeGraphDerivedStatePreparer &derivedStatePreparer,
							   const MetadataPublishOptions &options = {},
							   NativeGraphLogicalIdReservation logicalIdReservation = {});
		// Adds immutable LargeValue data and manifest pages to the same graph
		// generation as their owner-keyed Property references. It must never be
		// implemented as a preceding standalone LargeValue publication.
		[[nodiscard]] Superblock mutateNativeGraphPagesWithLargeValues(
				std::span<const EntityPageReplacementRequest> replacements,
				std::span<const EntityPageAppendBatchRequest> appends,
				std::span<const EntityPageNewPoolBatchRequest> newPools,
				std::span<const NativeGraphPropertyPageReplacementRequest> propertyReplacements,
				std::span<const NativeGraphPropertyPageBatchRequest> propertyPageBatches,
				std::span<const NativeGraphLargeValueMutation> largeValues,
				const NativeGraphDerivedStateMutation &derivedState, LogSequenceNumber checkpointLsn,
				const NativeGraphPageTransactionWithLargeValuesPreparer &preparer,
				const NativeGraphDerivedStatePreparer &derivedStatePreparer,
				const NativeGraphVectorBootstrapPreparer &vectorBootstrapPreparer = {},
				const NativeGraphVectorDeltaPreparer &vectorDeltaPreparer = {},
				std::span<const PageRetirement> retiredLargeValuePages = {},
				std::span<const LargeValueId> erasedLargeValueIds = {},
				const ExistingPoolPagesRetirementPreparer &largeValueRetirementPreparer = {},
				const MetadataPublishOptions &options = {}, NativeGraphLogicalIdReservation logicalIdReservation = {},
				std::optional<Generation> expectedMetadataGeneration = std::nullopt,
				const NativeGraphPrivateSegmentImagePreparer &privateSegmentImagePreparer = {},
				const NativeGraphPrivateSegmentPreparer &privateSegmentPreparer = {},
				const NativeGraphDeferredIndexPlanner &deferredIndexPlanner = {},
				const NativeGraphEarlyPublicationPlanner &earlyPublicationPlanner = {},
				const NativeGraphDeferredIndexPlansPlanner &deferredIndexPlansPlanner = {});
		// Retires one fixed Entity page and removes matching Node or Edge Entity Map
		// entries in the same publication. Prior snapshots retain the old directory
		// roots and may continue reading the retired physical frame.
		[[nodiscard]] Superblock retireEntityPageInExistingPool(const CatalogEntry &expectedPool,
																const PageLocation &expectedLocation,
																std::span<const uint64_t> erasedNodeIds,
																std::span<const uint64_t> erasedEdgeIds,
																LogSequenceNumber checkpointLsn,
																const ExistingPoolPageRetirementPreparer &preparer,
																const MetadataPublishOptions &options = {});
		// Retires a dedicated vector-manifest page and removes its Page Directory
		// and Vector Manifest Directory entries in one metadata publication. The former
		// frame remains readable through snapshots pinned to the prior roots.
		[[nodiscard]] Superblock retireVectorManifestPageInExistingPool(
				const CatalogEntry &expectedPool, const PageLocation &expectedLocation,
				const VectorManifestLocation &expectedManifest, LogSequenceNumber checkpointLsn,
				const ExistingPoolPageRetirementPreparer &preparer, const MetadataPublishOptions &options = {});
		// Moves one Page ID to a different current sealed pool through one Page
		// Directory COW publication. Prior snapshots keep resolving the source
		// frame until its retired owner can be reclaimed safely.
		[[nodiscard]] Superblock evacuatePageAcrossPools(const CatalogEntry &expectedSourcePool,
														 const PageLocation &expectedSourceLocation,
														 const CatalogEntry &expectedDestinationPool,
														 PageLocation destination, LogSequenceNumber checkpointLsn,
														 const CrossPoolPageEvacuationPreparer &preparer,
														 const MetadataPublishOptions &options = {});
		// Moves a B+Tree root page and rebinds its root generation in the same
		// Page Directory publication. A root page must not use the generic move.
		[[nodiscard]] Superblock evacuateIndexRootPageAcrossPools(
				const CatalogEntry &expectedSourcePool, const PageLocation &expectedSourceLocation,
				const CatalogEntry &expectedDestinationPool, PageLocation destination,
				const IndexRootLocation &expectedRoot, const IndexRootLocation &replacementRoot,
				LogSequenceNumber checkpointLsn, const CrossPoolPageEvacuationPreparer &preparer,
				const MetadataPublishOptions &options = {});
		// Moves an immutable Vector Manifest page and rebinds its Manifest Directory
		// generation in the same Page Directory publication.
		[[nodiscard]] Superblock evacuateVectorManifestPageAcrossPools(
				const CatalogEntry &expectedSourcePool, const PageLocation &expectedSourceLocation,
				const CatalogEntry &expectedDestinationPool, PageLocation destination,
				const VectorManifestLocation &expectedManifest, const VectorManifestLocation &replacementManifest,
				LogSequenceNumber checkpointLsn, const CrossPoolPageEvacuationPreparer &preparer,
				const MetadataPublishOptions &options = {});
		// Moves an immutable LargeValue Manifest page and rebinds its Manifest
		// Directory's physical-location generation in the same Page Directory
		// publication. LargeValue's own generation remains its logical immutable
		// identity and is deliberately not changed by physical relocation.
		[[nodiscard]] Superblock evacuateLargeValueManifestPageAcrossPools(
				const CatalogEntry &expectedSourcePool, const PageLocation &expectedSourceLocation,
				const CatalogEntry &expectedDestinationPool, PageLocation destination,
				const LargeValueManifestLocation &expectedManifest,
				const LargeValueManifestLocation &replacementManifest, LogSequenceNumber checkpointLsn,
				const CrossPoolPageEvacuationPreparer &preparer, const MetadataPublishOptions &options = {});
		// Serializes physical-pool recovery with Page Directory publication. The
		// reconciler may repair data bytes, but it must not publish metadata roots.
		void reconcileExistingPool(SegmentId segmentId, const ExistingPoolReconciler &reconciler);
		// Invokes the reconciler for every current sealed Catalog entry while one
		// writer guard holds the Page Directory root stable. Retired entries are
		// excluded because they are not current logical storage.
		void reconcileCurrentPools(const CurrentPoolReconciler &reconciler);
		[[nodiscard]] Superblock setPhysicalSlotAllocated(PhysicalSlotId slot, bool allocated,
														  LogSequenceNumber checkpointLsn,
														  const MetadataPublishOptions &options = {});
		[[nodiscard]] std::optional<AllocatorReservation>
		reserveFirstFreeSlot(PhysicalSlotId firstSlot, PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
							 const MetadataPublishOptions &options = {});

		// Reserves a physical slot and creates its Catalog mapping in the same COW
		// candidate, then makes both roots visible through one Superblock switch.
		[[nodiscard]] std::optional<CatalogSlotReservations>
		reserveFirstFreeAndUpsertCatalog(std::span<const CatalogSlotReservationRequest> requests,
										 PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
										 LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});

		// Same reservation transaction with a data-before-metadata preparation
		// boundary for physical segment writers.
		[[nodiscard]] std::optional<CatalogSlotReservations> reserveFirstFreeAndUpsertCatalog(
				std::span<const CatalogSlotReservationRequest> requests, PhysicalSlotId firstSlot,
				PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
				const CatalogSlotReservationPreparer &preparer, const MetadataPublishOptions &options = {});

		// Creates new Catalog mappings and their initial Page/Extent locations in
		// one COW candidate. Every supplied location must target one requested
		// Segment ID at that request's generation, so the publication cannot create
		// a directory reference to an unallocated pool.
		[[nodiscard]] std::optional<CatalogSlotReservations> reserveFirstFreeAndUpsertCatalogAndLocations(
				std::span<const CatalogSlotReservationRequest> requests, std::span<const PageLocation> pages,
				std::span<const ExtentLocation> extents, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
				LogSequenceNumber checkpointLsn, const CatalogSlotReservationPreparer &preparer,
				const MetadataPublishOptions &options = {});
		// Materializes a new immutable Vector Manifest page before atomically
		// exposing its Catalog, Page Directory, and Manifest Directory entries.
		[[nodiscard]] std::optional<CatalogSlotReservations> reserveFirstFreeAndUpsertCatalogAndVectorManifests(
				std::span<const CatalogSlotReservationRequest> requests, std::span<const PageLocation> pages,
				std::span<const VectorManifestLocation> manifests, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
				LogSequenceNumber checkpointLsn, const CatalogSlotReservationPreparer &preparer,
				const MetadataPublishOptions &options = {});

		// Creates one or more Entity page pools, their Page Directory entries, and
		// their initial Entity Map entries in a single data-before-metadata
		// publication. This is the first-page counterpart of the COW mutation API.
		[[nodiscard]] std::optional<CatalogSlotReservations> reserveFirstFreeAndUpsertCatalogAndEntityPageLocations(
				std::span<const CatalogSlotReservationRequest> requests, std::span<PageLocation> pages,
				std::span<const EntityPageLocation> nodes, std::span<const EntityPageLocation> edges,
				PhysicalSlotId firstSlot, PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
				const EntityPagePoolReservationPreparer &preparer, const MetadataPublishOptions &options = {});

		// Convenience wrapper for a one-segment allocation. It delegates to the
		// multi-segment path so both APIs have identical publication semantics.
		[[nodiscard]] std::optional<CatalogSlotReservation>
		reserveFirstFreeAndUpsertCatalog(CatalogSlotReservationRequest request, PhysicalSlotId firstSlot,
										 PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
										 const MetadataPublishOptions &options = {});

		// Stops new readers from resolving a sealed segment while preserving its
		// physical slot for snapshots that still reference the prior generation.
		[[nodiscard]] Superblock retireCatalogSegment(SegmentId segmentId, Generation expectedGeneration,
													  LogSequenceNumber checkpointLsn,
													  const MetadataPublishOptions &options = {});
		// Retires a bounded set through one Catalog/Retire Queue publication. This
		// is the cleanup primitive for graph-unreachable scratch and avoids one
		// Superblock switch per segment while retaining exact generation fences.
		[[nodiscard]] Superblock retireCatalogSegments(std::span<const CatalogSegmentRetirementRequest> requests,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &options = {});

		// Releases a retired slot only after every process-local metadata snapshot
		// has been released. The retired Catalog history remains diagnostic-only.
		[[nodiscard]] Superblock releaseRetiredCatalogSegment(SegmentId segmentId, Generation expectedGeneration,
															  LogSequenceNumber checkpointLsn,
															  const MetadataPublishOptions &options = {});

		// Reserves a destination and switches one Catalog mapping to it in one
		// generation. The old slot enters the Retire Queue in the same publication.
		[[nodiscard]] std::optional<CatalogSlotRelocation>
		relocateCatalogSegment(SegmentId segmentId, Generation expectedGeneration, PhysicalSlotId firstSlot,
							   PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
							   const CatalogSlotRelocationPreparer &preparer,
							   const MetadataPublishOptions &options = {});
		// Relocates one full immutable extent and advances its physical generation
		// in the Extent Directory with the Catalog switch. Subtype headers retain
		// their logical immutable generation, which is independent of placement.
		[[nodiscard]] std::optional<CatalogSlotRelocation>
		relocateCatalogExtentSegment(const ExtentLocation &expectedExtent, PhysicalSlotId firstSlot,
									 PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
									 const CatalogSlotRelocationPreparer &preparer,
									 const MetadataPublishOptions &options = {});

		// Releases the old physical source after a relocated generation has no
		// process-local metadata snapshots left to observe it.
		[[nodiscard]] Superblock releaseRelocatedCatalogSource(SegmentId segmentId, Generation expectedGeneration,
															   LogSequenceNumber checkpointLsn,
															   const MetadataPublishOptions &options = {});

		// Makes released snapshot generations reusable without requiring a new
		// Catalog or free-space mutation.
		void collectMetadataGarbage();
		// Releases completely empty metadata pools only after reachability from the
		// current generation and every active snapshot proves that no metadata frame
		// in the pool remains visible. The Catalog transition and allocator release
		// share one Superblock publication.
		[[nodiscard]] MetadataPagePoolReclamation
		reclaimEmptyMetadataPagePools(LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});
		// Rewrites one sparse metadata pool. With active snapshots, only fully
		// routed PageId targets are relocated; old snapshots retain their source
		// frames until garbage collection. Physical-root fallback remains blocked.
		// `maxLiveFrames` is a density threshold, capped by pool frame capacity.
		[[nodiscard]] MetadataPagePoolEvacuation
		evacuateSparseMetadataPagePool(LogSequenceNumber checkpointLsn,
									   uint32_t maxLiveFrames = RSS_PAGE_POOL_DATA_FRAME_COUNT / 4U,
									   const MetadataPublishOptions &options = {});
		// A reserve is rotated only after it has supplied at least one frame. The
		// operation requires space for one new tail metadata pool; a failure leaves
		// the published descriptor and former reserve intact.
		[[nodiscard]] MetadataEmergencyReserveRotation
		rotateMetadataEmergencyReserve(LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});

		[[nodiscard]] CatalogEntry findCatalog(const MetadataSnapshotPin &snapshot, SegmentId segmentId) const;
		[[nodiscard]] std::optional<RetireQueueEntry> findRetireQueueEntryOptional(const MetadataSnapshotPin &snapshot,
																				   PhysicalSlotId slot) const;
		// Streams pending physical retirement records from one pinned generation.
		// Maintenance callers must impose their own operation-specific bound before
		// retaining the resulting logical Segment identities.
		void visitRetireQueueEntries(const MetadataSnapshotPin &snapshot,
									 const std::function<void(const RetireQueueEntry &)> &visitor) const;
		void visitCatalogEntries(const MetadataSnapshotPin &snapshot,
								 const std::function<void(const CatalogEntry &)> &visitor) const;
		// Enumerates current index page pools from one pinned Catalog generation.
		// The result is sorted by SegmentId and excludes retired, non-index, and
		// physically unavailable entries so placement remains deterministic.
		[[nodiscard]] std::vector<CatalogEntry> findCurrentIndexPagePools(const MetadataSnapshotPin &snapshot) const;
		// Entity pools are shared containers for fixed Node and Edge pages. Their
		// logical records remain segregated by the page codec and Entity Map, not
		// by physical Segment ID.
		[[nodiscard]] std::vector<CatalogEntry> findCurrentEntityPagePools(const MetadataSnapshotPin &snapshot) const;
		[[nodiscard]] std::vector<CatalogEntry>
		findCurrentVectorAdjacencyPagePools(const MetadataSnapshotPin &snapshot) const;
		[[nodiscard]] std::vector<CatalogEntry>
		findCurrentVectorRawPagePools(const MetadataSnapshotPin &snapshot) const;
		[[nodiscard]] std::vector<CatalogEntry>
		findRetiredVectorRawPagePools(const MetadataSnapshotPin &snapshot) const;
		[[nodiscard]] std::vector<CatalogEntry> findCurrentVectorPqPagePools(const MetadataSnapshotPin &snapshot) const;
		// LargeValue data pools are shared across immutable values. The physical
		// pool owner names the shared namespace; individual Page owners retain the
		// value identity and are fenced before any append is published.
		[[nodiscard]] std::vector<CatalogEntry>
		findCurrentLargeValueDataPagePools(const MetadataSnapshotPin &snapshot) const;
		[[nodiscard]] std::vector<CatalogEntry>
		findCurrentLargeValueManifestPagePools(const MetadataSnapshotPin &snapshot) const;
		// Enumerates current Property page pools for one page-owner namespace.
		[[nodiscard]] std::vector<CatalogEntry> findCurrentPropertyPagePools(const MetadataSnapshotPin &snapshot,
																			 uint64_t poolOwnerId) const;
		[[nodiscard]] std::vector<CatalogEntry> findCurrentStatePagePools(const MetadataSnapshotPin &snapshot,
																		  uint64_t poolOwnerId) const;
		[[nodiscard]] PageLocation findPageLocation(const MetadataSnapshotPin &snapshot, PageId pageId) const;
		[[nodiscard]] std::optional<PageLocation> findPageLocationOptional(const MetadataSnapshotPin &snapshot,
																		   PageId pageId) const;
		[[nodiscard]] ExtentLocation findExtentLocation(const MetadataSnapshotPin &snapshot, ExtentId extentId) const;
		[[nodiscard]] std::optional<ExtentLocation> findExtentLocationOptional(const MetadataSnapshotPin &snapshot,
																			   ExtentId extentId) const;
		[[nodiscard]] EntityPageLocation findNodePageLocation(const MetadataSnapshotPin &snapshot,
															  uint64_t nodeId) const;
		[[nodiscard]] EntityPageLocation findEdgePageLocation(const MetadataSnapshotPin &snapshot,
															  uint64_t edgeId) const;
		[[nodiscard]] std::optional<EntityPageLocation>
		findNodePageLocationOptional(const MetadataSnapshotPin &snapshot, uint64_t nodeId) const;
		[[nodiscard]] std::optional<EntityPageLocation>
		findEdgePageLocationOptional(const MetadataSnapshotPin &snapshot, uint64_t edgeId) const;
		void findNodePageLocationsSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> nodeIds,
												 std::span<std::optional<EntityPageLocation>> results,
												 const CancellationCheckpoint *cancellation = nullptr) const;
		void findEdgePageLocationsSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> edgeIds,
												 std::span<std::optional<EntityPageLocation>> results,
												 const CancellationCheckpoint *cancellation = nullptr) const;
		void visitNodePageLocations(const MetadataSnapshotPin &snapshot,
									const std::function<void(const EntityPageLocation &)> &visitor) const;
		void visitEdgePageLocations(const MetadataSnapshotPin &snapshot,
									const std::function<void(const EntityPageLocation &)> &visitor) const;
		[[nodiscard]] bool
		visitNodePageLocationsUntil(const MetadataSnapshotPin &snapshot,
									const std::function<bool(const EntityPageLocation &)> &visitor) const;
		[[nodiscard]] bool
		visitEdgePageLocationsUntil(const MetadataSnapshotPin &snapshot,
									const std::function<bool(const EntityPageLocation &)> &visitor) const;
		[[nodiscard]] bool visitNodePageLocationsAfter(const MetadataSnapshotPin &snapshot, uint64_t afterNodeId,
													   size_t maximumMetadataPages,
													   const std::function<bool(const EntityPageLocation &)> &visitor,
													   size_t *visitedMetadataPages = nullptr) const;
		[[nodiscard]] bool visitPageLocationsUntil(const MetadataSnapshotPin &snapshot,
												   const std::function<bool(const PageLocation &)> &visitor) const;
		void visitEntityPropertyLocations(const MetadataSnapshotPin &snapshot,
										  const std::function<void(const EntityPropertyLocation &)> &visitor) const;
		[[nodiscard]] LocationDirectoryRangeCursor openEntityPropertyLocationRangeCursor(
				const MetadataSnapshotPin &snapshot, uint64_t lowerDirectoryId = 1U,
				uint64_t upperDirectoryId = std::numeric_limits<uint64_t>::max(),
				const CancellationCheckpoint *cancellation = nullptr) const;
		[[nodiscard]] bool
		visitEntityPropertyLocationsAfter(const MetadataSnapshotPin &snapshot, uint64_t afterDirectoryId,
										  size_t maximumMetadataPages,
										  const std::function<bool(const EntityPropertyLocation &)> &visitor,
										  size_t *visitedMetadataPages = nullptr) const;
		void visitStateLocations(const MetadataSnapshotPin &snapshot,
								 const std::function<void(const StateLocation &)> &visitor) const;
		[[nodiscard]] bool visitStateLocationsAfter(const MetadataSnapshotPin &snapshot, uint64_t afterStateId,
													size_t maximumMetadataPages,
													const std::function<bool(const StateLocation &)> &visitor,
													size_t *visitedMetadataPages = nullptr) const;
		void visitExtentLocations(const MetadataSnapshotPin &snapshot,
								  const std::function<void(const ExtentLocation &)> &visitor) const;
		void visitIndexRoots(const MetadataSnapshotPin &snapshot,
							 const std::function<void(const IndexRootLocation &)> &visitor) const;
		void visitVectorLocations(const MetadataSnapshotPin &snapshot,
								  const std::function<void(const VectorLocation &)> &visitor) const;
		void visitVectorManifestLocations(const MetadataSnapshotPin &snapshot,
										  const std::function<void(const VectorManifestLocation &)> &visitor) const;
		[[nodiscard]] bool
		visitVectorManifestLocationsAfter(const MetadataSnapshotPin &snapshot, uint64_t afterIndexId,
										  size_t maximumMetadataPages,
										  const std::function<bool(const VectorManifestLocation &)> &visitor,
										  size_t *visitedMetadataPages = nullptr) const;
		void
		visitLargeValueManifestLocations(const MetadataSnapshotPin &snapshot,
										 const std::function<void(const LargeValueManifestLocation &)> &visitor) const;
		[[nodiscard]] bool
		visitLargeValueManifestLocationsAfter(const MetadataSnapshotPin &snapshot, uint64_t afterValueId,
											  size_t maximumMetadataPages,
											  const std::function<bool(const LargeValueManifestLocation &)> &visitor,
											  size_t *visitedMetadataPages = nullptr) const;
		[[nodiscard]] EntityPropertyLocation findEntityPropertyLocation(const MetadataSnapshotPin &snapshot,
																		const EntityPropertyKey &key) const;
		[[nodiscard]] std::optional<EntityPropertyLocation>
		findEntityPropertyLocationOptional(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key) const;
		void findEntityPropertyLocationsSorted(
				const MetadataSnapshotPin &snapshot, std::span<const EntityPropertyKey> keys,
				std::span<std::optional<EntityPropertyLocation>> results,
				const CancellationCheckpoint *cancellation = nullptr) const;
		[[nodiscard]] StateLocation findStateLocation(const MetadataSnapshotPin &snapshot, uint64_t stateId) const;
		[[nodiscard]] std::optional<StateLocation> findStateLocationOptional(const MetadataSnapshotPin &snapshot,
																			 uint64_t stateId) const;
		[[nodiscard]] VectorLocation findVectorLocation(const MetadataSnapshotPin &snapshot, uint64_t vectorId) const;
		[[nodiscard]] std::optional<VectorLocation> findVectorLocationOptional(const MetadataSnapshotPin &snapshot,
																			   uint64_t vectorId) const;
		[[nodiscard]] VectorManifestLocation findVectorManifest(const MetadataSnapshotPin &snapshot,
																uint64_t indexId) const;
		[[nodiscard]] std::optional<VectorManifestLocation>
		findVectorManifestOptional(const MetadataSnapshotPin &snapshot, uint64_t indexId) const;
		[[nodiscard]] LargeValueManifestLocation findLargeValueManifest(const MetadataSnapshotPin &snapshot,
																		LargeValueId valueId) const;
		[[nodiscard]] std::optional<LargeValueManifestLocation>
		findLargeValueManifestOptional(const MetadataSnapshotPin &snapshot, LargeValueId valueId) const;
		[[nodiscard]] IndexRootLocation findIndexRoot(const MetadataSnapshotPin &snapshot, uint64_t indexId) const;
		[[nodiscard]] std::optional<IndexRootLocation> findIndexRootOptional(const MetadataSnapshotPin &snapshot,
																			 uint64_t indexId) const;
		[[nodiscard]] std::vector<PageLocation> findPageLocationsForSegment(const MetadataSnapshotPin &snapshot,
																			SegmentId segmentId) const;
		[[nodiscard]] bool isPhysicalSlotAllocated(const MetadataSnapshotPin &snapshot, PhysicalSlotId slot) const;
		// Removes only a contiguous suffix of physical slots that is already free
		// in the current durable Free Space Directory. The operation holds the
		// metadata writer guard, rejects pinned snapshots, and does not alter a
		// Superblock because it never changes logical reachability.
		[[nodiscard]] RelocatableTailTruncationResult truncateReleasedTail();
		[[nodiscard]] RelocatableCompactionObservation observeCompactionState() const;
		[[nodiscard]] MetadataPagePoolStats pagePoolStats() const;
		// Performs the one-time Page Directory format migration as a dedicated
		// transaction. A capacity preflight completes before candidate allocation,
		// keeping ordinary metadata COW failure ordering unchanged.
		[[nodiscard]] Superblock migratePageDirectoryToBootstrap(LogSequenceNumber checkpointLsn,
																 const MetadataPublishOptions &options = {});
		[[nodiscard]] uint64_t activeSnapshotCount() const;
		[[nodiscard]] bool usesDataStorage(const std::shared_ptr<StorageIO> &io) const { return io_ == io; }

	private:
		// The public backend creation path uses this stronger one-shot protocol.
		// Standalone migration remains available for format fixtures and recovery
		// tests, while new files converge ownership and routes before exposure.
		[[nodiscard]] Superblock convergeInitialBootstrap(LogSequenceNumber checkpointLsn,
														  const MetadataPublishOptions &options = {});
		// Completes the typed metadata route branch before the single A/B switch,
		// then performs pool ownership publication and snapshot reclamation.
		void prepareBootstrapRoutes(MetadataWriteBatch &batch);
		void publishBatch(MetadataWriteBatch &batch, const MetadataWriteGuard &writer,
						  const MetadataPublishOptions &options);
		[[nodiscard]] std::optional<CatalogSlotReservations> reserveFirstFreeAndUpsertCatalogImpl(
				std::span<const CatalogSlotReservationRequest> requests, std::span<const PageLocation> pages,
				std::span<const ExtentLocation> extents, std::span<const EntityPageLocation> entityPageNodes,
				std::span<const EntityPageLocation> entityPageEdges, std::span<const VectorManifestLocation> manifests,
				PhysicalSlotId firstSlot, PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
				const CatalogSlotReservationPreparer *preparer,
				const EntityPagePoolReservationPreparer *entityPagePreparer, std::span<PageLocation> mutableEntityPages,
				const MetadataPublishOptions &options);
		[[nodiscard]] Superblock upsertLocationsImpl(std::span<const PageLocation> pages,
													 std::span<const ExtentLocation> extents,
													 LogSequenceNumber checkpointLsn,
													 const LocationPublicationPreparer *preparer,
													 const MetadataPublishOptions &options);
		// BTreePageStore validates the typed page owner before calling this
		// metadata-only COW publication primitive.
		[[nodiscard]] Superblock upsertIndexRoots(std::span<const IndexRootLocation> roots,
												  std::span<const uint64_t> erasedIndexIds,
												  LogSequenceNumber checkpointLsn,
												  const MetadataPublishOptions &options);
		// BTreePageStore is the sole caller. This reserves new logical Page IDs,
		// materializes all corresponding index frames, publishes their Page
		// Directory entries, and switches the Index Root Directory in one
		// Superblock-published Superblock generation.
		[[nodiscard]] Superblock materializeNewIndexTreePages(SegmentId poolSegmentId, uint64_t indexId,
															  size_t pageCount, size_t rootPageOrdinal,
															  LogSequenceNumber checkpointLsn,
															  const NewIndexTreePagesPreparer &preparer,
															  const MetadataPublishOptions &options);
		// BTreePageStore is the sole caller. It uses this bulk path when a new
		// tree exceeds one shared-page pool: all pools and Page IDs are reserved
		// under one writer, every frame is made durable, then directories and the
		// root become recoverable together.
		[[nodiscard]] Superblock materializeNewIndexTreePagesAcrossPools(
				std::span<const NewIndexTreeMaterializationPoolRequest> pools, uint64_t indexId,
				IndexTreeMaterializationPageReference rootPage, LogSequenceNumber checkpointLsn,
				const IndexTreeMultiPoolMaterializationPreparer &preparer, const MetadataPublishOptions &options);
		// VectorPageStore is the sole caller. It reserves all raw pools and Page
		// IDs, persists every raw frame, then atomically publishes the Page and
		// Vector Location Directory entries under one Superblock-published Superblock.
		[[nodiscard]] Superblock
		materializeNewVectorRawPagesAcrossPools(std::span<const VectorRawMaterializationPoolRequest> pools,
												uint64_t indexId, LogSequenceNumber checkpointLsn,
												const VectorRawMultiPoolMaterializationPreparer &preparer,
												const MetadataPublishOptions &options);
		// VectorPageStore is the only caller. It makes all PQ frames and their
		// Page Directory entries durable in one generation, but deliberately does
		// not expose a VectorId directory mapping; the subsequent derived-data
		// publisher must bind this output through a dedicated immutable mapping.
		[[nodiscard]] Superblock
		materializeNewVectorPqPagesAcrossPools(std::span<const VectorPqMaterializationPoolRequest> pools,
											   uint64_t indexId, LogSequenceNumber checkpointLsn,
											   const VectorPqMultiPoolMaterializationPreparer &preparer,
											   const MetadataPublishOptions &options);
		// Offline derived Vector rebuilds are the sole callers. A PQ-only rebuild
		// and a compound base rebuild both publish their complete derived state
		// through this writer-owned metadata batch.
		[[nodiscard]] Superblock publishOfflineVectorDerivedRebuild(
				const OfflineVectorDerivedRebuildMetadataMutation &mutation, LogSequenceNumber checkpointLsn,
				const OfflineVectorDerivedRebuildMetadataPreparer &preparer, const MetadataPublishOptions &options);
		// Derived adjacency frames deliberately do not populate the old global
		// adjacency directory. A generation becomes query-visible only when its
		// immutable mapping root is bound by a Vector Manifest replacement.
		[[nodiscard]] Superblock
		materializeNewVectorAdjacencyPagesAcrossPools(std::span<const VectorAdjacencyMaterializationPoolRequest> pools,
													  uint64_t indexId, LogSequenceNumber checkpointLsn,
													  const VectorAdjacencyMultiPoolMaterializationPreparer &preparer,
													  const MetadataPublishOptions &options);
		[[nodiscard]] Superblock materializeNewVectorManifestPage(uint64_t indexId, PhysicalSlotId firstSlot,
																						  PhysicalSlotId lastSlot,
																						  LogSequenceNumber checkpointLsn,
																						  const NewVectorManifestPagePreparer &preparer,
																						  const MetadataPublishOptions &options);
		// Atomically removes a proven-unreachable page set from the Page Directory.
		// Typed reclaimers reach this only through their validating stores, so callers
		// cannot issue an arbitrary metadata-only page erase. Optional Vector IDs
		// must resolve to one of the retired pages under the same generation fence.
		// LargeValue pages are deliberately excluded: their Manifest Directory and
		// persistent content-index entry must be erased through the native graph
		// LargeValue publication boundary in the same generation.
		[[nodiscard]] Superblock retirePagesInExistingPools(Generation expectedMetadataGeneration,
																						std::span<const IndexRootLocation> erasedRoots,
																						std::span<const PageRetirement> retirements,
																						LogSequenceNumber checkpointLsn,
																						const ExistingPoolPagesRetirementPreparer &preparer,
																						const MetadataPublishOptions &options = {},
																						std::span<const uint64_t> erasedVectorIds = {});
		[[nodiscard]] Superblock retireVectorCodebookExtents(Generation expectedMetadataGeneration,
															 std::span<const ExtentRetirement> retirements,
															 LogSequenceNumber checkpointLsn,
															 const MetadataPublishOptions &options = {});
		// This is the sole index-tree mutation primitive: all current pools, source
		// locations, appended Page IDs, and final locations are validated under one
		// metadata writer. An optional new root names one append slot in the same
		// batch; otherwise the stable root mapping is retained.
		[[nodiscard]] Superblock mutateIndexTreePagesAcrossPools(
				std::span<const IndexTreeMutationPoolRequest> requests,
				std::optional<NewIndexTreePoolRequest> newPoolRequest, const IndexRootLocation &expectedRoot,
				std::optional<IndexTreeMutationAppendedPageReference> replacementRoot,
				std::optional<PageId> stableReplacementRoot, LogSequenceNumber checkpointLsn,
				const IndexTreeMultiPoolMutationPreparer &preparer, const MetadataPublishOptions &options);
		[[nodiscard]] Superblock
		evacuatePageAcrossPoolsImpl(const CatalogEntry &expectedSourcePool, const PageLocation &expectedSourceLocation,
									const CatalogEntry &expectedDestinationPool, PageLocation destination,
									const std::optional<IndexRootLocation> &expectedRoot,
									const std::optional<IndexRootLocation> &replacementRoot,
									const std::optional<VectorManifestLocation> &expectedManifest,
									const std::optional<VectorManifestLocation> &replacementManifest,
									const std::optional<LargeValueManifestLocation> &expectedLargeValueManifest,
									const std::optional<LargeValueManifestLocation> &replacementLargeValueManifest,
									LogSequenceNumber checkpointLsn, const CrossPoolPageEvacuationPreparer &preparer,
									const MetadataPublishOptions &options);
		[[nodiscard]] MetadataWriteGuard beginWrite();
		RelocatableMetadataStore(std::shared_ptr<StorageIO> io, const Superblock &current, bool readOnly);

		std::shared_ptr<StorageIO> io_;
		SuperblockFile bootstrap_;
		MetadataPageFile pages_;
		MetadataPagePoolAllocator metadata_page_pool_;
		CatalogTree catalog_;
		LocationDirectory locations_;
		AllocatorDirectory allocator_;
		MetadataPoolOwnership pool_ownership_;
		MetadataSnapshotRegistry snapshots_;
		MetadataRetirement retirement_;
		MetadataReservationCoordinator reservation_coordinator_;
		CatalogSegmentRetirementCoordinator segment_retirement_coordinator_;
		CatalogSegmentRelocationCoordinator segment_relocation_coordinator_;
		CatalogRetireQueueReclaimer retire_queue_reclaimer_;
		std::mutex logical_id_lease_mutex_;
		NativeGraphLogicalIdLeaseRange available_node_ids_;
		NativeGraphLogicalIdLeaseRange available_edge_ids_;
		std::map<uint64_t, uint64_t> outstanding_node_id_leases_;
		std::map<uint64_t, uint64_t> outstanding_edge_id_leases_;
		bool read_only_ = false;
	};

} // namespace graph::storage
