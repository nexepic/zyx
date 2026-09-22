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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"

namespace graph::storage {

	class MetadataWriteBatch;
	class CatalogTree;

	// A physical metadata page may bring a new metadata pool online. The batch
	// publishes ownership of that pool in the same free-space generation.
	struct MetadataPageAllocation {
		PhysicalPageRef page;
		std::optional<PhysicalSlotId> created_pool_slot;

		[[nodiscard]] constexpr operator PhysicalPageRef() const { return page; }
	};

	// A batch can reserve a contiguous data-only suffix without expanding it into
	// one PhysicalSlotId per segment. Metadata pages must not reuse this range
	// before the enclosing Superblock publication decides its reachability.
	struct PhysicalSlotRange {
		PhysicalSlotId first{};
		PhysicalSlotId last{};

		[[nodiscard]] constexpr bool isValid() const { return first.isValid() && last.isValid() && first <= last; }
		[[nodiscard]] constexpr bool contains(PhysicalSlotId slot) const {
			return isValid() && slot.isValid() && first <= slot && slot <= last;
		}
	};

	// Rollback keeps reservations in insertion order, whereas metadata COW
	// allocation needs frequent membership checks. This view exposes an indexed
	// lookup without changing the durable reservation contract.
	class MetadataSlotReservationLookup {
	public:
		virtual ~MetadataSlotReservationLookup() = default;
		[[nodiscard]] virtual bool isDataSlotReserved(PhysicalSlotId slot) const noexcept = 0;
		[[nodiscard]] virtual std::optional<PhysicalSlotRange>
		dataSlotRangeContaining(PhysicalSlotId slot) const noexcept = 0;
	};

	class MetadataPageAllocator {
	public:
		virtual ~MetadataPageAllocator() = default;

		// New logical page IDs may be monotonic and need not be reused when a
		// transaction is abandoned. Physical pages, however, must be fresh for
		// the requested metadata generation.
		[[nodiscard]] virtual MetadataPageId allocateMetadataPageId() = 0;
		[[nodiscard]] virtual MetadataPageAllocation
		allocatePage(Generation generation, LogSequenceNumber checkpointLsn,
					 std::span<const PhysicalSlotId> excludedPoolSlots) = 0;
		[[nodiscard]] virtual MetadataPageAllocation
		allocatePage(Generation generation, LogSequenceNumber checkpointLsn,
					 std::span<const PhysicalSlotId> excludedPoolSlots,
					 std::span<const PhysicalSlotRange> excludedDataSlotRanges) {
			static_cast<void>(excludedDataSlotRanges);
			return allocatePage(generation, checkpointLsn, excludedPoolSlots);
		}
		[[nodiscard]] virtual MetadataPageAllocation
		allocatePage(Generation generation, LogSequenceNumber checkpointLsn,
					 std::span<const PhysicalSlotId> excludedPoolSlots,
					 std::span<const PhysicalSlotRange> excludedDataSlotRanges,
					 const MetadataSlotReservationLookup *reservationLookup) {
			static_cast<void>(reservationLookup);
			return allocatePage(generation, checkpointLsn, excludedPoolSlots, excludedDataSlotRanges);
		}
		// Batches amortize durable pool-bitmap updates while preserving the same
		// fresh-frame and rollback contract as repeated scalar allocations.
		[[nodiscard]] virtual std::vector<MetadataPageAllocation>
		allocatePages(Generation generation, LogSequenceNumber checkpointLsn, size_t count,
					  std::span<const PhysicalSlotId> excludedPoolSlots,
					  std::span<const PhysicalSlotRange> excludedDataSlotRanges,
					  const MetadataSlotReservationLookup *reservationLookup) {
			std::vector<MetadataPageAllocation> allocations;
			allocations.reserve(count);
			try {
				for (size_t index = 0U; index < count; ++index) {
					allocations.push_back(allocatePage(generation, checkpointLsn, excludedPoolSlots,
												   excludedDataSlotRanges, reservationLookup));
				}
			} catch (...) {
				for (auto iterator = allocations.rbegin(); iterator != allocations.rend(); ++iterator) {
					abandonPage(iterator->page);
				}
				for (auto iterator = allocations.rbegin(); iterator != allocations.rend(); ++iterator) {
					if (iterator->created_pool_slot.has_value()) {
						abandonUnpublishedPool(*iterator->created_pool_slot);
					}
				}
				throw;
			}
			return allocations;
		}
		// A concrete allocator may inspect the current published roots while a
		// batch is active. This lets it reuse only a root-free physical tail slot
		// left by an interrupted unpublished candidate.
		virtual void beginUnpublishedBatch(const Superblock &) {}
		virtual void endUnpublishedBatch(const Superblock &) noexcept {}
		// A publication may reserve its exact metadata-frame budget after ordinary
		// COW work has converged. Concrete allocators bind this reservation to the
		// active batch and reject allocations that exceed it; lightweight test
		// allocators can retain the source-compatible no-op default.
		virtual void reserveMetadataFrames(size_t, std::span<const PhysicalSlotId>,
									  std::span<const PhysicalSlotRange>,
									  const MetadataSlotReservationLookup *) {}
		virtual void releaseMetadataFrames() noexcept {}
		// Final route/header publication needs a bounded number of existing free
		// frames. A production allocator can add a private empty pool before that
		// reservation is activated; lightweight test allocators retain this default.
		[[nodiscard]] virtual std::vector<PhysicalSlotId>
		ensureMetadataFrameCapacity(Generation, LogSequenceNumber, size_t, std::span<const PhysicalSlotId>,
										std::span<const PhysicalSlotRange>, const MetadataSlotReservationLookup *) {
			return {};
		}
		virtual void abandonPage(PhysicalPageRef reference) noexcept = 0;
		virtual void abandonUnpublishedPool(PhysicalSlotId slot) noexcept { static_cast<void>(slot); }
		// A concrete allocator may coordinate newly created metadata pools with
		// data slots provisionally appended by the same metadata batch.  The
		// default keeps lightweight test allocators source-compatible.
		[[nodiscard]] virtual uint64_t physicalFileSize() const { return 0U; }
		virtual void abandonUnpublishedBatch(std::span<const PhysicalSlotId> poolSlots, std::span<const PhysicalSlotId>,
											 uint64_t) noexcept {
			for (const PhysicalSlotId slot: poolSlots) {
				abandonUnpublishedPool(slot);
			}
		}
		virtual void abandonUnpublishedBatch(std::span<const PhysicalSlotId> poolSlots,
											 std::span<const PhysicalSlotId> dataSlots,
											 std::span<const PhysicalSlotRange>, uint64_t initialFileSize) noexcept {
			abandonUnpublishedBatch(poolSlots, dataSlots, initialFileSize);
		}
	};

	// Owns one bounded append-only Catalog COW path.  A caller may keep this
	// session beside other metadata append sessions while materializing a page,
	// then publish every completed root through the same MetadataWriteBatch.
	class CatalogAppendSession final {
	public:
		CatalogAppendSession(CatalogAppendSession &&) noexcept;
		CatalogAppendSession &operator=(CatalogAppendSession &&) noexcept;
		~CatalogAppendSession();

		CatalogAppendSession(const CatalogAppendSession &) = delete;
		CatalogAppendSession &operator=(const CatalogAppendSession &) = delete;

		void append(const CatalogEntry &entry);
		void finish();
		[[nodiscard]] bool isFinished() const noexcept;

	private:
		struct Impl;
		explicit CatalogAppendSession(std::unique_ptr<Impl> implementation) noexcept;

		std::unique_ptr<Impl> implementation_;

		friend class CatalogTree;
	};

	class CatalogTree final {
	public:
		// Streamed publication assigns a contiguous run of new logical Segment IDs.
		// This append-only form retains one Catalog leaf and one radix path instead
		// of materializing an import-sized Catalog-entry vector.
		using EntryCursor = std::function<std::optional<CatalogEntry>()>;
		using EntrySink = std::function<void(const CatalogEntry &)>;
		using EntryProducer = std::function<void(const EntrySink &)>;
		[[nodiscard]] CatalogAppendSession beginAppend(MetadataWriteBatch &batch, SegmentId firstSegmentId,
													   SegmentId lastSegmentId, size_t expectedCount);
		void prepareAppend(MetadataWriteBatch &batch, SegmentId firstSegmentId, SegmentId lastSegmentId,
						   size_t expectedCount, const EntryCursor &cursor);
		void prepareAppend(MetadataWriteBatch &batch, SegmentId firstSegmentId, SegmentId lastSegmentId,
						   size_t expectedCount, const EntryProducer &producer);

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
		// Runs after all same-batch Catalog mutations have converged. Candidate
		// roots remain physical while writers compose; the final root is then
		// rewritten with stable logical child identities for route publication.
		void prepareLogicalRootChildren(MetadataWriteBatch &batch);

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
