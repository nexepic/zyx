/**
 * @file test_LargeValueReachabilityReclaimer.cpp
 * @brief Reachability, retirement, and snapshot tests for generic LargeValue pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/metadata/StateRecordStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"

namespace {

	class TestLargeValueOwnershipScanner final : public LargeValueOwnershipScanner {
	public:
		TestLargeValueOwnershipScanner(std::string mediaType, LargeValueRef reference) :
			media_type_(std::move(mediaType)), reference_(std::move(reference)) {}

		[[nodiscard]] std::string_view mediaType() const noexcept override { return media_type_; }

		[[nodiscard]] Progress scanReachableAfter(const MetadataSnapshotPin &, uint64_t, size_t maximumLocations,
												  size_t maximumMetadataPages,
												  LargeValueOwnershipScanSink &sink) const override {
			if (maximumLocations == 0U || maximumMetadataPages == 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "test ownership scanner received no remaining scan budget");
			}
			sink.markReachable(reference_);
			return {1U, 1U, 1U, true};
		}

	private:
		std::string media_type_;
		LargeValueRef reference_;
	};

	class ConfigurableLargeValueOwnershipScanner final : public LargeValueOwnershipScanner {
	public:
		ConfigurableLargeValueOwnershipScanner(std::string mediaType, LargeValueRef reference, size_t markedReferences,
											   size_t reportedLocations) :
			media_type_(std::move(mediaType)), reference_(std::move(reference)), marked_references_(markedReferences),
			reported_locations_(reportedLocations) {}

		[[nodiscard]] std::string_view mediaType() const noexcept override { return media_type_; }

		[[nodiscard]] Progress scanReachableAfter(const MetadataSnapshotPin &, uint64_t, size_t, size_t,
												  LargeValueOwnershipScanSink &sink) const override {
			for (size_t index = 0U; index < marked_references_; ++index) {
				sink.markReachable(reference_);
			}
			return {reported_locations_, reported_locations_, 0U, true};
		}

	private:
		std::string media_type_;
		LargeValueRef reference_;
		size_t marked_references_ = 0U;
		size_t reported_locations_ = 0U;
	};

	[[nodiscard]] LargeValueWriteOptions propertyValueWriteOptions() {
		LargeValueWriteOptions options;
		options.first_data_slot = PhysicalSlotId(40U);
		options.last_data_slot = PhysicalSlotId(80U);
		options.first_manifest_slot = PhysicalSlotId(81U);
		options.last_manifest_slot = PhysicalSlotId(100U);
		options.kind = LargeValueKind::LVK_BYTES;
		options.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
		return options;
	}

	[[nodiscard]] LargeValueWriteOptions stateValueWriteOptions() {
		LargeValueWriteOptions options = propertyValueWriteOptions();
		options.media_type = std::string(RSS_STATE_LARGE_VALUE_MEDIA_TYPE);
		return options;
	}

	[[nodiscard]] StateRecordStoreOptions stateRecordStoreOptions() {
		return {PhysicalSlotId(101U), PhysicalSlotId(110U), 71U};
	}

	[[nodiscard]] LargeValueReachabilityReclaimOptions reclaimOptions() {
		LargeValueReachabilityReclaimOptions options;
		options.maximum_manifest_locations = 32U;
		options.maximum_manifest_metadata_pages = 32U;
		options.maximum_manifest_nodes = 32U;
		options.maximum_owner_locations = 32U;
		options.maximum_owner_metadata_pages = 32U;
		options.maximum_data_pages = 64U;
		options.maximum_retirements = 64U;
		options.maximum_page_locations = 128U;
		options.first_index_slot = PhysicalSlotId(40U);
		options.last_index_slot = PhysicalSlotId(100U);
		return options;
	}

	[[nodiscard]] LogSequenceNumber nextCheckpointLsn(RelocatableMetadataStore &metadata) {
		const MetadataSnapshotPin snapshot = metadata.openSnapshot();
		return LogSequenceNumber(snapshot.superblock().checkpoint_lsn.value() + 1U);
	}

	EntityPropertyLocation publishEntityPropertyPayload(RelocatableMetadataStore &metadata, SlottedPagePool &pages,
														EntityPropertyKey key, std::span<const std::byte> payload) {
		const PageId pageId = metadata.allocateLogicalPageId(nextCheckpointLsn(metadata));
		const std::vector<SegmentId> segmentIds = metadata.allocateLogicalSegmentIds(1U, nextCheckpointLsn(metadata));
		if (segmentIds.size() != 1U) {
			throw std::logic_error("test expected one reserved Property pool Segment ID");
		}
		const LogSequenceNumber pageLsn = nextCheckpointLsn(metadata);
		const PageOwnerHeader owner{
				pageId,			71U,	PageOwnerKind::POK_PROPERTY, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				Generation(1U), pageLsn};
		SlottedPage page = SlottedPage::create(owner);
		const SlottedSlotReference slot = page.insert(payload);
		SlottedPagePoolAllocationRequest request;
		request.segment_id = segmentIds.front();
		request.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
		request.pool_owner_id = owner.owner_id;
		request.generation = Generation(1U);
		request.page_id = pageId;
		request.page_owner_id = owner.owner_id;
		request.page_owner_kind = owner.kind;
		const std::optional<SlottedPagePoolAllocation> allocation =
				pages.allocatePage(request, page, PhysicalSlotId(101U), PhysicalSlotId(110U), pageLsn);
		if (!allocation.has_value()) {
			throw std::logic_error("test could not materialize an Entity Property reference page");
		}
		const EntityPropertyLocation location{key, pageId, slot.slot_id, slot.generation};
		static_cast<void>(
				metadata.upsertEntityPropertyLocations(std::span(&location, 1U), {}, nextCheckpointLsn(metadata)));
		return location;
	}

	EntityPropertyLocation publishEntityPropertyReference(RelocatableMetadataStore &metadata, SlottedPagePool &pages,
														  const LargeValueRef &reference,
														  EntityPropertyKey key = {EntityPropertyOwnerKind::EPOK_NODE,
																				   7U}) {
		const uint32_t ownerType =
				key.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? graph::Node::typeId : graph::Edge::typeId;
		const EntityPropertyLargeValueReference stored{key.entity_id, ownerType, reference};
		const std::vector<std::byte> payload = EntityPropertyLargeValueReferenceCodec::encode(stored);
		return publishEntityPropertyPayload(metadata, pages, key, payload);
	}

	[[nodiscard]] uint64_t pageFrameOffset(RelocatableMetadataStore &metadata, PageId pageId) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			pool.physical_slot) +
								static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		snapshot.release();
		return offset;
	}

	void overwritePageOwner(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata, PageId pageId,
							const PageOwnerHeader &owner) {
		const auto bytes = PageOwnerHeaderCodec::encode(owner);
		io->writeAt(pageFrameOffset(metadata, pageId), bytes.data(), bytes.size());
	}

	void overwriteLargeValueManifest(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
									 const LargeValueManifest &manifest) {
		const std::vector<std::byte> bytes = LargeValueManifestPageCodec::encode(manifest);
		io->writeAt(pageFrameOffset(metadata, manifest.page_id) + RSS_PAGE_OWNER_HEADER_BYTES, bytes.data(),
					bytes.size());
	}

} // namespace

TEST_F(RelocatableMetadataStoreTest,
	   LargeValueReclaimerRetiresOnlyUnreferencedEntityPropertyValuesAndPreservesSnapshots) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const std::array liveBytes{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
	const std::array orphanBytes{std::byte{0xA1}, std::byte{0xA2}, std::byte{0xA3}, std::byte{0xA4}};
	const LargeValueWriteResult live =
			values.write(liveBytes, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	const LargeValueWriteResult orphan =
			values.write(orphanBytes, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	const EntityPropertyLocation property = publishEntityPropertyReference(*metadata, propertyPages, live.reference);
	MetadataSnapshotPin layout = metadata->openSnapshot();
	const PageLocation orphanDataLocation =
			metadata->findPageLocation(layout, orphan.manifest.chunks.front().first_data_page_id);
	const PageLocation orphanManifestLocation = metadata->findPageLocation(layout, orphan.manifest.page_id);
	const PageLocation liveDataLocation =
			metadata->findPageLocation(layout, live.manifest.chunks.front().first_data_page_id);
	const PageLocation liveManifestLocation = metadata->findPageLocation(layout, live.manifest.page_id);
	ASSERT_EQ(orphanDataLocation.segment_id, liveDataLocation.segment_id);
	ASSERT_EQ(orphanManifestLocation.segment_id, liveManifestLocation.segment_id);
	const CatalogEntry sharedDataPool = metadata->findCatalog(layout, orphanDataLocation.segment_id);
	ASSERT_EQ(sharedDataPool.owner_id, RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID);
	const CatalogEntry sharedManifestPool = metadata->findCatalog(layout, orphanManifestLocation.segment_id);
	ASSERT_EQ(sharedManifestPool.owner_id, RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID);
	layout.release();

	MetadataSnapshotPin pinned = metadata->openSnapshot();
	EXPECT_EQ(values.read(pinned, live.reference), std::vector<std::byte>(liveBytes.begin(), liveBytes.end()));
	EXPECT_EQ(values.read(pinned, orphan.reference), std::vector<std::byte>(orphanBytes.begin(), orphanBytes.end()));
	EXPECT_EQ(metadata->findEntityPropertyLocation(pinned, property.key), property);

	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	const LargeValueReachabilityReclaimReport report =
			reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata));
	EXPECT_EQ(report.reachable_values, 1U);
	EXPECT_EQ(report.retired_values, 1U);
	EXPECT_EQ(report.retired_manifest_pages, 1U);
	EXPECT_EQ(report.retired_data_pages, 1U);
	EXPECT_EQ(report.retained_unregistered_values, 0U);
	EXPECT_EQ(report.pools_scanned, 2U);
	EXPECT_EQ(report.empty_pools_retired, 0U);
	EXPECT_EQ(report.retired_pools_released, 0U);
	EXPECT_GT(report.published_generation, report.scanned_generation);
	MetadataSnapshotPin retired = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(retired, sharedDataPool.segment_id).state, SegmentState::SGS_SEALED);
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(retired, sharedDataPool.physical_slot));
	EXPECT_EQ(metadata->findCatalog(retired, sharedManifestPool.segment_id).state, SegmentState::SGS_SEALED);
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(retired, sharedManifestPool.physical_slot));
	retired.release();

	// The old Page and Manifest directories remain pinned until this snapshot drains.
	EXPECT_EQ(values.read(pinned, orphan.reference), std::vector<std::byte>(orphanBytes.begin(), orphanBytes.end()));
	pinned.release();

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_TRUE(metadata->findLargeValueManifestOptional(current, live.reference.value_id).has_value());
	EXPECT_FALSE(metadata->findLargeValueManifestOptional(current, orphan.reference.value_id).has_value());
	EXPECT_EQ(values.read(current, live.reference), std::vector<std::byte>(liveBytes.begin(), liveBytes.end()));
	expectFormatError([&] { static_cast<void>(values.read(current, orphan.reference)); },
					  FormatError::FERR_INVALID_STATE);
	const BTreeIndex contentIndex(RSS_LARGE_VALUE_CONTENT_INDEX_ID, values.contentIndex(), *metadata);
	EXPECT_TRUE(contentIndex.find(current, values.contentIndexKey(live.reference)).has_value());
	EXPECT_FALSE(contentIndex.find(current, values.contentIndexKey(orphan.reference)).has_value());
	current.release();
	const LargeValueReachabilityReclaimReport releaseReport =
			reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata));
	EXPECT_EQ(releaseReport.retired_values, 0U);
	EXPECT_EQ(releaseReport.empty_pools_retired, 0U);
	EXPECT_EQ(releaseReport.retired_pools_released, 0U);
	MetadataSnapshotPin released = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(released, sharedDataPool.segment_id).state, SegmentState::SGS_SEALED);
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(released, sharedDataPool.physical_slot));
	EXPECT_EQ(metadata->findCatalog(released, sharedManifestPool.segment_id).state, SegmentState::SGS_SEALED);
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(released, sharedManifestPool.physical_slot));
	released.release();

	const LargeValueWriteResult reused =
			values.write(orphanBytes, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	EXPECT_NE(reused.reference, orphan.reference);
	EXPECT_EQ(reused.reference.value_id, LargeValueId(3U));
	MetadataSnapshotPin reusedSnapshot = metadata->openSnapshot();
	const PageLocation reusedDataLocation =
			metadata->findPageLocation(reusedSnapshot, reused.manifest.chunks.front().first_data_page_id);
	const PageLocation reusedManifestLocation = metadata->findPageLocation(reusedSnapshot, reused.manifest.page_id);
	EXPECT_EQ(reusedDataLocation.segment_id, sharedDataPool.segment_id);
	EXPECT_EQ(reusedDataLocation.page_index, orphanDataLocation.page_index);
	EXPECT_EQ(reusedManifestLocation.segment_id, sharedManifestPool.segment_id);
	EXPECT_EQ(reusedManifestLocation.page_index, orphanManifestLocation.page_index);
	EXPECT_EQ(values.read(reusedSnapshot, live.reference), std::vector<std::byte>(liveBytes.begin(), liveBytes.end()));
	EXPECT_EQ(values.read(reusedSnapshot, reused.reference),
			  std::vector<std::byte>(orphanBytes.begin(), orphanBytes.end()));
}

TEST_F(RelocatableMetadataStoreTest, LargeValueMaintenanceJournalAdvancesOnlyThroughCompleteOwnerEvidence) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const LargeValueWriteResult live =
			values.write(std::array{std::byte{0x11}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	const LargeValueWriteResult orphan =
			values.write(std::array{std::byte{0x22}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	static_cast<void>(publishEntityPropertyReference(*metadata, propertyPages, live.reference));
	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	LargeValueMaintenanceJournal journal;
	journal.scan_generation = snapshot.superblock().generation;

	LargeValueMaintenanceJournalAdvance manifests = reclaimer.advanceJournal(snapshot, journal, reclaimOptions());
	EXPECT_EQ(manifests.journal.phase, LargeValueMaintenanceJournalPhase::LVMJP_SCAN_ENTITY_PROPERTIES);
	EXPECT_EQ(manifests.journal.candidate_value_ids,
			  (std::vector<LargeValueId>{live.reference.value_id, orphan.reference.value_id}));
	EXPECT_TRUE(manifests.journal.reachable_value_ids.empty());

	LargeValueMaintenanceJournalAdvance properties =
			reclaimer.advanceJournal(snapshot, manifests.journal, reclaimOptions());
	EXPECT_EQ(properties.journal.phase, LargeValueMaintenanceJournalPhase::LVMJP_SCAN_STATES);
	EXPECT_EQ(properties.journal.reachable_value_ids, (std::vector<LargeValueId>{live.reference.value_id}));
	EXPECT_GT(properties.owner_locations_scanned, 0U);

	LargeValueMaintenanceJournalAdvance states = reclaimer.advanceJournal(snapshot, properties.journal, reclaimOptions());
	EXPECT_EQ(states.journal.phase, LargeValueMaintenanceJournalPhase::LVMJP_RECLAIM);
	EXPECT_EQ(states.journal.reachable_value_ids, (std::vector<LargeValueId>{live.reference.value_id}));

	LargeValueMaintenanceJournal stale = states.journal;
	stale.scan_generation = Generation(stale.scan_generation.value() + 1U);
	expectFormatError([&] { static_cast<void>(reclaimer.advanceJournal(snapshot, stale, reclaimOptions())); },
					  FormatError::FERR_INVALID_STATE);
	// Journal advancement records evidence only; it must not alter either route.
	EXPECT_TRUE(metadata->findLargeValueManifestOptional(snapshot, live.reference.value_id).has_value());
	EXPECT_TRUE(metadata->findLargeValueManifestOptional(snapshot, orphan.reference.value_id).has_value());
}

TEST_F(RelocatableMetadataStoreTest, LargeValueMaintenanceJournalReclaimsOnlyCandidatesAfterCompleteOwnerEvidence) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const LargeValueWriteResult live =
			values.write(std::array{std::byte{0x41}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	const LargeValueWriteResult orphan =
			values.write(std::array{std::byte{0x42}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	static_cast<void>(publishEntityPropertyReference(*metadata, propertyPages, live.reference));
	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	LargeValueMaintenanceJournal journal;
	journal.scan_generation = snapshot.superblock().generation;
	journal = reclaimer.advanceJournal(snapshot, journal, reclaimOptions()).journal;
	journal = reclaimer.advanceJournal(snapshot, journal, reclaimOptions()).journal;
	journal = reclaimer.advanceJournal(snapshot, journal, reclaimOptions()).journal;
	ASSERT_EQ(journal.phase, LargeValueMaintenanceJournalPhase::LVMJP_RECLAIM);

	const LargeValueReachabilityReclaimReport report =
			reclaimer.reclaimJournal(snapshot, journal, reclaimOptions(), nextCheckpointLsn(*metadata));
	EXPECT_EQ(report.scanned_generation, journal.scan_generation);
	EXPECT_EQ(report.reachable_values, 1U);
	EXPECT_EQ(report.retired_values, 1U);
	EXPECT_EQ(report.retired_manifest_pages, 1U);
	EXPECT_EQ(report.retired_data_pages, 1U);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_TRUE(metadata->findLargeValueManifestOptional(current, live.reference.value_id).has_value());
	EXPECT_FALSE(metadata->findLargeValueManifestOptional(current, orphan.reference.value_id).has_value());
	EXPECT_EQ(values.read(current, live.reference), std::vector<std::byte>{std::byte{0x41}});
	current.release();
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRetainsStateValuesThroughPinnedManifestReferences) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	const std::array liveBytes{std::byte{0x31}, std::byte{0x32}};
	const std::array orphanBytes{std::byte{0x41}, std::byte{0x42}, std::byte{0x43}};
	const LargeValueWriteResult live = values.write(liveBytes, stateValueWriteOptions(), nextCheckpointLsn(*metadata));
	const LargeValueWriteResult orphan =
			values.write(orphanBytes, stateValueWriteOptions(), nextCheckpointLsn(*metadata));
	StateRecordStore states(pages, *metadata);
	static_cast<void>(
			states.upsert({7U, "state.external", StateRecordStorageKind::SRS_LARGE_VALUE, live.reference.value_id, {}},
						  stateRecordStoreOptions()));
	static_cast<void>(
			states.upsert({8U, "state.inline", StateRecordStorageKind::SRS_INLINE, LargeValueId{}, {std::byte{0x55}}},
						  stateRecordStoreOptions()));

	MetadataSnapshotPin pinned = metadata->openSnapshot();
	EXPECT_EQ(values.read(pinned, orphan.reference), std::vector<std::byte>(orphanBytes.begin(), orphanBytes.end()));
	LargeValueReachabilityReclaimer reclaimer(values, pages, *metadata);
	const LargeValueReachabilityReclaimReport report =
			reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata));
	EXPECT_EQ(report.owner_locations_scanned, 2U);
	EXPECT_EQ(report.reachable_values, 1U);
	EXPECT_EQ(report.retired_values, 1U);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(states.read(current, 7U).large_value_id, live.reference.value_id);
	EXPECT_EQ(values.read(current, live.reference), std::vector<std::byte>(liveBytes.begin(), liveBytes.end()));
	EXPECT_FALSE(metadata->findLargeValueManifestOptional(current, orphan.reference.value_id).has_value());
	current.release();
	EXPECT_EQ(values.read(pinned, orphan.reference), std::vector<std::byte>(orphanBytes.begin(), orphanBytes.end()));
	pinned.release();
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()));
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsStateReferencesWithAnotherOwnerMediaType) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	const LargeValueWriteResult propertyValue =
			values.write(std::array{std::byte{0x61}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	StateRecordStore states(pages, *metadata);
	static_cast<void>(states.upsert(
			{9U, "state.invalid", StateRecordStorageKind::SRS_LARGE_VALUE, propertyValue.reference.value_id, {}},
			stateRecordStoreOptions()));
	LargeValueReachabilityReclaimer reclaimer(values, pages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_TRUE(metadata->findLargeValueManifestOptional(snapshot, propertyValue.reference.value_id).has_value());
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsStateRoutesWithMismatchedRecordIdentity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	StateRecordStore states(pages, *metadata);
	const StateLocation stored =
			states.upsert({7U, "state.inline", StateRecordStorageKind::SRS_INLINE, LargeValueId{}, {std::byte{0x55}}},
						  stateRecordStoreOptions());
	const StateLocation mismatched{8U, stored.page_id, stored.slot_id, stored.slot_generation};
	static_cast<void>(metadata->upsertStateLocations(std::span(&mismatched, 1U), {}, nextCheckpointLsn(*metadata)));

	LargeValueReachabilityReclaimer reclaimer(values, pages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsStateReferencesWithAnInvalidValueKind) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	LargeValueWriteOptions options = stateValueWriteOptions();
	options.kind = LargeValueKind::LVK_UTF8_TEXT;
	const LargeValueWriteResult text = values.write(std::array{std::byte{0x61}}, options, nextCheckpointLsn(*metadata));
	StateRecordStore states(pages, *metadata);
	static_cast<void>(
			states.upsert({7U, "state.text", StateRecordStorageKind::SRS_LARGE_VALUE, text.reference.value_id, {}},
						  stateRecordStoreOptions()));

	LargeValueReachabilityReclaimer reclaimer(values, pages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerAppliesTheSharedOwnerLocationBoundToStateRecords) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	StateRecordStore states(pages, *metadata);
	static_cast<void>(
			states.upsert({7U, "state.first", StateRecordStorageKind::SRS_INLINE, LargeValueId{}, {std::byte{0x55}}},
						  stateRecordStoreOptions()));
	static_cast<void>(
			states.upsert({8U, "state.second", StateRecordStorageKind::SRS_INLINE, LargeValueId{}, {std::byte{0x56}}},
						  stateRecordStoreOptions()));
	LargeValueReachabilityReclaimOptions options = reclaimOptions();
	options.maximum_owner_locations = 1U;
	options.maximum_owner_metadata_pages = 32U;
	LargeValueReachabilityReclaimer reclaimer(values, pages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(options, nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsDataPoolsOutsideTheSharedNamespace) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	static_cast<void>(
			values.write(std::array{std::byte{0x61}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata)));
	const LargeValueWriteResult written =
			values.write(std::array{std::byte{0x62}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	MetadataSnapshotPin before = metadata->openSnapshot();
	const Generation poolGeneration = before.superblock().generation;
	before.release();

	const PageId pageId(900001U);
	const LogSequenceNumber checkpointLsn = nextCheckpointLsn(*metadata);
	const PageOwnerHeader owner{pageId,		  written.reference.value_id.value(), PageOwnerKind::POK_LARGE_VALUE_DATA,
								0U,			  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,	  poolGeneration,
								checkpointLsn};
	const LargeValueDataPage data{written.reference.value_id, pageId, written.reference.generation, 0U, 0U, 1U, 1U, 0U,
								  {std::byte{0xA5U}}};
	SharedPagePoolAllocationRequest request;
	request.segment_id = SegmentId(900001U);
	request.pool_kind = SegmentKind::SGK_LARGE_VALUE_DATA_POOL;
	request.pool_owner_id = written.reference.value_id.value();
	request.generation = poolGeneration;
	request.page_id = pageId;
	request.page_owner_id = written.reference.value_id.value();
	request.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_DATA;
	request.payload = LargeValueDataFrame::create(owner, data).serialize();
	SharedPagePoolAllocator pools(io, *metadata);
	ASSERT_TRUE(
			pools.allocateNewPoolPage(request, PhysicalSlotId(101U), PhysicalSlotId(101U), checkpointLsn).has_value());
	MetadataSnapshotPin afterAllocation = metadata->openSnapshot();
	const CatalogEntry allocatedPool = metadata->findCatalog(afterAllocation, request.segment_id);
	EXPECT_EQ(allocatedPool.kind, SegmentKind::SGK_LARGE_VALUE_DATA_POOL);
	EXPECT_EQ(allocatedPool.owner_id, written.reference.value_id.value());
	afterAllocation.release();

	LargeValueReachabilityReclaimer reclaimer(values, pages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsManifestPoolsOutsideTheSharedNamespace) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	const LargeValueWriteResult written =
			values.write(std::array{std::byte{0x71}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	MetadataSnapshotPin before = metadata->openSnapshot();
	const Generation poolGeneration = before.superblock().generation;
	before.release();

	LargeValueManifest orphan = written.manifest;
	orphan.page_id = PageId(900002U);
	const LogSequenceNumber checkpointLsn = nextCheckpointLsn(*metadata);
	const PageOwnerHeader owner{orphan.page_id,
								written.reference.value_id.value(),
								PageOwnerKind::POK_LARGE_VALUE_MANIFEST,
								0U,
								RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								poolGeneration,
								checkpointLsn};
	SharedPagePoolAllocationRequest request;
	request.segment_id = SegmentId(900002U);
	request.pool_kind = SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL;
	request.pool_owner_id = written.reference.value_id.value();
	request.generation = poolGeneration;
	request.page_id = orphan.page_id;
	request.page_owner_id = written.reference.value_id.value();
	request.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
	request.payload = LargeValueManifestPage::create(owner, orphan).serialize();
	SharedPagePoolAllocator pools(io, *metadata);
	ASSERT_TRUE(
			pools.allocateNewPoolPage(request, PhysicalSlotId(102U), PhysicalSlotId(102U), checkpointLsn).has_value());
	MetadataSnapshotPin afterAllocation = metadata->openSnapshot();
	const CatalogEntry allocatedPool = metadata->findCatalog(afterAllocation, request.segment_id);
	EXPECT_EQ(allocatedPool.owner_id, written.reference.value_id.value());
	afterAllocation.release();

	LargeValueReachabilityReclaimer reclaimer(values, pages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsSealedLargeValuePoolsWithoutAnOwner) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const LargeValueWriteResult orphan =
			values.write(std::array{std::byte{0x63}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const PageLocation manifestLocation = metadata->findPageLocation(snapshot, orphan.manifest.page_id);
	CatalogEntry manifestPool = metadata->findCatalog(snapshot, manifestLocation.segment_id);
	snapshot.release();
	ASSERT_EQ(manifestPool.kind, SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL);
	ASSERT_EQ(manifestPool.state, SegmentState::SGS_SEALED);
	manifestPool.owner_id = 0U;
	const LogSequenceNumber corruptionLsn = nextCheckpointLsn(*metadata);
	manifestPool.committed_lsn = corruptionLsn;
	static_cast<void>(metadata->upsertCatalog(std::span(&manifestPool, 1U), corruptionLsn));

	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin after = metadata->openSnapshot();
	EXPECT_TRUE(metadata->findLargeValueManifestOptional(after, orphan.reference.value_id).has_value());
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsRetiredLargeValuePoolsWithoutAnOwner) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const LargeValueWriteResult orphan =
			values.write(std::array{std::byte{0x64}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	MetadataSnapshotPin layout = metadata->openSnapshot();
	const PageLocation manifestLocation = metadata->findPageLocation(layout, orphan.manifest.page_id);
	const SegmentId manifestSegmentId = manifestLocation.segment_id;
	layout.release();
	MetadataSnapshotPin retained = metadata->openSnapshot();
	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	CatalogEntry manifestPool = metadata->findCatalog(snapshot, manifestSegmentId);
	snapshot.release();
	ASSERT_EQ(manifestPool.kind, SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL);
	ASSERT_EQ(manifestPool.state, SegmentState::SGS_RETIRED);
	manifestPool.owner_id = 0U;
	const LogSequenceNumber corruptionLsn = nextCheckpointLsn(*metadata);
	manifestPool.committed_lsn = corruptionLsn;
	static_cast<void>(metadata->upsertCatalog(std::span(&manifestPool, 1U), corruptionLsn));

	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
	retained.release();
}

TEST_F(RelocatableMetadataStoreTest,
	   LargeValueReclaimerRetiresSharedDataPoolOnlyAfterItsLastCurrentValueBecomesUnreachable) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const std::array bytes{std::byte{0xCC}, std::byte{0xDD}, std::byte{0xEE}};
	const LargeValueWriteResult value = values.write(bytes, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	const EntityPropertyLocation property = publishEntityPropertyReference(*metadata, propertyPages, value.reference);
	MetadataSnapshotPin layout = metadata->openSnapshot();
	const PageLocation dataLocation =
			metadata->findPageLocation(layout, value.manifest.chunks.front().first_data_page_id);
	const PageLocation manifestLocation = metadata->findPageLocation(layout, value.manifest.page_id);
	const CatalogEntry dataPool = metadata->findCatalog(layout, dataLocation.segment_id);
	const CatalogEntry manifestPool = metadata->findCatalog(layout, manifestLocation.segment_id);
	ASSERT_EQ(dataPool.owner_id, RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID);
	ASSERT_EQ(manifestPool.owner_id, RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID);
	layout.release();

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	static_cast<void>(
			metadata->upsertEntityPropertyLocations({}, std::span(&property.key, 1U), nextCheckpointLsn(*metadata)));
	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	const LargeValueReachabilityReclaimReport retirement =
			reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata));
	EXPECT_EQ(retirement.retired_values, 1U);
	EXPECT_EQ(retirement.empty_pools_retired, 2U);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(current, dataPool.segment_id).state, SegmentState::SGS_RETIRED);
	EXPECT_EQ(metadata->findCatalog(current, manifestPool.segment_id).state, SegmentState::SGS_RETIRED);
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(current, dataPool.physical_slot));
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(current, manifestPool.physical_slot));
	current.release();
	LargeValueReachabilityReclaimOptions tooFewPools = reclaimOptions();
	tooFewPools.maximum_pools = 1U;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(tooFewPools, nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(values.read(oldSnapshot, value.reference), std::vector<std::byte>(bytes.begin(), bytes.end()));
	oldSnapshot.release();

	const LargeValueReachabilityReclaimReport release =
			reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata));
	EXPECT_EQ(release.retired_pools_released, 2U);
	MetadataSnapshotPin released = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(released, dataPool.segment_id).state, SegmentState::SGS_OBSOLETE);
	EXPECT_EQ(metadata->findCatalog(released, manifestPool.segment_id).state, SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(released, dataPool.physical_slot));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(released, manifestPool.physical_slot));
	released.release();
	const LargeValueReachabilityReclaimReport afterRelease =
			reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata));
	EXPECT_EQ(afterRelease.retired_values, 0U);
	EXPECT_EQ(afterRelease.pools_scanned, 0U);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsCurrentPoolsBeyondItsBoundBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const LargeValueWriteResult orphan =
			values.write(std::array{std::byte{0xA1}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	LargeValueReachabilityReclaimOptions tooFewPools = reclaimOptions();
	tooFewPools.maximum_pools = 1U;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(tooFewPools, nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_TRUE(metadata->findLargeValueManifestOptional(snapshot, orphan.reference.value_id).has_value());
	EXPECT_EQ(values.read(snapshot, orphan.reference), std::vector<std::byte>({std::byte{0xA1}}));
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsOverlappingManifestDataPageRanges) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	LargeValueWriteOptions options = propertyValueWriteOptions();
	options.compression.codec = LargeValueCodec::LVC_RAW;
	options.extent_class = LargeValueExtentClass::LVEC_16K;
	const std::vector<std::byte> bytes(2U * RSS_LARGE_VALUE_EXTENT_16K_BYTES, std::byte{0xA1});
	const LargeValueWriteResult written = values.write(bytes, options, nextCheckpointLsn(*metadata));
	ASSERT_GE(written.manifest.chunks.size(), 2U);
	LargeValueManifest corrupt = written.manifest;
	corrupt.chunks[1].first_data_page_id = corrupt.chunks[0].first_data_page_id;
	overwriteLargeValueManifest(io, *metadata, corrupt);

	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsInsufficientCheckpointLsnRangeBeforePublication) {
	constexpr uint64_t maximumLsn = std::numeric_limits<uint64_t>::max();
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, maximumLsn - 2U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const LargeValueWriteResult orphan =
			values.write(std::array{std::byte{0xA1}}, propertyValueWriteOptions(), LogSequenceNumber(maximumLsn - 1U));
	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), LogSequenceNumber(maximumLsn))); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(snapshot.superblock().checkpoint_lsn, LogSequenceNumber(maximumLsn - 1U));
	EXPECT_TRUE(metadata->findLargeValueManifestOptional(snapshot, orphan.reference.value_id).has_value());
	EXPECT_EQ(values.read(snapshot, orphan.reference), std::vector<std::byte>({std::byte{0xA1}}));
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerPublishesContentIndexDeletionAtTheSuperblockBoundary) {
	const std::array orphanBytes{std::byte{0xC1}, std::byte{0xC2}, std::byte{0xC3}};
	LargeValueRef reference;
	{
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		SlottedPagePool propertyPages(io, *metadata);
		const LargeValueWriteResult written =
				values.write(orphanBytes, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
		reference = written.reference;
		LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
		EXPECT_THROW(static_cast<void>(reclaimer.reclaim(
							 reclaimOptions(), nextCheckpointLsn(*metadata),
							 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
					 BootstrapPublishInterrupted);
		MetadataSnapshotPin before = metadata->openSnapshot();
		const BTreeIndex contentIndex(RSS_LARGE_VALUE_CONTENT_INDEX_ID, values.contentIndex(), *metadata);
		EXPECT_TRUE(metadata->findLargeValueManifestOptional(before, reference.value_id).has_value());
		EXPECT_TRUE(contentIndex.find(before, values.contentIndexKey(reference)).has_value());
		before.release();
	}

	{
		auto metadata = RelocatableMetadataStore::open(io);
		LargeValueStore values(io, *metadata);
		SlottedPagePool propertyPages(io, *metadata);
		LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
		EXPECT_THROW(static_cast<void>(reclaimer.reclaim(
							 reclaimOptions(), nextCheckpointLsn(*metadata),
							 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
					 BootstrapPublishInterrupted);
	}

	auto recovered = RelocatableMetadataStore::open(io);
	LargeValueStore values(io, *recovered);
	MetadataSnapshotPin after = recovered->openSnapshot();
	const BTreeIndex contentIndex(RSS_LARGE_VALUE_CONTENT_INDEX_ID, values.contentIndex(), *recovered);
	EXPECT_FALSE(recovered->findLargeValueManifestOptional(after, reference.value_id).has_value());
	EXPECT_FALSE(contentIndex.find(after, values.contentIndexKey(reference)).has_value());
	after.release();
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *recovered).scan()));
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerUsesIndependentRouteAndManifestNodeBoundsForMultiLevelValues) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	LargeValueWriteOptions options = propertyValueWriteOptions();
	options.extent_class = LargeValueExtentClass::LVEC_16K;
	const size_t logicalBytes =
			(static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) + 1U) * RSS_LARGE_VALUE_EXTENT_16K_BYTES + 19U;
	const std::vector<std::byte> bytes(logicalBytes, std::byte{0x5AU});
	const LargeValueWriteResult orphan = values.write(bytes, options, nextCheckpointLsn(*metadata));
	ASSERT_EQ(orphan.manifest.node_level, 1U);
	ASSERT_EQ(orphan.manifest.children.size(), 2U);

	LargeValueReachabilityReclaimOptions limits = reclaimOptions();
	limits.maximum_manifest_locations = 1U;
	limits.maximum_manifest_nodes = 3U;
	limits.maximum_data_pages = values.plannedDataPageCount(bytes, options.compression, options.extent_class);
	limits.maximum_retirements = limits.maximum_manifest_nodes + limits.maximum_data_pages;
	// Shared-pool retirement must prove that no current Page Directory route
	// survives in the candidate pools. The directory also contains the content
	// index route, so its independent bound covers the complete directory rather
	// than only this value's Manifest and data pages.
	size_t pageLocationCount = 0U;
	MetadataSnapshotPin beforeReclaim = metadata->openSnapshot();
	static_cast<void>(metadata->visitPageLocationsUntil(beforeReclaim, [&](const PageLocation &) {
		++pageLocationCount;
		return true;
	}));
	beforeReclaim.release();
	ASSERT_GT(pageLocationCount, limits.maximum_manifest_nodes + limits.maximum_data_pages);
	limits.maximum_page_locations = pageLocationCount;
	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	LargeValueReachabilityReclaimOptions insufficientNodes = limits;
	insufficientNodes.maximum_manifest_nodes = limits.maximum_manifest_nodes - 1U;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(insufficientNodes, nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
	const LargeValueReachabilityReclaimReport report = reclaimer.reclaim(limits, nextCheckpointLsn(*metadata));
	EXPECT_EQ(report.manifest_locations_scanned, 1U);
	EXPECT_EQ(report.manifest_nodes_scanned, 3U);
	EXPECT_EQ(report.page_locations_scanned, pageLocationCount);
	EXPECT_EQ(report.retired_values, 1U);
	EXPECT_EQ(report.retired_manifest_pages, 3U);
	EXPECT_EQ(report.retired_data_pages, limits.maximum_data_pages);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRetainsUnclassifiedMediaAndRejectsZeroBounds) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	LargeValueWriteOptions options = propertyValueWriteOptions();
	options.media_type = "application/octet-stream";
	const std::array payload{std::byte{0x44}};
	const LargeValueWriteResult opaque = values.write(payload, options, nextCheckpointLsn(*metadata));
	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);

	LargeValueReachabilityReclaimOptions invalid = reclaimOptions();
	invalid.maximum_manifest_nodes = 0U;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(invalid, nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = reclaimOptions();
	invalid.maximum_data_pages = 0U;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(invalid, nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = reclaimOptions();
	invalid.maximum_page_locations = 0U;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(invalid, nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = reclaimOptions();
	invalid.maximum_page_locations = 1U;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(invalid, nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
	const LargeValueReachabilityReclaimReport report =
			reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata));
	EXPECT_EQ(report.retained_unregistered_values, 1U);
	EXPECT_EQ(report.retired_values, 0U);
	EXPECT_EQ(report.published_generation, report.scanned_generation);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(values.read(snapshot, opaque.reference), std::vector<std::byte>(payload.begin(), payload.end()));
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerUsesRegisteredTypedMediaOwnershipScanner) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	LargeValueWriteOptions options = propertyValueWriteOptions();
	options.media_type = "application/vnd.zyx.test-attachment";
	const std::array payload{std::byte{0x1A}, std::byte{0x2B}};
	const LargeValueWriteResult value = values.write(payload, options, nextCheckpointLsn(*metadata));
	const LargeValueOwnershipScannerPtr scanner =
			std::make_shared<TestLargeValueOwnershipScanner>(options.media_type, value.reference);
	const std::array<LargeValueOwnershipScannerPtr, 1U> scanners{{scanner}};
	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata,
											  std::span<const LargeValueOwnershipScannerPtr>(scanners));

	const LargeValueReachabilityReclaimReport report =
			reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata));
	EXPECT_EQ(report.owner_locations_scanned, 1U);
	EXPECT_EQ(report.reachable_values, 1U);
	EXPECT_EQ(report.retained_unregistered_values, 0U);
	EXPECT_EQ(report.retired_values, 0U);
	EXPECT_EQ(report.published_generation, report.scanned_generation);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(values.read(snapshot, value.reference), std::vector<std::byte>(payload.begin(), payload.end()));
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsDuplicateTypedMediaOwnershipScanners) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const LargeValueOwnershipScannerPtr duplicate = std::make_shared<TestLargeValueOwnershipScanner>(
			RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE, LargeValueRef{});
	const std::array<LargeValueOwnershipScannerPtr, 1U> scanners{{duplicate}};
	expectFormatError(
			[&] {
				static_cast<void>(LargeValueReachabilityReclaimer(
						values, propertyPages, *metadata, std::span<const LargeValueOwnershipScannerPtr>(scanners)));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsMalformedOwnershipScannersBeforeScanningMetadata) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const auto expectInvalidScanner = [&](LargeValueOwnershipScannerPtr scanner) {
		const std::array<LargeValueOwnershipScannerPtr, 1U> scanners{{std::move(scanner)}};
		expectFormatError(
				[&] {
					static_cast<void>(
							LargeValueReachabilityReclaimer(values, propertyPages, *metadata,
															std::span<const LargeValueOwnershipScannerPtr>(scanners)));
				},
				FormatError::FERR_INVALID_HEADER);
	};

	expectInvalidScanner({});
	expectInvalidScanner(std::make_shared<ConfigurableLargeValueOwnershipScanner>("", LargeValueRef{}, 0U, 0U));
	expectInvalidScanner(std::make_shared<ConfigurableLargeValueOwnershipScanner>(
			std::string(RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES + 1U, 'a'), LargeValueRef{}, 0U, 0U));
	expectInvalidScanner(std::make_shared<ConfigurableLargeValueOwnershipScanner>("application/vnd.zyx\x01invalid",
																				  LargeValueRef{}, 0U, 0U));
	expectInvalidScanner(std::make_shared<ConfigurableLargeValueOwnershipScanner>("application/vnd.zyx\x7Finvalid",
																				  LargeValueRef{}, 0U, 0U));
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsStoresFromAnotherMetadataCoordinator) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	auto otherMetadata = RelocatableMetadataStore::open(io);
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	SlottedPagePool foreignPages(io, *otherMetadata);

	EXPECT_THROW(static_cast<void>(LargeValueReachabilityReclaimer(values, propertyPages, *otherMetadata)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(LargeValueReachabilityReclaimer(values, foreignPages, *metadata)),
				 std::invalid_argument);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsUntrustedScannerClaimsAcrossMediaAndBounds) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	LargeValueWriteOptions firstOptions = propertyValueWriteOptions();
	firstOptions.media_type = "application/vnd.zyx.first-owner";
	LargeValueWriteOptions secondOptions = propertyValueWriteOptions();
	secondOptions.media_type = "application/vnd.zyx.second-owner";
	const LargeValueWriteResult first =
			values.write(std::array{std::byte{0x11}}, firstOptions, nextCheckpointLsn(*metadata));
	const LargeValueWriteResult second =
			values.write(std::array{std::byte{0x22}}, secondOptions, nextCheckpointLsn(*metadata));

	const LargeValueOwnershipScannerPtr crossMedia =
			std::make_shared<ConfigurableLargeValueOwnershipScanner>(firstOptions.media_type, second.reference, 1U, 1U);
	const LargeValueOwnershipScannerPtr secondOwner = std::make_shared<ConfigurableLargeValueOwnershipScanner>(
			secondOptions.media_type, second.reference, 0U, 0U);
	const std::array<LargeValueOwnershipScannerPtr, 2U> crossMediaScanners{{crossMedia, secondOwner}};
	LargeValueReachabilityReclaimer crossMediaReclaimer(
			values, propertyPages, *metadata, std::span<const LargeValueOwnershipScannerPtr>(crossMediaScanners));
	expectFormatError(
			[&] { static_cast<void>(crossMediaReclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
			FormatError::FERR_INVALID_STATE);

	const LargeValueOwnershipScannerPtr absentReference =
			std::make_shared<ConfigurableLargeValueOwnershipScanner>(firstOptions.media_type,
																	 LargeValueRef{LargeValueId(999999U),
																				   Generation(1U),
																				   1U,
																				   {},
																				   LargeValueKind::LVK_BYTES,
																				   LargeValueCodec::LVC_RAW,
																				   firstOptions.media_type},
																	 1U, 1U);
	const std::array<LargeValueOwnershipScannerPtr, 1U> absentReferenceScanners{{absentReference}};
	LargeValueReachabilityReclaimer absentReferenceReclaimer(
			values, propertyPages, *metadata, std::span<const LargeValueOwnershipScannerPtr>(absentReferenceScanners));
	expectFormatError(
			[&] {
				static_cast<void>(absentReferenceReclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata)));
			},
			FormatError::FERR_INVALID_STATE);

	LargeValueRef staleReference = first.reference;
	staleReference.generation = Generation(first.reference.generation.value() + 1U);
	const LargeValueOwnershipScannerPtr staleReferenceScanner =
			std::make_shared<ConfigurableLargeValueOwnershipScanner>(firstOptions.media_type, staleReference, 1U, 1U);
	const std::array<LargeValueOwnershipScannerPtr, 1U> staleReferenceScanners{{staleReferenceScanner}};
	LargeValueReachabilityReclaimer staleReferenceReclaimer(
			values, propertyPages, *metadata, std::span<const LargeValueOwnershipScannerPtr>(staleReferenceScanners));
	expectFormatError(
			[&] { static_cast<void>(staleReferenceReclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
			FormatError::FERR_INVALID_STATE);

	LargeValueReachabilityReclaimOptions ownerBound = reclaimOptions();
	ownerBound.maximum_owner_locations = 1U;
	ownerBound.maximum_owner_metadata_pages = 32U;
	const LargeValueOwnershipScannerPtr oversizedResult = std::make_shared<ConfigurableLargeValueOwnershipScanner>(
			firstOptions.media_type, first.reference, 0U, ownerBound.maximum_owner_locations + 1U);
	const std::array<LargeValueOwnershipScannerPtr, 1U> oversizedResultScanners{{oversizedResult}};
	LargeValueReachabilityReclaimer oversizedResultReclaimer(
			values, propertyPages, *metadata, std::span<const LargeValueOwnershipScannerPtr>(oversizedResultScanners));
	expectFormatError(
			[&] { static_cast<void>(oversizedResultReclaimer.reclaim(ownerBound, nextCheckpointLsn(*metadata))); },
			FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(values.read(snapshot, first.reference), std::vector<std::byte>({std::byte{0x11}}));
	EXPECT_EQ(values.read(snapshot, second.reference), std::vector<std::byte>({std::byte{0x22}}));
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerScansInlineEntityPropertyRootsForBothEntityKinds) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const EntityPropertyKey nodeKey{EntityPropertyOwnerKind::EPOK_NODE, 71U};
	const EntityPropertyKey edgeKey{EntityPropertyOwnerKind::EPOK_EDGE, 72U};
	const PropertyCollectionRoot nodeRoot{nodeKey.entity_id, graph::Node::typeId, {}};
	const PropertyCollectionRoot edgeRoot{edgeKey.entity_id, graph::Edge::typeId, {}};
	const std::vector<std::byte> nodePayload = EntityPropertyCollectionRootCodec::encode(nodeRoot);
	const std::vector<std::byte> edgePayload = EntityPropertyCollectionRootCodec::encode(edgeRoot);
	static_cast<void>(publishEntityPropertyPayload(*metadata, propertyPages, nodeKey, nodePayload));
	static_cast<void>(publishEntityPropertyPayload(*metadata, propertyPages, edgeKey, edgePayload));

	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	const LargeValueReachabilityReclaimReport report =
			reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata));
	EXPECT_EQ(report.owner_locations_scanned, 2U);
	EXPECT_EQ(report.reachable_values, 0U);
	EXPECT_EQ(report.retired_values, 0U);
	EXPECT_EQ(report.published_generation, report.scanned_generation);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsEntityPropertyRoutesWithMismatchedOwners) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const EntityPropertyKey rootKey{EntityPropertyOwnerKind::EPOK_NODE, 71U};
	const PropertyCollectionRoot mismatchedRoot{rootKey.entity_id + 1U, graph::Node::typeId, {}};
	const std::vector<std::byte> rootPayload = EntityPropertyCollectionRootCodec::encode(mismatchedRoot);
	static_cast<void>(publishEntityPropertyPayload(*metadata, propertyPages, rootKey, rootPayload));

	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsEntityPropertyRootsWithMismatchedOwnerType) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const EntityPropertyKey rootKey{EntityPropertyOwnerKind::EPOK_NODE, 71U};
	const PropertyCollectionRoot mismatchedRoot{rootKey.entity_id, graph::Edge::typeId, {}};
	static_cast<void>(publishEntityPropertyPayload(*metadata, propertyPages, rootKey,
												   EntityPropertyCollectionRootCodec::encode(mismatchedRoot)));

	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsEntityPropertyRouteOutsidePropertyPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 71U};
	const PropertyCollectionRoot root{key.entity_id, graph::Node::typeId, {}};
	const EntityPropertyLocation location = publishEntityPropertyPayload(
			*metadata, propertyPages, key, EntityPropertyCollectionRootCodec::encode(root));
	const PageOwnerHeader foreignOwner{location.page_id,
									   71U,
									   PageOwnerKind::POK_STATE,
									   0U,
									   RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									   Generation(1U),
									   nextCheckpointLsn(*metadata)};
	overwritePageOwner(io, *metadata, location.page_id, foreignOwner);

	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsStateRouteOutsideStatePage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	StateRecordStore states(pages, *metadata);
	static_cast<void>(
			states.upsert({7U, "state.inline", StateRecordStorageKind::SRS_INLINE, LargeValueId{}, {std::byte{0x55}}},
						  stateRecordStoreOptions()));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const StateLocation location = metadata->findStateLocation(snapshot, 7U);
	snapshot.release();
	const PageOwnerHeader foreignOwner{location.page_id,
									   71U,
									   PageOwnerKind::POK_PROPERTY,
									   0U,
									   RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									   Generation(1U),
									   nextCheckpointLsn(*metadata)};
	overwritePageOwner(io, *metadata, location.page_id, foreignOwner);

	LargeValueReachabilityReclaimer reclaimer(values, pages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsEntityPropertyLargeValueReferencesWithMismatchedOwners) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const LargeValueWriteResult written =
			values.write(std::array{std::byte{0xA1}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 71U};
	const EntityPropertyLargeValueReference mismatchedReference{key.entity_id + 1U, graph::Node::typeId,
																written.reference};
	const std::vector<std::byte> payload = EntityPropertyLargeValueReferenceCodec::encode(mismatchedReference);
	static_cast<void>(publishEntityPropertyPayload(*metadata, propertyPages, key, payload));

	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsEntityPropertyLargeValueReferencesWithMismatchedType) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const LargeValueWriteResult written =
			values.write(std::array{std::byte{0xA1}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 71U};
	const EntityPropertyLargeValueReference mismatchedReference{key.entity_id, graph::Edge::typeId, written.reference};
	static_cast<void>(publishEntityPropertyPayload(
			*metadata, propertyPages, key, EntityPropertyLargeValueReferenceCodec::encode(mismatchedReference)));

	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsEntityPropertyReferencesWithInvalidTypeOrMedia) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const LargeValueWriteResult written =
			values.write(std::array{std::byte{0xA1}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	uint64_t entityId = 71U;
	const auto expectRejected = [&](LargeValueRef value) {
		const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, entityId++};
		const EntityPropertyLargeValueReference stored{key.entity_id, graph::Node::typeId, std::move(value)};
		static_cast<void>(publishEntityPropertyPayload(*metadata, propertyPages, key,
													   EntityPropertyLargeValueReferenceCodec::encode(stored)));
		expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))); },
						  FormatError::FERR_INVALID_STATE);
		static_cast<void>(
				metadata->upsertEntityPropertyLocations({}, std::span(&key, 1U), nextCheckpointLsn(*metadata)));
	};

	LargeValueRef text = written.reference;
	text.kind = LargeValueKind::LVK_UTF8_TEXT;
	expectRejected(std::move(text));
	LargeValueRef wrongMedia = written.reference;
	wrongMedia.media_type = "application/vnd.zyx.other-owner";
	expectRejected(std::move(wrongMedia));
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerAppliesTheSharedOwnerLocationBoundToPropertyRoots) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	for (const EntityPropertyKey key: {EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 71U},
									   EntityPropertyKey{EntityPropertyOwnerKind::EPOK_EDGE, 72U}}) {
		const uint32_t ownerType =
				key.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? graph::Node::typeId : graph::Edge::typeId;
		const PropertyCollectionRoot root{key.entity_id, ownerType, {}};
		const std::vector<std::byte> payload = EntityPropertyCollectionRootCodec::encode(root);
		static_cast<void>(publishEntityPropertyPayload(*metadata, propertyPages, key, payload));
	}

	LargeValueReachabilityReclaimOptions options = reclaimOptions();
	options.maximum_owner_locations = 1U;
	options.maximum_owner_metadata_pages = 32U;
	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(options, nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsEveryInvalidPolicyBeforeOpeningMetadata) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	const auto expectInvalid = [&](const std::function<void(LargeValueReachabilityReclaimOptions &)> &mutate,
								   LogSequenceNumber checkpointLsn = LogSequenceNumber(2U)) {
		LargeValueReachabilityReclaimOptions options = reclaimOptions();
		mutate(options);
		expectFormatError([&] { static_cast<void>(reclaimer.reclaim(options, checkpointLsn)); },
						  FormatError::FERR_INVALID_HEADER);
	};
	expectInvalid([](LargeValueReachabilityReclaimOptions &options) { options.maximum_manifest_locations = 0U; });
	expectInvalid([](LargeValueReachabilityReclaimOptions &options) { options.maximum_manifest_metadata_pages = 0U; });
	expectInvalid([](LargeValueReachabilityReclaimOptions &options) { options.maximum_owner_locations = 0U; });
	expectInvalid([](LargeValueReachabilityReclaimOptions &options) { options.maximum_owner_metadata_pages = 0U; });
	expectInvalid([](LargeValueReachabilityReclaimOptions &options) { options.maximum_retirements = 0U; });
	expectInvalid([](LargeValueReachabilityReclaimOptions &options) { options.maximum_pools = 0U; });
	expectInvalid([](LargeValueReachabilityReclaimOptions &options) { options.first_index_slot = PhysicalSlotId{}; });
	expectInvalid([](LargeValueReachabilityReclaimOptions &options) { options.last_index_slot = PhysicalSlotId{}; });
	expectInvalid([](LargeValueReachabilityReclaimOptions &options) {
		options.first_index_slot = PhysicalSlotId(options.last_index_slot.value() + 1U);
	});
	expectInvalid([](LargeValueReachabilityReclaimOptions &) {}, LogSequenceNumber{});
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsManifestRouteAndDataPageBoundsBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const LargeValueWriteResult first =
			values.write(std::array{std::byte{0xA1}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	const LargeValueWriteResult second =
			values.write(std::array{std::byte{0xB2}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	LargeValueReachabilityReclaimOptions routeBound = reclaimOptions();
	routeBound.maximum_manifest_locations = 1U;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(routeBound, nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin afterRouteFailure = metadata->openSnapshot();
	EXPECT_TRUE(metadata->findLargeValueManifestOptional(afterRouteFailure, first.reference.value_id).has_value());
	EXPECT_TRUE(metadata->findLargeValueManifestOptional(afterRouteFailure, second.reference.value_id).has_value());
	afterRouteFailure.release();

	LargeValueWriteOptions rawOptions = propertyValueWriteOptions();
	rawOptions.compression.codec = LargeValueCodec::LVC_RAW;
	const std::vector<std::byte> multiPage(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 1U, std::byte{0xC3});
	ASSERT_GT(values.plannedDataPageCount(multiPage, rawOptions.compression, rawOptions.extent_class), 1U);
	const LargeValueWriteResult large = values.write(multiPage, rawOptions, nextCheckpointLsn(*metadata));
	LargeValueReachabilityReclaimOptions dataBound = reclaimOptions();
	dataBound.maximum_manifest_locations = 4U;
	dataBound.maximum_data_pages = 1U;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(dataBound, nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin afterDataFailure = metadata->openSnapshot();
	EXPECT_EQ(values.read(afterDataFailure, large.reference), multiPage);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueReclaimerRejectsRetirementBudgetBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore values(io, *metadata);
	SlottedPagePool propertyPages(io, *metadata);
	const LargeValueWriteResult orphan =
			values.write(std::array{std::byte{0xA1}}, propertyValueWriteOptions(), nextCheckpointLsn(*metadata));
	LargeValueReachabilityReclaimer reclaimer(values, propertyPages, *metadata);
	LargeValueReachabilityReclaimOptions options = reclaimOptions();
	// One value owns both a Manifest page and at least one data page.
	options.maximum_retirements = 1U;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(options, nextCheckpointLsn(*metadata))); },
					  FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_TRUE(metadata->findLargeValueManifestOptional(snapshot, orphan.reference.value_id).has_value());
	EXPECT_EQ(values.read(snapshot, orphan.reference), std::vector<std::byte>({std::byte{0xA1}}));
}
