/**
 * @file test_MetadataStoreLifecycle.cpp
 * @brief Direct lifecycle invariants for relocatable metadata bootstrap.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <limits>

#include "graph/storage/relocatable/metadata/MetadataStoreLifecycle.hpp"

TEST_F(RelocatableMetadataStoreTest, MetadataStoreLifecyclePreservesAnAlreadyAllocatableSegmentId) {
	Superblock initial = superblock(1U, 1U);
	initial.next_logical_segment_id = RSS_FIRST_ALLOCATABLE_SEGMENT_ID;
	initializeRelocatableMetadataFile(io, initial);

	const Superblock recovered = recoverRelocatableMetadataSuperblock(io);
	EXPECT_EQ(recovered.next_logical_segment_id, RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
	EXPECT_TRUE(recovered.metadata_emergency_reserve.isValid());
}

TEST_F(RelocatableMetadataStoreTest, MetadataStoreLifecycleRejectsMissingAndMismatchedFileStates) {
	const Superblock initial = superblock(1U, 1U);
	const std::shared_ptr<StorageIO> missing;
	EXPECT_THROW(initializeRelocatableMetadataFile(missing, initial), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(recoverRelocatableMetadataSuperblock(missing)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(recoverRelocatableMetadataSuperblock(io)), FormatException);

	initializeRelocatableMetadataFile(io, initial);
	EXPECT_THROW(initializeRelocatableMetadataFile(io, initial), FormatException);
}

TEST_F(RelocatableMetadataStoreTest, CompleteInitializationPublishesBootstrapBeforeStandaloneReopen) {
	auto store = RelocatableMetadataStore::initializeNewComplete(io, superblock(1U, 1U));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(2U));
	EXPECT_TRUE(snapshot.superblock().usesPageDirectoryBootstrap());
	EXPECT_TRUE(snapshot.superblock().catalog_root.isValid());
	snapshot.release();
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(2U));
	EXPECT_TRUE(recovered.superblock().usesPageDirectoryBootstrap());
	EXPECT_TRUE(recovered.superblock().catalog_root.isValid());
}

TEST_F(RelocatableMetadataStoreTest, CompleteInitializationKeepsBootstrapRoutesAcrossTheFirstMetadataCow) {
	auto store = RelocatableMetadataStore::initializeNewComplete(io, superblock(1U, 1U));
	const Superblock updated = store->upsertCatalog(std::vector{entry(100U, 3U, 3U)}, LogSequenceNumber(3U));
	ASSERT_TRUE(updated.usesPageDirectoryBootstrap());
	ASSERT_GT(updated.generation, Generation(2U));

	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(store->findCatalog(current, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(100U));
	current.release();
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_TRUE(recovered.superblock().usesPageDirectoryBootstrap());
	EXPECT_EQ(reopened->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(100U));
}

TEST_F(RelocatableMetadataStoreTest, CompleteInitializationRejectsTerminalCheckpointBeforeWritingTheInitialFile) {
	Superblock initial = superblock(1U, std::numeric_limits<uint64_t>::max());
	EXPECT_THROW(static_cast<void>(RelocatableMetadataStore::initializeNewComplete(io, initial)), FormatException);
	EXPECT_THROW(static_cast<void>(recoverRelocatableMetadataSuperblock(io)), FormatException);
}
