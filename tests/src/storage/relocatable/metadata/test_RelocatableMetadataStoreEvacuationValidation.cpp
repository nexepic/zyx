/**
 * @file test_RelocatableMetadataStoreEvacuationValidation.cpp
 * @brief Fail-closed validation for evacuated relocatable metadata roots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"

TEST_F(RelocatableMetadataStoreTest, ReopenRejectsCorruptedTypedDirectoryRootsAfterMetadataEvacuation) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	// Allocate durable ownership for enough ordinary segments to force a second
	// metadata pool. Unlike a raw Catalog fixture, this keeps Catalog and the
	// Allocator Directory mutually consistent before typed-pool recovery runs.
	std::vector<CatalogSlotReservationRequest> requests = reservationRequests(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 30U);
	for (CatalogSlotReservationRequest &request: requests) {
		// These reservations exercise metadata-tree growth only. They intentionally
		// do not materialize typed page-pool bytes, so they must not claim the
		// shared Entity page-pool kind that startup reconciliation validates.
		request.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
	}
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(requests, PhysicalSlotId(200), PhysicalSlotId(229),
														LogSequenceNumber(2))
						.has_value());

	SlottedPagePool slottedPages(io, *store);
	BTreePageStore indexes(slottedPages, *store);
	const SlottedPagePoolAllocationRequest indexPageRequest{
			.segment_id = SegmentId(100),
			.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL,
			.pool_owner_id = 17,
			.generation = Generation(1),
			.page_id = PageId(101),
			.page_owner_id = 17,
			.page_owner_kind = PageOwnerKind::POK_INDEX,
	};
	const PageOwnerHeader indexOwner{
			.page_id = indexPageRequest.page_id,
			.owner_id = indexPageRequest.page_owner_id,
			.kind = indexPageRequest.page_owner_kind,
			.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
			.generation = indexPageRequest.generation,
			.committed_lsn = LogSequenceNumber(3),
	};
	const BTreePageHeader indexHeader{
			.index_id = indexPageRequest.page_owner_id,
			.page_id = indexPageRequest.page_id,
			.generation = indexPageRequest.generation,
	};
	ASSERT_TRUE(indexes.allocate(indexPageRequest, BTreePage::create(indexOwner, indexHeader), PhysicalSlotId(40),
								 PhysicalSlotId(40), LogSequenceNumber(3))
						.has_value());
	const IndexRootLocation indexRoot{17, indexPageRequest.page_id, indexPageRequest.generation};
	static_cast<void>(indexes.publishRoots(std::span(&indexRoot, 1), {}, LogSequenceNumber(4)));

	CatalogSlotReservationRequest retiredRequest = reservationRequest(101, 1);
	retiredRequest.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(retiredRequest, PhysicalSlotId(41), PhysicalSlotId(41),
														LogSequenceNumber(5))
						.has_value());
	static_cast<void>(store->retireCatalogSegment(retiredRequest.segment_id, retiredRequest.segment_generation,
												  LogSequenceNumber(6)));

	const MetadataPagePoolEvacuation evacuation =
			store->evacuateSparseMetadataPagePool(LogSequenceNumber(7), RSS_PAGE_POOL_DATA_FRAME_COUNT);
	ASSERT_TRUE(evacuation.source_pool_slot.isValid());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock evacuated = snapshot.superblock();
	ASSERT_TRUE(evacuated.index_root_directory_root.isValid());
	ASSERT_TRUE(evacuated.retire_queue_root.isValid());
	snapshot.release();
	store.reset();

	const MetadataPageFile pages(io, evacuated);
	const auto assertCorruptionFailsClosed = [&](PhysicalPageRef root) {
		std::array<std::byte, RSS_PAGE_SIZE> original{};
		const uint64_t offset = pages.pageOffset(root);
		ASSERT_EQ(io->readAt(offset, original.data(), original.size()), original.size());
		std::array<std::byte, RSS_PAGE_SIZE> corrupted = original;
		// A changed page byte leaves the stored page checksum stale.
		corrupted[0] ^= std::byte{0x01};
		io->writeAt(offset, corrupted.data(), corrupted.size());
		io->sync();
		expectFormatError([&] { static_cast<void>(RelocatableMetadataStore::open(io)); },
						  FormatError::FERR_CHECKSUM_MISMATCH);
		io->writeAt(offset, original.data(), original.size());
		io->sync();
	};

	assertCorruptionFailsClosed(evacuated.index_root_directory_root);
	assertCorruptionFailsClosed(evacuated.retire_queue_root);
}
