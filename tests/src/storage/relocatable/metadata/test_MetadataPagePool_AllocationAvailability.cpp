/**
 * @file test_MetadataPagePool_AllocationAvailability.cpp
 * @brief Availability accounting tests for metadata page pools.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <functional>

#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "storage/relocatable/metadata/MetadataPagePoolTestFixture.hpp"

using namespace graph::storage;
using graph::storage::test_support::RelocatableMetadataPagePoolTest;

namespace {

	class SingleSlotReservationLookup final : public MetadataSlotReservationLookup {
	public:
		explicit SingleSlotReservationLookup(PhysicalSlotId slot) : slot_(slot) {}

		[[nodiscard]] bool isDataSlotReserved(PhysicalSlotId slot) const noexcept override { return slot == slot_; }

		[[nodiscard]] std::optional<PhysicalSlotRange>
		dataSlotRangeContaining(PhysicalSlotId slot) const noexcept override {
			return slot == slot_ ? std::optional<PhysicalSlotRange>(PhysicalSlotRange{slot_, slot_}) : std::nullopt;
		}

	private:
		PhysicalSlotId slot_;
	};

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

} // namespace

TEST_F(RelocatableMetadataPagePoolTest, RebaseRejectsAnExternalPublicationWithADifferentDataRegionLayout) {
	const Superblock initial = initialSuperblock();
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const PhysicalPageRef allocated = allocator.allocatePage(Generation(1U));

	Superblock incompatible = initial;
	incompatible.data_region_offset += RSS_SEGMENT_SIZE;
	expectFormatError([&] { allocator.rebaseToPublishedSuperblock(incompatible); }, FormatError::FERR_INVALID_STATE);

	EXPECT_TRUE(allocator.isMetadataPoolSlot(allocated.physical_slot));
	EXPECT_EQ(allocator.stats().allocated_pages, 1U);
}

TEST_F(RelocatableMetadataPagePoolTest, AllocatableFreePagesExcludesReserveAndSnapshotProtectedPools) {
	const Superblock initial = initialSuperblock();
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);

	const PhysicalPageRef normalPage = allocator.allocatePage(Generation(1U));
	EXPECT_EQ(allocator.allocatableFreePages(), RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U);

	MetadataEmergencyReserveDescriptor reserve;
	reserve.physical_slot = PhysicalSlotId(2U);
	reserve.pool_token = SegmentId(71U);
	reserve.generation = Generation(1U);
	reserve.committed_lsn = LogSequenceNumber(1U);
	allocator.createEmergencyReserve(reserve);
	EXPECT_EQ(allocator.allocatableFreePages(), RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U);

	allocator.protectPoolUntilSnapshotsDrain(normalPage.physical_slot);
	EXPECT_EQ(allocator.allocatableFreePages(), 0U);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryReclaimsAReleasedSnapshotProtectedPoolForAllocation) {
	const Superblock initial = initialSuperblock();
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);

	const PhysicalPageRef normalPage = allocator.allocatePage(Generation(1U));
	allocator.protectPoolUntilSnapshotsDrain(normalPage.physical_slot);
	EXPECT_EQ(allocator.allocatableFreePages(), 0U);

	// The only protected superblock no longer references this transient pool, so
	// recovery must release the protection and make the pool reusable.
	allocator.recover(initial);
	EXPECT_EQ(allocator.allocatableFreePages(), RSS_PAGE_POOL_DATA_FRAME_COUNT);
}

TEST_F(RelocatableMetadataPagePoolTest, MetadataFrameReservationRejectsInvalidStateTransitions) {
	const Superblock initial = initialSuperblock();
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	static_cast<void>(allocator.allocatePage(Generation(1U)));

	expectFormatError(
			[&] {
				allocator.reserveMetadataFrames(0U, std::span<const PhysicalSlotId>{},
											std::span<const PhysicalSlotRange>{}, nullptr);
			},
			FormatError::FERR_INVALID_STATE);
	allocator.reserveMetadataFrames(1U, std::span<const PhysicalSlotId>{}, std::span<const PhysicalSlotRange>{},
								nullptr);
	expectFormatError(
			[&] {
				allocator.reserveMetadataFrames(1U, std::span<const PhysicalSlotId>{},
											std::span<const PhysicalSlotRange>{}, nullptr);
			},
			FormatError::FERR_INVALID_STATE);

	static_cast<void>(allocator.allocatePage(Generation(2U)));
	expectFormatError([&] { static_cast<void>(allocator.allocatePage(Generation(2U))); },
				  FormatError::FERR_INVALID_STATE);
	allocator.releaseMetadataFrames();
}

TEST_F(RelocatableMetadataPagePoolTest, MetadataFrameCapacityRejectsInvalidOrReservedRequests) {
	const Superblock initial = initialSuperblock();
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	static_cast<void>(allocator.allocatePage(Generation(1U)));

	expectFormatError(
			[&] {
				static_cast<void>(allocator.ensureMetadataFrameCapacity(
						Generation{}, LogSequenceNumber(1U), 1U, std::span<const PhysicalSlotId>{},
						std::span<const PhysicalSlotRange>{}, nullptr));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(allocator.ensureMetadataFrameCapacity(
						Generation(2U), LogSequenceNumber{}, 1U, std::span<const PhysicalSlotId>{},
						std::span<const PhysicalSlotRange>{}, nullptr));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(allocator.ensureMetadataFrameCapacity(
						Generation(2U), LogSequenceNumber(2U), 0U, std::span<const PhysicalSlotId>{},
						std::span<const PhysicalSlotRange>{}, nullptr));
			},
			FormatError::FERR_INVALID_STATE);

	allocator.reserveMetadataFrames(1U, std::span<const PhysicalSlotId>{}, std::span<const PhysicalSlotRange>{},
								nullptr);
	expectFormatError(
			[&] {
				static_cast<void>(allocator.ensureMetadataFrameCapacity(
						Generation(2U), LogSequenceNumber(2U), 1U, std::span<const PhysicalSlotId>{},
						std::span<const PhysicalSlotRange>{}, nullptr));
			},
			FormatError::FERR_INVALID_STATE);
	allocator.releaseMetadataFrames();
}

TEST_F(RelocatableMetadataPagePoolTest, MetadataFrameReservationDoesNotConsumeSnapshotProtectedPools) {
	const Superblock initial = initialSuperblock();
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const PhysicalPageRef allocated = allocator.allocatePage(Generation(1U));

	allocator.protectPoolUntilSnapshotsDrain(allocated.physical_slot);
	expectFormatError(
			[&] {
				allocator.reserveMetadataFrames(1U, std::span<const PhysicalSlotId>{},
											std::span<const PhysicalSlotRange>{}, nullptr);
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPagePoolTest, MetadataFrameCapacityCreatesPoolsAroundExcludedSlots) {
	const Superblock initial = initialSuperblock();
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	static_cast<void>(allocator.allocatePage(Generation(1U)));

	const std::array<PhysicalSlotRange, 1U> excluded{{PhysicalSlotRange{PhysicalSlotId(1U), PhysicalSlotId(1U)}}};
	const std::vector<PhysicalSlotId> created = allocator.ensureMetadataFrameCapacity(
			Generation(2U), LogSequenceNumber(2U), 1U, {}, excluded, nullptr);
	ASSERT_EQ(created.size(), 1U);
	EXPECT_TRUE(allocator.isMetadataPoolSlot(created.front()));
	EXPECT_EQ(allocator.stats().pools, 2U);
}

TEST_F(RelocatableMetadataPagePoolTest, MetadataFrameReservationConsultsDataSlotReservations) {
	const Superblock initial = initialSuperblock();
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	static_cast<void>(allocator.allocatePage(Generation(1U)));
	const SingleSlotReservationLookup lookup(PhysicalSlotId(1U));

	expectFormatError(
			[&] {
				allocator.reserveMetadataFrames(1U, {}, {}, &lookup);
			},
			FormatError::FERR_INVALID_STATE);
}
