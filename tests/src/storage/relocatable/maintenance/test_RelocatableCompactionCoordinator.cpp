/**
 * @file test_RelocatableCompactionCoordinator.cpp
 * @brief Bounded shared-pool evacuation tests for the relocatable compaction coordinator.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableCompactionCoordinator.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "storage/relocatable/TestSlottedPageRecord.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"

using namespace graph::storage;
using graph::storage::test::TestSlottedPageRecord;
using graph::storage::test::TestSlottedPageRecordCodec;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	PageOwnerHeader propertyOwner(PageId pageId, LogSequenceNumber lsn) {
		return {pageId, 99, PageOwnerKind::POK_PROPERTY, 0, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1), lsn};
	}

	SlottedPagePoolAllocationRequest propertyRequest(SegmentId segmentId, PageId pageId) {
		SlottedPagePoolAllocationRequest result;
		result.segment_id = segmentId;
		result.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
		result.pool_owner_id = 99;
		result.generation = Generation(1);
		result.page_id = pageId;
		result.page_owner_id = 99;
		result.page_owner_kind = PageOwnerKind::POK_PROPERTY;
		return result;
	}

	TestSlottedPageRecord testRecord(uint64_t logicalId) {
		TestSlottedPageRecord result;
		result.logical_id = logicalId;
		result.payload = {std::byte{0x12}, std::byte{0x34}};
		return result;
	}


	EntityPageNewPoolRequest entityPoolRequest(SegmentId segmentId, uint64_t ownerId, PageId pageId,
																   PhysicalSlotId physicalSlot) {
		return {.segment_id = segmentId,
				.pool_owner_id = ownerId,
				.generation = Generation(1U),
				.page_id = pageId,
				.first_slot = physicalSlot,
				.last_slot = physicalSlot};
	}

	NodeEntityRecord entityNode(uint64_t /*firstOutEdge*/, uint64_t /*firstInEdge*/, uint64_t firstLabel = 7U) {
		NodeEntityRecord result;
		result.inline_label_ids = {firstLabel, firstLabel + 2U, firstLabel + 4U};
		result.inline_label_count = 3U;
		return result;
	}

	EdgeEntityRecord entityEdge(uint64_t source, uint64_t target, uint64_t type) {
		return {.source_node_id = source,
				.target_node_id = target,
				.type_id = type};
	}

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	SlottedPagePoolAllocationRequest btreeRequest(SegmentId segmentId, PageId pageId, Generation generation,
												  uint64_t poolOwnerId) {
		SlottedPagePoolAllocationRequest result;
		result.segment_id = segmentId;
		result.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		result.pool_owner_id = poolOwnerId;
		result.generation = generation;
		result.page_id = pageId;
		result.page_owner_id = 17;
		result.page_owner_kind = PageOwnerKind::POK_INDEX;
		return result;
	}

	BTreePage btreePage(const SlottedPagePoolAllocationRequest &request, LogSequenceNumber lsn) {
		PageOwnerHeader owner;
		owner.page_id = request.page_id;
		owner.owner_id = request.page_owner_id;
		owner.kind = request.page_owner_kind;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = request.generation;
		owner.committed_lsn = lsn;
		BTreePageHeader header;
		header.index_id = owner.owner_id;
		header.page_id = owner.page_id;
		header.generation = owner.generation;
		return BTreePage::create(owner, header);
	}

	SharedPagePoolAllocationRequest vectorRequest(SegmentId segmentId, PageId pageId, Generation generation,
												  uint64_t poolOwnerId) {
		SharedPagePoolAllocationRequest result;
		result.segment_id = segmentId;
		result.pool_kind = SegmentKind::SGK_VECTOR_RAW_POOL;
		result.pool_owner_id = poolOwnerId;
		result.generation = generation;
		result.page_id = pageId;
		result.page_owner_id = 17;
		result.page_owner_kind = PageOwnerKind::POK_VECTOR_RAW;
		return result;
	}

	VectorPage vectorPage(const SharedPagePoolAllocationRequest &request, LogSequenceNumber lsn) {
		VectorPageHeader header;
		header.index_id = request.page_owner_id;
		header.page_id = request.page_id;
		header.generation = request.generation;
		header.kind = VectorStorageKind::VSK_RAW;
		header.record_bytes = 16;
		header.slot_capacity = 2;
		header.bitmap_bytes = 1;
		header.committed_lsn = lsn;
		PageOwnerHeader owner;
		owner.page_id = header.page_id;
		owner.owner_id = header.index_id;
		owner.kind = PageOwnerKind::POK_VECTOR_RAW;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = header.generation;
		owner.committed_lsn = lsn;
		return VectorPage::create(owner, header);
	}

	VectorManifest vectorManifest(uint64_t indexId, PageId pageId, Generation generation, LogSequenceNumber lsn) {
		VectorManifest result;
		result.index_id = indexId;
		result.page_id = pageId;
		result.generation = generation;
		result.committed_lsn = lsn;
		result.dimensions = 16;
		result.entries = {{PageId(pageId.value() + 100U), VectorStorageKind::VSK_RAW, 3}};
		return result;
	}

	PageOwnerHeader manifestOwner(const VectorManifest &manifest) {
		return {manifest.page_id,
				manifest.index_id,
				PageOwnerKind::POK_VECTOR_MANIFEST,
				0,
				RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				manifest.generation,
				manifest.committed_lsn};
	}

	SharedPagePoolAllocationRequest manifestRequest(const VectorManifest &manifest, SegmentId segmentId) {
		SharedPagePoolAllocationRequest result;
		result.segment_id = segmentId;
		result.pool_kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
		result.pool_owner_id = manifest.index_id;
		result.generation = manifest.generation;
		result.page_id = manifest.page_id;
		result.page_owner_id = manifest.index_id;
		result.page_owner_kind = PageOwnerKind::POK_VECTOR_MANIFEST;
		return result;
	}

	[[nodiscard]] LargeValueWriteOptions largeValueWriteOptions() {
		LargeValueWriteOptions result;
		result.first_data_slot = PhysicalSlotId(40U);
		result.last_data_slot = PhysicalSlotId(80U);
		result.first_manifest_slot = PhysicalSlotId(81U);
		result.last_manifest_slot = PhysicalSlotId(90U);
		result.kind = LargeValueKind::LVK_BYTES;
		result.media_type = "application/octet-stream";
		return result;
	}

	[[nodiscard]] std::vector<std::byte> largeValueBytes(size_t count) {
		std::vector<std::byte> result(count);
		for (size_t index = 0U; index < result.size(); ++index) {
			result[index] = static_cast<std::byte>((index * 29U + 7U) & 0xFFU);
		}
		return result;
	}

	void allocateLargeValueDataDestination(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
											  const LargeValueRef &reference, SegmentId segmentId, PageId pageId,
											  Generation poolGeneration, PhysicalSlotId slot,
											  LogSequenceNumber checkpointLsn) {
		SharedPagePoolAllocator pools(io, metadata);
		const PageOwnerHeader owner{pageId,
											 reference.value_id.value(),
											 PageOwnerKind::POK_LARGE_VALUE_DATA,
											 0U,
											 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
											 poolGeneration,
											 checkpointLsn};
		const LargeValueDataPage spare{reference.value_id,
											 pageId,
											 reference.generation,
											 0U,
											 0U,
											 1U,
											 1U,
											 0U,
											 {std::byte{0xE1U}}};
		SharedPagePoolAllocationRequest request;
		request.segment_id = segmentId;
		request.pool_kind = SegmentKind::SGK_LARGE_VALUE_DATA_POOL;
		request.pool_owner_id = RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID;
		request.generation = poolGeneration;
		request.page_id = pageId;
		request.page_owner_id = reference.value_id.value();
		request.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_DATA;
		request.payload = LargeValueDataFrame::create(owner, spare).serialize();
		ASSERT_TRUE(pools.allocateNewPoolPage(request, slot, slot, checkpointLsn).has_value());
	}

	void allocateLargeValueManifestDestination(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
											  const LargeValueManifest &source, SegmentId segmentId, PageId pageId,
											  Generation poolGeneration, PhysicalSlotId slot,
											  LogSequenceNumber checkpointLsn) {
		SharedPagePoolAllocator pools(io, metadata);
		LargeValueManifest spare = source;
		spare.page_id = pageId;
		const PageOwnerHeader owner{pageId,
											 source.value_id.value(),
											 PageOwnerKind::POK_LARGE_VALUE_MANIFEST,
											 0U,
											 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
											 poolGeneration,
											 checkpointLsn};
		SharedPagePoolAllocationRequest request;
		request.segment_id = segmentId;
		request.pool_kind = SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL;
		request.pool_owner_id = RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID;
		request.generation = poolGeneration;
		request.page_id = pageId;
		request.page_owner_id = source.value_id.value();
		request.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
		request.payload = LargeValueManifestPage::create(owner, spare).serialize();
		ASSERT_TRUE(pools.allocateNewPoolPage(request, slot, slot, checkpointLsn).has_value());
	}

} // namespace

class RelocatableCompactionCoordinatorTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;
	file_handle_t nativeHandle{INVALID_FILE_HANDLE};

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_compaction_" + to_string(uuid));
		file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		nativeHandle = portable_open_rw(path.string().c_str());
		ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
		io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
	}

	void TearDown() override {
		io.reset();
		if (nativeHandle != INVALID_FILE_HANDLE) {
			EXPECT_EQ(portable_close_rw(nativeHandle), 0);
			nativeHandle = INVALID_FILE_HANDLE;
		}
		if (file && file->is_open()) {
			file->close();
		}
		std::error_code error;
		std::filesystem::remove(path, error);
	}
};

TEST_F(RelocatableCompactionCoordinatorTest, EvacuatesPropertyPagesAndReleasesAnEmptySourcePool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	SlottedPage sourcePage = SlottedPage::create(propertyOwner(PageId(101), LogSequenceNumber(2)));
	const TestSlottedPageRecord record = testRecord(501);
	const SlottedSlotReference slot = sourcePage.insert(TestSlottedPageRecordCodec::encode(record));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(71), PageId(101)), sourcePage, PhysicalSlotId(40),
								   PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 99U};
	const EntityPropertyLocation location{key, slot.page_id, slot.slot_id, slot.generation};
	static_cast<void>(metadata->upsertEntityPropertyLocations(std::span(&location, 1), {}, LogSequenceNumber(3)));

	SlottedPage destinationPage = SlottedPage::create(propertyOwner(PageId(102), LogSequenceNumber(4)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(72), PageId(102)), destinationPage, PhysicalSlotId(41),
								   PhysicalSlotId(41), LogSequenceNumber(4))
						.has_value());

	RelocatableCompactionCoordinator coordinator(pages, *metadata);
	const RelocatableSlottedPoolCompactionRequest request{SegmentId(71), SegmentId(72), 1, LogSequenceNumber(5)};
	const RelocatableSlottedPoolCompactionResult result = coordinator.compactSlottedPool(request);
	EXPECT_EQ(result.status, RelocatableCompactionStatus::VCS_COMPLETED);
	EXPECT_EQ(result.moved_pages, 1U);
	EXPECT_TRUE(result.source_empty);
	EXPECT_TRUE(result.source_retired);
	EXPECT_TRUE(result.source_released);
	EXPECT_EQ(result.next_lsn, LogSequenceNumber(8));

	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(snapshot, PageId(101)).segment_id, SegmentId(72));
	EXPECT_EQ(metadata->findEntityPropertyLocation(snapshot, key), location);
	EXPECT_EQ(metadata->findCatalog(snapshot, SegmentId(71)).state, SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
	const SlottedPage relocated = pages.read(snapshot, PageId(101));
	EXPECT_EQ(TestSlottedPageRecordCodec::decode(relocated.read(slot)), record);
	RelocatableIntegrityScanner scanner(io, *metadata);
	// An opaque test record is not a canonical Entity Property root and must fail validation.
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableCompactionCoordinatorTest, EvacuatesEveryOtherSupportedGenericSlottedPoolKind) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);

	const auto compactOnePage = [&](SegmentKind poolKind, PageOwnerKind ownerKind, SegmentId sourceSegment,
								  SegmentId destinationSegment, PageId sourcePageId, PageId destinationPageId,
								  PhysicalSlotId sourceSlot, PhysicalSlotId destinationSlot,
								  LogSequenceNumber firstLsn) {
		const PageOwnerHeader owner{sourcePageId, 99U, ownerKind, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									Generation(1U), firstLsn};
		SlottedPage sourcePage = SlottedPage::create(owner);
		const TestSlottedPageRecord record = testRecord(sourcePageId.value());
		const SlottedSlotReference slot = sourcePage.insert(TestSlottedPageRecordCodec::encode(record));
		SlottedPagePoolAllocationRequest sourceRequest = propertyRequest(sourceSegment, sourcePageId);
		sourceRequest.pool_kind = poolKind;
		sourceRequest.page_owner_kind = ownerKind;
		ASSERT_TRUE(pages.allocatePage(sourceRequest, sourcePage, sourceSlot, sourceSlot, firstLsn).has_value());

		const PageOwnerHeader destinationOwner{destinationPageId, 99U, ownerKind, 0U,
											RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U),
											LogSequenceNumber(firstLsn.value() + 1U)};
		SlottedPage destinationPage = SlottedPage::create(destinationOwner);
		SlottedPagePoolAllocationRequest destinationRequest = propertyRequest(destinationSegment, destinationPageId);
		destinationRequest.pool_kind = poolKind;
		destinationRequest.page_owner_kind = ownerKind;
		ASSERT_TRUE(pages.allocatePage(destinationRequest, destinationPage, destinationSlot, destinationSlot,
										   LogSequenceNumber(firstLsn.value() + 1U))
						.has_value());

		RelocatableCompactionCoordinator coordinator(pages, *metadata);
		const RelocatableSlottedPoolCompactionResult result = coordinator.compactSlottedPool(
				{sourceSegment, destinationSegment, 1U, LogSequenceNumber(firstLsn.value() + 2U)});
		EXPECT_EQ(result.status, RelocatableCompactionStatus::VCS_COMPLETED);
		EXPECT_EQ(result.moved_pages, 1U);
		EXPECT_TRUE(result.source_empty);
		EXPECT_TRUE(result.source_retired);
		EXPECT_TRUE(result.source_released);

		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		EXPECT_EQ(metadata->findPageLocation(snapshot, sourcePageId).segment_id, destinationSegment);
		EXPECT_EQ(metadata->findCatalog(snapshot, sourceSegment).state, SegmentState::SGS_OBSOLETE);
		EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, sourceSlot));
		EXPECT_EQ(TestSlottedPageRecordCodec::decode(pages.read(snapshot, sourcePageId).read(slot)), record);
	};

	compactOnePage(SegmentKind::SGK_STATE_PAGE_POOL, PageOwnerKind::POK_STATE, SegmentId(73U), SegmentId(74U),
				   PageId(103U), PageId(104U), PhysicalSlotId(42U), PhysicalSlotId(43U), LogSequenceNumber(2U));
	compactOnePage(SegmentKind::SGK_VECTOR_ADJACENCY_POOL, PageOwnerKind::POK_VECTOR_ADJACENCY, SegmentId(75U),
				   SegmentId(76U), PageId(105U), PageId(106U), PhysicalSlotId(44U), PhysicalSlotId(45U),
				   LogSequenceNumber(7U));
}

TEST_F(RelocatableCompactionCoordinatorTest, RetainsRetiredPoolUntilTheLastSnapshotDrains) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	SlottedPage sourcePage = SlottedPage::create(propertyOwner(PageId(101), LogSequenceNumber(2)));
	const TestSlottedPageRecord record = testRecord(501);
	const SlottedSlotReference slot = sourcePage.insert(TestSlottedPageRecordCodec::encode(record));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(71), PageId(101)), sourcePage, PhysicalSlotId(40),
								   PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	SlottedPage destinationPage = SlottedPage::create(propertyOwner(PageId(102), LogSequenceNumber(3)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(72), PageId(102)), destinationPage, PhysicalSlotId(41),
								   PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	RelocatableCompactionCoordinator coordinator(pages, *metadata);
	const RelocatableSlottedPoolCompactionResult result =
			coordinator.compactSlottedPool({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(4)});
	EXPECT_EQ(result.status, RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS);
	EXPECT_TRUE(result.source_retired);
	EXPECT_FALSE(result.source_released);
	EXPECT_EQ(TestSlottedPageRecordCodec::decode(pages.read(oldSnapshot, PageId(101)).read(slot)), record);
	EXPECT_FALSE(coordinator.releaseRetiredSlottedPool(SegmentId(71), Generation(1), LogSequenceNumber(6)));
	oldSnapshot.release();
	EXPECT_TRUE(coordinator.releaseRetiredSlottedPool(SegmentId(71), Generation(1), LogSequenceNumber(6)));
	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(current, SegmentId(71)).state, SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(current, PhysicalSlotId(40)));
}

TEST_F(RelocatableCompactionCoordinatorTest,
		ReopensTheRetiredPoolAndRetriesReleaseAfterAnInterruptedInactiveSuperblockWrite) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	SlottedPage sourcePage = SlottedPage::create(propertyOwner(PageId(101), LogSequenceNumber(2)));
	const SlottedSlotReference slot = sourcePage.insert(TestSlottedPageRecordCodec::encode(testRecord(501)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(71), PageId(101)), sourcePage, PhysicalSlotId(40),
								   PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	SlottedPage destinationPage = SlottedPage::create(propertyOwner(PageId(102), LogSequenceNumber(3)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(72), PageId(102)), destinationPage, PhysicalSlotId(41),
								   PhysicalSlotId(41), LogSequenceNumber(3))
					.has_value());

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	RelocatableCompactionCoordinator coordinator(pages, *metadata);
	const RelocatableSlottedPoolCompactionResult compacted =
			coordinator.compactSlottedPool({SegmentId(71), SegmentId(72), 1U, LogSequenceNumber(4)});
	ASSERT_TRUE(compacted.source_retired);
	ASSERT_FALSE(compacted.source_released);
	EXPECT_EQ(TestSlottedPageRecordCodec::decode(pages.read(oldSnapshot, PageId(101)).read(slot)).logical_id, 501U);
	oldSnapshot.release();

	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(coordinator.releaseRetiredSlottedPool(
				 SegmentId(71), Generation(1), compacted.next_lsn, interrupted)),
			 BootstrapPublishInterrupted);

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopened);
	RelocatableCompactionCoordinator reopenedCoordinator(reopenedPages, *reopened);
	MetadataSnapshotPin retired = reopened->openSnapshot();
	EXPECT_EQ(reopened->findCatalog(retired, SegmentId(71)).state, SegmentState::SGS_RETIRED);
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(retired, PhysicalSlotId(40)));
	retired.release();

	EXPECT_TRUE(reopenedCoordinator.releaseRetiredSlottedPool(SegmentId(71), Generation(1), compacted.next_lsn));
	MetadataSnapshotPin released = reopened->openSnapshot();
	EXPECT_EQ(reopened->findCatalog(released, SegmentId(71)).state, SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(released, PhysicalSlotId(40)));
}

TEST_F(RelocatableCompactionCoordinatorTest,
		ReopensTheReleasedPoolAfterAnInterruptedPostPublicationSuperblockWrite) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	SlottedPage sourcePage = SlottedPage::create(propertyOwner(PageId(101), LogSequenceNumber(2)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(71), PageId(101)), sourcePage, PhysicalSlotId(40),
								   PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	SlottedPage destinationPage = SlottedPage::create(propertyOwner(PageId(102), LogSequenceNumber(3)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(72), PageId(102)), destinationPage, PhysicalSlotId(41),
								   PhysicalSlotId(41), LogSequenceNumber(3))
					.has_value());

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	RelocatableCompactionCoordinator coordinator(pages, *metadata);
	const RelocatableSlottedPoolCompactionResult compacted =
			coordinator.compactSlottedPool({SegmentId(71), SegmentId(72), 1U, LogSequenceNumber(4)});
	ASSERT_TRUE(compacted.source_retired);
	ASSERT_FALSE(compacted.source_released);
	oldSnapshot.release();

	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(coordinator.releaseRetiredSlottedPool(
				 SegmentId(71), Generation(1), compacted.next_lsn, interrupted)),
			 BootstrapPublishInterrupted);

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopened);
	MetadataSnapshotPin released = reopened->openSnapshot();
	EXPECT_EQ(reopened->findCatalog(released, SegmentId(71)).state, SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(released, PhysicalSlotId(40)));
	released.release();

	SlottedPage reusedPage = SlottedPage::create(propertyOwner(PageId(103), LogSequenceNumber(7)));
	EXPECT_TRUE(reopenedPages.allocatePage(propertyRequest(SegmentId(73), PageId(103)), reusedPage, PhysicalSlotId(40),
											 PhysicalSlotId(40), LogSequenceNumber(7))
					.has_value());
	MetadataSnapshotPin reused = reopened->openSnapshot();
	EXPECT_EQ(reopened->findCatalog(reused, SegmentId(73)).physical_slot, PhysicalSlotId(40));
}

TEST_F(RelocatableCompactionCoordinatorTest,
		LargeValueCompactionEvacuatesDataAndManifestPoolsAndPreservesSnapshots) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	LargeValueStore values(io, *metadata);
	const std::vector<std::byte> input = largeValueBytes(31U);
	const LargeValueWriteResult written = values.write(input, largeValueWriteOptions(), LogSequenceNumber(2U));
	ASSERT_EQ(written.manifest.chunks.size(), 1U);
	const PageId dataPageId = written.manifest.chunks.front().first_data_page_id;

	MetadataSnapshotPin before = metadata->openSnapshot();
	const PageLocation sourceDataLocation = metadata->findPageLocation(before, dataPageId);
	const LargeValueManifestLocation sourceManifestLocation =
			metadata->findLargeValueManifest(before, written.reference.value_id);
	const PageLocation sourceManifestPageLocation =
			metadata->findPageLocation(before, sourceManifestLocation.manifest_page_id);
	const CatalogEntry sourceDataPool = metadata->findCatalog(before, sourceDataLocation.segment_id);
	const CatalogEntry sourceManifestPool = metadata->findCatalog(before, sourceManifestPageLocation.segment_id);
	ASSERT_EQ(sourceDataPool.kind, SegmentKind::SGK_LARGE_VALUE_DATA_POOL);
	ASSERT_EQ(sourceManifestPool.kind, SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL);

	allocateLargeValueDataDestination(io, *metadata, written.reference, SegmentId(71U), PageId(501U), Generation(2U),
											PhysicalSlotId(100U), LogSequenceNumber(3U));
	allocateLargeValueManifestDestination(io, *metadata, written.manifest, SegmentId(72U), PageId(502U), Generation(2U),
											PhysicalSlotId(101U), LogSequenceNumber(4U));

	SlottedPagePool pages(io, *metadata);
	RelocatableCompactionCoordinator coordinator(pages, values, *metadata);
	const RelocatableSlottedPoolCompactionResult dataResult = coordinator.compactLargeValuePool(
			{sourceDataLocation.segment_id, SegmentId(71U), 1U, LogSequenceNumber(5U)});
	EXPECT_EQ(dataResult.status, RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS);
	EXPECT_EQ(dataResult.moved_pages, 1U);
	EXPECT_TRUE(dataResult.source_empty);
	EXPECT_TRUE(dataResult.source_retired);
	EXPECT_FALSE(dataResult.source_released);

	const RelocatableSlottedPoolCompactionResult manifestResult = coordinator.compactLargeValuePool(
			{sourceManifestPageLocation.segment_id, SegmentId(72U), 1U, dataResult.next_lsn});
	EXPECT_EQ(manifestResult.status, RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS);
	EXPECT_EQ(manifestResult.moved_pages, 1U);
	EXPECT_TRUE(manifestResult.source_empty);
	EXPECT_TRUE(manifestResult.source_retired);
	EXPECT_FALSE(manifestResult.source_released);

	EXPECT_EQ(values.read(before, written.reference), input);
	EXPECT_FALSE(coordinator.releaseRetiredSlottedPool(sourceDataLocation.segment_id, sourceDataPool.generation,
																		 dataResult.next_lsn));
	EXPECT_FALSE(coordinator.releaseRetiredSlottedPool(sourceManifestPageLocation.segment_id, sourceManifestPool.generation,
																		 manifestResult.next_lsn));
	before.release();
	EXPECT_TRUE(coordinator.releaseRetiredSlottedPool(sourceDataLocation.segment_id, sourceDataPool.generation,
																		 LogSequenceNumber(manifestResult.next_lsn.value() + 1U)));
	EXPECT_TRUE(coordinator.releaseRetiredSlottedPool(sourceManifestPageLocation.segment_id, sourceManifestPool.generation,
																		 LogSequenceNumber(manifestResult.next_lsn.value() + 2U)));

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(current, dataPageId).segment_id, SegmentId(71U));
	EXPECT_EQ(metadata->findPageLocation(current,
											 metadata->findLargeValueManifest(current, written.reference.value_id).manifest_page_id)
				  .segment_id,
			  SegmentId(72U));
	EXPECT_EQ(values.read(current, written.reference), input);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(current, sourceDataPool.physical_slot));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(current, sourceManifestPool.physical_slot));
	current.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	LargeValueStore reopenedValues(io, *reopened);
	const MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedValues.read(reopenedSnapshot, written.reference), input);
}

TEST_F(RelocatableCompactionCoordinatorTest, LeavesTheSourceUntouchedWhenTheDestinationHasNoFrames) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	SlottedPage sourcePage = SlottedPage::create(propertyOwner(PageId(101), LogSequenceNumber(2)));
	const TestSlottedPageRecord record = testRecord(501);
	static_cast<void>(sourcePage.insert(TestSlottedPageRecordCodec::encode(record)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(71), PageId(101)), sourcePage, PhysicalSlotId(40),
								   PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	SlottedPage destinationPage = SlottedPage::create(propertyOwner(PageId(200), LogSequenceNumber(3)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(72), PageId(200)), destinationPage, PhysicalSlotId(41),
								   PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());
	for (uint64_t pageValue = 201; pageValue <= 230; ++pageValue) {
		const LogSequenceNumber lsn(pageValue - 197);
		SlottedPage page = SlottedPage::create(propertyOwner(PageId(pageValue), lsn));
		SlottedPagePoolAppendRequest request;
		request.segment_id = SegmentId(72);
		request.generation = Generation(1);
		request.page_id = PageId(pageValue);
		request.page_owner_id = 99;
		request.page_owner_kind = PageOwnerKind::POK_PROPERTY;
		static_cast<void>(pages.appendPage(request, page, lsn));
	}

	RelocatableCompactionCoordinator coordinator(pages, *metadata);
	const RelocatableSlottedPoolCompactionResult result =
			coordinator.compactSlottedPool({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(40)});
	EXPECT_EQ(result.status, RelocatableCompactionStatus::VCS_NO_DESTINATION_CAPACITY);
	EXPECT_EQ(result.moved_pages, 0U);
	EXPECT_FALSE(result.source_empty);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(snapshot, PageId(101)).segment_id, SegmentId(71));
	EXPECT_EQ(metadata->findCatalog(snapshot, SegmentId(71)).state, SegmentState::SGS_SEALED);
}

TEST_F(RelocatableCompactionCoordinatorTest, RespectsThePageBudgetAndLeavesAStillPopulatedSourceSealed) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	SlottedPage first = SlottedPage::create(propertyOwner(PageId(101), LogSequenceNumber(2)));
	SlottedPage second = SlottedPage::create(propertyOwner(PageId(102), LogSequenceNumber(3)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(71), PageId(101)), first, PhysicalSlotId(40),
								   PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	SlottedPagePoolAppendRequest secondRequest;
	secondRequest.segment_id = SegmentId(71);
	secondRequest.generation = Generation(1);
	secondRequest.page_id = PageId(102);
	secondRequest.page_owner_id = 99;
	secondRequest.page_owner_kind = PageOwnerKind::POK_PROPERTY;
	static_cast<void>(pages.appendPage(secondRequest, second, LogSequenceNumber(3)));
	SlottedPage destination = SlottedPage::create(propertyOwner(PageId(200), LogSequenceNumber(4)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(72), PageId(200)), destination, PhysicalSlotId(41),
								   PhysicalSlotId(41), LogSequenceNumber(4))
						.has_value());

	RelocatableCompactionCoordinator coordinator(pages, *metadata);
	const RelocatableSlottedPoolCompactionResult result =
			coordinator.compactSlottedPool({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(5)});
	EXPECT_EQ(result.moved_pages, 1U);
	EXPECT_FALSE(result.source_empty);
	EXPECT_FALSE(result.source_retired);
	EXPECT_FALSE(result.source_released);
	EXPECT_EQ(result.next_lsn, LogSequenceNumber(6));
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(snapshot, PageId(101)).segment_id, SegmentId(72));
	EXPECT_EQ(metadata->findPageLocation(snapshot, PageId(102)).segment_id, SegmentId(71));
}

TEST_F(RelocatableCompactionCoordinatorTest, RejectsPageFormatsWithAnAdditionalTypedHeader) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	const PageOwnerHeader sourceOwner{
			PageId(101),		 17, PageOwnerKind::POK_INDEX, 0, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1),
			LogSequenceNumber(2)};
	SlottedPage source = SlottedPage::create(sourceOwner);
	SlottedPagePoolAllocationRequest sourceRequest;
	sourceRequest.segment_id = SegmentId(71);
	sourceRequest.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	sourceRequest.pool_owner_id = 17;
	sourceRequest.generation = Generation(1);
	sourceRequest.page_id = sourceOwner.page_id;
	sourceRequest.page_owner_id = sourceOwner.owner_id;
	sourceRequest.page_owner_kind = sourceOwner.kind;
	ASSERT_TRUE(pages.allocatePage(sourceRequest, source, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	const PageOwnerHeader destinationOwner{
			PageId(102),		 17, PageOwnerKind::POK_INDEX, 0, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1),
			LogSequenceNumber(3)};
	SlottedPage destination = SlottedPage::create(destinationOwner);
	SlottedPagePoolAllocationRequest destinationRequest = sourceRequest;
	destinationRequest.segment_id = SegmentId(72);
	destinationRequest.page_id = destinationOwner.page_id;
	ASSERT_TRUE(pages.allocatePage(destinationRequest, destination, PhysicalSlotId(41), PhysicalSlotId(41),
								   LogSequenceNumber(3))
						.has_value());
	RelocatableCompactionCoordinator coordinator(pages, *metadata);
	EXPECT_THROW(
			static_cast<void>(coordinator.compactSlottedPool({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(4)})),
			FormatException);
}

TEST_F(RelocatableCompactionCoordinatorTest, RejectsAnLsnRangeThatCannotPublishTheWholeEvacuation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	SlottedPage source = SlottedPage::create(propertyOwner(PageId(101), LogSequenceNumber(2)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(71), PageId(101)), source, PhysicalSlotId(40),
								   PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	SlottedPage destination = SlottedPage::create(propertyOwner(PageId(102), LogSequenceNumber(3)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(72), PageId(102)), destination, PhysicalSlotId(41),
								   PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());
	RelocatableCompactionCoordinator coordinator(pages, *metadata);
	EXPECT_THROW(
			static_cast<void>(coordinator.compactSlottedPool(
					{SegmentId(71), SegmentId(72), 1, LogSequenceNumber(std::numeric_limits<uint64_t>::max() - 2U)})),
			FormatException);
}

TEST_F(RelocatableCompactionCoordinatorTest, RejectsUnsupportedOrInvalidRequests) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	RelocatableCompactionCoordinator coordinator(pages, *metadata);
	auto foreignMetadata = RelocatableMetadataStore::open(io);
	EXPECT_THROW(static_cast<void>(RelocatableCompactionCoordinator(pages, *foreignMetadata)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(coordinator.compactSlottedPool({{}, SegmentId(72), 1, LogSequenceNumber(2)})),
				 FormatException);
	EXPECT_THROW(
			static_cast<void>(coordinator.compactSlottedPool({SegmentId(71), SegmentId(71), 1, LogSequenceNumber(2)})),
			FormatException);
	EXPECT_THROW(static_cast<void>(coordinator.releaseRetiredSlottedPool({}, Generation(1), LogSequenceNumber(2))),
				 FormatException);
	EXPECT_THROW(static_cast<void>(coordinator.releaseRetiredSlottedPool(SegmentId(71), Generation(1), {})),
				 FormatException);
}

TEST_F(RelocatableCompactionCoordinatorTest, EvacuatesNodeAndEdgePagesAndReleasesTheFormerPool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	EntityPageStore entities(io, *metadata);
	const NodeEntityRecord sourceNode = entityNode(11U, 12U);
	const EdgeEntityRecord sourceEdge = entityEdge(1000U, 1001U, 21U);
	const EntityPageInsertResult source =
			*entities.allocateNode(entityPoolRequest(SegmentId(71), 17U, PageId(101), PhysicalSlotId(40)), 1000U,
									 sourceNode, LogSequenceNumber(2));
	static_cast<void>(entities.appendEdgePage({source.pool, PageId(102)}, 2000U, sourceEdge, LogSequenceNumber(3)));
	const EntityPageInsertResult destination =
			*entities.allocateNode(entityPoolRequest(SegmentId(72), 17U, PageId(201), PhysicalSlotId(41)), 1001U,
									 entityNode(21U, 22U), LogSequenceNumber(4));
	static_cast<void>(entities.appendEdgePage({destination.pool, PageId(202)}, 2001U,
											 entityEdge(1001U, 1000U, 22U), LogSequenceNumber(5)));

	RelocatableCompactionCoordinator coordinator(pages, entities, *metadata);
	const RelocatableSlottedPoolCompactionResult result =
			coordinator.compactEntityPool({SegmentId(71), SegmentId(72), 2U, LogSequenceNumber(6)});
	EXPECT_EQ(result.status, RelocatableCompactionStatus::VCS_COMPLETED);
	EXPECT_EQ(result.moved_pages, 2U);
	EXPECT_TRUE(result.source_empty);
	EXPECT_TRUE(result.source_retired);
	EXPECT_TRUE(result.source_released);
	EXPECT_EQ(result.next_lsn, LogSequenceNumber(10));

	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(snapshot, SegmentId(71)).state, SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
	EXPECT_EQ(metadata->findPageLocation(snapshot, PageId(101)).segment_id, SegmentId(72));
	EXPECT_EQ(metadata->findPageLocation(snapshot, PageId(102)).segment_id, SegmentId(72));
	EXPECT_EQ(entities.readNode(snapshot, 1000U), sourceNode);
	EXPECT_EQ(entities.readEdge(snapshot, 2000U), sourceEdge);
}

TEST_F(RelocatableCompactionCoordinatorTest, EvacuatesBTreeRootPagesAndReleasesTheSourcePool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	const SlottedPagePoolAllocationRequest sourceRequest = btreeRequest(SegmentId(71), PageId(101), Generation(1), 77);
	ASSERT_TRUE(btreePages
						.allocate(sourceRequest, btreePage(sourceRequest, LogSequenceNumber(2)), PhysicalSlotId(40),
								  PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation root{17, sourceRequest.page_id, sourceRequest.generation};
	static_cast<void>(btreePages.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
	const SlottedPagePoolAllocationRequest destinationRequest =
			btreeRequest(SegmentId(72), PageId(102), Generation(2), 78);
	ASSERT_TRUE(btreePages
						.allocate(destinationRequest, btreePage(destinationRequest, LogSequenceNumber(4)),
								  PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(4))
						.has_value());

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	RelocatableCompactionCoordinator coordinator(pages, btreePages, *metadata);
	const RelocatableSlottedPoolCompactionResult result =
			coordinator.compactBTreePool({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(5)});
	EXPECT_EQ(result.status, RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS);
	EXPECT_EQ(result.moved_pages, 1U);
	EXPECT_TRUE(result.source_empty);
	EXPECT_TRUE(result.source_retired);
	EXPECT_FALSE(result.source_released);
	EXPECT_EQ(result.next_lsn, LogSequenceNumber(7));
	EXPECT_EQ(metadata->findIndexRoot(oldSnapshot, 17), root);
	EXPECT_EQ(btreePages.read(oldSnapshot, sourceRequest.page_id).header().generation, sourceRequest.generation);
	EXPECT_FALSE(coordinator.releaseRetiredSlottedPool(sourceRequest.segment_id, sourceRequest.generation,
													   LogSequenceNumber(7)));
	oldSnapshot.release();
	EXPECT_TRUE(coordinator.releaseRetiredSlottedPool(sourceRequest.segment_id, sourceRequest.generation,
													  LogSequenceNumber(7)));
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(snapshot, 17),
			  (IndexRootLocation{17, sourceRequest.page_id, destinationRequest.generation}));
	EXPECT_EQ(metadata->findPageLocation(snapshot, sourceRequest.page_id).segment_id, destinationRequest.segment_id);
	EXPECT_EQ(btreePages.read(snapshot, sourceRequest.page_id).header().generation, destinationRequest.generation);
	EXPECT_EQ(metadata->findCatalog(snapshot, sourceRequest.segment_id).state, SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
}

TEST_F(RelocatableCompactionCoordinatorTest, RespectsBTreePageBudgetAndLeavesTheSourcePoolCurrent) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	const SlottedPagePoolAllocationRequest sourceRequest = btreeRequest(SegmentId(71), PageId(101), Generation(1), 77);
	ASSERT_TRUE(btreePages
						.allocate(sourceRequest, btreePage(sourceRequest, LogSequenceNumber(2)), PhysicalSlotId(40),
								  PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation root{17, sourceRequest.page_id, sourceRequest.generation};
	static_cast<void>(btreePages.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
	SlottedPagePoolAppendRequest appendRequest;
	appendRequest.segment_id = sourceRequest.segment_id;
	appendRequest.generation = sourceRequest.generation;
	appendRequest.page_id = PageId(102);
	appendRequest.page_owner_id = 17;
	appendRequest.page_owner_kind = PageOwnerKind::POK_INDEX;
	const SlottedPagePoolAllocationRequest appendedRequest =
			btreeRequest(sourceRequest.segment_id, appendRequest.page_id, sourceRequest.generation, 77);
	static_cast<void>(
			btreePages.append(appendRequest, btreePage(appendedRequest, LogSequenceNumber(4)), LogSequenceNumber(4)));
	const SlottedPagePoolAllocationRequest destinationRequest =
			btreeRequest(SegmentId(72), PageId(200), Generation(2), 78);
	ASSERT_TRUE(btreePages
						.allocate(destinationRequest, btreePage(destinationRequest, LogSequenceNumber(5)),
								  PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(5))
						.has_value());

	RelocatableCompactionCoordinator coordinator(pages, btreePages, *metadata);
	const RelocatableSlottedPoolCompactionResult result =
			coordinator.compactBTreePool({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(6)});
	EXPECT_EQ(result.moved_pages, 1U);
	EXPECT_FALSE(result.source_empty);
	EXPECT_FALSE(result.source_retired);
	EXPECT_FALSE(result.source_released);
	EXPECT_EQ(result.next_lsn, LogSequenceNumber(7));
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(snapshot, PageId(101)).segment_id, destinationRequest.segment_id);
	EXPECT_EQ(metadata->findPageLocation(snapshot, PageId(102)).segment_id, sourceRequest.segment_id);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, 17).root_generation, destinationRequest.generation);
	EXPECT_EQ(metadata->findCatalog(snapshot, sourceRequest.segment_id).state, SegmentState::SGS_SEALED);
}

TEST_F(RelocatableCompactionCoordinatorTest, EvacuatesRawVectorPagesAndRetainsStableVectorLocations) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	VectorPageStore vectorPages(io, *metadata);
	const SharedPagePoolAllocationRequest sourceRequest = vectorRequest(SegmentId(71), PageId(101), Generation(1), 77);
	VectorPage sourcePage = vectorPage(sourceRequest, LogSequenceNumber(2));
	const std::vector<std::byte> record(16U, std::byte{0x31});
	const uint16_t slot = sourcePage.insert(record);
	ASSERT_TRUE(
			vectorPages
					.allocate(sourceRequest, sourcePage, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	const VectorLocation location{700, sourceRequest.page_id, slot, sourcePage.slotGeneration(slot)};
	static_cast<void>(metadata->upsertVectorLocations(std::span(&location, 1), {}, LogSequenceNumber(3)));
	const SharedPagePoolAllocationRequest destinationRequest =
			vectorRequest(SegmentId(72), PageId(102), Generation(2), 78);
	ASSERT_TRUE(vectorPages
						.allocate(destinationRequest, vectorPage(destinationRequest, LogSequenceNumber(4)),
								  PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(4))
						.has_value());

	RelocatableCompactionCoordinator coordinator(pages, vectorPages, *metadata);
	const RelocatableSlottedPoolCompactionResult result =
			coordinator.compactVectorPool({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(5)});
	EXPECT_EQ(result.status, RelocatableCompactionStatus::VCS_COMPLETED);
	EXPECT_EQ(result.moved_pages, 1U);
	EXPECT_TRUE(result.source_empty);
	EXPECT_TRUE(result.source_retired);
	EXPECT_TRUE(result.source_released);
	EXPECT_EQ(result.next_lsn, LogSequenceNumber(8));
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findVectorLocation(snapshot, location.vector_id), location);
	EXPECT_EQ(metadata->findPageLocation(snapshot, sourceRequest.page_id).segment_id, destinationRequest.segment_id);
	EXPECT_EQ(vectorPages.read(snapshot, sourceRequest.page_id).header().generation, destinationRequest.generation);
	EXPECT_EQ(vectorPages.read(snapshot, sourceRequest.page_id).read(slot, location.slot_generation), record);
	EXPECT_EQ(metadata->findCatalog(snapshot, sourceRequest.segment_id).state, SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
}

TEST_F(RelocatableCompactionCoordinatorTest, EvacuatesVectorManifestsAndRetainsOldSnapshotDirectoryBindings) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	const VectorManifest source = vectorManifest(17, PageId(101), Generation(1), LogSequenceNumber(2));
	ASSERT_TRUE(manifests
						.allocate(manifestRequest(source, SegmentId(71)),
								  VectorManifestPage::create(manifestOwner(source), source), PhysicalSlotId(40),
								  PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	const VectorManifest destination = vectorManifest(18, PageId(102), Generation(2), LogSequenceNumber(3));
	ASSERT_TRUE(manifests
						.allocate(manifestRequest(destination, SegmentId(72)),
								  VectorManifestPage::create(manifestOwner(destination), destination),
								  PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	RelocatableCompactionCoordinator coordinator(pages, manifests, *metadata);
	const RelocatableSlottedPoolCompactionResult result =
			coordinator.compactVectorManifestPool({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(4)});
	EXPECT_EQ(result.status, RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS);
	EXPECT_EQ(result.moved_pages, 1U);
	EXPECT_TRUE(result.source_empty);
	EXPECT_TRUE(result.source_retired);
	EXPECT_FALSE(result.source_released);
	EXPECT_EQ(result.next_lsn, LogSequenceNumber(6));
	EXPECT_EQ(manifests.read(oldSnapshot, source.index_id).manifest(), source);
	EXPECT_EQ(metadata->findVectorManifest(oldSnapshot, source.index_id),
			  (VectorManifestLocation{source.index_id, source.page_id, source.generation}));
	EXPECT_FALSE(coordinator.releaseRetiredSlottedPool(SegmentId(71), Generation(1), LogSequenceNumber(6)));

	oldSnapshot.release();
	EXPECT_TRUE(coordinator.releaseRetiredSlottedPool(SegmentId(71), Generation(1), LogSequenceNumber(6)));
	const MetadataSnapshotPin current = metadata->openSnapshot();
	VectorManifest expected = source;
	expected.generation = Generation(2);
	expected.committed_lsn = LogSequenceNumber(4);
	EXPECT_EQ(manifests.read(current, source.index_id).manifest(), expected);
	EXPECT_EQ(metadata->findVectorManifest(current, source.index_id),
			  (VectorManifestLocation{source.index_id, source.page_id, Generation(2)}));
	EXPECT_EQ(metadata->findPageLocation(current, source.page_id).segment_id, SegmentId(72));
	EXPECT_EQ(metadata->findCatalog(current, SegmentId(71)).state, SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(current, PhysicalSlotId(40)));
}

TEST_F(RelocatableCompactionCoordinatorTest, RelocatesCodebookExtentsAndDefersSourceReleaseForSnapshots) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	PhysicalSegmentAllocator physical(io, *metadata);
	VectorCodebookStore codebooks(physical, *metadata);
	VectorCodebookWriteRequest codebook;
	codebook.codebook_id = 17;
	codebook.manifest_segment_id = SegmentId(71);
	codebook.generation = Generation(1);
	codebook.element_type = VectorCodebookElementType::VCET_FLOAT32;
	codebook.dimensions = 2;
	codebook.subspace_count = 1;
	codebook.centroid_count = 3;
	const std::vector<std::byte> payload(24U, std::byte{0x31});
	static_cast<void>(
			codebooks.writeImmutable(codebook, payload, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)));

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	RelocatableCompactionCoordinator coordinator(pages, codebooks, physical, *metadata);
	const RelocatableExtentCompactionResult result = coordinator.compactCodebookExtent(
			{codebook.codebook_id, PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3)});
	EXPECT_EQ(result.status, RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS);
	EXPECT_TRUE(result.source_relocated);
	EXPECT_FALSE(result.source_released);
	EXPECT_EQ(result.next_lsn, LogSequenceNumber(4));
	EXPECT_EQ(codebooks.read(oldSnapshot, codebook.codebook_id).payload, payload);
	EXPECT_FALSE(coordinator.releaseRelocatedExtentSource(SegmentId(71), Generation(2), LogSequenceNumber(4)));

	oldSnapshot.release();
	EXPECT_TRUE(coordinator.releaseRelocatedExtentSource(SegmentId(71), Generation(2), LogSequenceNumber(4)));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(codebooks.read(current, codebook.codebook_id).header.generation, Generation(1));
	EXPECT_EQ(metadata
				  ->findExtentLocation(current, VectorCodebookExtentIdCodec::manifestExtentId(codebook.codebook_id))
				  .generation,
			  Generation(2));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(current, PhysicalSlotId(40)));
}

TEST_F(RelocatableCompactionCoordinatorTest, LeavesExtentSourcesUntouchedWithoutDestinationCapacity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	PhysicalSegmentAllocator physical(io, *metadata);
	VectorCodebookStore codebooks(physical, *metadata);
	VectorCodebookWriteRequest codebook;
	codebook.codebook_id = 17;
	codebook.manifest_segment_id = SegmentId(71);
	codebook.generation = Generation(1);
	codebook.element_type = VectorCodebookElementType::VCET_FLOAT32;
	codebook.dimensions = 2;
	codebook.subspace_count = 1;
	codebook.centroid_count = 3;
	const std::vector<std::byte> codebookPayload(24U, std::byte{0x31});
	static_cast<void>(codebooks.writeImmutable(codebook, codebookPayload, PhysicalSlotId(40), PhysicalSlotId(40),
											   LogSequenceNumber(2)));
	RelocatableCompactionCoordinator codebookCoordinator(pages, codebooks, physical, *metadata);
	const RelocatableExtentCompactionResult codebookResult = codebookCoordinator.compactCodebookExtent(
			{17, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(3)});
	EXPECT_EQ(codebookResult.status, RelocatableCompactionStatus::VCS_NO_DESTINATION_CAPACITY);
	EXPECT_FALSE(codebookResult.source_relocated);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(codebooks.read(snapshot, codebook.codebook_id).payload, codebookPayload);
}

TEST_F(RelocatableCompactionCoordinatorTest, ReleasesRelocatedExtentsImmediatelyWhenNoSnapshotIsPinned) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	PhysicalSegmentAllocator physical(io, *metadata);
	VectorCodebookStore codebooks(physical, *metadata);
	VectorCodebookWriteRequest codebook;
	codebook.codebook_id = 17;
	codebook.manifest_segment_id = SegmentId(71);
	codebook.generation = Generation(1);
	codebook.element_type = VectorCodebookElementType::VCET_FLOAT32;
	codebook.dimensions = 2;
	codebook.subspace_count = 1;
	codebook.centroid_count = 3;
	const std::vector<std::byte> codebookPayload(24U, std::byte{0x31});
	static_cast<void>(codebooks.writeImmutable(codebook, codebookPayload, PhysicalSlotId(40), PhysicalSlotId(40),
											   LogSequenceNumber(2)));
	RelocatableCompactionCoordinator codebookCoordinator(pages, codebooks, physical, *metadata);
	const RelocatableExtentCompactionResult codebookResult = codebookCoordinator.compactCodebookExtent(
			{codebook.codebook_id, PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3)});
	EXPECT_EQ(codebookResult.status, RelocatableCompactionStatus::VCS_COMPLETED);
	EXPECT_TRUE(codebookResult.source_relocated);
	EXPECT_TRUE(codebookResult.source_released);
	EXPECT_EQ(codebookResult.next_lsn, LogSequenceNumber(5));

	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(codebooks.read(current, codebook.codebook_id).payload, codebookPayload);
}

TEST_F(RelocatableCompactionCoordinatorTest, LeavesTypedPoolSourcesUntouchedWhenTheDestinationIsFull) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	const SlottedPagePoolAllocationRequest sourceRequest = btreeRequest(SegmentId(71), PageId(101), Generation(1), 77);
	ASSERT_TRUE(btreePages
						.allocate(sourceRequest, btreePage(sourceRequest, LogSequenceNumber(2)), PhysicalSlotId(40),
								  PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	const SlottedPagePoolAllocationRequest destinationRequest =
			btreeRequest(SegmentId(72), PageId(200), Generation(2), 78);
	ASSERT_TRUE(btreePages
						.allocate(destinationRequest, btreePage(destinationRequest, LogSequenceNumber(3)),
								  PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());
	for (uint64_t frame = 1; frame < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
		SlottedPagePoolAppendRequest append;
		append.segment_id = destinationRequest.segment_id;
		append.generation = destinationRequest.generation;
		append.page_id = PageId(destinationRequest.page_id.value() + frame);
		append.page_owner_id = destinationRequest.page_owner_id;
		append.page_owner_kind = PageOwnerKind::POK_INDEX;
		const SlottedPagePoolAllocationRequest pageRequest =
				btreeRequest(destinationRequest.segment_id, append.page_id, destinationRequest.generation, 78);
		static_cast<void>(btreePages.append(append, btreePage(pageRequest, LogSequenceNumber(4 + frame)),
											LogSequenceNumber(4 + frame)));
	}

	RelocatableCompactionCoordinator coordinator(pages, btreePages, *metadata);
	const RelocatableSlottedPoolCompactionResult result = coordinator.compactBTreePool(
			{sourceRequest.segment_id, destinationRequest.segment_id, 1, LogSequenceNumber(40)});
	EXPECT_EQ(result.status, RelocatableCompactionStatus::VCS_NO_DESTINATION_CAPACITY);
	EXPECT_EQ(result.moved_pages, 0U);
	EXPECT_FALSE(result.source_empty);
	EXPECT_EQ(result.next_lsn, LogSequenceNumber(40));
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(snapshot, sourceRequest.page_id).segment_id, sourceRequest.segment_id);
	EXPECT_EQ(metadata->findCatalog(snapshot, sourceRequest.segment_id).state, SegmentState::SGS_SEALED);
}

TEST_F(RelocatableCompactionCoordinatorTest, RetainsEntityPoolUntilTheLastSnapshotDrains) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	EntityPageStore entities(io, *metadata);
	const NodeEntityRecord sourceNode = entityNode(11U, 12U);
	ASSERT_TRUE(entities.allocateNode(entityPoolRequest(SegmentId(71U), 17U, PageId(101U), PhysicalSlotId(40U)),
									  1000U, sourceNode, LogSequenceNumber(2U))
						.has_value());
	ASSERT_TRUE(entities.allocateNode(entityPoolRequest(SegmentId(72U), 17U, PageId(201U), PhysicalSlotId(41U)),
									  2000U, entityNode(21U, 22U), LogSequenceNumber(3U))
						.has_value());
	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	RelocatableCompactionCoordinator coordinator(pages, entities, *metadata);
	const RelocatableSlottedPoolCompactionResult result =
			coordinator.compactEntityPool({SegmentId(71U), SegmentId(72U), 1U, LogSequenceNumber(4U)});
	EXPECT_EQ(result.status, RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS);
	EXPECT_EQ(result.moved_pages, 1U);
	EXPECT_TRUE(result.source_empty);
	EXPECT_TRUE(result.source_retired);
	EXPECT_FALSE(result.source_released);
	EXPECT_EQ(result.next_lsn, LogSequenceNumber(6U));
	EXPECT_EQ(entities.readNode(oldSnapshot, 1000U), sourceNode);
	EXPECT_FALSE(coordinator.releaseRetiredEntityPool(SegmentId(71U), Generation(1U), LogSequenceNumber(6U)));
	oldSnapshot.release();
	EXPECT_TRUE(coordinator.releaseRetiredEntityPool(SegmentId(71U), Generation(1U), LogSequenceNumber(6U)));
	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(current, PhysicalSlotId(40U)));
	EXPECT_EQ(entities.readNode(current, 1000U), sourceNode);
}

TEST_F(RelocatableCompactionCoordinatorTest, LeavesEntityPoolUntouchedWhenDestinationIsFull) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	EntityPageStore entities(io, *metadata);
	const NodeEntityRecord sourceNode = entityNode(11U, 12U);
	ASSERT_TRUE(entities.allocateNode(entityPoolRequest(SegmentId(71U), 17U, PageId(101U), PhysicalSlotId(40U)),
									  1000U, sourceNode, LogSequenceNumber(2U))
						.has_value());
	EntityPageInsertResult destination =
			entities
					.allocateNode(entityPoolRequest(SegmentId(72U), 17U, PageId(201U), PhysicalSlotId(41U)), 2000U,
								  entityNode(21U, 22U), LogSequenceNumber(3U))
					.value();
	CatalogEntry destinationPool = destination.pool;
	for (uint64_t frame = 1U; frame < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
		destination = entities.appendNodePage({destinationPool, PageId(201U + frame)}, 2000U + frame,
													 entityNode(30U + frame, 40U + frame), LogSequenceNumber(3U + frame));
		destinationPool = destination.pool;
	}
	RelocatableCompactionCoordinator coordinator(pages, entities, *metadata);
	const RelocatableSlottedPoolCompactionResult result = coordinator.compactEntityPool(
			{SegmentId(71U), SegmentId(72U), 1U, LogSequenceNumber(4U + RSS_PAGE_POOL_DATA_FRAME_COUNT)});
	EXPECT_EQ(result.status, RelocatableCompactionStatus::VCS_NO_DESTINATION_CAPACITY);
	EXPECT_EQ(result.moved_pages, 0U);
	EXPECT_FALSE(result.source_retired);
	EXPECT_FALSE(result.source_released);
	EXPECT_EQ(result.next_lsn, LogSequenceNumber(4U + RSS_PAGE_POOL_DATA_FRAME_COUNT));
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(snapshot, SegmentId(71U)).physical_slot, PhysicalSlotId(40U));
	EXPECT_EQ(metadata->findCatalog(snapshot, SegmentId(71U)).state, SegmentState::SGS_SEALED);
	EXPECT_EQ(metadata->findPageLocation(snapshot, PageId(101U)).segment_id, SegmentId(71U));
	EXPECT_EQ(entities.readNode(snapshot, 1000U), sourceNode);
}

TEST_F(RelocatableCompactionCoordinatorTest, RejectsInvalidEntityPoolCompactionDependenciesAndRequests) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	PhysicalSegmentAllocator physical(io, *metadata);
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(entityPoolRequest(SegmentId(71U), 17U, PageId(101U), PhysicalSlotId(40U)),
									  1000U, entityNode(11U, 12U), LogSequenceNumber(2U))
						.has_value());
	ASSERT_TRUE(entities.allocateNode(entityPoolRequest(SegmentId(72U), 17U, PageId(201U), PhysicalSlotId(41U)),
									  2000U, entityNode(21U, 22U), LogSequenceNumber(3U))
						.has_value());
	RelocatableCompactionCoordinator coordinator(pages, entities, *metadata);
	expectFormatError(
			[&] {
				static_cast<void>(coordinator.compactEntityPool({{}, SegmentId(72U), 1U, LogSequenceNumber(4U)}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						coordinator.compactEntityPool({SegmentId(71U), SegmentId(71U), 1U, LogSequenceNumber(4U)}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						coordinator.compactEntityPool({SegmentId(71U), SegmentId(72U), 1U, LogSequenceNumber{}}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(coordinator.compactEntityPool(
						{SegmentId(71U), SegmentId(72U), 1U, LogSequenceNumber(std::numeric_limits<uint64_t>::max() - 2U)}));
			},
			FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError(
			[&] { static_cast<void>(coordinator.releaseRetiredEntityPool({}, Generation(1U), LogSequenceNumber(4U))); },
			FormatError::FERR_INVALID_HEADER);

	RelocatableCompactionCoordinator pagesOnly(pages, *metadata);
	expectFormatError(
			[&] {
				static_cast<void>(pagesOnly.compactEntityPool({SegmentId(71U), SegmentId(72U), 1U, LogSequenceNumber(4U)}));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(pagesOnly.compactBTreePool({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(3)}));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(pagesOnly.compactVectorPool({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(3)}));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(
						pagesOnly.compactVectorManifestPool({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(3)}));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(pagesOnly.compactCodebookExtent(
						{17, PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3)}));
			},
			FormatError::FERR_INVALID_STATE);
	SlottedPage property = SlottedPage::create(propertyOwner(PageId(301U), LogSequenceNumber(4U)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(73U), PageId(301U)), property, PhysicalSlotId(42U),
								   PhysicalSlotId(42U), LogSequenceNumber(4U))
						.has_value());
	expectFormatError(
			[&] {
				static_cast<void>(coordinator.compactEntityPool({SegmentId(73U), SegmentId(72U), 1U, LogSequenceNumber(5U)}));
			},
			FormatError::FERR_INVALID_STATE);
	auto foreignMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool foreignPages(io, *foreignMetadata);
	PhysicalSegmentAllocator foreignPhysical(io, *foreignMetadata);
	EntityPageStore foreignEntities(io, *foreignMetadata);
	BTreePageStore foreignBtreePages(foreignPages, *foreignMetadata);
	BTreePageStore currentBtreePages(pages, *metadata);
	VectorPageStore foreignVectorPages(io, *foreignMetadata);
	VectorPageStore currentVectorPages(io, *metadata);
	VectorManifestStore foreignManifests(io, *foreignMetadata);
	VectorManifestStore currentManifests(io, *metadata);
	VectorCodebookStore foreignCodebooks(foreignPhysical, *foreignMetadata);
	VectorCodebookStore currentCodebooks(physical, *metadata);
	LargeValueStore foreignLargeValues(io, *foreignMetadata);
	LargeValueStore currentLargeValues(io, *metadata);
	EXPECT_THROW(static_cast<void>(RelocatableCompactionCoordinator(foreignPages, currentBtreePages, *metadata)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(RelocatableCompactionCoordinator(pages, foreignBtreePages, *metadata)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(RelocatableCompactionCoordinator(foreignPages, entities, *metadata)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(RelocatableCompactionCoordinator(pages, foreignEntities, *metadata)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(RelocatableCompactionCoordinator(foreignPages, currentVectorPages, *metadata)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(RelocatableCompactionCoordinator(pages, foreignVectorPages, *metadata)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(RelocatableCompactionCoordinator(foreignPages, currentManifests, *metadata)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(RelocatableCompactionCoordinator(pages, foreignManifests, *metadata)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(RelocatableCompactionCoordinator(foreignPages, currentCodebooks, physical, *metadata)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(RelocatableCompactionCoordinator(pages, foreignCodebooks, physical, *metadata)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(RelocatableCompactionCoordinator(foreignPages, currentLargeValues, *metadata)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(RelocatableCompactionCoordinator(pages, foreignLargeValues, *metadata)),
				 std::invalid_argument);
}
