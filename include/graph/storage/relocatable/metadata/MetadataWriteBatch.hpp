/**
 * @file MetadataWriteBatch.hpp
 * @brief One-superblock copy-on-write batch for relocatable metadata roots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageRoute.hpp"

namespace graph::storage {

	struct MetadataPublishOptions {
		BootstrapPublishFailpoint bootstrap_failpoint = BootstrapPublishFailpoint::BPF_NONE;
	};

	class MetadataWriteBatch final : public MetadataSlotReservationLookup {
	public:
		MetadataWriteBatch(MetadataPageAllocator &allocator, const Superblock &current,
						   LogSequenceNumber checkpointLsn);
		~MetadataWriteBatch();
		MetadataWriteBatch(const MetadataWriteBatch &) = delete;
		MetadataWriteBatch &operator=(const MetadataWriteBatch &) = delete;

		[[nodiscard]] const Superblock &current() const { return current_; }
		[[nodiscard]] const Superblock &candidate() const { return candidate_; }
		[[nodiscard]] Generation generation() const { return candidate_.generation; }
		[[nodiscard]] LogSequenceNumber checkpointLsn() const { return candidate_.checkpoint_lsn; }
		[[nodiscard]] bool usesAllocator(const MetadataPageAllocator &allocator) const {
			return &allocator_ == &allocator;
		}
		// A candidate root may refer to COW frames built by this batch.  Callers
		// must distinguish those frames from an untrusted future-generation page
		// found in the published tree when validating visibility.
		[[nodiscard]] bool ownsAllocatedPage(PhysicalPageRef reference) const;
		[[nodiscard]] size_t createdPoolCount() const { return created_pool_slots_.size(); }
		// Number of physical metadata frames staged by this publication.  Writers
		// use the delta around one typed directory operation for bounded COW
		// amplification accounting; it does not expose physical identity to readers.
		[[nodiscard]] size_t allocatedPageCount() const noexcept { return allocated_pages_.size(); }
		[[nodiscard]] PhysicalSlotId createdPoolSlot(size_t index) const;
		// A maintenance operation may materialize an empty pool before it has a
		// metadata frame to allocate. Track it so every pre-publication failure
		// returns that tail slot through the same RAII cleanup as ordinary pools.
		void trackUnpublishedPool(PhysicalSlotId slot);
		void reserveDataSlot(PhysicalSlotId slot);
		void reserveDataSlotRange(PhysicalSlotRange range);
		[[nodiscard]] bool isDataSlotReserved(PhysicalSlotId slot) const noexcept override;
		[[nodiscard]] std::optional<PhysicalSlotRange>
		dataSlotRangeContaining(PhysicalSlotId slot) const noexcept override;
		[[nodiscard]] bool isMetadataPoolSlot(PhysicalSlotId slot) const;
		[[nodiscard]] bool catalogPoolIdentityBound(PhysicalSlotId slot) const noexcept {
			return catalog_pool_identity_bindings_.contains(slot.value());
		}
		void markCatalogPoolIdentityBound(PhysicalSlotId slot) {
			catalog_pool_identity_bindings_.insert(slot.value());
		}
		[[nodiscard]] bool catalogPoolOwnershipPrepared(PhysicalSlotId slot) const noexcept {
			return catalog_pool_ownership_prepared_.contains(slot.value());
		}
		void markCatalogPoolOwnershipPrepared(PhysicalSlotId slot) {
			catalog_pool_ownership_prepared_.insert(slot.value());
		}

		[[nodiscard]] MetadataPageId allocateMetadataPageId();
		// A publication helper may materialize the bootstrap route/header once per
		// batch. This prevents independent metadata writers from publishing a
		// second header after a data-directory writer already updated it.
		[[nodiscard]] bool bootstrapRoutePublished() const noexcept { return bootstrap_route_published_; }
		void markBootstrapRoutePublished() noexcept { bootstrap_route_published_ = true; }
		// A physical route-branch source can be evacuated without changing a typed
		// metadata target. It still needs a new branch and header before that source
		// becomes unreachable at the next Superblock publication.
		void requestBootstrapRouteRefresh() noexcept { bootstrap_route_refresh_requested_ = true; }
		[[nodiscard]] bool bootstrapRouteRefreshRequested() const noexcept {
			return bootstrap_route_refresh_requested_;
		}
		// Binds the final route/header frame budget to this batch. The allocator
		// consumes one reservation unit for every subsequent metadata allocation
		// and rejects both underflow and accidental pool creation.
		void reserveMetadataFrames(size_t count);
		[[nodiscard]] size_t ensureMetadataFrameCapacity(size_t count);
		// Location writers defer a data-directory root replacement until the common
		// publication path can write one final bootstrap header and route branch.
		void setPendingBootstrapDataRoot(PhysicalPageRef root);
		[[nodiscard]] std::optional<PhysicalPageRef> pendingBootstrapDataRoot() const noexcept {
			return pending_bootstrap_data_root_;
		}
		// Non-bootstrap metadata writers register the stable page identity and its
		// freshly allocated physical frame together. A COW path may replace an
		// earlier private frame for the same identity; only its final frame is
		// retained for the future typed route branch at A/B publication.
		void registerMetadataPageRoute(MetadataPageId pageId, PhysicalPageRef reference);
		[[nodiscard]] std::optional<PhysicalPageRef> pendingMetadataPageRoute(MetadataPageId pageId) const;
		[[nodiscard]] std::vector<MetadataPageRoute> metadataPageRoutes() const;
		// Allocates an application-visible Page ID from the durable Superblock
		// high-water mark. Unlike metadata-page IDs, this value survives reopen
		// independently of which metadata COW frames are later reclaimed.
		[[nodiscard]] PageId allocateLogicalPageId();
		// Reserves a contiguous logical Page-ID run without materializing a
		// transaction-sized identity vector. Streamed writers derive each PageId
		// from the returned first ID and its bounded page ordinal.
		[[nodiscard]] PageId allocateLogicalPageIdRange(uint64_t count);
		// Allocates a durable logical Segment ID. The ID is independent from the
		// physical slot selected by free-space management, so relocation never
		// changes references to this segment.
		[[nodiscard]] SegmentId allocateLogicalSegmentId();
		// Bulk candidate manifests derive sibling identities by ordinal. Reserving
		// one contiguous SegmentId range avoids an O(fragment-count) identity table.
		[[nodiscard]] SegmentId allocateLogicalSegmentIdRange(uint64_t count);
		// Node and Edge IDs use separate durable namespaces so neither entity kind
		// can consume or invalidate the other's logical identity space.
		[[nodiscard]] uint64_t allocateLogicalNodeId();
		[[nodiscard]] uint64_t allocateLogicalEdgeId();
		// Reserve a contiguous logical-ID interval without materializing each ID.
		// Leases advance durable high-water marks only; they do not create pages.
		[[nodiscard]] uint64_t allocateLogicalNodeIdRange(uint64_t count);
		[[nodiscard]] uint64_t allocateLogicalEdgeIdRange(uint64_t count);
		[[nodiscard]] uint64_t allocateLogicalVectorId();
		[[nodiscard]] LargeValueId allocateLargeValueId();
		// A streamed Property publication derives its compact references from this
		// contiguous durable range without retaining one LargeValueId per root.
		[[nodiscard]] LargeValueId allocateLargeValueIdRange(uint64_t count);
		[[nodiscard]] uint32_t allocatePropertyKeyToken();
		[[nodiscard]] uint64_t allocateSchemaTokenId();
		void observeLogicalPageId(PageId pageId);
		void observeLogicalSegmentId(SegmentId segmentId);
		void observeLogicalNodeId(uint64_t nodeId);
		void observeLogicalEdgeId(uint64_t edgeId);
		void observeLogicalVectorId(uint64_t vectorId);
		void observeLargeValueId(LargeValueId valueId);
		void observePropertyKeyToken(uint32_t token);
		void observeSchemaTokenId(uint64_t tokenId);
		[[nodiscard]] PhysicalPageRef allocatePage();
		[[nodiscard]] std::vector<PhysicalPageRef> allocatePages(size_t count);
		void setCatalogRoot(PhysicalPageRef root);
		void setPageDirectoryRoot(PhysicalPageRef root);
		// The bootstrap header is the only legal Superblock Page Directory root
		// after migration. Ordinary data-root changes must update its data branch.
		void setPageDirectoryBootstrapRoot(PhysicalPageRef root);
		void setExtentDirectoryRoot(PhysicalPageRef root);
		void setAllocatorRoot(PhysicalPageRef root);
		void setNodeEntityDirectoryRoot(PhysicalPageRef root);
		void setEdgeEntityDirectoryRoot(PhysicalPageRef root);
		void setEntityPropertyDirectoryRoot(PhysicalPageRef root);
		void setIndexRootDirectoryRoot(PhysicalPageRef root);
		void setVectorLocationDirectoryRoot(PhysicalPageRef root);
		void setVectorManifestDirectoryRoot(PhysicalPageRef root);
		void setLargeValueManifestDirectoryRoot(PhysicalPageRef root);
		void setStateDirectoryRoot(PhysicalPageRef root);
		void setRetireQueueRoot(PhysicalPageRef root);
		void setMetadataEmergencyReserve(MetadataEmergencyReserveDescriptor reserve);

		// Makes every new page durable before the candidate's one A/B root switch.
		void publish(SuperblockFile &bootstrap,
					 BootstrapPublishFailpoint failpoint = BootstrapPublishFailpoint::BPF_NONE);

	private:
		void validateOwnedRoot(PhysicalPageRef root, const char *message) const;
		void abandonUnpublishedPages() noexcept;

		MetadataPageAllocator &allocator_;
		Superblock current_;
		Superblock candidate_;
		std::vector<PhysicalPageRef> allocated_pages_;
		std::map<uint64_t, PhysicalPageRef> metadata_page_routes_;
		// Keep rollback order in allocated_pages_; this index only rejects an
		// allocator reusing the same physical frame within a COW batch.
		std::set<std::pair<uint64_t, uint8_t>> allocated_page_locations_;
		std::vector<PhysicalSlotId> created_pool_slots_;
		std::vector<PhysicalSlotId> reserved_data_slots_;
		std::vector<PhysicalSlotRange> reserved_data_slot_ranges_;
		std::set<uint64_t> reserved_data_slot_index_;
		std::set<uint64_t> catalog_pool_identity_bindings_;
		std::set<uint64_t> catalog_pool_ownership_prepared_;
		std::map<uint64_t, uint64_t> reserved_data_slot_range_index_;
		uint64_t initial_physical_file_size_ = 0U;
		bool batch_context_active_ = false;
		bool published_ = false;
		bool bootstrap_route_published_ = false;
		bool bootstrap_route_refresh_requested_ = false;
		bool metadata_frame_reservation_active_ = false;
		std::optional<PhysicalPageRef> pending_bootstrap_data_root_;
	};

} // namespace graph::storage
