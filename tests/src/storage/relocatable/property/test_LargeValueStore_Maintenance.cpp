/**
 * @file test_LargeValueStore_Maintenance.cpp
 * @brief Retirement and reclamation tests for LargeValueStore.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"

namespace {

	[[nodiscard]] LargeValueWriteOptions writeOptions() {
		LargeValueWriteOptions result;
		result.first_data_slot = PhysicalSlotId(40U);
		result.last_data_slot = PhysicalSlotId(80U);
		result.first_manifest_slot = PhysicalSlotId(81U);
		result.last_manifest_slot = PhysicalSlotId(90U);
		result.kind = LargeValueKind::LVK_UTF8_TEXT;
		result.media_type = "text/plain";
		return result;
	}

	[[nodiscard]] std::vector<std::byte> bytes(size_t count) {
		std::vector<std::byte> result(count);
		for (size_t index = 0U; index < result.size(); ++index) {
			result[index] = static_cast<std::byte>((index * 37U + 11U) & 0xFFU);
		}
		return result;
	}

} // namespace

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRetiresEveryFrameAndContentIdentityAtomically) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> input = bytes(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 1U);
	const LargeValueWriteResult written = store.write(input, writeOptions(), LogSequenceNumber(2U));
	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	std::vector<PageRetirement> retirements;
	for (const LargeValueChunk &chunk: written.manifest.chunks) {
		for (uint16_t ordinal = 0U; ordinal < chunk.data_page_count; ++ordinal) {
			retirements.push_back(
					store.planPageRetirement(oldSnapshot, PageId(chunk.first_data_page_id.value() + ordinal),
											 written.reference.value_id, PageOwnerKind::POK_LARGE_VALUE_DATA));
		}
	}
	retirements.push_back(store.planPageRetirement(oldSnapshot, written.manifest.page_id, written.reference.value_id,
											   PageOwnerKind::POK_LARGE_VALUE_MANIFEST));
	const Generation expectedGeneration = oldSnapshot.superblock().generation;
	const std::array<LargeValueId, 1U> erasedValueIds{{written.reference.value_id}};
	const Superblock retired =
			store.retireUnreachablePages(expectedGeneration, erasedValueIds, retirements, PhysicalSlotId(40U),
										 PhysicalSlotId(80U), LogSequenceNumber(3U));
	EXPECT_GT(retired.generation, expectedGeneration);
	EXPECT_EQ(store.read(oldSnapshot, written.reference), input);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findLargeValueManifestOptional(current, written.reference.value_id).has_value());
	EXPECT_THROW(static_cast<void>(store.read(current, written.reference)), FormatException);
	const BTreeIndex contentIndex(RSS_LARGE_VALUE_CONTENT_INDEX_ID, store.contentIndex(), *metadata);
	EXPECT_TRUE(contentIndex.scan(current, std::nullopt, 1U).empty());
	oldSnapshot.release();
}

TEST_F(RelocatableMetadataStoreTest,
		LargeValueStoreReclaimsAndReleasesAnEmptySharedDataPoolAfterSnapshotsDrain) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult written = store.write(bytes(97U), writeOptions(), LogSequenceNumber(2U));
	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	const PageId dataPageId = written.manifest.chunks.front().first_data_page_id;
	const CatalogEntry dataPool =
			metadata->findCatalog(oldSnapshot, metadata->findPageLocation(oldSnapshot, dataPageId).segment_id);
	const CatalogEntry manifestPool = metadata->findCatalog(
			oldSnapshot, metadata->findPageLocation(oldSnapshot, written.manifest.page_id).segment_id);
	ASSERT_NE(dataPool.segment_id, manifestPool.segment_id);
	std::array<PageRetirement, 2U> retirements{{
			store.planPageRetirement(oldSnapshot, dataPageId, written.reference.value_id,
									PageOwnerKind::POK_LARGE_VALUE_DATA),
			store.planPageRetirement(oldSnapshot, written.manifest.page_id, written.reference.value_id,
									PageOwnerKind::POK_LARGE_VALUE_MANIFEST),
	}};
	const std::array<LargeValueId, 1U> erasedValueIds{{written.reference.value_id}};
	static_cast<void>(store.retireUnreachablePages(oldSnapshot.superblock().generation, erasedValueIds, retirements,
													PhysicalSlotId(40U), PhysicalSlotId(80U), LogSequenceNumber(3U)));

	SharedPagePoolAllocator pools(io, *metadata);
	MetadataSnapshotPin current = metadata->openSnapshot();
	const uint8_t availableDataFramesWhilePinned = pools.availableFrameCount(dataPool);
	const uint8_t availableManifestFramesWhilePinned = pools.availableFrameCount(manifestPool);
	store.reclaimRetiredFrames(dataPool.segment_id);
	store.reclaimRetiredFrames(manifestPool.segment_id);
	EXPECT_EQ(pools.availableFrameCount(dataPool), availableDataFramesWhilePinned);
	EXPECT_EQ(pools.availableFrameCount(manifestPool), availableManifestFramesWhilePinned);
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(current, dataPool.physical_slot));
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(current, manifestPool.physical_slot));
	current.release();

	oldSnapshot.release();
	store.reclaimRetiredFrames(dataPool.segment_id);
	store.reclaimRetiredFrames(manifestPool.segment_id);
	EXPECT_EQ(pools.availableFrameCount(dataPool), RSS_PAGE_POOL_DATA_FRAME_COUNT);
	EXPECT_EQ(pools.availableFrameCount(manifestPool), RSS_PAGE_POOL_DATA_FRAME_COUNT);
	static_cast<void>(store.retireEmptyPool(dataPool.segment_id, dataPool.generation, LogSequenceNumber(4U)));
	static_cast<void>(store.retireEmptyPool(manifestPool.segment_id, manifestPool.generation, LogSequenceNumber(5U)));
	static_cast<void>(store.releaseRetiredEmptyPool(dataPool.segment_id, dataPool.generation, LogSequenceNumber(6U)));
	static_cast<void>(
			store.releaseRetiredEmptyPool(manifestPool.segment_id, manifestPool.generation, LogSequenceNumber(7U)));

	MetadataSnapshotPin released = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(released, dataPool.physical_slot));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(released, manifestPool.physical_slot));
	released.release();
}
