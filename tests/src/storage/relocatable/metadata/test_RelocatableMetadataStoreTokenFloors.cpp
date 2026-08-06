/**
 * @file test_RelocatableMetadataStoreTokenFloors.cpp
 * @brief Token high-water-floor tests for relocatable metadata publication.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <limits>

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

TEST_F(RelocatableMetadataStoreTest, AdvancesTokenFloorsDurablyWithoutRepublishingAlreadyPastHighWaterMarks) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));

	expectFormatError(
			[&] { store->advancePropertyKeyTokenFloor(0U, LogSequenceNumber(2U)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { store->advancePropertyKeyTokenFloor(7U, LogSequenceNumber{}); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { store->advanceSchemaTokenIdFloor(0U, LogSequenceNumber(2U)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { store->advanceSchemaTokenIdFloor(7U, LogSequenceNumber{}); },
			FormatError::FERR_INVALID_HEADER);

	store->advancePropertyKeyTokenFloor(8U, LogSequenceNumber(2U));
	MetadataSnapshotPin propertyAdvanced = store->openSnapshot();
	EXPECT_EQ(propertyAdvanced.superblock().next_property_key_token, 9U);
	EXPECT_EQ(propertyAdvanced.superblock().generation, Generation(2U));
	propertyAdvanced.release();

	// A lower observed token must be a no-op, including no needless Superblock publication.
	store->advancePropertyKeyTokenFloor(8U, LogSequenceNumber(3U));
	MetadataSnapshotPin propertyUnchanged = store->openSnapshot();
	EXPECT_EQ(propertyUnchanged.superblock().next_property_key_token, 9U);
	EXPECT_EQ(propertyUnchanged.superblock().generation, Generation(2U));
	EXPECT_EQ(propertyUnchanged.superblock().checkpoint_lsn, LogSequenceNumber(2U));
	propertyUnchanged.release();

	store->advancePropertyKeyTokenFloor(std::numeric_limits<uint32_t>::max(), LogSequenceNumber(3U));
	store->advancePropertyKeyTokenFloor(1U, LogSequenceNumber(4U));
	MetadataSnapshotPin propertyExhausted = store->openSnapshot();
	EXPECT_EQ(propertyExhausted.superblock().next_property_key_token, 0U);
	EXPECT_EQ(propertyExhausted.superblock().generation, Generation(3U));
	EXPECT_EQ(propertyExhausted.superblock().checkpoint_lsn, LogSequenceNumber(3U));
	propertyExhausted.release();

	store->advanceSchemaTokenIdFloor(12U, LogSequenceNumber(4U));
	MetadataSnapshotPin schemaAdvanced = store->openSnapshot();
	EXPECT_EQ(schemaAdvanced.superblock().next_schema_token_id, 13U);
	EXPECT_EQ(schemaAdvanced.superblock().generation, Generation(4U));
	schemaAdvanced.release();

	store->advanceSchemaTokenIdFloor(12U, LogSequenceNumber(5U));
	MetadataSnapshotPin schemaUnchanged = store->openSnapshot();
	EXPECT_EQ(schemaUnchanged.superblock().next_schema_token_id, 13U);
	EXPECT_EQ(schemaUnchanged.superblock().generation, Generation(4U));
	EXPECT_EQ(schemaUnchanged.superblock().checkpoint_lsn, LogSequenceNumber(4U));
	schemaUnchanged.release();

	store->advanceSchemaTokenIdFloor(std::numeric_limits<uint64_t>::max(), LogSequenceNumber(5U));
	store->advanceSchemaTokenIdFloor(1U, LogSequenceNumber(6U));
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin persisted = reopened->openSnapshot();
	EXPECT_EQ(persisted.superblock().next_property_key_token, 0U);
	EXPECT_EQ(persisted.superblock().next_schema_token_id, 0U);
	EXPECT_EQ(persisted.superblock().generation, Generation(5U));
	EXPECT_EQ(persisted.superblock().checkpoint_lsn, LogSequenceNumber(5U));
}

TEST_F(RelocatableMetadataStoreTest, RecoversTokenFloorPublicationAcrossBothSuperblockInterruptions) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	EXPECT_THROW(store->advancePropertyKeyTokenFloor(
				 9U, LogSequenceNumber(2U),
				 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE)),
			 BootstrapPublishInterrupted);
	store.reset();

	auto beforePublished = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin before = beforePublished->openSnapshot();
	EXPECT_EQ(before.superblock().generation, Generation(1U));
	EXPECT_EQ(before.superblock().next_property_key_token, 1U);
	EXPECT_EQ(before.superblock().next_schema_token_id, 1U);
	before.release();

	EXPECT_THROW(beforePublished->advanceSchemaTokenIdFloor(
				 9U, LogSequenceNumber(2U),
				 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE)),
			 BootstrapPublishInterrupted);
	beforePublished.reset();

	auto afterPublished = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin after = afterPublished->openSnapshot();
	EXPECT_EQ(after.superblock().generation, Generation(2U));
	EXPECT_EQ(after.superblock().checkpoint_lsn, LogSequenceNumber(2U));
	EXPECT_EQ(after.superblock().next_property_key_token, 1U);
	EXPECT_EQ(after.superblock().next_schema_token_id, 10U);
}

TEST_F(RelocatableMetadataStoreTest, ReadOnlyMetadataStoreCannotAdvanceTokenFloors) {
	auto writable = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	auto readOnly = RelocatableMetadataStore::openReadOnly(io);

	EXPECT_THROW(readOnly->advancePropertyKeyTokenFloor(9U, LogSequenceNumber(2U)), FormatException);
	EXPECT_THROW(readOnly->advanceSchemaTokenIdFloor(9U, LogSequenceNumber(2U)), FormatException);
	readOnly.reset();
	writable.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(1U));
	EXPECT_EQ(snapshot.superblock().next_property_key_token, 1U);
	EXPECT_EQ(snapshot.superblock().next_schema_token_id, 1U);
}

TEST_F(RelocatableMetadataStoreTest, ReadOnlyMetadataStoreCannotReserveAnyLogicalIdentityOrAllocatorBit) {
	auto writable = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = writable->openSnapshot();
	const Superblock expected = before.superblock();
	before.release();
	auto readOnly = RelocatableMetadataStore::openReadOnly(io);

	EXPECT_THROW(static_cast<void>(readOnly->allocateLogicalPageId(LogSequenceNumber(2U))), FormatException);
	EXPECT_THROW(static_cast<void>(readOnly->allocateLogicalPageIds(1U, LogSequenceNumber(2U))), FormatException);
	EXPECT_THROW(static_cast<void>(readOnly->allocateLogicalSegmentIds(1U, LogSequenceNumber(2U))), FormatException);
	EXPECT_THROW(static_cast<void>(readOnly->allocateLogicalNodeId(LogSequenceNumber(2U))), FormatException);
	EXPECT_THROW(static_cast<void>(readOnly->allocateLogicalEdgeId(LogSequenceNumber(2U))), FormatException);
	EXPECT_THROW(static_cast<void>(readOnly->allocateLogicalVectorId(LogSequenceNumber(2U))), FormatException);
	EXPECT_THROW(static_cast<void>(readOnly->allocatePropertyKeyToken(LogSequenceNumber(2U))), FormatException);
	EXPECT_THROW(static_cast<void>(readOnly->allocateSchemaTokenId(LogSequenceNumber(2U))), FormatException);
	EXPECT_THROW(static_cast<void>(readOnly->setPhysicalSlotAllocated(PhysicalSlotId(40U), true,
																							 LogSequenceNumber(2U))),
			 FormatException);

	readOnly.reset();
	writable.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, expected.generation);
	EXPECT_EQ(snapshot.superblock().checkpoint_lsn, expected.checkpoint_lsn);
	EXPECT_EQ(snapshot.superblock().next_logical_page_id, expected.next_logical_page_id);
	EXPECT_EQ(snapshot.superblock().next_logical_segment_id, expected.next_logical_segment_id);
	EXPECT_EQ(snapshot.superblock().next_logical_node_id, expected.next_logical_node_id);
	EXPECT_EQ(snapshot.superblock().next_logical_edge_id, expected.next_logical_edge_id);
	EXPECT_EQ(snapshot.superblock().next_logical_vector_id, expected.next_logical_vector_id);
	EXPECT_EQ(snapshot.superblock().next_property_key_token, expected.next_property_key_token);
	EXPECT_EQ(snapshot.superblock().next_schema_token_id, expected.next_schema_token_id);
}
