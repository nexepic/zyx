/**
 * @file MetadataPagePool.hpp
 * @brief Recoverable fixed-slot page-pool allocator for isolated Catalog metadata.
 *
 * The pool owns only `SGK_METADATA_PAGE_POOL` slots. It stores a checksummed frame bitmap
 * in each slot's reserved frame zero and supplies fresh frames to CatalogTree.
 * Its slots coexist with other relocatable kinds and are recorded as allocated in the
 * durable free-space directory.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "graph/storage/relocatable/metadata/CatalogTree.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_METADATA_PAGE_POOL_MAGIC = 0x5058595AU; // Little-endian bytes: ZYXP
	constexpr uint16_t RSS_METADATA_PAGE_POOL_VERSION = 2;
	constexpr uint16_t RSS_METADATA_PAGE_POOL_HEADER_BYTES = 64;
	constexpr uint32_t RSS_METADATA_PAGE_POOL_VALID_FRAME_MASK = (1U << RSS_PAGE_POOL_DATA_FRAME_COUNT) - 1U;

	struct MetadataPagePoolHeader {
		PhysicalSlotId physical_slot{};
		SegmentId segment_id{};
		uint32_t allocated_frames = 0;
		Generation generation{};
		LogSequenceNumber committed_lsn{};
	};

	class MetadataPagePoolHeaderCodec {
	public:
		[[nodiscard]] static std::array<std::byte, RSS_METADATA_PAGE_POOL_HEADER_BYTES>
		encode(const MetadataPagePoolHeader &header);
		[[nodiscard]] static MetadataPagePoolHeader decode(std::span<const std::byte> bytes);
	};

	struct MetadataPagePoolStats {
		uint64_t pools = 0;
		uint64_t allocated_pages = 0;
		uint64_t free_pages = 0;
		PageId next_page_id{};
		bool poisoned = false;
	};

	enum class MetadataPagePoolOpenMode : uint8_t {
		CPOM_RECOVER = 1,
		CPOM_DISCOVER_ONLY = 2,
		// Production open starts from A/B-Superblock roots rather than inspecting
		// every physical slot. The scan modes are limited to empty-file bootstrap
		// and isolated codec tests, where no published metadata root exists yet.
		CPOM_ROOT_RECOVER = 3,
		CPOM_ROOT_DISCOVER_ONLY = 4,
	};

	class MetadataPagePoolAllocator final : public MetadataPageAllocator {
	public:
		MetadataPagePoolAllocator(std::shared_ptr<StorageIO> io, const Superblock &superblock,
								  MetadataPagePoolOpenMode openMode = MetadataPagePoolOpenMode::CPOM_RECOVER);

		[[nodiscard]] PageId allocatePageId() override;
		[[nodiscard]] MetadataPageAllocation allocatePage(Generation generation, LogSequenceNumber checkpointLsn,
														  std::span<const PhysicalSlotId> excludedPoolSlots) override;
		// Direct component tests may allocate an unpublished page. Production
		// metadata mutations use the context-bearing interface above.
		[[nodiscard]] PhysicalPageRef allocatePage(Generation generation);
		void abandonPage(PhysicalPageRef reference) noexcept override;
		void abandonUnpublishedPool(PhysicalSlotId slot) noexcept override;
		[[nodiscard]] uint64_t physicalFileSize() const override;
		void abandonUnpublishedBatch(std::span<const PhysicalSlotId> poolSlots,
									 std::span<const PhysicalSlotId> dataSlots,
									 uint64_t initialFileSize) noexcept override;

		// Open-time recovery preserves pages reachable from the recovered Catalog
		// root and returns only unreachable pool frames to the local free bitmap.
		void recover(const Superblock &superblock);
		// Validates a candidate tree without changing frame ownership. Publication
		// uses this before making the candidate Superblock recoverable.
		void validateReachability(const Superblock &superblock);
		// Registers a pool retained by the durable Catalog even when it currently
		// has no root-reachable frames. This prevents an empty pool from becoming
		// an untracked allocated slot after reopen.
		void registerCatalogPool(PhysicalSlotId slot, SegmentId segmentId, Generation generation,
								 LogSequenceNumber committedLsn);
		// A newly appended pool is private to its MetadataWriteBatch until its
		// Catalog entry and allocator bit are published. Bind the stable internal
		// SegmentId before that one root switch makes the pool reachable.
		void bindCatalogPoolIdentity(PhysicalSlotId slot, SegmentId segmentId, Generation generation,
									 LogSequenceNumber committedLsn);
		// Materializes the bootstrap-addressed reserve at a caller-reserved physical
		// slot. Its descriptor is published with the same A/B generation that
		// marks the slot allocated, so it never becomes a normal Catalog pool.
		void createEmergencyReserve(const MetadataEmergencyReserveDescriptor &reserve);
		// Replaces a consumed reserve with a new, empty tail pool. The former pool
		// keeps its permanent token and becomes eligible for normal Catalog
		// ownership in the enclosing metadata batch.
		[[nodiscard]] MetadataEmergencyReserveDescriptor
		rotateEmergencyReserve(SegmentId nextPoolToken, Generation generation, LogSequenceNumber committedLsn);
		void restoreEmergencyReserve(const MetadataEmergencyReserveDescriptor &previous,
									 const MetadataEmergencyReserveDescriptor &expectedCurrent) noexcept;
		[[nodiscard]] MetadataEmergencyReserveDescriptor emergencyReserve() const;
		[[nodiscard]] bool emergencyReserveHasAllocatedFrames() const;
		[[nodiscard]] bool isEmergencyReserveSlot(PhysicalSlotId slot) const;

		// Returns pages unreachable from the current root or any active snapshot
		// root. A snapshot is represented by its immutable Superblock generation.
		// The caller must serialize this operation with metadata publication so no
		// in-flight copy-on-write generation can lose an unpublished page.
		void reclaimUnreachable(const Superblock &current, std::span<const Superblock> activeSnapshots);
		[[nodiscard]] MetadataPagePoolStats stats() const;
		[[nodiscard]] bool isMetadataPoolSlot(PhysicalSlotId slot) const;
		[[nodiscard]] bool isMetadataPoolEmpty(PhysicalSlotId slot) const;
		[[nodiscard]] MetadataPagePoolHeader poolHeader(PhysicalSlotId slot) const;
		// The caller must first publish the Catalog/Allocator transition that
		// releases these slots. The in-memory registry then stops allocating from
		// the now-free physical locations.
		void releaseEmptyCatalogPools(std::span<const PhysicalSlotId> slots);
		[[nodiscard]] std::vector<PhysicalSlotId> poolSlots() const;

#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
		// Test-only seams for proving fail-closed behavior when a process-local
		// registry update is interrupted after durable pool bytes were written.
		void eraseRegisteredPoolForTesting(PhysicalSlotId slot);
		void replaceRegisteredPoolHeaderForTesting(PhysicalSlotId slot, MetadataPagePoolHeader header);
#endif

	private:
		struct PoolState {
			MetadataPagePoolHeader header;
		};

		[[nodiscard]] uint64_t slotOffset(PhysicalSlotId slot) const;
		void discoverPoolsLocked();
		void discoverReachablePoolsLocked(const Superblock &superblock);
		[[nodiscard]] PoolState readPoolLocked(PhysicalSlotId slot, SegmentId expectedSegmentId,
											   Generation expectedGeneration, LogSequenceNumber expectedLsn,
											   std::array<std::byte, RSS_PAGE_SIZE> *controlPage = nullptr) const;
		[[nodiscard]] PoolState &createPoolLocked(Generation generation, LogSequenceNumber checkpointLsn,
												  std::span<const PhysicalSlotId> excludedPoolSlots);
		[[nodiscard]] PoolState &createPoolAtLocked(PhysicalSlotId slot, SegmentId segmentId, Generation generation,
													LogSequenceNumber checkpointLsn);
		void eraseUnpublishedPoolLocked(PhysicalSlotId slot);
		void abandonUnpublishedBatchLocked(std::span<const PhysicalSlotId> poolSlots,
										   std::span<const PhysicalSlotId> dataSlots, uint64_t initialFileSize);
		void writePoolLocked(const PoolState &pool);
		// `recover()` can only clear poison after every retained in-memory pool
		// still has the exact durable envelope/header it claims to represent.
		void validateRegisteredPoolsLocked() const;
		void reconcileReachabilityLocked(std::span<const Superblock> protectedSuperblocks, bool resetPageId,
										 bool reclaimUnreachable, bool persistReclaimedFrames);
		void requireHealthyLocked() const;

		std::shared_ptr<StorageIO> io_;
		uint64_t data_region_offset_ = 0;
		std::map<uint64_t, PoolState> pools_;
		MetadataEmergencyReserveDescriptor emergency_reserve_{};
		PageId next_page_id_{1};
		bool page_id_exhausted_ = false;
		bool poisoned_ = false;
		mutable std::mutex mutex_;
	};

} // namespace graph::storage
