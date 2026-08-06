/**
 * @file test_MetadataStoreLifecycle.cpp
 * @brief Direct lifecycle invariants for relocatable metadata bootstrap.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include "graph/storage/relocatable/metadata/MetadataStoreLifecycle.hpp"

TEST_F(RelocatableMetadataStoreTest, MetadataStoreLifecyclePreservesAnAlreadyAllocatableSegmentId) {
	Superblock initial = superblock(1U, 1U);
	initial.next_logical_segment_id = RSS_FIRST_ALLOCATABLE_SEGMENT_ID;
	initializeRelocatableMetadataFile(io, initial);

	const Superblock recovered = recoverRelocatableMetadataSuperblock(io);
	EXPECT_EQ(recovered.next_logical_segment_id, RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
	EXPECT_TRUE(recovered.metadata_emergency_reserve.isValid());
}
