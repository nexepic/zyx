/**
 * @file SlottedPagePool.hpp
 * @brief Durable initial materialization of typed relocatable slotted pages.
 *
 * This adapter reads and writes complete slotted pages only. Published pages
 * are replaced through the shared-pool COW/retirement primitive so a pinned
 * snapshot continues to resolve its prior frame.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPage.hpp"

namespace graph::storage {
	class PropertyCollectionRootStore;
	class StateRecordStore;

	class BTreePageStore;

	struct SlottedPagePoolAllocationRequest {
		SegmentId segment_id{};
		SegmentKind pool_kind = SegmentKind::SGK_NONE;
		uint64_t pool_owner_id = 0;
		Generation generation{};
		PageId page_id{};
		uint64_t page_owner_id = 0;
		PageOwnerKind page_owner_kind = PageOwnerKind::POK_NONE;
	};

	struct SlottedPagePoolAllocation {
		SharedPagePoolAllocation allocation;
		SlottedPage page;
	};

	// Keeps the Buffer Pool pin alive for a read-only slotted-page codec. The
	// borrowed record spans returned by `page` therefore remain valid until this
	// storage-internal object is destroyed.
	struct SlottedPagePinnedRead {
		SharedPagePoolPinnedView contents;
		SlottedPageReadView page;

		SlottedPagePinnedRead(SharedPagePoolPinnedView contentsValue, SlottedPageReadView pageValue) :
				contents(std::move(contentsValue)), page(std::move(pageValue)) {}

		SlottedPagePinnedRead(SlottedPagePinnedRead &&) noexcept = default;
		SlottedPagePinnedRead &operator=(SlottedPagePinnedRead &&) noexcept = default;
		SlottedPagePinnedRead(const SlottedPagePinnedRead &) = delete;
		SlottedPagePinnedRead &operator=(const SlottedPagePinnedRead &) = delete;
	};

	struct SlottedPagePoolAppendRequest {
		SegmentId segment_id{};
		Generation generation{};
		PageId page_id{};
		uint64_t page_owner_id = 0;
		PageOwnerKind page_owner_kind = PageOwnerKind::POK_NONE;
	};

	struct SlottedPagePlacement {
		SegmentId segment_id{};
		PageId page_id{};
		uint32_t contiguous_free_bytes = 0;

		constexpr bool operator==(const SlottedPagePlacement &) const = default;
	};

	class SlottedPagePool final {
	public:
		SlottedPagePool(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
						SharedPagePoolOpenMode openMode = SharedPagePoolOpenMode::SPPOM_RECONCILE);
		[[nodiscard]] bool usesMetadataStore(const RelocatableMetadataStore &metadata) const {
			return &metadata_ == &metadata;
		}

		// Persists an empty, fully checksummed slotted page before its Page
		// Directory location becomes recoverable in the same metadata transaction.
		[[nodiscard]] std::optional<SlottedPagePoolAllocation>
		allocateEmptyPage(const SlottedPagePoolAllocationRequest &request, PhysicalSlotId firstSlot,
						  PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
						  const MetadataPublishOptions &options = {});
		// Atomically materializes a populated typed page while creating its pool.
		// This avoids publishing a reachable but semantically uninitialized page.
		[[nodiscard]] std::optional<SlottedPagePoolAllocation>
		allocatePage(const SlottedPagePoolAllocationRequest &request, const SlottedPage &page, PhysicalSlotId firstSlot,
					 PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
					 const MetadataPublishOptions &options = {});

		// Adds an empty typed page to an existing shared pool. The owner remains
		// page-local, so different logical indexes can share one pool segment.
		[[nodiscard]] SlottedPagePoolAllocation appendEmptyPage(const SlottedPagePoolAppendRequest &request,
																LogSequenceNumber checkpointLsn,
																const MetadataPublishOptions &options = {});

		// Atomically materializes an already-populated page in an existing pool.
		// The page owner must exactly match `request` and `checkpointLsn`.
		[[nodiscard]] SharedPagePoolAllocation appendPage(const SlottedPagePoolAppendRequest &request,
														  const SlottedPage &page, LogSequenceNumber checkpointLsn,
														  const MetadataPublishOptions &options = {});

		// `snapshot` pins both the Page Directory and the physical pool mapping
		// while the shared-pool reader validates and copies page bytes.
		[[nodiscard]] SlottedPage read(const MetadataSnapshotPin &snapshot, PageId pageId,
									   BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
		// Returns an authenticated borrowed page only when the shared Buffer Pool
		// can retain its backing frame; callers fall back to `read` otherwise.
		[[nodiscard]] std::optional<SlottedPagePinnedRead>
		readPinned(const MetadataSnapshotPin &snapshot, PageId pageId,
					   BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;

		// Publishes a complete page replacement through the shared pool's frame
		// COW path. The supplied page must retain the current Page ID and
		// generation; owner identity and kind remain immutable across updates.
		[[nodiscard]] SharedPagePoolAllocation replacePage(const SlottedPage &page, LogSequenceNumber checkpointLsn,
														   const MetadataPublishOptions &options = {});

		// Moves a fully validated typed page into another compatible pool while
		// preserving its Page ID and slot references.
		[[nodiscard]] SharedPagePoolAllocation evacuatePage(const SlottedPage &page, SegmentId destinationSegmentId,
															LogSequenceNumber checkpointLsn,
															const MetadataPublishOptions &options = {});

		// Returns the smallest page with at least the requested contiguous free
		// bytes. The returned Page ID is a current-root hint; callers must still
		// validate the page under their mutation snapshot before writing it.
		[[nodiscard]] std::optional<PageId> findBestFitPage(SegmentId segmentId,
															uint32_t minimumContiguousFreeBytes) const;
		[[nodiscard]] std::optional<SlottedPagePlacement> findBestFitPage(std::span<const SegmentId> segmentIds,
																		  uint32_t minimumContiguousFreeBytes) const;
		// A shared Entity pool can hold fixed Entity and slotted LabelSet frames.
		// Typed selection prevents a variable-record caller from selecting a
		// different payload codec only because its free-space hint is favorable.
		[[nodiscard]] std::optional<PageId> findBestFitPage(SegmentId segmentId, PageOwnerKind ownerKind,
															uint32_t minimumContiguousFreeBytes) const;
		[[nodiscard]] std::optional<SlottedPagePlacement> findBestFitPage(std::span<const SegmentId> segmentIds,
																		  PageOwnerKind ownerKind,
																		  uint32_t minimumContiguousFreeBytes) const;
		// Selects a page for one variable-size record. A reusable dead slot makes
		// the record cost only its payload bytes; otherwise a new slot-directory
		// entry is included. This keeps typed best-fit placement from rejecting
		// space that the page can safely reuse.
		[[nodiscard]] std::optional<SlottedPagePlacement>
		findBestFitPageForRecord(std::span<const SegmentId> segmentIds, PageOwnerKind ownerKind,
								 uint32_t recordBytes) const;
		// Uses the caller's metadata snapshot so a COW plan cannot select a page
		// that is newer than the state it will subsequently read and publish from.
		[[nodiscard]] std::optional<SlottedPagePlacement>
		findBestFitPageForRecord(const MetadataSnapshotPin &snapshot, std::span<const SegmentId> segmentIds,
								 PageOwnerKind ownerKind, uint32_t recordBytes) const;
		// Returns current free 4 KiB frame capacity for one sealed shared pool.
		// It is used only to rank placements; mutation preparation validates it
		// again under the metadata writer before publishing reachability.
		[[nodiscard]] uint8_t availableFrameCount(const MetadataSnapshotPin &snapshot, SegmentId segmentId) const;
		void reclaimRetiredFrames(SegmentId segmentId) const;
		void reconcilePool(SegmentId segmentId) const;
		[[nodiscard]] Superblock retireEmptyPool(SegmentId segmentId, Generation expectedGeneration,
												 LogSequenceNumber checkpointLsn,
												 const MetadataPublishOptions &options = {});
		[[nodiscard]] Superblock releaseRetiredEmptyPool(SegmentId segmentId, Generation expectedGeneration,
														 LogSequenceNumber checkpointLsn,
														 const MetadataPublishOptions &options = {});

	private:
		friend class BTreePageStore;
		friend class TransactionBTreeOverlay;
		friend class PropertyCollectionRootStore;
		friend class StateRecordStore;
		void prepareAppendPages(const CatalogEntry &pool, std::span<const SharedPagePoolPreparedAppend> pages,
								std::span<PageLocation> locations, LogSequenceNumber checkpointLsn) const;
		void prepareNewPoolPages(const CatalogEntry &pool, std::span<const SharedPagePoolPreparedAppend> pages,
								 std::span<PageLocation> locations, LogSequenceNumber checkpointLsn,
								 size_t preparedPrefixCount = 0U) const;
		void prepareMutationPages(const CatalogEntry &pool, std::span<const PageLocation> expectedReplacements,
								  std::span<const SharedPagePoolPreparedReplacement> replacements,
								  std::span<const SharedPagePoolPreparedAppend> appends,
								  std::span<PageLocation> replacementLocations, std::span<PageLocation> appendLocations,
								  LogSequenceNumber checkpointLsn,
								  std::span<const SharedPagePoolPreparedRetirement> retirements = {}) const;
		void prepareCrossPoolReplacementPages(const CatalogEntry &destinationPool,
											  std::span<const SharedPagePoolPreparedCrossPoolReplacement> replacements,
											  std::span<PageLocation> replacementLocations,
											  LogSequenceNumber checkpointLsn) const;
		struct OwnerFreeSpaceIndex {
			struct PageInfo {
				PageLocation location;
				uint16_t reusable_free_slot_count = 0U;
			};

			std::map<PageId, PageInfo> pages;
			std::map<uint32_t, std::set<PageId>> pages_by_free_bytes;
		};

		struct RecordPlacement {
			PageLocation location;
			uint32_t required_free_bytes = 0U;
		};

		struct PoolFreeSpaceIndex {
			Generation metadata_generation{};
			// A shared-pool SegmentKind permits at most one slotted-page codec.
			// Fixed Entity frames are deliberately excluded from this index, leaving
			// Entity LabelSet as the Entity pool's sole slotted owner kind.
			std::optional<PageOwnerKind> owner_kind;
			OwnerFreeSpaceIndex pages;
		};

		// Builds from one pinned generation without touching the shared cache. A
		// caller can therefore complete a selection even if a concurrent
		// publication invalidates that cache entry.
		[[nodiscard]] PoolFreeSpaceIndex buildFreeSpaceIndex(const MetadataSnapshotPin &snapshot,
															 SegmentId segmentId) const;
		void cacheFreeSpaceIndexIfNotOlder(SegmentId segmentId, PoolFreeSpaceIndex rebuilt) const;
		[[nodiscard]] std::optional<PageLocation> findBestFitLocation(const MetadataSnapshotPin &snapshot,
																	  SegmentId segmentId,
																	  uint32_t minimumContiguousFreeBytes,
																	  std::optional<PageOwnerKind> ownerKind) const;
		[[nodiscard]] std::optional<RecordPlacement> findBestFitRecordLocation(const MetadataSnapshotPin &snapshot,
																			   SegmentId segmentId,
																			   PageOwnerKind ownerKind,
																			   uint32_t recordBytes) const;
		[[nodiscard]] static std::optional<PageLocation> selectBestFitLocation(const PoolFreeSpaceIndex &index,
																			   uint32_t minimumContiguousFreeBytes,
																			   std::optional<PageOwnerKind> ownerKind);
		[[nodiscard]] static std::optional<RecordPlacement>
		selectBestFitRecordLocation(const PoolFreeSpaceIndex &index, PageOwnerKind ownerKind, uint32_t recordBytes);
		static void validateSelectionRequest(uint32_t minimumContiguousFreeBytes,
											 std::optional<PageOwnerKind> ownerKind);
		static void validateRecordSelectionRequest(uint32_t recordBytes, PageOwnerKind ownerKind);
		void recordPublishedPage(const SharedPagePoolAllocation &allocation, const SlottedPageStats &stats) const;
		void invalidateFreeSpaceIndex(SegmentId segmentId) const;
		SharedPagePoolAllocator pool_;
		RelocatableMetadataStore &metadata_;
		mutable std::mutex free_space_mutex_;
		mutable std::map<SegmentId, PoolFreeSpaceIndex> free_space_indexes_;
	};

} // namespace graph::storage
