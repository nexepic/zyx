/**
 * @file test_SuperblockPair.cpp
 * @brief Copy-on-write and crash-boundary tests for relocatable A/B superblocks.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <cstddef>
#include <cstdint>
#include <functional>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/format/SuperblockPair.hpp"

using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	void expectInterrupted(const std::function<void()> &operation) {
		EXPECT_THROW(operation(), BootstrapPublishInterrupted);
	}

	PhysicalPageRef pageRef(uint64_t slot, uint8_t frame, uint64_t generation) {
		PhysicalPageRef result;
		result.physical_slot = PhysicalSlotId(slot);
		result.frame_index = frame;
		result.generation = Generation(generation);
		return result;
	}

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn, uint64_t rootSlot) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		result.catalog_root = pageRef(rootSlot, 1, generation);
		result.page_directory_root = pageRef(rootSlot + 1U, 2, generation);
		result.extent_directory_root = pageRef(rootSlot + 2U, 3, generation);
		result.allocator_root = pageRef(rootSlot + 3U, 4, generation);
		return result;
	}

} // namespace

TEST(RelocatableSuperblockPairTest, PublishesNewRootsIntoTheInactiveSlotAndRecoversThem) {
	SuperblockPair pair(superblock(1, 10, 100));
	EXPECT_EQ(pair.recover().generation, Generation(1));
	pair.publish(superblock(2, 11, 200));
	EXPECT_EQ(pair.recover().generation, Generation(2));
	EXPECT_EQ(pair.recover().catalog_root.physical_slot, PhysicalSlotId(200));
	EXPECT_EQ(SuperblockCodec::decode(pair.slotBytes(0)).generation, Generation(1));
	EXPECT_EQ(SuperblockCodec::decode(pair.slotBytes(1)).generation, Generation(2));

	pair.publish(superblock(3, 12, 300));
	EXPECT_EQ(pair.recover().generation, Generation(3));
	EXPECT_EQ(SuperblockCodec::decode(pair.slotBytes(0)).generation, Generation(3));
	EXPECT_EQ(SuperblockCodec::decode(pair.slotBytes(1)).generation, Generation(2));
	pair.publish(superblock(4, 13, 400));
	EXPECT_EQ(SuperblockCodec::decode(pair.slotBytes(0)).generation, Generation(3));
	EXPECT_EQ(SuperblockCodec::decode(pair.slotBytes(1)).generation, Generation(4));
}

TEST(RelocatableSuperblockPairTest, RejectsNonAdvancingAndMalformedPublicationRequests) {
	SuperblockPair pair(superblock(3, 20, 100));
	expectFormatError([&] { pair.publish(superblock(3, 21, 200)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { pair.publish(superblock(4, 20, 200)); }, FormatError::FERR_INVALID_STATE);
	Superblock malformed = superblock(4, 21, 200);
	malformed.catalog_root.frame_index = 0;
	expectFormatError([&] { pair.publish(malformed); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(pair.slotBytes(2)); }, FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(pair.recover().generation, Generation(3));
}

TEST(RelocatableSuperblockPairTest, CrashBeforeOrDuringInactiveWriteRetainsThePriorGeneration) {
	SuperblockPair pair(superblock(5, 30, 100));
	expectInterrupted(
			[&] { pair.publish(superblock(6, 31, 200), BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE); });
	EXPECT_EQ(pair.recover().generation, Generation(5));

	expectInterrupted(
			[&] { pair.publish(superblock(6, 31, 200), BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE); });
	EXPECT_EQ(pair.recover().generation, Generation(5));
	EXPECT_EQ(SuperblockCodec::decode(pair.slotBytes(0)).generation, Generation(5));
}

TEST(RelocatableSuperblockPairTest, CrashAfterCompleteInactiveWriteRecoversTheNewGeneration) {
	SuperblockPair pair(superblock(8, 40, 100));
	expectInterrupted(
			[&] { pair.publish(superblock(9, 41, 200), BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE); });
	EXPECT_EQ(pair.recover().generation, Generation(9));
	EXPECT_EQ(pair.recover().catalog_root.physical_slot, PhysicalSlotId(200));
}

TEST(RelocatableSuperblockPairTest, RebuildsEitherCorruptCopyInsteadOfOverwritingTheValidGeneration) {
	SuperblockPair::Slots slots{};
	slots[0] = SuperblockCodec::encode(superblock(1, 10, 100));
	slots[1] = SuperblockCodec::encode(superblock(2, 11, 200));
	slots[0][0] ^= std::byte{0x01};
	SuperblockPair firstCorrupt(slots);
	EXPECT_EQ(firstCorrupt.recover().generation, Generation(2));
	firstCorrupt.publish(superblock(3, 12, 300));
	EXPECT_EQ(SuperblockCodec::decode(firstCorrupt.slotBytes(0)).generation, Generation(3));

	slots[0] = SuperblockCodec::encode(superblock(4, 20, 400));
	slots[1] = SuperblockCodec::encode(superblock(3, 19, 300));
	slots[1][0] ^= std::byte{0x01};
	SuperblockPair secondCorrupt(slots);
	EXPECT_EQ(secondCorrupt.recover().generation, Generation(4));
	secondCorrupt.publish(superblock(5, 21, 500));
	EXPECT_EQ(SuperblockCodec::decode(secondCorrupt.slotBytes(1)).generation, Generation(5));

	SuperblockPair::Slots invalid{};
	SuperblockPair neitherValid(invalid);
	expectFormatError([&] { static_cast<void>(neitherValid.recover()); }, FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST(RelocatableSuperblockPairTest, RejectsUnknownFailpointWithoutChangingTheRecoveredGeneration) {
	SuperblockPair pair(superblock(10, 50, 100));
	expectFormatError([&] { pair.publish(superblock(11, 51, 200), static_cast<BootstrapPublishFailpoint>(99)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(pair.recover().generation, Generation(10));
}
