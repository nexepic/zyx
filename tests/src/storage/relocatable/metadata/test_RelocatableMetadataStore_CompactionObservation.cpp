/**
 * @file test_RelocatableMetadataStore_CompactionObservation.cpp
 * @brief Public compaction-observation contracts for metadata Catalog lifecycle states.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include "graph/utils/ChecksumUtils.hpp"

TEST_F(RelocatableMetadataStoreTest, CompactionObservationTracksPublicCatalogLifecycleStates) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(request, PhysicalSlotId(40U), PhysicalSlotId(40U),
																	 LogSequenceNumber(2U))
					.has_value());

	const RelocatableCompactionObservation sealed = store->observeCompactionState();
	EXPECT_EQ(sealed.sealed_segments, 2U);
	EXPECT_EQ(sealed.retired_segments, 0U);
	EXPECT_EQ(sealed.obsolete_segments, 0U);
	EXPECT_EQ(sealed.pending_relocation_sources, 0U);

	static_cast<void>(store->retireCatalogSegment(request.segment_id, request.segment_generation, LogSequenceNumber(3U)));
	const RelocatableCompactionObservation retired = store->observeCompactionState();
	EXPECT_EQ(retired.sealed_segments, 1U);
	EXPECT_EQ(retired.retired_segments, 1U);
	EXPECT_EQ(retired.obsolete_segments, 0U);
	EXPECT_EQ(retired.pending_relocation_sources, 0U);

	static_cast<void>(
			store->releaseRetiredCatalogSegment(request.segment_id, request.segment_generation, LogSequenceNumber(4U)));
	const RelocatableCompactionObservation obsolete = store->observeCompactionState();
	EXPECT_EQ(obsolete.sealed_segments, 1U);
	EXPECT_EQ(obsolete.retired_segments, 0U);
	EXPECT_EQ(obsolete.obsolete_segments, 1U);
	EXPECT_EQ(obsolete.pending_relocation_sources, 0U);
}

TEST_F(RelocatableMetadataStoreTest, CompactionObservationTracksRelocatedSourceUntilSnapshotRelease) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(request, PhysicalSlotId(40U), PhysicalSlotId(40U),
																 LogSequenceNumber(2U))
						.has_value());

	MetadataSnapshotPin sourceSnapshot = store->openSnapshot();
	const uint64_t sourceOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(sourceSnapshot.superblock().data_region_offset, PhysicalSlotId(40U));
	std::array<std::byte, 32U> payload{};
	for (size_t index = 0U; index < payload.size(); ++index) {
		payload[index] = static_cast<std::byte>(index + 1U);
	}
	std::vector<std::byte> sourceBytes(RSS_SEGMENT_SIZE);
	SegmentEnvelope sourceEnvelope;
	sourceEnvelope.kind = request.kind;
	sourceEnvelope.state = SegmentState::SGS_SEALED;
	sourceEnvelope.segment_id = request.segment_id;
	sourceEnvelope.owner_id = request.owner_id;
	sourceEnvelope.generation = request.segment_generation;
	sourceEnvelope.committed_lsn = LogSequenceNumber(2U);
	sourceEnvelope.payload_bytes = static_cast<uint32_t>(payload.size());
	sourceEnvelope.payload_crc = graph::utils::calculateCrc(payload.data(), payload.size());
	const auto sourceEnvelopeBytes = SegmentEnvelopeCodec::encode(sourceEnvelope);
	std::copy(sourceEnvelopeBytes.begin(), sourceEnvelopeBytes.end(), sourceBytes.begin());
	std::copy(payload.begin(), payload.end(), sourceBytes.begin() + sourceEnvelope.header_bytes);
	io->writeAt(sourceOffset, sourceBytes.data(), sourceBytes.size());
	io->sync();

	const std::optional<CatalogSlotRelocation> relocation = store->relocateCatalogSegment(
			request.segment_id, request.segment_generation, PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(3U),
			[&](const CatalogEntry &source, const CatalogEntry &destination) {
				std::vector<std::byte> copied(RSS_SEGMENT_SIZE);
				const uint64_t sourceSlotOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						sourceSnapshot.superblock().data_region_offset, source.physical_slot);
				ASSERT_EQ(io->readAt(sourceSlotOffset, copied.data(), copied.size()), copied.size());
				EXPECT_EQ(copied, sourceBytes);
				SegmentEnvelope copiedEnvelope =
						SegmentEnvelopeCodec::decode(std::span<const std::byte>(copied).first(RSS_ENVELOPE_BYTES));
				copiedEnvelope.generation = destination.generation;
				copiedEnvelope.committed_lsn = destination.committed_lsn;
				const auto copiedEnvelopeBytes = SegmentEnvelopeCodec::encode(copiedEnvelope);
				std::copy(copiedEnvelopeBytes.begin(), copiedEnvelopeBytes.end(), copied.begin());
				const uint64_t destinationOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						sourceSnapshot.superblock().data_region_offset, destination.physical_slot);
				io->writeAt(destinationOffset, copied.data(), copied.size());
				io->sync();
			});
	ASSERT_TRUE(relocation.has_value());

	std::vector<std::byte> destinationBytes(RSS_SEGMENT_SIZE);
	const uint64_t destinationOffset = PhysicalSegmentAllocator::checkedSlotOffset(
			sourceSnapshot.superblock().data_region_offset, relocation->destination.physical_slot);
	ASSERT_EQ(io->readAt(destinationOffset, destinationBytes.data(), destinationBytes.size()), destinationBytes.size());
	SegmentEnvelope expectedDestinationEnvelope = sourceEnvelope;
	expectedDestinationEnvelope.generation = relocation->destination.generation;
	expectedDestinationEnvelope.committed_lsn = relocation->destination.committed_lsn;
	const auto expectedDestinationEnvelopeBytes = SegmentEnvelopeCodec::encode(expectedDestinationEnvelope);
	std::copy(expectedDestinationEnvelopeBytes.begin(), expectedDestinationEnvelopeBytes.end(), sourceBytes.begin());
	EXPECT_EQ(destinationBytes, sourceBytes);

	const RelocatableCompactionObservation pending = store->observeCompactionState();
	EXPECT_EQ(pending.sealed_segments, 2U);
	EXPECT_EQ(pending.pending_relocation_sources, 1U);
	EXPECT_EQ(pending.retire_queue_entries, 1U);
	EXPECT_TRUE(pending.oldest_retired_generation.isValid());
	EXPECT_EQ(pending.oldest_snapshot_generation, sourceSnapshot.superblock().generation);
	EXPECT_EQ(pending.snapshot_generation_lag, pending.generation.value()-sourceSnapshot.superblock().generation.value());
	EXPECT_EQ(pending.retainedGenerationCount(), 1U);
	expectFormatError(
			[&] {
				static_cast<void>(store->releaseRelocatedCatalogSource(request.segment_id, Generation(2U),
																								LogSequenceNumber(4U)));
			},
			FormatError::FERR_INVALID_STATE);

	sourceSnapshot.release();
	static_cast<void>(
			store->releaseRelocatedCatalogSource(request.segment_id, Generation(2U), LogSequenceNumber(4U)));
	const RelocatableCompactionObservation released = store->observeCompactionState();
	EXPECT_EQ(released.pending_relocation_sources, 0U);
	EXPECT_EQ(released.retire_queue_entries, 0U);
	EXPECT_FALSE(released.oldest_retired_generation.isValid());
	EXPECT_FALSE(released.oldest_snapshot_generation.isValid());
	EXPECT_EQ(released.retainedGenerationCount(), 0U);
}
