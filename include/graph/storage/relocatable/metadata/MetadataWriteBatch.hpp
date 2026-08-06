/**
 * @file MetadataWriteBatch.hpp
 * @brief One-superblock copy-on-write batch for relocatable metadata roots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"

namespace graph::storage {

	struct MetadataPublishOptions {
		BootstrapPublishFailpoint bootstrap_failpoint = BootstrapPublishFailpoint::BPF_NONE;
	};

	class MetadataWriteBatch final {
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
		[[nodiscard]] size_t createdPoolCount() const { return created_pool_slots_.size(); }
		[[nodiscard]] PhysicalSlotId createdPoolSlot(size_t index) const;
		// A maintenance operation may materialize an empty pool before it has a
		// metadata frame to allocate. Track it so every pre-publication failure
		// returns that tail slot through the same RAII cleanup as ordinary pools.
		void trackUnpublishedPool(PhysicalSlotId slot);
		void reserveDataSlot(PhysicalSlotId slot);
		[[nodiscard]] bool isDataSlotReserved(PhysicalSlotId slot) const;

		[[nodiscard]] PageId allocatePageId();
		// Allocates an application-visible Page ID from the durable Superblock
		// high-water mark. Unlike metadata-page IDs, this value survives reopen
		// independently of which metadata COW frames are later reclaimed.
		[[nodiscard]] PageId allocateLogicalPageId();
		// Allocates a durable logical Segment ID. The ID is independent from the
		// physical slot selected by free-space management, so relocation never
		// changes references to this segment.
		[[nodiscard]] SegmentId allocateLogicalSegmentId();
		// Node and Edge IDs use separate durable namespaces so neither entity kind
		// can consume or invalidate the other's logical identity space.
		[[nodiscard]] uint64_t allocateLogicalNodeId();
		[[nodiscard]] uint64_t allocateLogicalEdgeId();
		[[nodiscard]] uint64_t allocateLogicalVectorId();
		[[nodiscard]] LargeValueId allocateLargeValueId();
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
		void setCatalogRoot(PhysicalPageRef root);
		void setPageDirectoryRoot(PhysicalPageRef root);
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
		std::vector<PhysicalSlotId> created_pool_slots_;
		std::vector<PhysicalSlotId> reserved_data_slots_;
		uint64_t initial_physical_file_size_ = 0U;
		bool published_ = false;
	};

} // namespace graph::storage
