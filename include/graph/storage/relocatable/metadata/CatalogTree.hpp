/**
 * @file CatalogTree.hpp
 * @brief Copy-on-write persistence and lookup for the isolated relocatable Catalog tree.
 *
 * The allocator is intentionally an interface. A later free-space directory
 * will own durable metadata-page allocation; this tree only requires fresh
 * physical pages and never overwrites a page reachable from an old root.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"

namespace graph::storage {

	class MetadataWriteBatch;

	// A physical metadata page may bring a new metadata pool online. The batch
	// publishes ownership of that pool in the same free-space generation.
	struct MetadataPageAllocation {
		PhysicalPageRef page;
		std::optional<PhysicalSlotId> created_pool_slot;

		[[nodiscard]] constexpr operator PhysicalPageRef() const { return page; }
	};

	class MetadataPageAllocator {
	public:
		virtual ~MetadataPageAllocator() = default;

		// New logical page IDs may be monotonic and need not be reused when a
		// transaction is abandoned. Physical pages, however, must be fresh for
		// the requested metadata generation.
		[[nodiscard]] virtual PageId allocatePageId() = 0;
		[[nodiscard]] virtual MetadataPageAllocation
		allocatePage(Generation generation, LogSequenceNumber checkpointLsn,
					 std::span<const PhysicalSlotId> excludedPoolSlots) = 0;
		virtual void abandonPage(PhysicalPageRef reference) noexcept = 0;
		virtual void abandonUnpublishedPool(PhysicalSlotId slot) noexcept { static_cast<void>(slot); }
		// A concrete allocator may coordinate newly created metadata pools with
		// data slots provisionally appended by the same metadata batch.  The
		// default keeps lightweight test allocators source-compatible.
		[[nodiscard]] virtual uint64_t physicalFileSize() const { return 0U; }
		virtual void abandonUnpublishedBatch(std::span<const PhysicalSlotId> poolSlots,
											std::span<const PhysicalSlotId>, uint64_t) noexcept {
			for (const PhysicalSlotId slot: poolSlots) {
				abandonUnpublishedPool(slot);
			}
		}
	};

	class CatalogTree final {
	public:
		CatalogTree(MetadataPageFile &pages, MetadataPageAllocator &allocator);

		// Upserts strictly Segment-ID-ordered entries as one copy-on-write metadata
		// generation. The page file and SuperblockFile must share one StorageIO so
		// SuperblockFile::publish can enforce data-before-root durability.
		[[nodiscard]] Superblock
		upsertAndPublish(SuperblockFile &bootstrap, const Superblock &current, std::span<const CatalogEntry> entries,
						 LogSequenceNumber checkpointLsn,
						 BootstrapPublishFailpoint failpoint = BootstrapPublishFailpoint::BPF_NONE);

		// Adds a replacement Catalog root to an unpublished multi-root COW batch.
		// The caller owns the batch and publishes its candidate Superblock exactly once.
		void prepareUpsert(MetadataWriteBatch &batch, std::span<const CatalogEntry> entries);
		// Removes strictly Segment-ID-ordered entries from an unpublished COW
		// generation. Empty leaves are unlinked rather than serialized so removed
		// metadata-pool ownership cannot accumulate as permanent Catalog history.
		void prepareErase(MetadataWriteBatch &batch, std::span<const SegmentId> segmentIds);

		// Resolves one entry from a published Superblock generation. It validates
		// every traversed root/internal/leaf relationship before returning data.
		[[nodiscard]] CatalogEntry find(const Superblock &superblock, SegmentId segmentId) const;
		// Returns no value only when a valid published tree has no mapping for the
		// requested ID. Corrupt paths still fail closed with FormatException.
		[[nodiscard]] std::optional<CatalogEntry> findOptional(const Superblock &superblock, SegmentId segmentId) const;
		// Visits every entry reachable from one published Catalog root. The walk
		// validates radix routes and rejects duplicate physical page references
		// before exposing an entry to the callback.
		void visit(const Superblock &superblock, const std::function<void(const CatalogEntry &)> &visitor) const;

	private:
		MetadataPageFile &pages_;
		MetadataPageAllocator &allocator_;
	};

} // namespace graph::storage
