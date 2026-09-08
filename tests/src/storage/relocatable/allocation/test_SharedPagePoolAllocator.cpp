/**
 * @file test_SharedPagePoolAllocator.cpp
 * @brief Atomic allocation, read, recovery, and corruption tests for shared pools.
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
#include <source_location>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"

using namespace graph::storage;

namespace graph::storage {

	class SharedPagePoolAllocatorTestAccess final {
	public:
		static void prefetch(const SharedPagePoolAllocator &allocator, const MetadataSnapshotPin &snapshot,
							 std::span<const PageId> pageIds,
							 BufferPoolReadHint admissionHint = BufferPoolReadHint::BPRH_PREFETCH) {
			allocator.prefetch(snapshot, pageIds, admissionHint);
		}

		static void prepareNewPoolPages(const SharedPagePoolAllocator &allocator, const CatalogEntry &pool,
										std::span<const SharedPagePoolPreparedAppend> pages,
										std::span<PageLocation> locations, LogSequenceNumber checkpointLsn,
										size_t preparedPrefixCount) {
			allocator.prepareNewPoolPages(pool, pages, locations, checkpointLsn, preparedPrefixCount);
		}

		static void prepareAppendPages(const SharedPagePoolAllocator &allocator, const CatalogEntry &pool,
								   std::span<const SharedPagePoolPreparedAppend> pages,
								   std::span<PageLocation> locations, LogSequenceNumber checkpointLsn) {
			allocator.prepareAppendPages(pool, pages, locations, checkpointLsn);
		}

		static void prepareAppendPagesUnderWrite(StorageIO &io, uint64_t dataRegionOffset, const CatalogEntry &pool,
																 std::span<const SharedPagePoolPreparedAppend> pages,
																 std::span<PageLocation> locations,
																 LogSequenceNumber checkpointLsn) {
			SharedPagePoolAllocator::prepareAppendPagesUnderWrite(io, dataRegionOffset, pool, pages, locations,
																		 checkpointLsn);
		}

		static void prepareMutationPages(const SharedPagePoolAllocator &allocator, const CatalogEntry &pool,
									 std::span<const PageLocation> expectedReplacements,
									 std::span<const SharedPagePoolPreparedReplacement> replacements,
									 std::span<const SharedPagePoolPreparedAppend> appends,
									 std::span<PageLocation> replacementLocations,
									 std::span<PageLocation> appendLocations, LogSequenceNumber checkpointLsn,
									 std::span<const SharedPagePoolPreparedRetirement> retirements = {}) {
			allocator.prepareMutationPages(pool, expectedReplacements, replacements, appends, replacementLocations,
										   appendLocations, checkpointLsn, retirements);
		}

		static void
		prepareCrossPoolReplacementPages(const SharedPagePoolAllocator &allocator, const CatalogEntry &destinationPool,
										 std::span<const SharedPagePoolPreparedCrossPoolReplacement> replacements,
										 std::span<PageLocation> replacementLocations,
										 LogSequenceNumber checkpointLsn) {
			allocator.prepareCrossPoolReplacementPages(destinationPool, replacements, replacementLocations,
													   checkpointLsn);
		}

		static uint8_t availableFrameCountUnderWrite(const StorageIO &io, uint64_t dataRegionOffset,
													 const CatalogEntry &pool) {
			return SharedPagePoolAllocator::availableFrameCountUnderWrite(io, dataRegionOffset, pool);
		}

		static void validatePreparedPublicationsUnderWrite(
				const StorageIO &io, uint64_t dataRegionOffset, const CatalogEntry &pool,
				std::span<const SharedPagePoolPreparedPublication> publications) {
			SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(io, dataRegionOffset, pool,
																				 publications);
		}
	};

} // namespace graph::storage

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected,
						   const std::source_location location = std::source_location::current()) {
		try {
			operation();
			ADD_FAILURE_AT(location.file_name(), static_cast<int>(location.line())) << "Expected FormatException";
		} catch (const FormatException &error) {
			if (error.error() != expected) {
				ADD_FAILURE_AT(location.file_name(), static_cast<int>(location.line()))
						<< "Unexpected FormatError: " << error.what();
			}
		}
	}

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		return result;
	}

	SharedPagePoolAllocationRequest request(uint64_t segmentId, uint64_t pageId,
											std::span<const std::byte> payload = {}) {
		SharedPagePoolAllocationRequest result;
		result.segment_id = SegmentId(segmentId);
		result.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		result.pool_owner_id = 77;
		result.generation = Generation(1);
		result.page_id = PageId(pageId);
		result.page_owner_id = 99;
		result.page_owner_kind = PageOwnerKind::POK_INDEX;
		result.payload.assign(payload.begin(), payload.end());
		return result;
	}

} // namespace

class RelocatableSharedPagePoolAllocatorTest : public ::testing::Test {
protected:
	std::filesystem::path dataPath;
	std::filesystem::path walPath;
	std::shared_ptr<std::fstream> dataFile;
	std::shared_ptr<std::fstream> walFile;
	std::shared_ptr<StorageIO> io;
	std::shared_ptr<StorageIO> walIo;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dataPath = std::filesystem::temp_directory_path() / ("relocatable_shared_pool_" + to_string(uuid) + ".dat");
		walPath = dataPath.string() + ".foreign";
		dataFile = std::make_shared<std::fstream>(dataPath,
												  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		walFile = std::make_shared<std::fstream>(walPath,
												 std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(dataFile->is_open());
		ASSERT_TRUE(walFile->is_open());
		io = std::make_shared<StorageIO>(dataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		walIo = std::make_shared<StorageIO>(walFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		walIo.reset();
		if (dataFile && dataFile->is_open()) {
			dataFile->close();
		}
		if (walFile && walFile->is_open()) {
			walFile->close();
		}
		dataFile.reset();
		walFile.reset();
		std::error_code error;
		std::filesystem::remove(dataPath, error);
		std::filesystem::remove(walPath, error);
	}
};

TEST_F(RelocatableSharedPagePoolAllocatorTest, AtomicallyCreatesReadsAndReopensTheFirstSharedPage) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array payload = {std::byte{0xA1}, std::byte{0xB2}, std::byte{0xC3}};
	const std::optional<SharedPagePoolAllocation> allocation = allocator.allocateNewPoolPage(
			request(31, 91, payload), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2));
	ASSERT_TRUE(allocation.has_value());
	EXPECT_EQ(allocation->pool.physical_slot, PhysicalSlotId(40));
	EXPECT_EQ(allocation->location.page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME);
	EXPECT_EQ(allocation->owner.payload_bytes, payload.size());

	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_TRUE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
	const SharedPagePoolContents contents = allocator.read(snapshot, PageId(91));
	EXPECT_EQ(contents.pool.segment_id, SegmentId(31));
	EXPECT_EQ(contents.location, allocation->location);
	EXPECT_EQ(contents.pool_header.allocated_frames, 1U);
	EXPECT_EQ(contents.owner.kind, PageOwnerKind::POK_INDEX);
	EXPECT_EQ(contents.payload, std::vector<std::byte>(payload.begin(), payload.end()));
	EXPECT_EQ(allocator.read(snapshot, PageId(91), BufferPoolReadHint::BPRH_SEQUENTIAL).payload,
			  std::vector<std::byte>(payload.begin(), payload.end()));
	snapshot.release();

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SharedPagePoolAllocator reopenedAllocator(io, *reopened);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedAllocator.read(reopenedSnapshot, PageId(91)).payload,
			  std::vector<std::byte>(payload.begin(), payload.end()));
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsUnknownDemandReadHintsBeforeRouteLookup) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const MetadataSnapshotPin snapshot = store->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(allocator.read(snapshot, PageId(91), static_cast<BufferPoolReadHint>(0xFFU))); },
			FormatError::FERR_INVALID_ENUM);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, KeepsPoolControlAndOwnedPagesUnderTheirDurableGenerationKeys) {
	io->enableBufferPool(256U * RSS_PAGE_SIZE);
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array payload = {std::byte{0xA1}, std::byte{0xB2}, std::byte{0xC3}};
	const std::optional<SharedPagePoolAllocation> allocation = allocator.allocateNewPoolPage(
			request(31, 91, payload), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2));
	ASSERT_TRUE(allocation.has_value());
	io->sync();

	const MetadataSnapshotPin snapshot = store->openSnapshot();
	const uint64_t poolOffset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			allocation->pool.physical_slot);
	const uint64_t pageOffset = poolOffset + static_cast<uint64_t>(allocation->location.page_index) * RSS_PAGE_SIZE;
	std::array<std::byte, RSS_PAGE_SIZE> bytes{};
	io->resetBufferPoolStats();
	EXPECT_EQ(io->readAt({poolOffset, allocation->pool.generation.value()}, bytes.data(), bytes.size()), bytes.size());
	EXPECT_EQ(io->readAt({pageOffset, allocation->location.generation.value()}, bytes.data(), bytes.size()),
			  bytes.size());

	const BufferPoolStats stats = io->bufferPoolStats();
	EXPECT_EQ(stats.misses, 0U);
	EXPECT_EQ(stats.hits, 2U);
	EXPECT_EQ(stats.disk_read_bytes, 0U);
	EXPECT_EQ(allocator.read(snapshot, PageId(91)).payload, std::vector<std::byte>(payload.begin(), payload.end()));
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsInvalidTypedPrefetchContractsBeforeRouteLookup) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array validPageIds = {PageId(91)};
	const std::array invalidPageIds = {PageId{}};
	const std::span<const PageId> noPageIds;

	expectFormatError(
			[&] { SharedPagePoolAllocatorTestAccess::prefetch(allocator, MetadataSnapshotPin{}, validPageIds); },
			FormatError::FERR_INVALID_HEADER);
	const MetadataSnapshotPin snapshot = store->openSnapshot();
	expectFormatError(
			[&] {
				SharedPagePoolAllocatorTestAccess::prefetch(allocator, snapshot, validPageIds,
															static_cast<BufferPoolReadHint>(0xFFU));
			},
			FormatError::FERR_INVALID_ENUM);
	expectFormatError([&] { SharedPagePoolAllocatorTestAccess::prefetch(allocator, snapshot, noPageIds); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { SharedPagePoolAllocatorTestAccess::prefetch(allocator, snapshot, invalidPageIds); },
					  FormatError::FERR_INVALID_HEADER);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	const MetadataSnapshotPin currentSnapshot = store->openSnapshot();
	EXPECT_NO_THROW(SharedPagePoolAllocatorTestAccess::prefetch(allocator, currentSnapshot, validPageIds));
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, MaterializesAValidatedSuffixAfterAPreparedFreshPoolPrefix) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	const MetadataSnapshotPin snapshot = store->openSnapshot();
	const CatalogEntry pool = store->findCatalog(snapshot, SegmentId(31));
	const std::array payload = {std::byte{0x44}, std::byte{0x55}};
	const SharedPagePoolPreparedAppend prepared{.owner = {.page_id = PageId(92),
														  .owner_id = 99,
														  .kind = PageOwnerKind::POK_INDEX,
														  .payload_bytes = static_cast<uint32_t>(payload.size()),
														  .generation = pool.generation,
														  .committed_lsn = pool.committed_lsn},
												.payload = std::vector<std::byte>(payload.begin(), payload.end())};
	PageLocation location{.page_id = PageId(92), .segment_id = pool.segment_id, .generation = pool.generation};
	SharedPagePoolAllocatorTestAccess::prepareNewPoolPages(allocator, pool, std::span(&prepared, 1),
														   std::span(&location, 1), pool.committed_lsn, 1U);
	EXPECT_EQ(location.page_index, 2U);

	const uint64_t poolOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, pool.physical_slot);
	std::array<std::byte, RSS_PAGE_SIZE> page{};
	ASSERT_EQ(io->readAt({poolOffset, pool.generation.value()}, page.data(), page.size()), page.size());
	std::array<std::byte, RSS_SHARED_PAGE_POOL_HEADER_BYTES> firstControlRecord{};
	std::array<std::byte, RSS_SHARED_PAGE_POOL_HEADER_BYTES> secondControlRecord{};
	std::copy_n(page.begin() + RSS_ENVELOPE_BYTES, firstControlRecord.size(), firstControlRecord.begin());
	std::copy_n(page.begin() + RSS_ENVELOPE_BYTES + RSS_SHARED_PAGE_POOL_HEADER_BYTES,
				secondControlRecord.size(), secondControlRecord.begin());
	EXPECT_EQ(std::max(SharedPagePoolHeaderCodec::decode(firstControlRecord).allocated_frames,
						   SharedPagePoolHeaderCodec::decode(secondControlRecord).allocated_frames),
			  3U);

	const uint64_t pageOffset = poolOffset + static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
	ASSERT_EQ(io->readAt({pageOffset, pool.generation.value()}, page.data(), page.size()), page.size());
	std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> ownerBytes{};
	std::copy_n(page.begin(), ownerBytes.size(), ownerBytes.begin());
	EXPECT_EQ(PageOwnerHeaderCodec::decode(ownerBytes).page_id, PageId(92));
	EXPECT_TRUE(std::equal(payload.begin(), payload.end(), page.begin() + RSS_PAGE_OWNER_HEADER_BYTES));
	EXPECT_EQ(allocator.read(snapshot, PageId(91)).owner.page_id, PageId(91));
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, AuthenticatesBoundedPreparedPublicationFrames) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	const MetadataSnapshotPin snapshot = store->openSnapshot();
	const CatalogEntry pool = store->findCatalog(snapshot, SegmentId(31));
	const PageLocation location = store->findPageLocation(snapshot, PageId(91));
	const SharedPagePoolPreparedPublication publication{
			location, 99U, PageOwnerKind::POK_INDEX, LogSequenceNumber(2U)};
	EXPECT_NO_THROW(SharedPagePoolAllocatorTestAccess::validatePreparedPublicationsUnderWrite(
				*io, snapshot.superblock().data_region_offset, pool, std::span(&publication, 1U)));

	SharedPagePoolPreparedPublication wrongOwner = publication;
	wrongOwner.expected_owner_id = 100U;
	expectFormatError(
			[&] {
				SharedPagePoolAllocatorTestAccess::validatePreparedPublicationsUnderWrite(
						*io, snapshot.superblock().data_region_offset, pool, std::span(&wrongOwner, 1U));
			},
			FormatError::FERR_INVALID_STATE);
	SharedPagePoolPreparedPublication staleLsn = publication;
	staleLsn.expected_owner_lsn = LogSequenceNumber(3U);
	expectFormatError(
			[&] {
				SharedPagePoolAllocatorTestAccess::validatePreparedPublicationsUnderWrite(
						*io, snapshot.superblock().data_region_offset, pool, std::span(&staleLsn, 1U));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, MaterializesPrivateAppendAndMutationPlansBeforePublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array initialPayload = {std::byte{0x11}};
	ASSERT_TRUE(allocator
						.allocateNewPoolPage(request(31, 91, initialPayload), PhysicalSlotId(40), PhysicalSlotId(40),
										 LogSequenceNumber(2))
						.has_value());
	const MetadataSnapshotPin snapshot = store->openSnapshot();
	const CatalogEntry pool = store->findCatalog(snapshot, SegmentId(31));
	const PageLocation initialLocation = store->findPageLocation(snapshot, PageId(91));

	const std::array appendedPayload = {std::byte{0x21}, std::byte{0x22}};
	const SharedPagePoolPreparedAppend append{.owner = {.page_id = PageId(92),
															.owner_id = 99,
															.kind = PageOwnerKind::POK_INDEX,
															.payload_bytes = static_cast<uint32_t>(appendedPayload.size()),
															.generation = pool.generation,
															.committed_lsn = pool.committed_lsn},
														 .payload = std::vector<std::byte>(appendedPayload.begin(), appendedPayload.end())};
	PageLocation appendLocation{.page_id = PageId(92), .segment_id = pool.segment_id, .generation = pool.generation};
	SharedPagePoolAllocatorTestAccess::prepareAppendPages(allocator, pool, std::span(&append, 1),
														  std::span(&appendLocation, 1), pool.committed_lsn);
	EXPECT_EQ(appendLocation.page_index, 2U);

	const std::array replacementPayload = {std::byte{0x31}, std::byte{0x32}, std::byte{0x33}};
	const SharedPagePoolPreparedReplacement replacement{
				.expected_owner_lsn = LogSequenceNumber(2),
				.owner = {.page_id = PageId(91),
							  .owner_id = 99,
							  .kind = PageOwnerKind::POK_INDEX,
							  .payload_bytes = static_cast<uint32_t>(replacementPayload.size()),
							  .generation = pool.generation,
							  .committed_lsn = LogSequenceNumber(3)},
				.payload = std::vector<std::byte>(replacementPayload.begin(), replacementPayload.end())};
	const std::array mutationAppendPayload = {std::byte{0x41}};
	const SharedPagePoolPreparedAppend mutationAppend{
				.owner = {.page_id = PageId(93),
							  .owner_id = 99,
							  .kind = PageOwnerKind::POK_INDEX,
							  .payload_bytes = static_cast<uint32_t>(mutationAppendPayload.size()),
							  .generation = pool.generation,
							  .committed_lsn = LogSequenceNumber(3)},
				.payload = std::vector<std::byte>(mutationAppendPayload.begin(), mutationAppendPayload.end())};
	PageLocation replacementLocation;
	PageLocation mutationAppendLocation{.page_id = PageId(93), .segment_id = pool.segment_id,
													.generation = pool.generation};
	const SharedPagePoolPreparedRetirement retirement{.location = appendLocation,
															.expected_owner_id = 99,
															.expected_owner_kind = PageOwnerKind::POK_INDEX,
															.expected_owner_lsn = pool.committed_lsn};
	SharedPagePoolAllocatorTestAccess::prepareMutationPages(
			allocator, pool, std::span(&initialLocation, 1), std::span(&replacement, 1),
			std::span(&mutationAppend, 1), std::span(&replacementLocation, 1), std::span(&mutationAppendLocation, 1),
			LogSequenceNumber(3), std::span(&retirement, 1));
	EXPECT_EQ(replacementLocation.page_index, 3U);
	EXPECT_EQ(mutationAppendLocation.page_index, 4U);

	const uint64_t poolOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, pool.physical_slot);
	const auto readOwnerAt = [&](uint8_t frame) {
		std::array<std::byte, RSS_PAGE_SIZE> pageBytes{};
		EXPECT_EQ(io->readAt({poolOffset + static_cast<uint64_t>(frame) * RSS_PAGE_SIZE, pool.generation.value()},
								pageBytes.data(), pageBytes.size()),
				  pageBytes.size());
		std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> ownerBytes{};
		std::copy_n(pageBytes.begin(), ownerBytes.size(), ownerBytes.begin());
		return PageOwnerHeaderCodec::decode(ownerBytes);
	};
	EXPECT_EQ(readOwnerAt(replacementLocation.page_index).committed_lsn, LogSequenceNumber(3));
	EXPECT_EQ(readOwnerAt(mutationAppendLocation.page_index).page_id, PageId(93));
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, MaterializesAppendUnderAnExistingWriterFence) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	const MetadataSnapshotPin snapshot = store->openSnapshot();
	const CatalogEntry pool = store->findCatalog(snapshot, SegmentId(31));
	const std::array payload = {std::byte{0xA5}, std::byte{0x5A}};
	const SharedPagePoolPreparedAppend append{
			.owner = {.page_id = PageId(92),
					  .owner_id = 99,
					  .kind = PageOwnerKind::POK_INDEX,
					  .payload_bytes = static_cast<uint32_t>(payload.size()),
					  .generation = pool.generation,
					  .committed_lsn = pool.committed_lsn},
			.payload = std::vector<std::byte>(payload.begin(), payload.end())};
	PageLocation location{.page_id = PageId(92), .segment_id = pool.segment_id, .generation = pool.generation};

	SharedPagePoolAllocatorTestAccess::prepareAppendPagesUnderWrite(
				*io, snapshot.superblock().data_region_offset, pool, std::span(&append, 1), std::span(&location, 1),
				pool.committed_lsn);
	EXPECT_EQ(location.page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U);
	EXPECT_EQ(location.free_bytes, 0U);

	const uint64_t poolOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, pool.physical_slot);
	std::array<std::byte, RSS_PAGE_SIZE> page{};
	ASSERT_EQ(io->readAt({poolOffset + static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE,
						  pool.generation.value()},
					 page.data(), page.size()),
			  page.size());
	std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> ownerBytes{};
	std::copy_n(page.begin(), ownerBytes.size(), ownerBytes.begin());
	const PageOwnerHeader owner = PageOwnerHeaderCodec::decode(ownerBytes);
	EXPECT_EQ(owner.page_id, PageId(92));
	EXPECT_EQ(owner.owner_id, 99U);
	EXPECT_EQ(owner.committed_lsn, pool.committed_lsn);
	EXPECT_TRUE(std::equal(payload.begin(), payload.end(), page.begin() + RSS_PAGE_OWNER_HEADER_BYTES));

	std::array<std::byte, RSS_PAGE_SIZE> control{};
	ASSERT_EQ(io->readAt({poolOffset, pool.generation.value()}, control.data(), control.size()), control.size());
	std::array<std::byte, RSS_SHARED_PAGE_POOL_HEADER_BYTES> firstControlRecord{};
	std::array<std::byte, RSS_SHARED_PAGE_POOL_HEADER_BYTES> secondControlRecord{};
	std::copy_n(control.begin() + RSS_ENVELOPE_BYTES, firstControlRecord.size(), firstControlRecord.begin());
	std::copy_n(control.begin() + RSS_ENVELOPE_BYTES + RSS_SHARED_PAGE_POOL_HEADER_BYTES,
				secondControlRecord.size(), secondControlRecord.begin());
	EXPECT_EQ(std::max(SharedPagePoolHeaderCodec::decode(firstControlRecord).allocated_frames,
					   SharedPagePoolHeaderCodec::decode(secondControlRecord).allocated_frames),
			  UINT32_C(3));
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsASuffixWhoseDeclaredFreshPoolPrefixIsNotDurable) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	const MetadataSnapshotPin snapshot = store->openSnapshot();
	const CatalogEntry pool = store->findCatalog(snapshot, SegmentId(31));
	const std::array payload = {std::byte{0x44}};
	const SharedPagePoolPreparedAppend prepared{.owner = {.page_id = PageId(92),
															  .owner_id = 99,
															  .kind = PageOwnerKind::POK_INDEX,
															  .payload_bytes = static_cast<uint32_t>(payload.size()),
															  .generation = pool.generation,
															  .committed_lsn = pool.committed_lsn},
													 .payload = std::vector<std::byte>(payload.begin(), payload.end())};
	PageLocation location{.page_id = PageId(92), .segment_id = pool.segment_id, .generation = pool.generation};
	expectFormatError(
			[&] {
				SharedPagePoolAllocatorTestAccess::prepareNewPoolPages(
						allocator, pool, std::span(&prepared, 1), std::span(&location, 1), pool.committed_lsn, 2U);
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(location.page_index, 0U);
	EXPECT_EQ(allocator.read(snapshot, PageId(91)).owner.page_id, PageId(91));
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsInvalidCrossPoolBatchContractsBeforeStorageMutation) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const SharedPagePoolPreparedCrossPoolReplacement replacement;
	const std::array replacements = {replacement};
	std::array<PageLocation, 1U> locations{};
	CatalogEntry destination;
	destination.state = SegmentState::SGS_SEALED;
	destination.kind = SegmentKind::SGK_INDEX_PAGE_POOL;

	expectFormatError(
			[&] {
				SharedPagePoolAllocatorTestAccess::prepareCrossPoolReplacementPages(
						allocator, destination, std::span<const SharedPagePoolPreparedCrossPoolReplacement>{},
						locations, LogSequenceNumber(2));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				SharedPagePoolAllocatorTestAccess::prepareCrossPoolReplacementPages(
						allocator, destination, replacements, std::span<PageLocation>{}, LogSequenceNumber(2));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				SharedPagePoolAllocatorTestAccess::prepareCrossPoolReplacementPages(
						allocator, destination, replacements, locations, LogSequenceNumber{});
			},
			FormatError::FERR_INVALID_HEADER);
	destination.state = SegmentState::SGS_BUILDING;
	expectFormatError(
			[&] {
				SharedPagePoolAllocatorTestAccess::prepareCrossPoolReplacementPages(
						allocator, destination, replacements, locations, LogSequenceNumber(2));
			},
			FormatError::FERR_INVALID_HEADER);
	destination.state = SegmentState::SGS_SEALED;
	destination.kind = SegmentKind::SGK_NONE;
	expectFormatError(
			[&] {
				SharedPagePoolAllocatorTestAccess::prepareCrossPoolReplacementPages(
						allocator, destination, replacements, locations, LogSequenceNumber(2));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsStaleOrCapacityInvalidCrossPoolBatchState) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array payload = {std::byte{0x44}, std::byte{0x55}};
	SharedPagePoolAllocationRequest sourceRequest = request(31, 91, payload);
	sourceRequest.pool_owner_id = sourceRequest.page_owner_id;
	SharedPagePoolAllocationRequest destinationRequest = request(32, 92);
	destinationRequest.pool_owner_id = destinationRequest.page_owner_id;
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(sourceRequest, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	ASSERT_TRUE(allocator
						.allocateNewPoolPage(destinationRequest, PhysicalSlotId(41), PhysicalSlotId(41),
											 LogSequenceNumber(3))
						.has_value());
	const MetadataSnapshotPin snapshot = store->openSnapshot();
	const CatalogEntry sourcePool = store->findCatalog(snapshot, SegmentId(31));
	const CatalogEntry destinationPool = store->findCatalog(snapshot, SegmentId(32));
	const PageLocation sourceLocation = store->findPageLocation(snapshot, PageId(91));
	SharedPagePoolPreparedCrossPoolReplacement valid{.source_pool = sourcePool,
													 .source_location = sourceLocation,
													 .expected_owner_lsn = LogSequenceNumber(2),
													 .owner = {.page_id = PageId(91),
															   .owner_id = destinationPool.owner_id,
															   .kind = PageOwnerKind::POK_INDEX,
															   .payload_bytes = static_cast<uint32_t>(payload.size()),
															   .generation = destinationPool.generation,
															   .committed_lsn = LogSequenceNumber(4)},
													 .payload = std::vector<std::byte>(payload.begin(), payload.end())};
	std::array<PageLocation, 1U> replacementLocations{};
	const auto expectInvalid = [&](const SharedPagePoolPreparedCrossPoolReplacement &candidate) {
		expectFormatError(
				[&] {
					SharedPagePoolAllocatorTestAccess::prepareCrossPoolReplacementPages(
							allocator, destinationPool, std::span(&candidate, 1), replacementLocations,
							LogSequenceNumber(4));
				},
				FormatError::FERR_INVALID_STATE);
	};

	SharedPagePoolPreparedCrossPoolReplacement stalePool = valid;
	stalePool.source_pool.state = SegmentState::SGS_BUILDING;
	expectInvalid(stalePool);
	SharedPagePoolPreparedCrossPoolReplacement unallocatedFrame = valid;
	unallocatedFrame.source_location.page_index = 2U;
	expectInvalid(unallocatedFrame);
	SharedPagePoolPreparedCrossPoolReplacement staleOwner = valid;
	staleOwner.expected_owner_lsn = LogSequenceNumber(99);
	expectInvalid(staleOwner);

	const uint64_t destinationOffset = PhysicalSegmentAllocator::checkedSlotOffset(
			snapshot.superblock().data_region_offset, destinationPool.physical_slot);
	std::array<std::byte, RSS_PAGE_SIZE> controlPage{};
	ASSERT_EQ(
			io->readAt({destinationOffset, destinationPool.generation.value()}, controlPage.data(), controlPage.size()),
			controlPage.size());
	std::array<std::byte, RSS_SHARED_PAGE_POOL_HEADER_BYTES> headerBytes{};
	std::copy_n(controlPage.begin() + RSS_ENVELOPE_BYTES, headerBytes.size(), headerBytes.begin());
	SharedPagePoolHeader fullHeader = SharedPagePoolHeaderCodec::decode(headerBytes);
	fullHeader.allocated_frames = RSS_SHARED_PAGE_POOL_VALID_FRAME_MASK;
	headerBytes = SharedPagePoolHeaderCodec::encode(fullHeader);
	std::copy(headerBytes.begin(), headerBytes.end(), controlPage.begin() + RSS_ENVELOPE_BYTES);
	io->writeAt({destinationOffset, destinationPool.generation.value()}, controlPage.data(), controlPage.size());
	expectInvalid(valid);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, MaterializesCrossPoolAndSamePoolPreparedReplacements) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array sourcePayload = {std::byte{0x51}, std::byte{0x52}};
	SharedPagePoolAllocationRequest sourceRequest = request(31, 91, sourcePayload);
	sourceRequest.pool_owner_id = sourceRequest.page_owner_id;
	SharedPagePoolAllocationRequest destinationRequest = request(32, 92);
	destinationRequest.pool_owner_id = sourceRequest.page_owner_id;
	ASSERT_TRUE(allocator
						.allocateNewPoolPage(sourceRequest, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	ASSERT_TRUE(allocator
						.allocateNewPoolPage(destinationRequest, PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());
	const MetadataSnapshotPin snapshot = store->openSnapshot();
	const CatalogEntry sourcePool = store->findCatalog(snapshot, SegmentId(31));
	const CatalogEntry destinationPool = store->findCatalog(snapshot, SegmentId(32));
	const PageLocation sourceLocation = store->findPageLocation(snapshot, PageId(91));
	const std::array firstPayload = {std::byte{0x61}, std::byte{0x62}, std::byte{0x63}};
	SharedPagePoolPreparedCrossPoolReplacement crossPool{
				.source_pool = sourcePool,
				.source_location = sourceLocation,
				.expected_owner_lsn = LogSequenceNumber(2),
				.owner = {.page_id = PageId(91),
							  .owner_id = destinationPool.owner_id,
							  .kind = PageOwnerKind::POK_INDEX,
							  .payload_bytes = static_cast<uint32_t>(firstPayload.size()),
							  .generation = destinationPool.generation,
							  .committed_lsn = LogSequenceNumber(4)},
				.payload = std::vector<std::byte>(firstPayload.begin(), firstPayload.end())};
	PageLocation firstReplacement;
	SharedPagePoolAllocatorTestAccess::prepareCrossPoolReplacementPages(
			allocator, destinationPool, std::span(&crossPool, 1), std::span(&firstReplacement, 1), LogSequenceNumber(4));
	EXPECT_EQ(firstReplacement.segment_id, destinationPool.segment_id);
	EXPECT_EQ(firstReplacement.page_index, 2U);

	const std::array secondPayload = {std::byte{0x71}};
	SharedPagePoolPreparedCrossPoolReplacement samePool = crossPool;
	samePool.source_pool = destinationPool;
	samePool.source_location = firstReplacement;
	samePool.expected_owner_lsn = LogSequenceNumber(4);
	samePool.owner.committed_lsn = LogSequenceNumber(5);
	samePool.payload.assign(secondPayload.begin(), secondPayload.end());
	samePool.owner.payload_bytes = static_cast<uint32_t>(secondPayload.size());
	PageLocation secondReplacement;
	SharedPagePoolAllocatorTestAccess::prepareCrossPoolReplacementPages(
			allocator, destinationPool, std::span(&samePool, 1), std::span(&secondReplacement, 1), LogSequenceNumber(5));
	EXPECT_EQ(secondReplacement.segment_id, destinationPool.segment_id);
	EXPECT_EQ(secondReplacement.page_index, 3U);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, EvacuatesIndexPageWithoutAnIndexRootBinding) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array sourcePayload = {std::byte{0x81}, std::byte{0x82}};
	ASSERT_TRUE(allocator
						.allocateNewPoolPage(request(31, 91, sourcePayload), PhysicalSlotId(40), PhysicalSlotId(40),
										 LogSequenceNumber(2))
						.has_value());
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(32, 92), PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
					.has_value());
	const std::array replacementPayload = {std::byte{0x91}, std::byte{0x92}, std::byte{0x93}};
	SharedPagePoolEvacuationRequest evacuation;
	evacuation.source_segment_id = SegmentId(31);
	evacuation.source_generation = Generation(1);
	evacuation.destination_segment_id = SegmentId(32);
	evacuation.destination_generation = Generation(1);
	evacuation.page_id = PageId(91);
	evacuation.expected_owner_lsn = LogSequenceNumber(2);
	evacuation.page_owner_id = 99;
	evacuation.page_owner_kind = PageOwnerKind::POK_INDEX;
	evacuation.payload.assign(replacementPayload.begin(), replacementPayload.end());
	const SharedPagePoolAllocation result =
			allocator.evacuateIndexPage(evacuation, std::nullopt, std::nullopt, LogSequenceNumber(4));
	EXPECT_EQ(result.pool.segment_id, SegmentId(32));
	EXPECT_EQ(result.location.page_index, 2U);
	const MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(allocator.read(snapshot, PageId(91)).payload,
			  std::vector<std::byte>(replacementPayload.begin(), replacementPayload.end()));
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsAnUnpairedIndexRootBindingBeforeEvacuation) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	expectFormatError(
			[&] {
				static_cast<void>(allocator.evacuateIndexPage(SharedPagePoolEvacuationRequest{},
																	 std::optional<IndexRootLocation>(IndexRootLocation{}), std::nullopt,
																	 LogSequenceNumber(1)));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, OwnsPayloadBeforeMetadataPublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::vector<std::byte> expected = {std::byte{0xA1}, std::byte{0xB2}, std::byte{0xC3}};
	std::vector<std::byte> callerBytes = expected;
	SharedPagePoolAllocationRequest allocationRequest = request(31, 91);
	allocationRequest.payload = callerBytes;
	callerBytes.assign(expected.size(), std::byte{0xFF});

	ASSERT_TRUE(allocator
						.allocateNewPoolPage(allocationRequest, PhysicalSlotId(40), PhysicalSlotId(40),
											 LogSequenceNumber(2))
						.has_value());
	const MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(allocator.read(snapshot, PageId(91)).payload, expected);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, AppendsAnotherPageToTheSamePoolWithoutASecondCatalogSlot) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	const std::array payload = {std::byte{0x44}, std::byte{0x55}};
	SharedPagePoolAppendRequest append;
	append.segment_id = SegmentId(31);
	append.generation = Generation(1);
	append.page_id = PageId(92);
	append.page_owner_id = 100;
	append.page_owner_kind = PageOwnerKind::POK_INDEX;
	append.payload.assign(payload.begin(), payload.end());
	const SharedPagePoolAllocation allocation = allocator.appendPage(append, LogSequenceNumber(3));
	EXPECT_EQ(allocation.pool.physical_slot, PhysicalSlotId(40));
	EXPECT_EQ(allocation.location.page_index, 2U);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(allocator.read(snapshot, PageId(92)).payload, std::vector<std::byte>(payload.begin(), payload.end()));
	EXPECT_EQ(allocator.read(snapshot, PageId(91)).owner.page_id, PageId(91));
	EXPECT_EQ(allocator.read(snapshot, PageId(92)).pool_header.allocated_frames, 3U);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, PreservesEveryDeclaredSharedPoolAndOwnerKindPair) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	struct PoolCase {
		SegmentKind pool_kind;
		PageOwnerKind owner_kind;
	};
	const std::array cases = {
			PoolCase{SegmentKind::SGK_ENTITY_PAGE_POOL, PageOwnerKind::POK_ENTITY},
			PoolCase{SegmentKind::SGK_ENTITY_PAGE_POOL, PageOwnerKind::POK_ENTITY_LABEL},
			PoolCase{SegmentKind::SGK_PROPERTY_PAGE_POOL, PageOwnerKind::POK_PROPERTY},
			PoolCase{SegmentKind::SGK_INDEX_PAGE_POOL, PageOwnerKind::POK_INDEX},
			PoolCase{SegmentKind::SGK_VECTOR_RAW_POOL, PageOwnerKind::POK_VECTOR_RAW},
			PoolCase{SegmentKind::SGK_VECTOR_PQ_POOL, PageOwnerKind::POK_VECTOR_PQ},
			PoolCase{SegmentKind::SGK_VECTOR_ADJACENCY_POOL, PageOwnerKind::POK_VECTOR_ADJACENCY},
			PoolCase{SegmentKind::SGK_VECTOR_MANIFEST_POOL, PageOwnerKind::POK_VECTOR_MANIFEST},
			PoolCase{SegmentKind::SGK_STATE_PAGE_POOL, PageOwnerKind::POK_STATE},
			PoolCase{SegmentKind::SGK_LARGE_VALUE_DATA_POOL, PageOwnerKind::POK_LARGE_VALUE_DATA},
			PoolCase{SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL, PageOwnerKind::POK_LARGE_VALUE_MANIFEST},
	};
	for (size_t index = 0; index < cases.size(); ++index) {
		SharedPagePoolAllocationRequest first = request(31U + index, 91U + index * 2U);
		first.pool_kind = cases[index].pool_kind;
		first.pool_owner_id = 100U + index;
		first.page_owner_id = 200U + index;
		first.page_owner_kind = cases[index].owner_kind;
		const LogSequenceNumber firstLsn(2U + index * 2U);
		ASSERT_TRUE(
				allocator.allocateNewPoolPage(first, PhysicalSlotId(40U + index), PhysicalSlotId(40U + index), firstLsn)
						.has_value());

		SharedPagePoolAppendRequest appended;
		appended.segment_id = first.segment_id;
		appended.generation = first.generation;
		appended.page_id = PageId(92U + index * 2U);
		appended.page_owner_id = 300U + index;
		appended.page_owner_kind = cases[index].owner_kind;
		const LogSequenceNumber appendLsn(3U + index * 2U);
		static_cast<void>(allocator.appendPage(appended, appendLsn));
	}

	MetadataSnapshotPin snapshot = store->openSnapshot();
	for (size_t index = 0; index < cases.size(); ++index) {
		const SharedPagePoolContents contents = allocator.read(snapshot, PageId(92U + index * 2U));
		EXPECT_EQ(contents.pool.kind, cases[index].pool_kind);
		EXPECT_EQ(contents.owner.kind, cases[index].owner_kind);
	}
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, ReplacesPageThroughCowAndRetainsItsOldFrameForSnapshots) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array firstPayload = {std::byte{0x11}, std::byte{0x22}};
	ASSERT_TRUE(allocator
						.allocateNewPoolPage(request(31, 91, firstPayload), PhysicalSlotId(40), PhysicalSlotId(40),
											 LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin oldSnapshot = store->openSnapshot();

	SharedPagePoolReplacementRequest replacement;
	replacement.segment_id = SegmentId(31);
	replacement.generation = Generation(1);
	replacement.page_id = PageId(91);
	replacement.expected_owner_lsn = LogSequenceNumber(2);
	replacement.page_owner_id = 99;
	replacement.page_owner_kind = PageOwnerKind::POK_INDEX;
	const std::array replacementPayload = {std::byte{0x33}, std::byte{0x44}, std::byte{0x55}};
	replacement.payload.assign(replacementPayload.begin(), replacementPayload.end());
	const SharedPagePoolAllocation published = allocator.replacePage(replacement, LogSequenceNumber(3));
	EXPECT_EQ(published.location.page_index, 2U);
	EXPECT_EQ(allocator.read(oldSnapshot, PageId(91)).payload,
			  std::vector<std::byte>(firstPayload.begin(), firstPayload.end()));
	MetadataSnapshotPin currentSnapshot = store->openSnapshot();
	EXPECT_EQ(allocator.read(currentSnapshot, PageId(91)).payload,
			  std::vector<std::byte>(replacementPayload.begin(), replacementPayload.end()));
	// A second allocator performs the startup scan while the old snapshot still
	// pins the retired source frame, so that frame must remain allocated.
	SharedPagePoolAllocator snapshotAwareAllocator(io, *store);
	EXPECT_EQ(snapshotAwareAllocator.read(currentSnapshot, PageId(91)).pool_header.allocated_frames, 3U);
	expectFormatError([&] { static_cast<void>(allocator.replacePage(replacement, LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);
	oldSnapshot.release();
	// The newer snapshot resolves the replacement frame, not the retired one,
	// so it must not prevent safe reuse of the old physical frame.
	allocator.reclaimRetiredFrames(SegmentId(31));
	EXPECT_EQ(allocator.read(currentSnapshot, PageId(91)).pool_header.allocated_frames, 2U);
	EXPECT_EQ(allocator.read(currentSnapshot, PageId(91)).location.page_index, 2U);
	EXPECT_EQ(allocator.read(currentSnapshot, PageId(91)).payload,
			  std::vector<std::byte>(replacementPayload.begin(), replacementPayload.end()));
	currentSnapshot.release();
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, EvacuatesAPageAcrossPoolsAndReusesTheRetiredSourceFrame) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array sourcePayload = {std::byte{0x11}, std::byte{0x22}};
	ASSERT_TRUE(allocator
						.allocateNewPoolPage(request(31, 91, sourcePayload), PhysicalSlotId(40), PhysicalSlotId(40),
											 LogSequenceNumber(2))
						.has_value());
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(32, 92), PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
					.has_value());
	MetadataSnapshotPin oldSnapshot = store->openSnapshot();

	SharedPagePoolEvacuationRequest evacuation;
	evacuation.source_segment_id = SegmentId(31);
	evacuation.source_generation = Generation(1);
	evacuation.destination_segment_id = SegmentId(32);
	evacuation.destination_generation = Generation(1);
	evacuation.page_id = PageId(91);
	evacuation.expected_owner_lsn = LogSequenceNumber(2);
	evacuation.page_owner_id = 99;
	evacuation.page_owner_kind = PageOwnerKind::POK_INDEX;
	evacuation.payload.assign(sourcePayload.begin(), sourcePayload.end());
	const SharedPagePoolAllocation published = allocator.evacuatePage(evacuation, LogSequenceNumber(4));
	EXPECT_EQ(published.pool.segment_id, SegmentId(32));
	EXPECT_EQ(published.location.page_index, 2U);
	EXPECT_EQ(allocator.read(oldSnapshot, PageId(91)).payload,
			  std::vector<std::byte>(sourcePayload.begin(), sourcePayload.end()));
	MetadataSnapshotPin currentSnapshot = store->openSnapshot();
	EXPECT_EQ(allocator.read(currentSnapshot, PageId(91)).pool.segment_id, SegmentId(32));
	EXPECT_EQ(allocator.read(currentSnapshot, PageId(91)).payload,
			  std::vector<std::byte>(sourcePayload.begin(), sourcePayload.end()));
	EXPECT_EQ(allocator.read(currentSnapshot, PageId(92)).pool_header.allocated_frames, 3U);
	allocator.reclaimRetiredFrames(SegmentId(31));
	EXPECT_EQ(allocator.read(oldSnapshot, PageId(91)).payload,
			  std::vector<std::byte>(sourcePayload.begin(), sourcePayload.end()));
	oldSnapshot.release();
	currentSnapshot.release();

	allocator.reclaimRetiredFrames(SegmentId(31));
	SharedPagePoolAppendRequest append;
	append.segment_id = SegmentId(31);
	append.generation = Generation(1);
	append.page_id = PageId(93);
	append.page_owner_id = 100;
	append.page_owner_kind = PageOwnerKind::POK_INDEX;
	const SharedPagePoolAllocation reused = allocator.appendPage(append, LogSequenceNumber(5));
	EXPECT_EQ(reused.location.page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsStaleOrUnplaceableCrossPoolEvacuationRequests) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(32, 92), PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
					.has_value());
	SharedPagePoolEvacuationRequest evacuation;
	evacuation.source_segment_id = SegmentId(31);
	evacuation.source_generation = Generation(1);
	evacuation.destination_segment_id = SegmentId(32);
	evacuation.destination_generation = Generation(1);
	evacuation.page_id = PageId(91);
	evacuation.expected_owner_lsn = LogSequenceNumber(1);
	evacuation.page_owner_id = 99;
	evacuation.page_owner_kind = PageOwnerKind::POK_INDEX;
	expectFormatError([&] { static_cast<void>(allocator.evacuatePage(evacuation, LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);

	evacuation.expected_owner_lsn = LogSequenceNumber(2);
	SharedPagePoolEvacuationRequest invalid = evacuation;
	invalid.destination_segment_id = invalid.source_segment_id;
	expectFormatError([&] { static_cast<void>(allocator.evacuatePage(invalid, LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_HEADER);
	SharedPagePoolEvacuationRequest wrongSourcePage = evacuation;
	wrongSourcePage.page_id = PageId(92);
	expectFormatError([&] { static_cast<void>(allocator.evacuatePage(wrongSourcePage, LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);
	SharedPagePoolAppendRequest append;
	append.segment_id = SegmentId(32);
	append.generation = Generation(1);
	append.page_owner_id = 100;
	append.page_owner_kind = PageOwnerKind::POK_INDEX;
	for (uint64_t pageId = 93; pageId <= 122; ++pageId) {
		append.page_id = PageId(pageId);
		static_cast<void>(allocator.appendPage(append, LogSequenceNumber(pageId - 89U)));
	}
	expectFormatError([&] { static_cast<void>(allocator.evacuatePage(evacuation, LogSequenceNumber(34))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, ReconcilesAnUnpublishedCrossPoolEvacuationAfterReopen) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array sourcePayload = {std::byte{0x51}, std::byte{0x52}};
	ASSERT_TRUE(allocator
						.allocateNewPoolPage(request(31, 91, sourcePayload), PhysicalSlotId(40), PhysicalSlotId(40),
											 LogSequenceNumber(2))
						.has_value());
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(32, 92), PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
					.has_value());
	SharedPagePoolEvacuationRequest evacuation;
	evacuation.source_segment_id = SegmentId(31);
	evacuation.source_generation = Generation(1);
	evacuation.destination_segment_id = SegmentId(32);
	evacuation.destination_generation = Generation(1);
	evacuation.page_id = PageId(91);
	evacuation.expected_owner_lsn = LogSequenceNumber(2);
	evacuation.page_owner_id = 99;
	evacuation.page_owner_kind = PageOwnerKind::POK_INDEX;
	evacuation.payload.assign(sourcePayload.begin(), sourcePayload.end());
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(allocator.evacuatePage(evacuation, LogSequenceNumber(4), options)),
				 BootstrapPublishInterrupted);

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SharedPagePoolAllocator reopenedAllocator(io, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedAllocator.read(snapshot, PageId(91)).pool.segment_id, SegmentId(31));
	EXPECT_EQ(reopenedAllocator.read(snapshot, PageId(91)).payload,
			  std::vector<std::byte>(sourcePayload.begin(), sourcePayload.end()));
	EXPECT_EQ(reopenedAllocator.read(snapshot, PageId(92)).pool_header.allocated_frames, 1U);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RecoversAPublishedCrossPoolEvacuationBeforeSuperblockSwitch) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array sourcePayload = {std::byte{0x61}, std::byte{0x62}};
	ASSERT_TRUE(allocator
						.allocateNewPoolPage(request(31, 91, sourcePayload), PhysicalSlotId(40), PhysicalSlotId(40),
											 LogSequenceNumber(2))
						.has_value());
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(32, 92), PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
					.has_value());
	SharedPagePoolEvacuationRequest evacuation;
	evacuation.source_segment_id = SegmentId(31);
	evacuation.source_generation = Generation(1);
	evacuation.destination_segment_id = SegmentId(32);
	evacuation.destination_generation = Generation(1);
	evacuation.page_id = PageId(91);
	evacuation.expected_owner_lsn = LogSequenceNumber(2);
	evacuation.page_owner_id = 99;
	evacuation.page_owner_kind = PageOwnerKind::POK_INDEX;
	evacuation.payload.assign(sourcePayload.begin(), sourcePayload.end());
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(allocator.evacuatePage(evacuation, LogSequenceNumber(4), options)),
				 BootstrapPublishInterrupted);

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SharedPagePoolAllocator reopenedAllocator(io, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedAllocator.read(snapshot, PageId(91)).pool.segment_id, SegmentId(32));
	EXPECT_EQ(reopenedAllocator.read(snapshot, PageId(91)).payload,
			  std::vector<std::byte>(sourcePayload.begin(), sourcePayload.end()));
	EXPECT_EQ(reopenedAllocator.read(snapshot, PageId(92)).pool_header.allocated_frames, 3U);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RetiresAndReleasesAnEvacuatedEmptyPoolAfterSnapshotsDrain) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(32, 92), PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
					.has_value());
	MetadataSnapshotPin oldSnapshot = store->openSnapshot();
	SharedPagePoolEvacuationRequest evacuation;
	evacuation.source_segment_id = SegmentId(31);
	evacuation.source_generation = Generation(1);
	evacuation.destination_segment_id = SegmentId(32);
	evacuation.destination_generation = Generation(1);
	evacuation.page_id = PageId(91);
	evacuation.expected_owner_lsn = LogSequenceNumber(2);
	evacuation.page_owner_id = 99;
	evacuation.page_owner_kind = PageOwnerKind::POK_INDEX;
	static_cast<void>(allocator.evacuatePage(evacuation, LogSequenceNumber(4)));
	static_cast<void>(allocator.retireEmptyPool(SegmentId(31), Generation(1), LogSequenceNumber(5)));
	EXPECT_EQ(store->findCatalog(oldSnapshot, SegmentId(31)).state, SegmentState::SGS_SEALED);
	MetadataSnapshotPin retiredSnapshot = store->openSnapshot();
	EXPECT_EQ(store->findCatalog(retiredSnapshot, SegmentId(31)).state, SegmentState::SGS_RETIRED);
	EXPECT_EQ(allocator.read(retiredSnapshot, PageId(91)).pool.segment_id, SegmentId(32));
	expectFormatError(
			[&] {
				static_cast<void>(
						allocator.releaseRetiredEmptyPool(SegmentId(31), Generation(1), LogSequenceNumber(6)));
			},
			FormatError::FERR_INVALID_STATE);
	oldSnapshot.release();
	retiredSnapshot.release();
	static_cast<void>(allocator.releaseRetiredEmptyPool(SegmentId(31), Generation(1), LogSequenceNumber(6)));
	MetadataSnapshotPin releasedSnapshot = store->openSnapshot();
	EXPECT_FALSE(store->isPhysicalSlotAllocated(releasedSnapshot, PhysicalSlotId(40)));
	EXPECT_EQ(store->findCatalog(releasedSnapshot, SegmentId(31)).state, SegmentState::SGS_OBSOLETE);
	releasedSnapshot.release();
	const std::optional<SharedPagePoolAllocation> reused = allocator.allocateNewPoolPage(
			request(33, 93), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(7));
	ASSERT_TRUE(reused.has_value());
	EXPECT_EQ(reused->pool.physical_slot, PhysicalSlotId(40));
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsRetirementOfAPoolWithCurrentPages) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	expectFormatError(
			[&] { static_cast<void>(allocator.retireEmptyPool(SegmentId(31), Generation(1), LogSequenceNumber(3))); },
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, ReclaimsPublishedCowFramesWhenReopenedWithoutSnapshots) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array firstPayload = {std::byte{0x11}};
	ASSERT_TRUE(allocator
						.allocateNewPoolPage(request(31, 91, firstPayload), PhysicalSlotId(40), PhysicalSlotId(40),
											 LogSequenceNumber(2))
						.has_value());
	SharedPagePoolReplacementRequest replacement;
	replacement.segment_id = SegmentId(31);
	replacement.generation = Generation(1);
	replacement.page_id = PageId(91);
	replacement.expected_owner_lsn = LogSequenceNumber(2);
	replacement.page_owner_id = 99;
	replacement.page_owner_kind = PageOwnerKind::POK_INDEX;
	const std::array replacementPayload = {std::byte{0x22}};
	replacement.payload.assign(replacementPayload.begin(), replacementPayload.end());
	static_cast<void>(allocator.replacePage(replacement, LogSequenceNumber(3)));

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SharedPagePoolAllocator reopenedAllocator(io, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	const SharedPagePoolContents contents = reopenedAllocator.read(snapshot, PageId(91));
	EXPECT_EQ(contents.payload, std::vector<std::byte>(replacementPayload.begin(), replacementPayload.end()));
	EXPECT_EQ(contents.location.page_index, 2U);
	EXPECT_EQ(contents.pool_header.allocated_frames, 2U);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, ReusesCowFramesAcrossMoreReplacementsThanOnePoolCanHold) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	SharedPagePoolReplacementRequest replacement;
	replacement.segment_id = SegmentId(31);
	replacement.generation = Generation(1);
	replacement.page_id = PageId(91);
	replacement.page_owner_id = 99;
	replacement.page_owner_kind = PageOwnerKind::POK_INDEX;
	for (uint64_t lsn = 3; lsn <= 40; ++lsn) {
		const std::array payload = {std::byte{static_cast<unsigned char>(lsn)}};
		replacement.expected_owner_lsn = LogSequenceNumber(lsn - 1U);
		replacement.payload.assign(payload.begin(), payload.end());
		const SharedPagePoolAllocation published = allocator.replacePage(replacement, LogSequenceNumber(lsn));
		EXPECT_EQ(published.location.page_index, lsn % 2U == 0U ? 1U : 2U);
		allocator.reclaimRetiredFrames(SegmentId(31));
		MetadataSnapshotPin snapshot = store->openSnapshot();
		const SharedPagePoolContents current = allocator.read(snapshot, PageId(91));
		EXPECT_EQ(current.payload, std::vector<std::byte>(payload.begin(), payload.end()));
		EXPECT_EQ(current.pool_header.allocated_frames, current.location.page_index == 1U ? 1U : 2U);
		snapshot.release();
	}
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsDuplicateCurrentDirectoryFramesDuringStartupRecovery) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	const PageLocation duplicate{.page_id = PageId(92),
								 .segment_id = SegmentId(31),
								 .page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME,
								 .generation = Generation(1)};
	static_cast<void>(store->upsertLocations(std::span(&duplicate, 1), {}, LogSequenceNumber(3)));

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	expectFormatError([&] { static_cast<void>(SharedPagePoolAllocator(io, *reopened)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RecoversPublishedCowReplacementAfterWalPublicationBeforeSuperblock) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array firstPayload = {std::byte{0x11}};
	ASSERT_TRUE(allocator
						.allocateNewPoolPage(request(31, 91, firstPayload), PhysicalSlotId(40), PhysicalSlotId(40),
											 LogSequenceNumber(2))
						.has_value());
	SharedPagePoolReplacementRequest replacement;
	replacement.segment_id = SegmentId(31);
	replacement.generation = Generation(1);
	replacement.page_id = PageId(91);
	replacement.expected_owner_lsn = LogSequenceNumber(2);
	replacement.page_owner_id = 99;
	replacement.page_owner_kind = PageOwnerKind::POK_INDEX;
	const std::array replacementPayload = {std::byte{0x33}};
	replacement.payload.assign(replacementPayload.begin(), replacementPayload.end());
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(allocator.replacePage(replacement, LogSequenceNumber(3), options)),
				 BootstrapPublishInterrupted);

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SharedPagePoolAllocator reopenedAllocator(io, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	const SharedPagePoolContents recovered = reopenedAllocator.read(snapshot, PageId(91));
	EXPECT_EQ(recovered.payload, std::vector<std::byte>(replacementPayload.begin(), replacementPayload.end()));
	EXPECT_EQ(recovered.location.page_index, 2U);
	EXPECT_EQ(recovered.pool_header.allocated_frames, 2U);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, ReconcilesInterruptedReplacementWithoutDiscardingTheCurrentPage) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array firstPayload = {std::byte{0x11}};
	ASSERT_TRUE(allocator
						.allocateNewPoolPage(request(31, 91, firstPayload), PhysicalSlotId(40), PhysicalSlotId(40),
											 LogSequenceNumber(2))
						.has_value());
	SharedPagePoolReplacementRequest replacement;
	replacement.segment_id = SegmentId(31);
	replacement.generation = Generation(1);
	replacement.page_id = PageId(91);
	replacement.expected_owner_lsn = LogSequenceNumber(2);
	replacement.page_owner_id = 99;
	replacement.page_owner_kind = PageOwnerKind::POK_INDEX;
	const std::array replacementPayload = {std::byte{0x99}};
	replacement.payload.assign(replacementPayload.begin(), replacementPayload.end());
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(allocator.replacePage(replacement, LogSequenceNumber(3), options)),
				 BootstrapPublishInterrupted);
	allocator.reconcilePool(SegmentId(31));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const SharedPagePoolContents contents = allocator.read(snapshot, PageId(91));
	EXPECT_EQ(contents.payload, std::vector<std::byte>(firstPayload.begin(), firstPayload.end()));
	EXPECT_EQ(contents.pool_header.allocated_frames, 1U);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, ReconcilesInterruptedReplacementDuringStartup) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array firstPayload = {std::byte{0x11}};
	ASSERT_TRUE(allocator
						.allocateNewPoolPage(request(31, 91, firstPayload), PhysicalSlotId(40), PhysicalSlotId(40),
											 LogSequenceNumber(2))
						.has_value());
	SharedPagePoolReplacementRequest replacement;
	replacement.segment_id = SegmentId(31);
	replacement.generation = Generation(1);
	replacement.page_id = PageId(91);
	replacement.expected_owner_lsn = LogSequenceNumber(2);
	replacement.page_owner_id = 99;
	replacement.page_owner_kind = PageOwnerKind::POK_INDEX;
	const std::array replacementPayload = {std::byte{0x99}};
	replacement.payload.assign(replacementPayload.begin(), replacementPayload.end());
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(allocator.replacePage(replacement, LogSequenceNumber(3), options)),
				 BootstrapPublishInterrupted);

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SharedPagePoolAllocator reopenedAllocator(io, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	const SharedPagePoolContents contents = reopenedAllocator.read(snapshot, PageId(91));
	EXPECT_EQ(contents.payload, std::vector<std::byte>(firstPayload.begin(), firstPayload.end()));
	EXPECT_EQ(contents.location.page_index, 1U);
	EXPECT_EQ(contents.pool_header.allocated_frames, 1U);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsUnallocatedCurrentFrameDuringStartupRecovery) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	const PageLocation unallocated{
			.page_id = PageId(92), .segment_id = SegmentId(31), .page_index = 2, .generation = Generation(1)};
	static_cast<void>(store->upsertLocations(std::span(&unallocated, 1), {}, LogSequenceNumber(3)));

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	expectFormatError([&] { static_cast<void>(SharedPagePoolAllocator(io, *reopened)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsMismatchedCurrentOwnerDuringStartupRecovery) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const uint64_t poolOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, PhysicalSlotId(40));
	snapshot.release();
	std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> ownerBytes{};
	ASSERT_EQ(io->readAt(poolOffset + RSS_PAGE_SIZE, ownerBytes.data(), ownerBytes.size()), ownerBytes.size());
	PageOwnerHeader owner = PageOwnerHeaderCodec::decode(ownerBytes);
	owner.page_id = PageId(92);
	ownerBytes = PageOwnerHeaderCodec::encode(owner);
	io->writeAt(poolOffset + RSS_PAGE_SIZE, ownerBytes.data(), ownerBytes.size());

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	expectFormatError([&] { static_cast<void>(SharedPagePoolAllocator(io, *reopened)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsDuplicatePageIdsWithoutChangingThePoolBitmap) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	SharedPagePoolAppendRequest append;
	append.segment_id = SegmentId(31);
	append.generation = Generation(1);
	append.page_id = PageId(91);
	append.page_owner_id = 100;
	append.page_owner_kind = PageOwnerKind::POK_INDEX;
	expectFormatError([&] { static_cast<void>(allocator.appendPage(append, LogSequenceNumber(3))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(allocator.read(snapshot, PageId(91)).pool_header.allocated_frames, 1U);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, ReconcilesAnInterruptedAppendWhenTheAllocatorOpens) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	SharedPagePoolAppendRequest append;
	append.segment_id = SegmentId(31);
	append.generation = Generation(1);
	append.page_id = PageId(92);
	append.page_owner_id = 100;
	append.page_owner_kind = PageOwnerKind::POK_INDEX;
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(allocator.appendPage(append, LogSequenceNumber(3), options)),
				 BootstrapPublishInterrupted);
	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SharedPagePoolAllocator reopenedAllocator(io, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedAllocator.read(snapshot, PageId(91)).pool_header.allocated_frames, 1U);
	expectFormatError([&] { static_cast<void>(reopenedAllocator.read(snapshot, PageId(92))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, ReconcilesAnInterruptedAppendAndReusesItsFrame) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	SharedPagePoolAppendRequest append;
	append.segment_id = SegmentId(31);
	append.generation = Generation(1);
	append.page_id = PageId(92);
	append.page_owner_id = 100;
	append.page_owner_kind = PageOwnerKind::POK_INDEX;
	const std::array payload = {std::byte{0x44}, std::byte{0x55}};
	append.payload.assign(payload.begin(), payload.end());
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(allocator.appendPage(append, LogSequenceNumber(3), options)),
				 BootstrapPublishInterrupted);

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SharedPagePoolAllocator reopenedAllocator(io, *reopened);
	// Construction has already reconciled the interrupted physical write.
	reopenedAllocator.reconcilePool(SegmentId(31));
	MetadataSnapshotPin reconciled = reopened->openSnapshot();
	EXPECT_EQ(reopenedAllocator.read(reconciled, PageId(91)).pool_header.allocated_frames, 1U);
	expectFormatError([&] { static_cast<void>(reopenedAllocator.read(reconciled, PageId(92))); },
					  FormatError::FERR_INVALID_STATE);
	reconciled.release();

	const SharedPagePoolAllocation allocation = reopenedAllocator.appendPage(append, LogSequenceNumber(3));
	EXPECT_EQ(allocation.location.page_index, 2U);
	MetadataSnapshotPin afterReuse = reopened->openSnapshot();
	EXPECT_EQ(reopenedAllocator.read(afterReuse, PageId(92)).payload,
			  std::vector<std::byte>(payload.begin(), payload.end()));
	EXPECT_EQ(reopenedAllocator.read(afterReuse, PageId(91)).pool_header.allocated_frames, 3U);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, ManuallyReconcilesAFrameWrittenBeforeFailedPublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	SharedPagePoolAppendRequest append;
	append.segment_id = SegmentId(31);
	append.generation = Generation(1);
	append.page_id = PageId(92);
	append.page_owner_id = 100;
	append.page_owner_kind = PageOwnerKind::POK_INDEX;
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(allocator.appendPage(append, LogSequenceNumber(3), options)),
				 BootstrapPublishInterrupted);
	MetadataSnapshotPin beforeRecovery = store->openSnapshot();
	EXPECT_EQ(allocator.read(beforeRecovery, PageId(91)).pool_header.allocated_frames, 3U);
	beforeRecovery.release();

	allocator.reconcilePool(SegmentId(31));
	MetadataSnapshotPin reconciled = store->openSnapshot();
	EXPECT_EQ(allocator.read(reconciled, PageId(91)).pool_header.allocated_frames, 1U);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, ReactivatesAReachableFrameMarkedRetiredBeforeAndDuringStartupRecovery) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::array payload = {std::byte{0x11}, std::byte{0x22}};
	const std::optional<SharedPagePoolAllocation> allocation = allocator.allocateNewPoolPage(
			request(31, 91, payload), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2));
	ASSERT_TRUE(allocation.has_value());

	const auto markCurrentFrameRetired = [&] {
		MetadataSnapshotPin snapshot = store->openSnapshot();
		const uint64_t poolOffset = PhysicalSegmentAllocator::checkedSlotOffset(
				snapshot.superblock().data_region_offset, PhysicalSlotId(40));
		snapshot.release();
		std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> ownerBytes{};
		const uint64_t ownerOffset =
				poolOffset + static_cast<uint64_t>(allocation->location.page_index) * RSS_PAGE_SIZE;
		ASSERT_EQ(io->readAt(ownerOffset, ownerBytes.data(), ownerBytes.size()), ownerBytes.size());
		PageOwnerHeader owner = PageOwnerHeaderCodec::decode(ownerBytes);
		owner.flags = RSS_PAGE_OWNER_FLAG_RETIRED;
		ownerBytes = PageOwnerHeaderCodec::encode(owner);
		io->writeAt(ownerOffset, ownerBytes.data(), ownerBytes.size());
	};

	markCurrentFrameRetired();
	allocator.reconcilePool(SegmentId(31));
	MetadataSnapshotPin afterManualReconciliation = store->openSnapshot();
	EXPECT_EQ(allocator.read(afterManualReconciliation, PageId(91)).owner.flags, 0U);
	EXPECT_EQ(allocator.read(afterManualReconciliation, PageId(91)).payload,
			  std::vector<std::byte>(payload.begin(), payload.end()));
	afterManualReconciliation.release();

	markCurrentFrameRetired();
	SharedPagePoolAllocator startupReconciledAllocator(io, *store);
	MetadataSnapshotPin afterStartupReconciliation = store->openSnapshot();
	EXPECT_EQ(startupReconciledAllocator.read(afterStartupReconciliation, PageId(91)).owner.flags, 0U);
	EXPECT_EQ(startupReconciledAllocator.read(afterStartupReconciliation, PageId(91)).payload,
			  std::vector<std::byte>(payload.begin(), payload.end()));
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsEveryEnvelopeAndPoolHeaderIdentityMismatchDuringRecovery) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const uint64_t poolOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, PhysicalSlotId(40));
	snapshot.release();
	std::array<std::byte, RSS_ENVELOPE_BYTES> originalEnvelopeBytes{};
	ASSERT_EQ(io->readAt(poolOffset, originalEnvelopeBytes.data(), originalEnvelopeBytes.size()),
			  originalEnvelopeBytes.size());
	const SegmentEnvelope originalEnvelope = SegmentEnvelopeCodec::decode(originalEnvelopeBytes);
	const auto expectEnvelopeMismatch = [&](const auto &mutate) {
		SegmentEnvelope changed = originalEnvelope;
		mutate(changed);
		const auto bytes = SegmentEnvelopeCodec::encode(changed);
		io->writeAt(poolOffset, bytes.data(), bytes.size());
		expectFormatError([&] { allocator.reconcilePool(SegmentId(31)); }, FormatError::FERR_INVALID_STATE);
		io->writeAt(poolOffset, originalEnvelopeBytes.data(), originalEnvelopeBytes.size());
	};
	expectEnvelopeMismatch([](SegmentEnvelope &envelope) { envelope.state = SegmentState::SGS_BUILDING; });
	expectEnvelopeMismatch([](SegmentEnvelope &envelope) { envelope.header_bytes = RSS_ENVELOPE_BYTES; });
	expectEnvelopeMismatch([](SegmentEnvelope &envelope) { envelope.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL; });
	expectEnvelopeMismatch([](SegmentEnvelope &envelope) { envelope.segment_id = SegmentId(32); });
	expectEnvelopeMismatch([](SegmentEnvelope &envelope) { ++envelope.owner_id; });
	expectEnvelopeMismatch([](SegmentEnvelope &envelope) { envelope.generation = Generation(2); });
	expectEnvelopeMismatch([](SegmentEnvelope &envelope) { envelope.committed_lsn = LogSequenceNumber(3); });

	std::array<std::byte, RSS_SHARED_PAGE_POOL_HEADER_BYTES> originalHeaderBytes{};
	ASSERT_EQ(io->readAt(poolOffset + RSS_ENVELOPE_BYTES, originalHeaderBytes.data(), originalHeaderBytes.size()),
			  originalHeaderBytes.size());
	const SharedPagePoolHeader originalHeader = SharedPagePoolHeaderCodec::decode(originalHeaderBytes);
	const auto expectHeaderMismatch = [&](const auto &mutate) {
		SharedPagePoolHeader changed = originalHeader;
		mutate(changed);
		const auto bytes = SharedPagePoolHeaderCodec::encode(changed);
		io->writeAt(poolOffset + RSS_ENVELOPE_BYTES, bytes.data(), bytes.size());
		expectFormatError([&] { allocator.reconcilePool(SegmentId(31)); }, FormatError::FERR_INVALID_STATE);
		io->writeAt(poolOffset + RSS_ENVELOPE_BYTES, originalHeaderBytes.data(), originalHeaderBytes.size());
	};
	expectHeaderMismatch([](SharedPagePoolHeader &header) { header.segment_id = SegmentId(32); });
	expectHeaderMismatch([](SharedPagePoolHeader &header) { header.generation = Generation(2); });
	expectHeaderMismatch([](SharedPagePoolHeader &header) { header.committed_lsn = LogSequenceNumber(3); });
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsInvalidAppendsAndAFullSharedPool) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	SharedPagePoolAppendRequest append;
	append.segment_id = SegmentId(31);
	append.generation = Generation(2);
	append.page_id = PageId(92);
	append.page_owner_id = 100;
	append.page_owner_kind = PageOwnerKind::POK_INDEX;
	expectFormatError([&] { static_cast<void>(allocator.appendPage(append, LogSequenceNumber(3))); },
					  FormatError::FERR_INVALID_HEADER);
	append.generation = Generation(1);
	for (uint64_t page = 92; page <= 121; ++page) {
		append.page_id = PageId(page);
		const SharedPagePoolAllocation allocation = allocator.appendPage(append, LogSequenceNumber(page - 89U));
		EXPECT_EQ(allocation.location.page_index, page - 90U);
	}
	append.page_id = PageId(122);
	expectFormatError([&] { static_cast<void>(allocator.appendPage(append, LogSequenceNumber(33))); },
					  FormatError::FERR_INVALID_STATE);
	SharedPagePoolReplacementRequest replacement;
	replacement.segment_id = SegmentId(31);
	replacement.generation = Generation(1);
	replacement.page_id = PageId(91);
	replacement.expected_owner_lsn = LogSequenceNumber(2);
	replacement.page_owner_id = 99;
	replacement.page_owner_kind = PageOwnerKind::POK_INDEX;
	expectFormatError([&] { static_cast<void>(allocator.replacePage(replacement, LogSequenceNumber(33))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(allocator.read(snapshot, PageId(91)).pool_header.allocated_frames, RSS_SHARED_PAGE_POOL_VALID_FRAME_MASK);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, ReconciliationFailsClosedForCorruptOrMismatchedFrames) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	const std::optional<SharedPagePoolAllocation> allocation = allocator.allocateNewPoolPage(
			request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2));
	ASSERT_TRUE(allocation.has_value());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const uint64_t poolOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, PhysicalSlotId(40));
	snapshot.release();

	const std::byte corrupt{0xFF};
	io->writeAt(poolOffset + RSS_PAGE_SIZE, &corrupt, sizeof(corrupt));
	expectFormatError([&] { allocator.reconcilePool(SegmentId(31)); }, FormatError::FERR_CHECKSUM_MISMATCH);

	const PageOwnerHeader repairedOwner{.page_id = PageId(91),
										.owner_id = 99,
										.kind = PageOwnerKind::POK_INDEX,
										.payload_bytes = 0,
										.generation = Generation(1),
										.committed_lsn = LogSequenceNumber(2)};
	const auto repairedOwnerBytes = PageOwnerHeaderCodec::encode(repairedOwner);
	io->writeAt(poolOffset + RSS_PAGE_SIZE, repairedOwnerBytes.data(), repairedOwnerBytes.size());
	PageLocation mismatched = allocation->location;
	mismatched.page_index = 2;
	static_cast<void>(store->upsertLocations(std::span(&mismatched, 1), {}, LogSequenceNumber(3)));
	expectFormatError([&] { allocator.reconcilePool(SegmentId(31)); }, FormatError::FERR_INVALID_STATE);
	PageLocation foreign = allocation->location;
	foreign.segment_id = SegmentId(32);
	static_cast<void>(store->upsertLocations(std::span(&foreign, 1), {}, LogSequenceNumber(4)));
	expectFormatError([&] { allocator.reconcilePool(SegmentId(31)); }, FormatError::FERR_INVALID_STATE);
	CatalogSlotReservationRequest foreignPool;
	foreignPool.segment_id = SegmentId(32);
	foreignPool.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	foreignPool.owner_id = 77;
	foreignPool.segment_generation = Generation(1);
	foreignPool.state = SegmentState::SGS_SEALED;
	foreignPool.require_absent = true;
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(foreignPool, PhysicalSlotId(41), PhysicalSlotId(41),
														LogSequenceNumber(5))
						.has_value());
	expectFormatError([&] { allocator.reconcilePool(SegmentId(31)); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsMalformedRequestsAndDuplicatePageIdsBeforeReuse) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	expectFormatError(
			[&] {
				SharedPagePoolAllocationRequest invalid = request(31, 91);
				invalid.segment_id = {};
				static_cast<void>(allocator.allocateNewPoolPage(invalid, PhysicalSlotId(40), PhysicalSlotId(40),
																LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				SharedPagePoolAllocationRequest invalid = request(31, 91);
				invalid.pool_kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
				static_cast<void>(allocator.allocateNewPoolPage(invalid, PhysicalSlotId(40), PhysicalSlotId(40),
																LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				SharedPagePoolAllocationRequest invalid = request(31, 91);
				invalid.page_owner_kind = PageOwnerKind::POK_PROPERTY;
				static_cast<void>(allocator.allocateNewPoolPage(invalid, PhysicalSlotId(40), PhysicalSlotId(40),
																LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> oversized(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 1U);
	expectFormatError(
			[&] {
				static_cast<void>(allocator.allocateNewPoolPage(request(31, 91, oversized), PhysicalSlotId(40),
																PhysicalSlotId(40), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				SharedPagePoolAllocationRequest invalid = request(31, 91);
				invalid.page_free_bytes = 1;
				static_cast<void>(allocator.allocateNewPoolPage(invalid, PhysicalSlotId(40), PhysicalSlotId(40),
																LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);

	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	SharedPagePoolReplacementRequest invalidReplacement;
	invalidReplacement.segment_id = SegmentId(31);
	invalidReplacement.generation = Generation(1);
	invalidReplacement.page_id = PageId(91);
	invalidReplacement.page_owner_id = 99;
	invalidReplacement.page_owner_kind = PageOwnerKind::POK_INDEX;
	expectFormatError([&] { static_cast<void>(allocator.replacePage(invalidReplacement, LogSequenceNumber(3))); },
					  FormatError::FERR_INVALID_HEADER);
	invalidReplacement.expected_owner_lsn = LogSequenceNumber(2);
	invalidReplacement.page_owner_kind = PageOwnerKind::POK_PROPERTY;
	expectFormatError([&] { static_cast<void>(allocator.replacePage(invalidReplacement, LogSequenceNumber(3))); },
					  FormatError::FERR_INVALID_HEADER);
	invalidReplacement.page_owner_kind = PageOwnerKind::POK_INDEX;
	invalidReplacement.page_free_bytes = 1;
	expectFormatError([&] { static_cast<void>(allocator.replacePage(invalidReplacement, LogSequenceNumber(3))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(allocator.allocateNewPoolPage(request(32, 91), PhysicalSlotId(41), PhysicalSlotId(41),
																LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_FALSE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(41)));
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, LeavesPreparedPoolUnreachableWhenPublicationStopsBeforeSuperblock) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40),
																 PhysicalSlotId(40), LogSequenceNumber(2), options)),
				 BootstrapPublishInterrupted);
	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(1));
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
	snapshot.release();
	SharedPagePoolAllocator reopenedAllocator(io, *reopened);
	EXPECT_TRUE(
			reopenedAllocator
					.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsCorruptPoolAndOwnerHeadersWithoutReadingPayload) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const uint64_t poolOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, PhysicalSlotId(40));
	std::array<std::byte, RSS_SHARED_PAGE_POOL_HEADER_BYTES> originalFirstControlRecord{};
	std::array<std::byte, RSS_SHARED_PAGE_POOL_HEADER_BYTES> originalSecondControlRecord{};
	ASSERT_EQ(io->readAt(poolOffset + RSS_ENVELOPE_BYTES, originalFirstControlRecord.data(),
						originalFirstControlRecord.size()),
			  originalFirstControlRecord.size());
	ASSERT_EQ(io->readAt(poolOffset + RSS_ENVELOPE_BYTES + RSS_SHARED_PAGE_POOL_HEADER_BYTES,
						originalSecondControlRecord.data(), originalSecondControlRecord.size()),
			  originalSecondControlRecord.size());
	const std::byte corrupt{0xFF};
	io->writeAt(poolOffset + RSS_ENVELOPE_BYTES, &corrupt, sizeof(corrupt));
	io->writeAt(poolOffset + RSS_ENVELOPE_BYTES + RSS_SHARED_PAGE_POOL_HEADER_BYTES, &corrupt, sizeof(corrupt));
	expectFormatError([&] { static_cast<void>(allocator.read(snapshot, PageId(91))); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
	snapshot.release();
	io->writeAt(poolOffset + RSS_ENVELOPE_BYTES, originalFirstControlRecord.data(),
				originalFirstControlRecord.size());
	io->writeAt(poolOffset + RSS_ENVELOPE_BYTES + RSS_SHARED_PAGE_POOL_HEADER_BYTES,
				originalSecondControlRecord.data(), originalSecondControlRecord.size());

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SharedPagePoolAllocator reopenedAllocator(io, *reopened);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	io->writeAt(poolOffset + RSS_PAGE_SIZE, &corrupt, sizeof(corrupt));
	expectFormatError([&] { static_cast<void>(reopenedAllocator.read(reopenedSnapshot, PageId(91))); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RequiresTheMetadataDataStorageHandle) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	EXPECT_NO_THROW(SharedPagePoolAllocator(io, *store));
	EXPECT_THROW(SharedPagePoolAllocator(walIo, *store), std::invalid_argument);
	EXPECT_THROW(SharedPagePoolAllocator(nullptr, *store), std::invalid_argument);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, SupportsOnlyDeclaredOpenModes) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	EXPECT_NO_THROW(SharedPagePoolAllocator(io, *store, SharedPagePoolOpenMode::SPPOM_READ_ONLY));
	EXPECT_NO_THROW(SharedPagePoolAllocator(io, *store, SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION));
	EXPECT_THROW(SharedPagePoolAllocator(io, *store, static_cast<SharedPagePoolOpenMode>(0)), std::invalid_argument);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsIncompleteTypedIndexRootEvacuationBeforeStorageMutation) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	SharedPagePoolEvacuationRequest evacuation;
	const IndexRootLocation expectedRoot{77, PageId(91), Generation(1)};
	expectFormatError(
			[&] {
				static_cast<void>(
						allocator.evacuateIndexPage(evacuation, expectedRoot, std::nullopt, LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, CountsOnlyCurrentSealedSharedPools) {
	io->enableBufferPool(256U * RSS_PAGE_SIZE);
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const CatalogEntry current = store->findCatalog(snapshot, SegmentId(31));
	io->sync();
	io->resetBufferPoolStats();
	EXPECT_EQ(SharedPagePoolAllocatorTestAccess::availableFrameCountUnderWrite(
					  *io, snapshot.superblock().data_region_offset, current),
			  RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U);
	EXPECT_EQ(allocator.availableFrameCount(current), RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U);
	const BufferPoolStats stats = io->bufferPoolStats();
	EXPECT_EQ(stats.hits, 2U);
	EXPECT_EQ(stats.misses, 0U);
	EXPECT_EQ(stats.disk_read_bytes, 0U);

	CatalogEntry building = current;
	building.state = SegmentState::SGS_BUILDING;
	expectFormatError([&] { static_cast<void>(allocator.availableFrameCount(building)); },
					  FormatError::FERR_INVALID_STATE);
	CatalogEntry fixedRecords = current;
	fixedRecords.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	expectFormatError([&] { static_cast<void>(allocator.availableFrameCount(fixedRecords)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, ReturnsNoAllocationWhenTheRequestedRangeHasNoFreeSlot) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	EXPECT_FALSE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(1), PhysicalSlotId(1), LogSequenceNumber(2))
					.has_value());
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsCatalogFreeSpaceAndEnvelopeDisagreementsOnRead) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40), false, LogSequenceNumber(3)));
	MetadataSnapshotPin released = store->openSnapshot();
	expectFormatError([&] { static_cast<void>(allocator.read(released, PageId(91))); },
					  FormatError::FERR_INVALID_STATE);
	released.release();

	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40), true, LogSequenceNumber(4)));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const uint64_t poolOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, PhysicalSlotId(40));
	std::array<std::byte, RSS_ENVELOPE_BYTES> envelopeBytes{};
	ASSERT_EQ(io->readAt(poolOffset, envelopeBytes.data(), envelopeBytes.size()), envelopeBytes.size());
	SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(envelopeBytes);
	++envelope.owner_id;
	envelopeBytes = SegmentEnvelopeCodec::encode(envelope);
	io->writeAt(poolOffset, envelopeBytes.data(), envelopeBytes.size());
	expectFormatError([&] { static_cast<void>(allocator.read(snapshot, PageId(91))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsValidButInconsistentPoolAndOwnerHeaders) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const uint64_t poolOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, PhysicalSlotId(40));
	std::array<std::byte, RSS_SHARED_PAGE_POOL_HEADER_BYTES> poolHeaderBytes{};
	ASSERT_EQ(io->readAt(poolOffset + RSS_ENVELOPE_BYTES, poolHeaderBytes.data(), poolHeaderBytes.size()),
			  poolHeaderBytes.size());
	SharedPagePoolHeader poolHeader = SharedPagePoolHeaderCodec::decode(poolHeaderBytes);
	poolHeader.segment_id = SegmentId(32);
	poolHeaderBytes = SharedPagePoolHeaderCodec::encode(poolHeader);
	io->writeAt(poolOffset + RSS_ENVELOPE_BYTES, poolHeaderBytes.data(), poolHeaderBytes.size());
	expectFormatError([&] { static_cast<void>(allocator.read(snapshot, PageId(91))); },
					  FormatError::FERR_INVALID_STATE);
	snapshot.release();

	poolHeader.segment_id = SegmentId(31);
	poolHeaderBytes = SharedPagePoolHeaderCodec::encode(poolHeader);
	io->writeAt(poolOffset + RSS_ENVELOPE_BYTES, poolHeaderBytes.data(), poolHeaderBytes.size());
	MetadataSnapshotPin repairedSnapshot = store->openSnapshot();
	std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> ownerBytes{};
	ASSERT_EQ(io->readAt(poolOffset + RSS_PAGE_SIZE, ownerBytes.data(), ownerBytes.size()), ownerBytes.size());
	PageOwnerHeader owner = PageOwnerHeaderCodec::decode(ownerBytes);
	owner.kind = PageOwnerKind::POK_PROPERTY;
	ownerBytes = PageOwnerHeaderCodec::encode(owner);
	io->writeAt(poolOffset + RSS_PAGE_SIZE, ownerBytes.data(), ownerBytes.size());
	expectFormatError([&] { static_cast<void>(allocator.read(repairedSnapshot, PageId(91))); },
					  FormatError::FERR_INVALID_STATE);
	repairedSnapshot.release();

	owner.kind = PageOwnerKind::POK_INDEX;
	owner.generation = Generation(2);
	ownerBytes = PageOwnerHeaderCodec::encode(owner);
	io->writeAt(poolOffset + RSS_PAGE_SIZE, ownerBytes.data(), ownerBytes.size());
	MetadataSnapshotPin generationMismatchSnapshot = store->openSnapshot();
	expectFormatError([&] { static_cast<void>(allocator.read(generationMismatchSnapshot, PageId(91))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsTruncatedPoolFramesWithoutConstructingPayloadRanges) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocateNewPoolPage(request(31, 91), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const uint64_t poolOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, PhysicalSlotId(40));
	dataFile->flush();
	dataFile->close();
	std::filesystem::resize_file(dataPath, poolOffset + RSS_ENVELOPE_BYTES);
	dataFile->open(dataPath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(dataFile->is_open());
	expectFormatError([&] { static_cast<void>(allocator.read(snapshot, PageId(91))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
}

TEST_F(RelocatableSharedPagePoolAllocatorTest, RejectsNonSharedPoolKindsIncludingTransactionPrivateBulkScratch) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	SharedPagePoolAllocator allocator(io, *store);
	SharedPagePoolAllocationRequest invalid = request(31, 91);
	invalid.pool_kind = SegmentKind::SGK_BULK_SCRATCH;
	expectFormatError(
			[&] {
				static_cast<void>(allocator.allocateNewPoolPage(invalid, PhysicalSlotId(40), PhysicalSlotId(40),
																LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
}
