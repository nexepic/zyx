/**
 * @file test_SlottedPagePool.cpp
 * @brief Durable materialization tests for relocatable typed slotted pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/query/execution/PrepaidMemoryResource.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"

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

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		return result;
	}

	SlottedPagePoolAllocationRequest request(uint64_t segmentId, uint64_t pageId) {
		SlottedPagePoolAllocationRequest result;
		result.segment_id = SegmentId(segmentId);
		result.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
		result.pool_owner_id = 41;
		result.generation = Generation(1);
		result.page_id = PageId(pageId);
		result.page_owner_id = 99;
		result.page_owner_kind = PageOwnerKind::POK_PROPERTY;
		return result;
	}

	void overwritePageOwner(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata, PageId pageId,
							const PageOwnerHeader &owner) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																									 pool.physical_slot) +
									static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		snapshot.release();
		const auto bytes = PageOwnerHeaderCodec::encode(owner);
		io->writeAt(offset, bytes.data(), bytes.size());
	}

} // namespace

class RelocatableSlottedPagePoolTest : public ::testing::Test {
protected:
	std::filesystem::path dataPath;
	std::shared_ptr<std::fstream> dataFile;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dataPath = std::filesystem::temp_directory_path() / ("relocatable_slotted_pool_" + to_string(uuid) + ".dat");
		dataFile = std::make_shared<std::fstream>(dataPath,
												  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(dataFile->is_open());
		io = std::make_shared<StorageIO>(dataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		if (dataFile && dataFile->is_open()) {
			dataFile->close();
		}
		dataFile.reset();
		std::error_code error;
		std::filesystem::remove(dataPath, error);
	}
};

TEST_F(RelocatableSlottedPagePoolTest, AtomicallyMaterializesAndReopensAnEmptyTypedPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	const std::optional<SlottedPagePoolAllocation> allocation =
			pool.allocateEmptyPage(request(51, 101), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2));
	ASSERT_TRUE(allocation.has_value());
	EXPECT_EQ(allocation->allocation.location.page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME);
	EXPECT_EQ(allocation->allocation.owner.payload_bytes, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
	EXPECT_EQ(allocation->page.stats().live_slot_count, 0U);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SlottedPage persisted = pool.read(snapshot, PageId(101));
	EXPECT_EQ(persisted.pageId(), PageId(101));
	EXPECT_EQ(persisted.pageGeneration(), Generation(1));
	EXPECT_EQ(persisted.stats().contiguous_free_bytes,
			  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES);
	const std::array value = {std::byte{0xAB}, std::byte{0xCD}};
	const SlottedSlotReference reference = persisted.insert(value);
	const SharedPagePoolAllocation replacement = pool.replacePage(persisted, LogSequenceNumber(3));
	EXPECT_EQ(replacement.location.page_index, 2U);
	EXPECT_EQ(pool.read(snapshot, PageId(101)).stats().live_slot_count, 0U);
	MetadataSnapshotPin currentSnapshot = metadata->openSnapshot();
	SlottedPage current = pool.read(currentSnapshot, PageId(101));
	EXPECT_EQ(current.read(reference), std::vector<std::byte>(value.begin(), value.end()));
	expectFormatError([&] { static_cast<void>(pool.replacePage(persisted, LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);
	snapshot.release();
	currentSnapshot.release();
	pool.reclaimRetiredFrames(SegmentId(51));

	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	MetadataSnapshotPin reopenedSnapshot = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedPool.read(reopenedSnapshot, PageId(101)).read(reference),
			  std::vector<std::byte>(value.begin(), value.end()));
}

TEST_F(RelocatableSlottedPagePoolTest, PinsBorrowedSlottedRecordsWhenTheBufferPoolIsAvailable) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(
			pool.allocateEmptyPage(request(52U, 102U), PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(2U))
					.has_value());
	MetadataSnapshotPin beforeUpdate = metadata->openSnapshot();
	SlottedPage writable = pool.read(beforeUpdate, PageId(102U));
	const std::array value = {std::byte{0x11U}, std::byte{0x22U}, std::byte{0x33U}};
	const SlottedSlotReference reference = writable.insert(value);
	static_cast<void>(pool.replacePage(writable, LogSequenceNumber(3U)));
	beforeUpdate.release();

	io->sync();
	io->enableBufferPool(8U * RSS_PAGE_SIZE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	{
		const std::optional<SlottedPagePinnedRead> pinned = pool.readPinned(snapshot, PageId(102U));
		ASSERT_TRUE(pinned.has_value());
		const std::span<const std::byte> borrowed = pinned->page.read(reference);
		EXPECT_EQ(std::vector<std::byte>(borrowed.begin(), borrowed.end()),
				  std::vector<std::byte>(value.begin(), value.end()));
		EXPECT_EQ(pinned->page.pageId(), PageId(102U));
	}
	snapshot.release();
}

TEST_F(RelocatableSlottedPagePoolTest, AtomicallyMaterializesAPopulatedPageOnlyAfterItsDirectoryPublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	const SlottedPagePoolAllocationRequest allocation = request(58, 108);
	PageOwnerHeader owner;
	owner.page_id = allocation.page_id;
	owner.owner_id = allocation.page_owner_id;
	owner.kind = allocation.page_owner_kind;
	owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	owner.generation = allocation.generation;
	owner.committed_lsn = LogSequenceNumber(2);
	SlottedPage populated = SlottedPage::create(owner);
	const std::array value = {std::byte{0x91}, std::byte{0x92}, std::byte{0x93}};
	const SlottedSlotReference reference = populated.insert(value);

	MetadataSnapshotPin beforePublication = metadata->openSnapshot();
	const std::optional<SlottedPagePoolAllocation> published =
			pool.allocatePage(allocation, populated, PhysicalSlotId(45), PhysicalSlotId(45), LogSequenceNumber(2));
	ASSERT_TRUE(published.has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(beforePublication, allocation.page_id).has_value());
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(pool.read(current, allocation.page_id).read(reference),
			  std::vector<std::byte>(value.begin(), value.end()));
	beforePublication.release();
	current.release();

	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedPool.read(reopened, allocation.page_id).read(reference),
			  std::vector<std::byte>(value.begin(), value.end()));
}

TEST_F(RelocatableSlottedPagePoolTest, RejectsMismatchedOrUnreservablePopulatedPageAllocation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	const SlottedPagePoolAllocationRequest allocation = request(59, 109);
	PageOwnerHeader owner;
	owner.page_id = allocation.page_id;
	owner.owner_id = allocation.page_owner_id;
	owner.kind = allocation.page_owner_kind;
	owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	owner.generation = allocation.generation;
	owner.committed_lsn = LogSequenceNumber(2);
	const SlottedPage populated = SlottedPage::create(owner);

	SlottedPagePoolAllocationRequest mismatched = allocation;
	mismatched.page_owner_id = allocation.page_owner_id + 1U;
	expectFormatError(
			[&] {
				static_cast<void>(pool.allocatePage(mismatched, populated, PhysicalSlotId(46), PhysicalSlotId(46),
													LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin absent = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findPageLocationOptional(absent, allocation.page_id).has_value());
	absent.release();

	ASSERT_TRUE(pool.allocatePage(allocation, populated, PhysicalSlotId(46), PhysicalSlotId(46), LogSequenceNumber(2))
						.has_value());
	SlottedPagePoolAllocationRequest exhausted = request(60, 110);
	owner.page_id = exhausted.page_id;
	owner.committed_lsn = LogSequenceNumber(3);
	const SlottedPage next = SlottedPage::create(owner);
	EXPECT_FALSE(pool.allocatePage(exhausted, next, PhysicalSlotId(46), PhysicalSlotId(46), LogSequenceNumber(3))
						 .has_value());
}

TEST_F(RelocatableSlottedPagePoolTest, AppendsAPopulatedPageOnlyWhenItsOwnerMatchesTheRequest) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(pool.allocateEmptyPage(request(51, 101), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	SlottedPagePoolAppendRequest append;
	append.segment_id = SegmentId(51);
	append.generation = Generation(1);
	append.page_id = PageId(102);
	append.page_owner_id = 99;
	append.page_owner_kind = PageOwnerKind::POK_PROPERTY;
	PageOwnerHeader owner;
	owner.page_id = append.page_id;
	owner.owner_id = append.page_owner_id;
	owner.kind = append.page_owner_kind;
	owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	owner.generation = append.generation;
	owner.committed_lsn = LogSequenceNumber(3);
	SlottedPage populated = SlottedPage::create(owner);
	const std::array value = {std::byte{0xAB}, std::byte{0xCD}};
	const SlottedSlotReference reference = populated.insert(value);
	SlottedPagePoolAppendRequest wrong = append;
	wrong.page_id = PageId(103);
	expectFormatError([&] { static_cast<void>(pool.appendPage(wrong, populated, LogSequenceNumber(3))); },
					  FormatError::FERR_INVALID_STATE);
	const SharedPagePoolAllocation allocation = pool.appendPage(append, populated, LogSequenceNumber(3));
	EXPECT_EQ(allocation.location.page_index, 2U);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(pool.read(snapshot, append.page_id).read(reference), std::vector<std::byte>(value.begin(), value.end()));
}

TEST_F(RelocatableSlottedPagePoolTest, ReplacesTypedPagesAcrossRepeatedReclamationAndReopen) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(pool.allocateEmptyPage(request(51, 101), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	std::vector<SlottedSlotReference> references;
	std::vector<std::byte> expected;
	for (uint64_t lsn = 3; lsn <= 40; ++lsn) {
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		SlottedPage page = pool.read(snapshot, PageId(101));
		const std::byte value = std::byte{static_cast<unsigned char>(lsn)};
		references.push_back(page.insert(std::span(&value, 1)));
		expected.push_back(value);
		snapshot.release();
		const SharedPagePoolAllocation replacement = pool.replacePage(page, LogSequenceNumber(lsn));
		EXPECT_EQ(replacement.location.page_index, lsn % 2U == 0U ? 1U : 2U);
		pool.reclaimRetiredFrames(SegmentId(51));
	}

	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	MetadataSnapshotPin snapshot = reopenedMetadata->openSnapshot();
	SlottedPage reopened = reopenedPool.read(snapshot, PageId(101));
	EXPECT_EQ(reopened.stats().live_slot_count, expected.size());
	for (size_t index = 0; index < references.size(); ++index) {
		EXPECT_EQ(reopened.read(references[index]), std::vector<std::byte>{expected[index]});
	}
}

TEST_F(RelocatableSlottedPagePoolTest, PersistsAndSelectsBestFitContiguousFreeSpaceAcrossCowAndReopen) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	const std::optional<SlottedPagePoolAllocation> first =
			pool.allocateEmptyPage(request(51, 101), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2));
	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(first->allocation.location.free_bytes, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES);

	SlottedPagePoolAppendRequest append;
	append.segment_id = SegmentId(51);
	append.generation = Generation(1);
	append.page_id = PageId(102);
	append.page_owner_id = 100;
	append.page_owner_kind = PageOwnerKind::POK_PROPERTY;
	const SlottedPagePoolAllocation second = pool.appendEmptyPage(append, LogSequenceNumber(3));
	EXPECT_EQ(second.allocation.location.page_index, 2U);
	EXPECT_EQ(second.allocation.owner.owner_id, 100U);

	EXPECT_EQ(pool.findBestFitPage(SegmentId(51), 3900), std::optional<PageId>(PageId(101)));
	MetadataSnapshotPin indexedSnapshot = metadata->openSnapshot();
	const PageLocation indexedLocation = metadata->findPageLocation(indexedSnapshot, PageId(101));
	indexedSnapshot.release();
	static_cast<void>(metadata->upsertLocations(std::span(&indexedLocation, 1), {}, LogSequenceNumber(4)));
	// The selector sees the changed Page Directory root and rebuilds instead of
	// using a cache from the previous metadata generation.
	EXPECT_EQ(pool.findBestFitPage(SegmentId(51), 3900), std::optional<PageId>(PageId(101)));
	static_cast<void>(metadata->upsertLocations(std::span(&indexedLocation, 1), {}, LogSequenceNumber(5)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SlottedPage firstPage = pool.read(snapshot, PageId(101));
	const std::vector<std::byte> largeValue(300, std::byte{0xA1});
	static_cast<void>(firstPage.insert(largeValue));
	snapshot.release();
	const SharedPagePoolAllocation replacement = pool.replacePage(firstPage, LogSequenceNumber(6));
	EXPECT_LT(replacement.location.free_bytes, 3900U);
	EXPECT_EQ(pool.findBestFitPage(SegmentId(51), 3900), std::optional<PageId>(PageId(102)));
	expectFormatError([&] { static_cast<void>(pool.findBestFitPage(SegmentId{}, 1)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(pool.findBestFitPage(SegmentId(51), 3969)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(pool.findBestFitPage(SegmentId(51), 0)); },
					  FormatError::FERR_INVALID_HEADER);
	MetadataSnapshotPin secondSnapshot = metadata->openSnapshot();
	SlottedPage secondPage = pool.read(secondSnapshot, PageId(102));
	static_cast<void>(secondPage.insert(largeValue));
	secondSnapshot.release();
	static_cast<void>(pool.replacePage(secondPage, LogSequenceNumber(7)));
	EXPECT_FALSE(pool.findBestFitPage(SegmentId(51), 3900).has_value());
	EXPECT_EQ(pool.findBestFitPage(SegmentId(51), 3000), std::optional<PageId>(PageId(101)));

	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	EXPECT_FALSE(reopenedPool.findBestFitPage(SegmentId(51), 3900).has_value());
	MetadataSnapshotPin reopenedSnapshot = reopenedMetadata->openSnapshot();
	SlottedPage reopened = reopenedPool.read(reopenedSnapshot, PageId(101));
	EXPECT_EQ(reopened.stats().contiguous_free_bytes, replacement.location.free_bytes);
}

TEST_F(RelocatableSlottedPagePoolTest, RetainsPeerWhenReplacingOnePageInASharedFreeSpaceBucket) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(
			pool.allocateEmptyPage(request(51U, 101U), PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
					.has_value());

	SlottedPagePoolAppendRequest append;
	append.segment_id = SegmentId(51U);
	append.generation = Generation(1U);
	append.page_id = PageId(102U);
	append.page_owner_id = 100U;
	append.page_owner_kind = PageOwnerKind::POK_PROPERTY;
	static_cast<void>(pool.appendEmptyPage(append, LogSequenceNumber(3U)));

	// Seed one free-space bucket containing both empty pages. Replacing page 101
	// must remove only its entry, not discard page 102 from the same bucket.
	EXPECT_EQ(pool.findBestFitPage(SegmentId(51U), 1U), std::optional<PageId>(PageId(101U)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SlottedPage first = pool.read(snapshot, PageId(101U));
	static_cast<void>(first.insert(std::vector<std::byte>(128U, std::byte{0xA5U})));
	snapshot.release();
	const SharedPagePoolAllocation replacement = pool.replacePage(first, LogSequenceNumber(4U));

	EXPECT_EQ(pool.findBestFitPage(SegmentId(51U), replacement.location.free_bytes + 1U),
			  std::optional<PageId>(PageId(102U)));
}

TEST_F(RelocatableSlottedPagePoolTest, RejectsOutOfRangeDurableFreeSpaceHintsBeforeSelection) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(pool.allocateEmptyPage(request(55, 105), PhysicalSlotId(42), PhysicalSlotId(42), LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(pool.availableFrameCount(snapshot, SegmentId{})); },
					  FormatError::FERR_INVALID_HEADER);
	PageLocation invalid = metadata->findPageLocation(snapshot, PageId(105));
	snapshot.release();
	invalid.free_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES + 1U;
	static_cast<void>(metadata->upsertLocations(std::span(&invalid, 1), {}, LogSequenceNumber(3)));
	SlottedPagePool reopenedSelector(io, *metadata);
	expectFormatError([&] { static_cast<void>(reopenedSelector.findBestFitPage(SegmentId(55), 1)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSlottedPagePoolTest, SelectsTheBestFitPageAcrossPoolsFromOneMetadataSnapshot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	SlottedPagePoolAllocationRequest firstRequest = request(60, 160);
	SlottedPagePoolAllocationRequest secondRequest = request(61, 161);
	ASSERT_TRUE(pool.allocateEmptyPage(firstRequest, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	ASSERT_TRUE(pool.allocateEmptyPage(secondRequest, PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());
	const std::array pools = {SegmentId(60), SegmentId(61)};
	EXPECT_EQ(pool.findBestFitPage(pools, 3900),
			  std::optional<SlottedPagePlacement>(SlottedPagePlacement{SegmentId(60), PageId(160), 3968}));
	const std::array reversePools = {SegmentId(61), SegmentId(60)};
	EXPECT_EQ(pool.findBestFitPage(reversePools, 3900),
			  std::optional<SlottedPagePlacement>(SlottedPagePlacement{SegmentId(60), PageId(160), 3968}));
	EXPECT_EQ(pool.findBestFitPage(pools, PageOwnerKind::POK_PROPERTY, 3900),
			  std::optional<SlottedPagePlacement>(SlottedPagePlacement{SegmentId(60), PageId(160), 3968}));
	EXPECT_EQ(pool.findBestFitPage(reversePools, PageOwnerKind::POK_PROPERTY, 3900),
			  std::optional<SlottedPagePlacement>(SlottedPagePlacement{SegmentId(60), PageId(160), 3968}));
	EXPECT_FALSE(pool.findBestFitPage(pools, PageOwnerKind::POK_INDEX, 1U).has_value());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SlottedPage page = pool.read(snapshot, PageId(160));
	const std::vector<std::byte> value(300, std::byte{0xB2});
	static_cast<void>(page.insert(value));
	snapshot.release();
	static_cast<void>(pool.replacePage(page, LogSequenceNumber(4)));
	const std::optional<SlottedPagePlacement> selected = pool.findBestFitPage(pools, 3000);
	ASSERT_TRUE(selected.has_value());
	EXPECT_EQ(selected->segment_id, SegmentId(60));
	EXPECT_EQ(selected->page_id, PageId(160));
	EXPECT_LT(selected->contiguous_free_bytes, 3900U);
	// Visit the looser candidate first, then prove the tighter later candidate
	// replaces it regardless of the caller-provided pool order.
	EXPECT_EQ(pool.findBestFitPage(reversePools, 3000),
			  std::optional<SlottedPagePlacement>(
					  SlottedPagePlacement{SegmentId(60), PageId(160), selected->contiguous_free_bytes}));
	EXPECT_EQ(pool.findBestFitPage(pools, 3900),
			  std::optional<SlottedPagePlacement>(SlottedPagePlacement{SegmentId(61), PageId(161), 3968}));
	expectFormatError([&] { static_cast<void>(pool.findBestFitPage(std::span<const SegmentId>{}, 1)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(pool.findBestFitPage(pools, 0)); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(pool.findBestFitPage(pools, 3969)); }, FormatError::FERR_INVALID_HEADER);
	const std::array invalidPools = {SegmentId{}};
	expectFormatError([&] { static_cast<void>(pool.findBestFitPage(invalidPools, 1)); },
					  FormatError::FERR_INVALID_HEADER);

	MetadataSnapshotPin secondSnapshot = metadata->openSnapshot();
	SlottedPage secondPage = pool.read(secondSnapshot, PageId(161));
	static_cast<void>(secondPage.insert(std::vector<std::byte>(400, std::byte{0xC3})));
	secondSnapshot.release();
	static_cast<void>(pool.replacePage(secondPage, LogSequenceNumber(5)));
	EXPECT_EQ(pool.findBestFitPage(pools, 3000),
			  std::optional<SlottedPagePlacement>(
					  SlottedPagePlacement{SegmentId(61), PageId(161),
										   RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - 416U}));
}

TEST_F(RelocatableSlottedPagePoolTest, SelectsRecordPlacementUsingReusableDeadSlots) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(
			pool.allocateEmptyPage(request(60U, 160U), PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
					.has_value());
	ASSERT_TRUE(
			pool.allocateEmptyPage(request(61U, 161U), PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(3U))
					.has_value());

	MetadataSnapshotPin seedSnapshot = metadata->openSnapshot();
	SlottedPage seed = pool.read(seedSnapshot, PageId(160U));
	const std::vector<std::byte> emptyValue;
	const SlottedSlotReference deadSlot = seed.insert(emptyValue);
	seed.erase(deadSlot);
	const SlottedPageStats seededStats = seed.stats();
	EXPECT_EQ(seededStats.reusable_free_slot_count, 1U);
	seedSnapshot.release();
	static_cast<void>(pool.replacePage(seed, LogSequenceNumber(4U)));

	const std::array pools = {SegmentId(60U), SegmentId(61U)};
	const std::array reversePools = {SegmentId(61U), SegmentId(60U)};
	const uint32_t recordBytes = seededStats.contiguous_free_bytes;
	const SlottedPagePlacement expected{SegmentId(60U), PageId(160U), seededStats.contiguous_free_bytes};
	EXPECT_EQ(pool.findBestFitPageForRecord(pools, PageOwnerKind::POK_PROPERTY, recordBytes),
			  std::optional<SlottedPagePlacement>(expected));
	// Reversing the candidates proves tie-breaking is independent of caller order.
	EXPECT_EQ(pool.findBestFitPageForRecord(reversePools, PageOwnerKind::POK_PROPERTY, recordBytes),
			  std::optional<SlottedPagePlacement>(expected));
	// A second request stays within the current cache generation while retaining
	// the derived reusable-slot information.
	EXPECT_EQ(pool.findBestFitPageForRecord(pools, PageOwnerKind::POK_PROPERTY, recordBytes),
			  std::optional<SlottedPagePlacement>(expected));
	EXPECT_FALSE(pool.findBestFitPageForRecord(pools, PageOwnerKind::POK_INDEX, 1U).has_value());
	EXPECT_FALSE(pool.findBestFitPageForRecord(pools, PageOwnerKind::POK_PROPERTY, recordBytes + 1U).has_value());

	const std::array<SegmentId, 0U> noPools{};
	const std::array invalidPools = {SegmentId{}};
	expectFormatError(
			[&] { static_cast<void>(pool.findBestFitPageForRecord(noPools, PageOwnerKind::POK_PROPERTY, 1U)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(pool.findBestFitPageForRecord(invalidPools, PageOwnerKind::POK_PROPERTY, 1U)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(pool.findBestFitPageForRecord(pools, PageOwnerKind::POK_PROPERTY, 0U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(pool.findBestFitPageForRecord(pools, PageOwnerKind::POK_PROPERTY,
																RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES -
																		RSS_SLOTTED_PAGE_HEADER_BYTES + 1U));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(pool.findBestFitPageForRecord(pools, PageOwnerKind::POK_ENTITY, 1U)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableSlottedPagePoolTest, SelectsRecordPlacementFromTheCallerSnapshotWithoutDowngradingNewerCache) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(
			pool.allocateEmptyPage(request(60U, 160U), PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
					.has_value());
	MetadataSnapshotPin beforeAppend = metadata->openSnapshot();

	SlottedPagePoolAppendRequest append;
	append.segment_id = SegmentId(60U);
	append.generation = Generation(1U);
	append.page_id = PageId(161U);
	append.page_owner_id = 99U;
	append.page_owner_kind = PageOwnerKind::POK_PROPERTY;
	static_cast<void>(pool.appendEmptyPage(append, LogSequenceNumber(3U)));
	MetadataSnapshotPin current = metadata->openSnapshot();
	SlottedPage first = pool.read(current, PageId(160U));
	static_cast<void>(first.insert(std::vector<std::byte>(3000U, std::byte{0xD5})));
	current.release();
	static_cast<void>(pool.replacePage(first, LogSequenceNumber(4U)));

	const std::array pools = {SegmentId(60U)};
	io->sync();
	io->enableBufferPool(64U * RSS_PAGE_SIZE);
	// Seed the cache from the newest root before consulting the older pin below.
	const std::optional<SlottedPagePlacement> latestBeforePinned =
			pool.findBestFitPageForRecord(pools, PageOwnerKind::POK_PROPERTY, 2000U);
	ASSERT_TRUE(latestBeforePinned.has_value());
	EXPECT_EQ(latestBeforePinned->page_id, PageId(161U));
	const std::optional<SlottedPagePlacement> pinned =
			pool.findBestFitPageForRecord(beforeAppend, pools, PageOwnerKind::POK_PROPERTY, 2000U);
	ASSERT_TRUE(pinned.has_value());
	EXPECT_EQ(pinned->page_id, PageId(160U));
	EXPECT_NO_THROW(static_cast<void>(pool.read(beforeAppend, pinned->page_id)));

	// Drop only physical cache state. The newer free-space index must survive
	// the older pinned selection and avoid rereading the current data frame.
	io->disableBufferPool();
	io->enableBufferPool(64U * RSS_PAGE_SIZE);
	MetadataSnapshotPin warmCurrent = metadata->openSnapshot();
	ASSERT_GT(pool.availableFrameCount(warmCurrent, SegmentId(60U)), 0U);
	warmCurrent.release();
	io->resetBufferPoolStats();
	const std::optional<SlottedPagePlacement> latest =
			pool.findBestFitPageForRecord(pools, PageOwnerKind::POK_PROPERTY, 2000U);
	ASSERT_TRUE(latest.has_value());
	EXPECT_EQ(latest->page_id, PageId(161U));
	EXPECT_EQ(io->bufferPoolStats().disk_read_bytes, 0U);
	beforeAppend.release();
}

TEST_F(RelocatableSlottedPagePoolTest, RebuildsRecordPlacementCacheAfterMetadataOnlyPublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(
			pool.allocateEmptyPage(request(60U, 160U), PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
					.has_value());
	const std::array pools = {SegmentId(60U)};
	const SlottedPagePlacement expected{SegmentId(60U), PageId(160U),
										 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES};
	EXPECT_EQ(pool.findBestFitPageForRecord(pools, PageOwnerKind::POK_PROPERTY, 1U),
			  std::optional<SlottedPagePlacement>(expected));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const PageLocation location = metadata->findPageLocation(snapshot, PageId(160U));
	snapshot.release();
	static_cast<void>(metadata->upsertLocations(std::span(&location, 1U), {}, LogSequenceNumber(3U)));
	EXPECT_EQ(pool.findBestFitPageForRecord(pools, PageOwnerKind::POK_PROPERTY, 1U),
			  std::optional<SlottedPagePlacement>(expected));
}

TEST_F(RelocatableSlottedPagePoolTest, NeverSelectsAVectorRawPoolForSlottedRecordPlacement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	SharedPagePoolAllocator shared(io, *metadata);
	SharedPagePoolAllocationRequest rawPage;
	rawPage.segment_id = SegmentId(70U);
	rawPage.pool_kind = SegmentKind::SGK_VECTOR_RAW_POOL;
	rawPage.pool_owner_id = 17U;
	rawPage.generation = Generation(1U);
	rawPage.page_id = PageId(170U);
	rawPage.page_owner_id = 17U;
	rawPage.page_owner_kind = PageOwnerKind::POK_VECTOR_RAW;
	ASSERT_TRUE(shared.allocateNewPoolPage(rawPage, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
					.has_value());

	SlottedPagePool pool(io, *metadata);
	const std::array pools = {SegmentId(70U)};
	EXPECT_FALSE(pool.findBestFitPageForRecord(pools, PageOwnerKind::POK_PROPERTY, 1U).has_value());
}

TEST_F(RelocatableSlottedPagePoolTest, SelectsTightestRecordPlacementAcrossPoolsRegardlessOfCallerOrder) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(
			pool.allocateEmptyPage(request(62U, 162U), PhysicalSlotId(42U), PhysicalSlotId(42U), LogSequenceNumber(2U))
					.has_value());
	ASSERT_TRUE(
			pool.allocateEmptyPage(request(63U, 163U), PhysicalSlotId(43U), PhysicalSlotId(43U), LogSequenceNumber(3U))
					.has_value());

	MetadataSnapshotPin firstSnapshot = metadata->openSnapshot();
	SlottedPage first = pool.read(firstSnapshot, PageId(162U));
	static_cast<void>(first.insert(std::vector<std::byte>(128U, std::byte{0xD1})));
	const SlottedPageStats firstStats = first.stats();
	firstSnapshot.release();
	static_cast<void>(pool.replacePage(first, LogSequenceNumber(4U)));

	MetadataSnapshotPin secondSnapshot = metadata->openSnapshot();
	SlottedPage second = pool.read(secondSnapshot, PageId(163U));
	static_cast<void>(second.insert(std::vector<std::byte>(320U, std::byte{0xD2})));
	const SlottedPageStats secondStats = second.stats();
	secondSnapshot.release();
	static_cast<void>(pool.replacePage(second, LogSequenceNumber(5U)));
	ASSERT_LT(secondStats.contiguous_free_bytes, firstStats.contiguous_free_bytes);

	const std::array pools = {SegmentId(62U), SegmentId(63U)};
	const std::array reversePools = {SegmentId(63U), SegmentId(62U)};
	const SlottedPagePlacement expected{SegmentId(63U), PageId(163U), secondStats.contiguous_free_bytes};
	EXPECT_EQ(pool.findBestFitPageForRecord(pools, PageOwnerKind::POK_PROPERTY, 64U),
			  std::optional<SlottedPagePlacement>(expected));
	EXPECT_EQ(pool.findBestFitPageForRecord(reversePools, PageOwnerKind::POK_PROPERTY, 64U),
			  std::optional<SlottedPagePlacement>(expected));
}

TEST_F(RelocatableSlottedPagePoolTest, SelectsTightestRecordPlacementWithinOnePool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(
			pool.allocateEmptyPage(request(66U, 166U), PhysicalSlotId(46U), PhysicalSlotId(46U), LogSequenceNumber(2U))
					.has_value());
	for (uint64_t ordinal = 1U; ordinal <= 2U; ++ordinal) {
		SlottedPagePoolAppendRequest append;
		append.segment_id = SegmentId(66U);
		append.generation = Generation(1U);
		append.page_id = PageId(166U + ordinal);
		append.page_owner_id = 99U;
		append.page_owner_kind = PageOwnerKind::POK_PROPERTY;
		static_cast<void>(pool.appendEmptyPage(append, LogSequenceNumber(2U + ordinal)));
	}

	const auto consume = [&pool, &metadata](PageId pageId, size_t bytes, LogSequenceNumber checkpointLsn) {
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		SlottedPage page = pool.read(snapshot, pageId);
		static_cast<void>(page.insert(std::vector<std::byte>(bytes, std::byte{0xD4})));
		const SlottedPageStats stats = page.stats();
		snapshot.release();
		static_cast<void>(pool.replacePage(page, checkpointLsn));
		return stats;
	};
	const SlottedPageStats firstStats = consume(PageId(166U), 128U, LogSequenceNumber(5U));
	const SlottedPageStats tightestStats = consume(PageId(167U), 320U, LogSequenceNumber(6U));
	const SlottedPageStats thirdStats = consume(PageId(168U), 64U, LogSequenceNumber(7U));
	ASSERT_LT(tightestStats.contiguous_free_bytes, firstStats.contiguous_free_bytes);
	ASSERT_LT(tightestStats.contiguous_free_bytes, thirdStats.contiguous_free_bytes);

	const std::array poolIds = {SegmentId(66U)};
	EXPECT_EQ(pool.findBestFitPageForRecord(poolIds, PageOwnerKind::POK_PROPERTY, 64U),
			  std::optional<SlottedPagePlacement>(
					  SlottedPagePlacement{SegmentId(66U), PageId(167U), tightestStats.contiguous_free_bytes}));
}

TEST_F(RelocatableSlottedPagePoolTest, SelectsOnlyMatchingSlottedOwnerKindsInASharedEntityPool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	EntityPageStore entities(io, *metadata);
	EntityPageNewPoolRequest entityRequest;
	entityRequest.segment_id = SegmentId(70);
	entityRequest.pool_owner_id = 41U;
	entityRequest.generation = Generation(1);
	entityRequest.page_id = PageId(170);
	entityRequest.first_slot = PhysicalSlotId(40);
	entityRequest.last_slot = PhysicalSlotId(40);
	const std::optional<EntityPageInsertResult> fixed =
			entities.allocateNode(entityRequest, 1U, NodeEntityRecord{}, LogSequenceNumber(2));
	ASSERT_TRUE(fixed.has_value());

	SlottedPagePool pages(io, *metadata);
	SlottedPagePoolAppendRequest labelRequest;
	labelRequest.segment_id = fixed->pool.segment_id;
	labelRequest.generation = fixed->pool.generation;
	labelRequest.page_id = PageId(171);
	labelRequest.page_owner_id = fixed->pool.owner_id;
	labelRequest.page_owner_kind = PageOwnerKind::POK_ENTITY_LABEL;
	static_cast<void>(pages.appendEmptyPage(labelRequest, LogSequenceNumber(3)));

	EXPECT_EQ(pages.findBestFitPage(fixed->pool.segment_id, PageOwnerKind::POK_ENTITY_LABEL, 3900U),
			  std::optional<PageId>(labelRequest.page_id));
	EXPECT_FALSE(pages.findBestFitPage(fixed->pool.segment_id, PageOwnerKind::POK_PROPERTY, 1U).has_value());
	EXPECT_EQ(pages.findBestFitPage(fixed->pool.segment_id, 3900U), std::optional<PageId>(labelRequest.page_id));
	expectFormatError(
			[&] { static_cast<void>(pages.findBestFitPage(fixed->pool.segment_id, PageOwnerKind::POK_ENTITY, 1U)); },
			FormatError::FERR_INVALID_HEADER);

	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopened(io, *reopenedMetadata);
	EXPECT_EQ(reopened.findBestFitPage(fixed->pool.segment_id, PageOwnerKind::POK_ENTITY_LABEL, 3900U),
			  std::optional<PageId>(labelRequest.page_id));
}

TEST_F(RelocatableSlottedPagePoolTest, EvacuatesTypedPagesAcrossPoolsWithoutChangingSlotReferences) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(pool.allocateEmptyPage(request(60, 160), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	ASSERT_TRUE(pool.allocateEmptyPage(request(61, 161), PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());
	MetadataSnapshotPin updateSnapshot = metadata->openSnapshot();
	SlottedPage updated = pool.read(updateSnapshot, PageId(160));
	const std::array value = {std::byte{0xC1}, std::byte{0xC2}};
	const SlottedSlotReference reference = updated.insert(value);
	updateSnapshot.release();
	static_cast<void>(pool.replacePage(updated, LogSequenceNumber(4)));
	EXPECT_EQ(pool.findBestFitPage(SegmentId(60), 1), std::optional<PageId>(PageId(160)));

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	SlottedPage source = pool.read(oldSnapshot, PageId(160));
	const SharedPagePoolAllocation evacuated = pool.evacuatePage(source, SegmentId(61), LogSequenceNumber(5));
	EXPECT_EQ(evacuated.pool.segment_id, SegmentId(61));
	EXPECT_EQ(pool.read(oldSnapshot, PageId(160)).read(reference), std::vector<std::byte>(value.begin(), value.end()));
	MetadataSnapshotPin currentSnapshot = metadata->openSnapshot();
	SlottedPage current = pool.read(currentSnapshot, PageId(160));
	EXPECT_EQ(current.read(reference), std::vector<std::byte>(value.begin(), value.end()));
	EXPECT_EQ(current.pageGeneration(), Generation(1));
	EXPECT_EQ(pool.findBestFitPage(SegmentId(60), 1), std::nullopt);
	EXPECT_EQ(pool.findBestFitPage(SegmentId(61), 1), std::optional<PageId>(PageId(160)));
	oldSnapshot.release();
	currentSnapshot.release();
	pool.reclaimRetiredFrames(SegmentId(60));
	SlottedPagePoolAppendRequest append;
	append.segment_id = SegmentId(60);
	append.generation = Generation(1);
	append.page_id = PageId(162);
	append.page_owner_id = 101;
	append.page_owner_kind = PageOwnerKind::POK_PROPERTY;
	EXPECT_EQ(pool.appendEmptyPage(append, LogSequenceNumber(6)).allocation.location.page_index,
			  RSS_PAGE_POOL_FIRST_DATA_FRAME);
}

TEST_F(RelocatableSlottedPagePoolTest, RejectsInvalidOrNonCurrentTypedPageEvacuations) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(pool.allocateEmptyPage(request(60, 170), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	ASSERT_TRUE(pool.allocateEmptyPage(request(61, 171), PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());
	SlottedPage unowned = SlottedPage::create(PageId(170), Generation(1));
	expectFormatError([&] { static_cast<void>(pool.evacuatePage(unowned, SegmentId(61), LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SlottedPage current = pool.read(snapshot, PageId(170));
	expectFormatError([&] { static_cast<void>(pool.evacuatePage(current, SegmentId{}, LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(pool.evacuatePage(current, SegmentId(60), LogSequenceNumber(4))); },
				  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSlottedPagePoolTest, RejectsEvacuationOfAPageSupersededAfterItWasRead) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(
			pool.allocateEmptyPage(request(64U, 164U), PhysicalSlotId(44U), PhysicalSlotId(44U), LogSequenceNumber(2U))
					.has_value());
	ASSERT_TRUE(
			pool.allocateEmptyPage(request(65U, 165U), PhysicalSlotId(45U), PhysicalSlotId(45U), LogSequenceNumber(3U))
					.has_value());

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	const SlottedPage stale = pool.read(oldSnapshot, PageId(164U));
	MetadataSnapshotPin updateSnapshot = metadata->openSnapshot();
	SlottedPage replacement = pool.read(updateSnapshot, PageId(164U));
	const std::array value = {std::byte{0xD3}};
	const SlottedSlotReference reference = replacement.insert(value);
	updateSnapshot.release();
	static_cast<void>(pool.replacePage(replacement, LogSequenceNumber(4U)));

	expectFormatError([&] { static_cast<void>(pool.evacuatePage(stale, SegmentId(65U), LogSequenceNumber(5U))); },
				  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(pool.read(oldSnapshot, PageId(164U)).stats().live_slot_count, 0U);
	oldSnapshot.release();
	MetadataSnapshotPin currentSnapshot = metadata->openSnapshot();
	EXPECT_EQ(pool.read(currentSnapshot, PageId(164U)).read(reference),
			  std::vector<std::byte>(value.begin(), value.end()));
}

TEST_F(RelocatableSlottedPagePoolTest, RejectsReplacementAndEvacuationAfterTheCurrentOwnerIsRetired) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(
			pool.allocateEmptyPage(request(64U, 164U), PhysicalSlotId(44U), PhysicalSlotId(44U), LogSequenceNumber(2U))
					.has_value());
	ASSERT_TRUE(
			pool.allocateEmptyPage(request(65U, 165U), PhysicalSlotId(45U), PhysicalSlotId(45U), LogSequenceNumber(3U))
					.has_value());

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const SlottedPage stalePage = pool.read(snapshot, PageId(164U));
	snapshot.release();
	PageOwnerHeader retiredOwner{.page_id = PageId(164U),
								 .owner_id = 99U,
								 .kind = PageOwnerKind::POK_PROPERTY,
								 .flags = RSS_PAGE_OWNER_FLAG_RETIRED,
								 .payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								 .generation = Generation(1U),
								 .committed_lsn = LogSequenceNumber(2U)};
	overwritePageOwner(io, *metadata, PageId(164U), retiredOwner);

	expectFormatError([&] { static_cast<void>(pool.replacePage(stalePage, LogSequenceNumber(4U))); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(pool.evacuatePage(stalePage, SegmentId(65U), LogSequenceNumber(4U))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSlottedPagePoolTest, RetiresAndReleasesAnEmptyTypedPool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(pool.allocateEmptyPage(request(60, 180), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	ASSERT_TRUE(pool.allocateEmptyPage(request(61, 181), PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SlottedPage source = pool.read(snapshot, PageId(180));
	snapshot.release();
	static_cast<void>(pool.evacuatePage(source, SegmentId(61), LogSequenceNumber(4)));
	static_cast<void>(pool.retireEmptyPool(SegmentId(60), Generation(1), LogSequenceNumber(5)));
	static_cast<void>(pool.releaseRetiredEmptyPool(SegmentId(60), Generation(1), LogSequenceNumber(6)));
	MetadataSnapshotPin releasedSnapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(releasedSnapshot, PhysicalSlotId(40)));
}

TEST_F(RelocatableSlottedPagePoolTest, RejectsWrongTypedOwnerAndNeverPublishesRawPagePayloadAsSlotted) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	SlottedPagePoolAllocationRequest rawVector = request(52, 102);
	rawVector.pool_kind = SegmentKind::SGK_VECTOR_RAW_POOL;
	rawVector.page_owner_kind = PageOwnerKind::POK_VECTOR_RAW;
	expectFormatError(
			[&] {
				static_cast<void>(pool.allocateEmptyPage(rawVector, PhysicalSlotId(41), PhysicalSlotId(41),
														 LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findPageLocationOptional(snapshot, PageId(102)).has_value());
	snapshot.release();

	SharedPagePoolAllocator genericPool(io, *metadata);
	SharedPagePoolAllocationRequest generic;
	generic.segment_id = SegmentId(53);
	generic.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	generic.pool_owner_id = 41;
	generic.generation = Generation(1);
	generic.page_id = PageId(103);
	generic.page_owner_id = 99;
	generic.page_owner_kind = PageOwnerKind::POK_PROPERTY;
	const std::array payload = {std::byte{0xA1}};
	generic.payload.assign(payload.begin(), payload.end());
	ASSERT_TRUE(genericPool.allocateNewPoolPage(generic, PhysicalSlotId(42), PhysicalSlotId(42), LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin genericSnapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(pool.read(genericSnapshot, PageId(103))); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableSlottedPagePoolTest, ReturnsEmptyWhenNoPhysicalSlotCanBeReserved) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(pool.allocateEmptyPage(request(54, 104), PhysicalSlotId(43), PhysicalSlotId(43), LogSequenceNumber(2))
						.has_value());
	EXPECT_FALSE(pool.allocateEmptyPage(request(55, 105), PhysicalSlotId(43), PhysicalSlotId(43), LogSequenceNumber(3))
						 .has_value());
}

TEST_F(RelocatableSlottedPagePoolTest, ReturnsNoBestFitWhenAnExistingPoolHasNoFreeFrames) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(pool.allocateEmptyPage(request(54, 200), PhysicalSlotId(43), PhysicalSlotId(43), LogSequenceNumber(2))
						.has_value());
	for (uint64_t ordinal = 1U; ordinal < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++ordinal) {
		SlottedPagePoolAppendRequest append;
		append.segment_id = SegmentId(54);
		append.generation = Generation(1);
		append.page_id = PageId(200U + ordinal);
		append.page_owner_id = 99U;
		append.page_owner_kind = PageOwnerKind::POK_PROPERTY;
		static_cast<void>(pool.appendEmptyPage(append, LogSequenceNumber(3U + ordinal)));
	}
	EXPECT_FALSE(pool.findBestFitPage(SegmentId(54), 1U).has_value());
	EXPECT_FALSE(pool.findBestFitPage(SegmentId(54), PageOwnerKind::POK_PROPERTY, 1U).has_value());
	const std::array exhausted = {SegmentId(54)};
	EXPECT_FALSE(pool.findBestFitPage(exhausted, 1U).has_value());
	EXPECT_FALSE(pool.findBestFitPage(exhausted, PageOwnerKind::POK_PROPERTY, 1U).has_value());
	EXPECT_FALSE(pool.findBestFitPageForRecord(exhausted, PageOwnerKind::POK_PROPERTY, 1U).has_value());
}

TEST_F(RelocatableSlottedPagePoolTest, RejectsInvalidTypedPageSelectionBeforeOpeningMetadata) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	SlottedPagePool pool(io, *metadata);
	expectFormatError(
			[&] { static_cast<void>(pool.findBestFitPage(SegmentId{}, PageOwnerKind::POK_PROPERTY, 1U)); },
			FormatError::FERR_INVALID_HEADER);

	const std::array<SegmentId, 0U> noPools{};
	expectFormatError(
			[&] { static_cast<void>(pool.findBestFitPage(noPools, PageOwnerKind::POK_PROPERTY, 1U)); },
			FormatError::FERR_INVALID_HEADER);
	const std::array invalidPools = {SegmentId{}};
	expectFormatError(
			[&] { static_cast<void>(pool.findBestFitPage(invalidPools, PageOwnerKind::POK_PROPERTY, 1U)); },
			FormatError::FERR_INVALID_HEADER);
	const std::array validPools = {SegmentId(54U)};
	expectFormatError(
			[&] { static_cast<void>(pool.findBestFitPage(validPools, PageOwnerKind::POK_ENTITY, 1U)); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableSlottedPagePoolTest, RejectsReplacementWithoutAnOwnedVersion) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	SlottedPage unowned = SlottedPage::create(PageId(106), Generation(1));
	expectFormatError([&] { static_cast<void>(pool.replacePage(unowned, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSlottedPagePoolTest, RejectsAAllocatorDirectoryEntryThatDisagreesWithTheTypedPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(pool.allocateEmptyPage(request(56, 106), PhysicalSlotId(43), PhysicalSlotId(43), LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	PageLocation mismatched = metadata->findPageLocation(snapshot, PageId(106));
	snapshot.release();
	ASSERT_GT(mismatched.free_bytes, 0U);
	--mismatched.free_bytes;
	static_cast<void>(metadata->upsertLocations(std::span(&mismatched, 1), {}, LogSequenceNumber(3)));
	SlottedPagePool selector(io, *metadata);
	expectFormatError([&] { static_cast<void>(selector.findBestFitPage(SegmentId(56), 1U)); },
				  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin corruptedSnapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(pool.read(corruptedSnapshot, PageId(106))); },
				  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSlottedPagePoolTest, KeepsTheSourceOwnerLiveAfterInterruptedPublicationUntilReplacementCommits) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(pool.allocateEmptyPage(request(57, 107), PhysicalSlotId(44), PhysicalSlotId(44), LogSequenceNumber(2))
					.has_value());
	ASSERT_TRUE(pool.allocateEmptyPage(request(58, 108), PhysicalSlotId(45), PhysicalSlotId(45), LogSequenceNumber(3))
					.has_value());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SlottedPage page = pool.read(snapshot, PageId(107));
	const std::array value = {std::byte{0xA1}};
	static_cast<void>(page.insert(value));
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(pool.replacePage(page, LogSequenceNumber(4), options)), BootstrapPublishInterrupted);
	const SharedPagePoolAllocation evacuated = pool.evacuatePage(page, SegmentId(58), LogSequenceNumber(5));
	EXPECT_EQ(evacuated.pool.segment_id, SegmentId(58));
	// The failed publication must not pre-retire the source. Once the later COW
	// move commits, the original page image becomes stale and is rejected.
	expectFormatError([&] { static_cast<void>(pool.replacePage(page, LogSequenceNumber(6))); },
				  FormatError::FERR_INVALID_STATE);
	snapshot.release();
	pool.reclaimRetiredFrames(SegmentId(57));
}

TEST_F(RelocatableSlottedPagePoolTest, ReconcilesInterruptedTypedReplacementBeforeRetryingItsPublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	SlottedPagePool pool(io, *metadata);
	ASSERT_TRUE(pool.allocateEmptyPage(request(57U, 107U), PhysicalSlotId(44U), PhysicalSlotId(44U),
											  LogSequenceNumber(2U))
					.has_value());

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	SlottedPage replacement = pool.read(oldSnapshot, PageId(107U));
	const std::array value = {std::byte{0xA1U}, std::byte{0xB2U}};
	const SlottedSlotReference reference = replacement.insert(value);
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(pool.replacePage(replacement, LogSequenceNumber(4U), options)),
				 BootstrapPublishInterrupted);

	// Reconciliation must discard only the unreachable candidate frame. The old
	// Page Directory route remains authoritative until a later publication succeeds.
	pool.reconcilePool(SegmentId(57U));
	MetadataSnapshotPin recoveredSnapshot = metadata->openSnapshot();
	EXPECT_EQ(pool.read(recoveredSnapshot, PageId(107U)).stats().live_slot_count, 0U);
	EXPECT_EQ(pool.read(oldSnapshot, PageId(107U)).stats().live_slot_count, 0U);
	recoveredSnapshot.release();

	static_cast<void>(pool.replacePage(replacement, LogSequenceNumber(5U)));
	MetadataSnapshotPin currentSnapshot = metadata->openSnapshot();
	EXPECT_EQ(pool.read(currentSnapshot, PageId(107U)).read(reference),
			  std::vector<std::byte>(value.begin(), value.end()));
	EXPECT_EQ(pool.read(oldSnapshot, PageId(107U)).stats().live_slot_count, 0U);
	currentSnapshot.release();
	oldSnapshot.release();

	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	MetadataSnapshotPin reopenedSnapshot = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedPool.read(reopenedSnapshot, PageId(107U)).read(reference),
			  std::vector<std::byte>(value.begin(), value.end()));
}

TEST_F(RelocatableSlottedPagePoolTest, RejectsOrExactlyRoundTripsEverySingleBitPageMutation) {
	PageOwnerHeader owner;
	owner.page_id = PageId(901U);
	owner.owner_id = 902U;
	owner.kind = PageOwnerKind::POK_PROPERTY;
	owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	owner.generation = Generation(3U);
	owner.committed_lsn = LogSequenceNumber(4U);
	SlottedPage page = SlottedPage::create(owner);
	const std::array first{std::byte{0x11U}, std::byte{0x22U}, std::byte{0x33U}};
	const std::array second{std::byte{0xA1U}, std::byte{0xB2U}};
	static_cast<void>(page.insert(first));
	static_cast<void>(page.insert(second));
	const std::vector<std::byte> canonical = page.serialize();
	ASSERT_EQ(canonical.size(), RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);

	for (size_t byteOffset = 0U; byteOffset < canonical.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			std::vector<std::byte> mutation = canonical;
			mutation[byteOffset] ^= std::byte{static_cast<unsigned char>(1U << bit)};
			try {
				const SlottedPage decoded = SlottedPage::open(mutation);
				EXPECT_EQ(decoded.serialize(), mutation)
						<< "checksum-valid mutation was normalized at byte " << byteOffset << ", bit "
						<< static_cast<unsigned>(bit);
			} catch (const FormatException &) {
				// A mutation may be rejected when it breaks the authenticated page
				// header, slot directory, or record framing.
			}
		}
	}
}

TEST_F(RelocatableSlottedPagePoolTest, SharedReadPayloadKeepsTheSnapshotAllocationGrantAfterMetadataCloses) {
    using namespace graph::query::execution::detail;
    constexpr size_t maximum = 2U * 1024U * 1024U;
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(maximum));
    auto domain = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(maximum));
    auto memory = std::make_shared<PrepaidMemoryResource>(
        std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_CACHE, maximum), maximum);
    std::weak_ptr<PrepaidMemoryResource> weak = memory;
    std::optional<SharedPagePoolContents> escaped;
    {
        auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U), memory);
        SlottedPagePool pool(io, *metadata);
        auto allocation = pool.allocateEmptyPage(request(51U, 101U), PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U));
        ASSERT_TRUE(allocation.has_value());
        auto snapshot = metadata->openSnapshot();
        SharedPagePoolAllocator shared(io, *metadata);
        escaped.emplace(shared.read(snapshot, PageId(101U)));
        EXPECT_EQ(escaped->payload.get_allocator().owner(), memory);
        EXPECT_GE(memory->used(), escaped->payload.capacity());
    }
    EXPECT_EQ(memory->used(), escaped->payload.capacity());
    memory.reset();
    EXPECT_FALSE(weak.expired());
    EXPECT_EQ(SlottedPage::open(escaped->owner, escaped->payload).pageId(), PageId(101U));
    escaped.reset();
    EXPECT_TRUE(weak.expired());
}
