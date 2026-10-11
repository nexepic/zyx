/**
 * @file test_RelocatableMetadataStoreIdentity.cpp
 * @brief Public input validation for durable logical identity operations.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

TEST_F(RelocatableMetadataStoreTest, RejectsInvalidCheckpointLsnBeforeIdentityOrRetireQueuePublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const LogSequenceNumber invalidCheckpoint{};
	expectFormatError(
			[&] { static_cast<void>(store->allocateLogicalPageIds(1U, invalidCheckpoint)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->allocateLogicalSegmentIds(1U, invalidCheckpoint)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->reclaimRetiredPhysicalSlots(invalidCheckpoint)); },
			FormatError::FERR_INVALID_HEADER);

	const MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(1U));
	EXPECT_EQ(snapshot.superblock().next_logical_page_id, 1U);
	EXPECT_EQ(snapshot.superblock().next_logical_segment_id, RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
}
