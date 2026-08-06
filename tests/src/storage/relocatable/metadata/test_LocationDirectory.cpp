/**
 * @file test_LocationDirectory.cpp
 * @brief Codec, COW publication, and reopen tests for logical location directories.
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
#include <span>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	constexpr uint64_t TEST_DATABASE_ID = 0xA0B0C0D0E0F00123ULL;
	constexpr size_t HEADER_CRC_OFFSET = 52;
	constexpr size_t PAYLOAD_CRC_OFFSET = 48;

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	Superblock superblock(uint64_t generation = 1, uint64_t lsn = 1) {
		Superblock result;
		result.database_id = DatabaseId(TEST_DATABASE_ID);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(lsn);
		return result;
	}

	LocationLeafPage leaf(MetadataDirectoryType type = MetadataDirectoryType::MDT_PAGE) {
		LocationLeafPage result;
		result.directory_type = type;
		result.page_id = PageId(4);
		result.generation = Generation(2);
		result.committed_lsn = LogSequenceNumber(3);
		if (type == MetadataDirectoryType::MDT_PAGE) {
			result.entries = {{1, SegmentId(10), Generation(7), 1, 1, 1024},
							  {126, SegmentId(11), Generation(8), 31, 1, 2048}};
		} else {
			result.entries = {{1, SegmentId(20), Generation(4), 1, 2}, {126, SegmentId(21), Generation(5), 8, 24}};
		}
		return result;
	}

	void recalculateCrc(LocationLeafPageCodec::Bytes &bytes) {
		LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET,
									graph::utils::calculateCrc(bytes.data() + RSS_LOCATION_LEAF_HEADER_BYTES,
															   bytes.size() - RSS_LOCATION_LEAF_HEADER_BYTES));
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0);
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET,
									graph::utils::calculateCrc(bytes.data(), RSS_LOCATION_LEAF_HEADER_BYTES));
	}

	PageLocation page(uint64_t pageId, uint64_t segmentId, uint16_t pageIndex, uint64_t generation = 1,
					  uint32_t freeBytes = 0) {
		return {PageId(pageId), SegmentId(segmentId), pageIndex, Generation(generation), freeBytes};
	}

	ExtentLocation extent(uint64_t extentId, uint64_t segmentId, uint16_t firstPage, uint16_t pageCount,
						  uint64_t generation = 1) {
		return {ExtentId(extentId), SegmentId(segmentId), firstPage, pageCount, Generation(generation)};
	}

} // namespace

class RelocatableLocationDirectoryTest : public ::testing::Test {
protected:
	std::filesystem::path dataPath;
	std::filesystem::path walPath;
	std::shared_ptr<std::fstream> dataFile;
	std::shared_ptr<std::fstream> walFile;
	std::shared_ptr<StorageIO> dataIo;
	std::shared_ptr<StorageIO> walIo;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dataPath =
				std::filesystem::temp_directory_path() / ("relocatable_location_directory_" + to_string(uuid) + ".dat");
		walPath = dataPath.string() + ".foreign";
		dataFile = std::make_shared<std::fstream>(dataPath,
												  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		walFile = std::make_shared<std::fstream>(walPath,
												 std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(dataFile->is_open());
		ASSERT_TRUE(walFile->is_open());
		dataIo = std::make_shared<StorageIO>(dataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		walIo = std::make_shared<StorageIO>(walFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		dataIo.reset();
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

TEST(RelocatableLocationLeafCodecTest, UsesCanonicalBytesAndRoundTripsPageAndExtentDirectories) {
	for (const MetadataDirectoryType type: {MetadataDirectoryType::MDT_PAGE, MetadataDirectoryType::MDT_EXTENT}) {
		const LocationLeafPage expected = leaf(type);
		const auto encoded = LocationLeafPageCodec::encode(expected);
		EXPECT_EQ(encoded.size(), RSS_PAGE_SIZE);
		EXPECT_EQ(std::to_integer<uint8_t>(encoded[0]), 0x5AU);
		EXPECT_EQ(std::to_integer<uint8_t>(encoded[1]), 0x59U);
		EXPECT_EQ(std::to_integer<uint8_t>(encoded[2]), 0x58U);
		EXPECT_EQ(std::to_integer<uint8_t>(encoded[3]), 0x4CU);
		EXPECT_EQ(LittleEndianCodec::readU16(encoded, 4), RSS_LOCATION_LEAF_VERSION);
		EXPECT_EQ(LittleEndianCodec::readU32(encoded, 40), static_cast<uint32_t>(type));
		EXPECT_EQ(LocationLeafPageCodec::decode(encoded).entries, expected.entries);
		EXPECT_EQ(LocationLeafPageCodec::decode(encoded).directory_type, type);
	}
}

TEST(RelocatableLocationLeafCodecTest, SupportsNodeAndEdgeEntityLocationsWithZeroBasedSlots) {
	for (const MetadataDirectoryType type:
		 {MetadataDirectoryType::MDT_NODE_ENTITY, MetadataDirectoryType::MDT_EDGE_ENTITY}) {
		LocationLeafPage entityLeaf;
		entityLeaf.directory_type = type;
		entityLeaf.page_id = PageId(1);
		entityLeaf.generation = Generation(1);
		entityLeaf.committed_lsn = LogSequenceNumber(1);
		entityLeaf.entries = {{1, SegmentId(10), Generation(2), 0, 1}, {2, SegmentId(10), Generation(2), 1633, 1}};
		const auto encoded = LocationLeafPageCodec::encode(entityLeaf);
		const LocationLeafPage decoded = LocationLeafPageCodec::decode(encoded);
		EXPECT_EQ(decoded.directory_type, entityLeaf.directory_type);
		EXPECT_EQ(decoded.entries, entityLeaf.entries);
		entityLeaf.entries.front().page_count = 2;
		expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(entityLeaf)); },
						  FormatError::FERR_INVALID_HEADER);
	}
}

TEST(RelocatableEntityPropertyKeyTest, EncodesNodeAndEdgeNamespacesWithoutPropertyIdAllocation) {
	const EntityPropertyKey node{EntityPropertyOwnerKind::EPOK_NODE, 7U};
	const EntityPropertyKey edge{EntityPropertyOwnerKind::EPOK_EDGE, 7U};
	EXPECT_EQ(node.directoryId(), 7U);
	EXPECT_EQ(edge.directoryId(), (UINT64_C(1) << 63U) | 7U);
	EXPECT_EQ(EntityPropertyKey::fromDirectoryId(node.directoryId()), node);
	EXPECT_EQ(EntityPropertyKey::fromDirectoryId(edge.directoryId()), edge);
	EXPECT_EQ(EntityPropertyKey::fromDirectoryId(std::numeric_limits<uint64_t>::max()),
			  (EntityPropertyKey{EntityPropertyOwnerKind::EPOK_EDGE, (UINT64_C(1) << 63U) - 1U}));

	expectFormatError(
			[&] { static_cast<void>(EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 0U}.directoryId()); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(EntityPropertyKey::fromDirectoryId(0U)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableLocationLeafCodecTest, SupportsEntityPropertyLocations) {
	LocationLeafPage page;
	page.directory_type = MetadataDirectoryType::MDT_ENTITY_PROPERTY;
	page.page_id = PageId(1);
	page.generation = Generation(1);
	page.committed_lsn = LogSequenceNumber(1);
	page.entries = {{1U, SegmentId(20), Generation(1), 0U, 1U},
					{(UINT64_C(1) << 63U) | 1U, SegmentId(20), Generation(1), 1U, 1U}};
	const auto encoded = LocationLeafPageCodec::encode(page);
	const LocationLeafPage decoded = LocationLeafPageCodec::decode(encoded);
	EXPECT_EQ(decoded.directory_type, MetadataDirectoryType::MDT_ENTITY_PROPERTY);
	EXPECT_EQ(decoded.entries, page.entries);
}

TEST(RelocatableLocationLeafCodecTest, SupportsDedicatedIndexRootDirectoryEntries) {
	LocationLeafPage indexRoots;
	indexRoots.directory_type = MetadataDirectoryType::MDT_INDEX_ROOT;
	indexRoots.page_id = PageId(1);
	indexRoots.generation = Generation(1);
	indexRoots.committed_lsn = LogSequenceNumber(1);
	indexRoots.entries = {{7, SegmentId(101), Generation(3), RSS_PAGE_POOL_FIRST_DATA_FRAME, 1},
						  {9, SegmentId(102), Generation(4), RSS_PAGE_POOL_FIRST_DATA_FRAME, 1}};
	const auto encoded = LocationLeafPageCodec::encode(indexRoots);
	const LocationLeafPage decoded = LocationLeafPageCodec::decode(encoded);
	EXPECT_EQ(decoded.directory_type, indexRoots.directory_type);
	EXPECT_EQ(decoded.page_id, indexRoots.page_id);
	EXPECT_EQ(decoded.generation, indexRoots.generation);
	EXPECT_EQ(decoded.committed_lsn, indexRoots.committed_lsn);
	EXPECT_EQ(decoded.entries, indexRoots.entries);

	indexRoots.entries.front().first_page = RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(indexRoots)); },
					  FormatError::FERR_INVALID_HEADER);
	indexRoots.entries.front().first_page = RSS_PAGE_POOL_FIRST_DATA_FRAME;
	indexRoots.entries.front().page_count = 2;
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(indexRoots)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableLocationLeafCodecTest, SupportsAndValidatesRetireQueueEntries) {
	LocationLeafPage queue;
	queue.directory_type = MetadataDirectoryType::MDT_RETIRE_QUEUE;
	queue.page_id = PageId(1);
	queue.generation = Generation(4);
	queue.committed_lsn = LogSequenceNumber(8);
	queue.entries = {
			{7, SegmentId(11), Generation(4), static_cast<uint16_t>(RetireQueueDisposition::RQD_RELOCATED_SOURCE), 1},
			{9, SegmentId(12), Generation(4), static_cast<uint16_t>(RetireQueueDisposition::RQD_RETIRED_SEGMENT), 1}};

	const auto encoded = LocationLeafPageCodec::encode(queue);
	const LocationLeafPage decoded = LocationLeafPageCodec::decode(encoded);
	EXPECT_EQ(decoded.directory_type, MetadataDirectoryType::MDT_RETIRE_QUEUE);
	EXPECT_EQ(decoded.entries, queue.entries);

	queue.entries.front().first_page = 0;
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(queue)); },
					  FormatError::FERR_INVALID_HEADER);
	queue.entries.front().first_page = static_cast<uint16_t>(RetireQueueDisposition::RQD_RELOCATED_SOURCE);
	queue.entries.front().free_bytes = 1;
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(queue)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableLocationLeafCodecTest, RejectsInvalidEntriesAndCorruptDiskBytes) {
	LocationLeafPage invalid = leaf();
	invalid.directory_type = MetadataDirectoryType::MDT_CATALOG;
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = leaf();
	invalid.entries[0].page_count = 2;
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = leaf(MetadataDirectoryType::MDT_EXTENT);
	invalid.entries[1].first_page = 31;
	invalid.entries[1].page_count = 2;
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = leaf(MetadataDirectoryType::MDT_EXTENT);
	invalid.entries.front().free_bytes = 1;
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = leaf();
	invalid.entries[1].logical_id = 1;
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	const std::array<std::function<void(LocationLeafPage &)>, 10> mutations = {
			[](LocationLeafPage &page) { page.page_id = PageId{}; },
			[](LocationLeafPage &page) { page.generation = Generation{}; },
			[](LocationLeafPage &page) { page.committed_lsn = LogSequenceNumber{}; },
			[](LocationLeafPage &page) { page.entries.clear(); },
			[](LocationLeafPage &page) { page.entries[0].logical_id = 0; },
			[](LocationLeafPage &page) { page.entries[0].segment_id = SegmentId{}; },
			[](LocationLeafPage &page) { page.entries[0].generation = Generation{}; },
			[](LocationLeafPage &page) { page.entries[0].first_page = 0; },
			[](LocationLeafPage &page) { page.entries[0].page_count = 0; },
			[](LocationLeafPage &page) { page.entries[0].first_page = 32; },
	};
	for (const auto &mutate: mutations) {
		invalid = leaf();
		mutate(invalid);
		expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(invalid)); },
						  FormatError::FERR_INVALID_HEADER);
	}
	invalid = leaf();
	invalid.entries.clear();
	for (uint64_t index = 1; index <= RSS_LOCATION_LEAF_MAX_ENTRIES + 1U; ++index) {
		invalid.entries.push_back({index, SegmentId(index), Generation(1), 1, 1});
	}
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);

	const auto encoded = LocationLeafPageCodec::encode(leaf());
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::decode(std::span(encoded).first(64))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	auto corrupt = encoded;
	std::vector<std::byte> oversized(encoded.begin(), encoded.end());
	oversized.push_back(std::byte{});
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::decode(oversized)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt[0] ^= std::byte{1};
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 40, static_cast<uint32_t>(MetadataDirectoryType::MDT_CATALOG));
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 4, RSS_LOCATION_LEAF_VERSION - 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 92, RSS_PAGE_SIZE + 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	for (const auto [offset, value]: std::array<std::pair<size_t, uint32_t>, 4>{{{0, 0}, {8, 31}, {12, 0}, {44, 1}}}) {
		corrupt = encoded;
		LittleEndianCodec::writeU32(corrupt, offset, value);
		recalculateCrc(corrupt);
		expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::decode(corrupt)); },
						  FormatError::FERR_INVALID_HEADER);
	}
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 12, RSS_LOCATION_LEAF_MAX_ENTRIES + 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	corrupt[RSS_LOCATION_LEAF_HEADER_BYTES] ^= std::byte{1};
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST_F(RelocatableLocationDirectoryTest, AtomicallyPublishesPageAndExtentLocationsAndReopens) {
	auto store = RelocatableMetadataStore::initializeNew(dataIo, superblock());
	const std::vector<PageLocation> pages = {page(1, 10, 1), page(126, 11, 31), page(127, 12, 2)};
	const std::vector<ExtentLocation> extents = {extent(1, 20, 1, 4), extent(126, 21, 8, 24), extent(127, 22, 16, 8)};
	const Superblock published = store->upsertLocations(pages, extents, LogSequenceNumber(2));
	EXPECT_TRUE(published.page_directory_root.isValid());
	EXPECT_TRUE(published.extent_directory_root.isValid());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(store->findPageLocation(snapshot, PageId(127)), pages.back());
	EXPECT_EQ(store->findExtentLocation(snapshot, ExtentId(126)), extents[1]);
	expectFormatError([&] { static_cast<void>(store->findPageLocation(snapshot, PageId(2))); },
					  FormatError::FERR_INVALID_STATE);
	snapshot.release();

	auto reopened = RelocatableMetadataStore::open(dataIo);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedSnapshot.superblock(), published);
	EXPECT_EQ(reopened->findPageLocation(reopenedSnapshot, PageId(1)), pages.front());
	EXPECT_EQ(reopened->findExtentLocation(reopenedSnapshot, ExtentId(127)), extents.back());
}

TEST_F(RelocatableLocationDirectoryTest, PreservesOldSnapshotAndUpdatesOneDirectoryWithoutChangingTheOther) {
	auto store = RelocatableMetadataStore::initializeNew(dataIo, superblock());
	const Superblock first =
			store->upsertLocations(std::vector{page(1, 10, 1)}, std::vector{extent(1, 20, 1, 2)}, LogSequenceNumber(2));
	MetadataSnapshotPin oldSnapshot = store->openSnapshot();
	const Superblock second = store->upsertLocations(std::vector{page(1, 30, 5, 2)}, std::span<const ExtentLocation>{},
													 LogSequenceNumber(3));
	EXPECT_NE(first.page_directory_root, second.page_directory_root);
	EXPECT_EQ(first.extent_directory_root, second.extent_directory_root);
	EXPECT_EQ(store->findPageLocation(oldSnapshot, PageId(1)), page(1, 10, 1));
	EXPECT_EQ(store->findExtentLocation(oldSnapshot, ExtentId(1)), extent(1, 20, 1, 2));
	MetadataSnapshotPin currentSnapshot = store->openSnapshot();
	EXPECT_EQ(store->findPageLocation(currentSnapshot, PageId(1)), page(1, 30, 5, 2));
	expectFormatError([&] { static_cast<void>(store->findPageLocation(currentSnapshot, PageId(2))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableLocationDirectoryTest, GrowsAndReloadsAnInternalRadixPath) {
	auto store = RelocatableMetadataStore::initializeNew(dataIo, superblock());
	const uint64_t deepId = 1U + static_cast<uint64_t>(RSS_LOCATION_LEAF_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT;
	const Superblock first = store->upsertLocations(std::vector{page(1, 10, 1)}, {}, LogSequenceNumber(2));
	const Superblock second = store->upsertLocations(
			std::vector{page(deepId, 20, 2), page(deepId + RSS_LOCATION_LEAF_MAX_ENTRIES, 21, 3)}, {},
			LogSequenceNumber(3));
	EXPECT_NE(first.page_directory_root, second.page_directory_root);
	MetadataSnapshotPin beforeUpdate = store->openSnapshot();
	const Superblock third = store->upsertLocations(std::vector{page(deepId, 30, 3, 2)}, {}, LogSequenceNumber(4));
	EXPECT_NE(second.page_directory_root, third.page_directory_root);
	EXPECT_EQ(store->findPageLocation(beforeUpdate, PageId(deepId)), page(deepId, 20, 2));
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(store->findPageLocation(current, PageId(deepId)), page(deepId, 30, 3, 2));
}

TEST_F(RelocatableLocationDirectoryTest, ReplacesAndErasesNativeEntityPageLocationsInOneCowRootWithoutLostUpdates) {
	const Superblock initial = superblock();
	SuperblockFile bootstrap(dataIo);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);
	const std::array first = {EntityPageLocation{1, PageId(10), 0, 1}, EntityPageLocation{2, PageId(10), 1, 1}};
	MetadataWriteBatch firstBatch(allocator, initial, LogSequenceNumber(2));
	directory.prepareReplaceNodePageLocations(firstBatch, first, {});
	directory.prepareReplaceEdgePageLocations(firstBatch, first, {});
	firstBatch.publish(bootstrap);
	const Superblock firstPublished = firstBatch.candidate();
	EXPECT_EQ(directory.findNodePageLocation(firstPublished, 1), first[0]);
	EXPECT_EQ(directory.findNodePageLocation(firstPublished, 2), first[1]);
	EXPECT_EQ(directory.findEdgePageLocationOptional(firstPublished, 1), first[0]);

	const EntityPageLocation replacement{2, PageId(10), 7, 2};
	const std::array erased = {uint64_t{1}};
	MetadataWriteBatch secondBatch(allocator, firstPublished, LogSequenceNumber(3));
	directory.prepareReplaceNodePageLocations(secondBatch, std::span(&replacement, 1), erased);
	secondBatch.publish(bootstrap);
	const Superblock secondPublished = secondBatch.candidate();
	expectFormatError([&] { static_cast<void>(directory.findNodePageLocation(secondPublished, 1)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(directory.findNodePageLocation(secondPublished, 2), replacement);
	EXPECT_EQ(directory.findNodePageLocation(firstPublished, 1), first[0]);

	const uint64_t deepId = 1U + static_cast<uint64_t>(RSS_LOCATION_LEAF_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT;
	const EntityPageLocation deep{deepId, PageId(10), 3, 2};
	MetadataWriteBatch deepBatch(allocator, secondPublished, LogSequenceNumber(4));
	directory.prepareReplaceNodePageLocations(deepBatch, std::span(&deep, 1), {});
	deepBatch.publish(bootstrap);
	const Superblock deepPublished = deepBatch.candidate();
	EXPECT_EQ(pages.readMetadataRadix(deepPublished.node_entity_directory_root).level, 1U);
	EXPECT_EQ(directory.findNodePageLocation(deepPublished, deepId), deep);

	MetadataWriteBatch invalidEmpty(allocator, deepPublished, LogSequenceNumber(5));
	expectFormatError([&] { directory.prepareReplaceNodePageLocations(invalidEmpty, {}, {}); },
					  FormatError::FERR_INVALID_STATE);
	const std::array unordered = {EntityPageLocation{3, PageId(10), 1, 2}, EntityPageLocation{2, PageId(10), 2, 2}};
	MetadataWriteBatch invalidOrder(allocator, deepPublished, LogSequenceNumber(5));
	expectFormatError([&] { directory.prepareReplaceNodePageLocations(invalidOrder, unordered, {}); },
					  FormatError::FERR_INVALID_HEADER);
	const std::array overlapping = {uint64_t{2}};
	MetadataWriteBatch invalidOverlap(allocator, deepPublished, LogSequenceNumber(5));
	expectFormatError(
			[&] { directory.prepareReplaceNodePageLocations(invalidOverlap, std::span(&replacement, 1), overlapping); },
					  FormatError::FERR_INVALID_HEADER);
	const std::array missing = {uint64_t{99}};
	MetadataWriteBatch missingErase(allocator, deepPublished, LogSequenceNumber(5));
	expectFormatError([&] { directory.prepareReplaceNodePageLocations(missingErase, {}, missing); },
					  FormatError::FERR_INVALID_STATE);
	const uint64_t missingRouteId =
			1U + static_cast<uint64_t>(RSS_LOCATION_LEAF_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT * 2U;
	const std::array missingRoute = {missingRouteId};
	MetadataWriteBatch missingRouteErase(allocator, deepPublished, LogSequenceNumber(5));
	expectFormatError([&] { directory.prepareReplaceNodePageLocations(missingRouteErase, {}, missingRoute); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { directory.visitNodePageLocations(deepPublished, {}); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { directory.visitEdgePageLocations(deepPublished, {}); }, FormatError::FERR_INVALID_HEADER);

	const MetadataRadixPage deepRoot = pages.readMetadataRadix(deepPublished.node_entity_directory_root);
	MetadataRadixPage duplicateRoot = deepRoot;
	duplicateRoot.children[2] = deepRoot.children[0];
	pages.writeMetadataRadix(deepPublished.node_entity_directory_root, duplicateRoot);
	expectFormatError([&] { directory.visitNodePageLocations(deepPublished, [](const EntityPageLocation &) {}); },
					  FormatError::FERR_INVALID_HEADER);
	pages.writeMetadataRadix(deepPublished.node_entity_directory_root, deepRoot);

	const MetadataRadixPage firstRoot = pages.readMetadataRadix(firstPublished.node_entity_directory_root);
	const LocationLeafPage firstLeaf = pages.readLocationLeaf(firstRoot.children[0]);
	LocationLeafPage wrongTypeLeaf = firstLeaf;
	wrongTypeLeaf.directory_type = MetadataDirectoryType::MDT_EDGE_ENTITY;
	pages.writeLocationLeaf(firstRoot.children[0], wrongTypeLeaf);
	expectFormatError([&] { directory.visitNodePageLocations(firstPublished, [](const EntityPageLocation &) {}); },
					  FormatError::FERR_INVALID_HEADER);
	pages.writeLocationLeaf(firstRoot.children[0], firstLeaf);

	const uint32_t deepChildIndex = CatalogRadixPageCodec::childIndexForLeafOrdinal(
			(deepId - 1U) / RSS_LOCATION_LEAF_MAX_ENTRIES, deepRoot.level);
	const MetadataRadixPage deepChild = pages.readMetadataRadix(deepRoot.children[deepChildIndex]);
	MetadataRadixPage malformedChild = deepChild;
	malformedChild.kind = CatalogRadixPageKind::CRPK_ROOT;
	pages.writeMetadataRadix(deepRoot.children[deepChildIndex], malformedChild);
	expectFormatError([&] { directory.visitNodePageLocations(deepPublished, [](const EntityPageLocation &) {}); },
					  FormatError::FERR_INVALID_HEADER);
	pages.writeMetadataRadix(deepRoot.children[deepChildIndex], deepChild);

	Superblock emptyEntityRoot = initial;
	MetadataWriteBatch absentErase(allocator, emptyEntityRoot, LogSequenceNumber(2));
	expectFormatError([&] { directory.prepareReplaceNodePageLocations(absentErase, {}, erased); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableLocationDirectoryTest, StreamsNativeEntityPageMappingsWithoutLegacySegmentLocations) {
	const Superblock initial = superblock();
	SuperblockFile bootstrap(dataIo);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);

	const std::array nodes = {EntityPageLocation{3U, PageId(101U), 0U, 1U},
							  EntityPageLocation{7U, PageId(101U), 1U, 1U}};
	const std::array edges = {EntityPageLocation{5U, PageId(201U), 0U, 2U}};
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2U));
	directory.prepareReplaceNodePageLocations(batch, nodes, {});
	directory.prepareReplaceEdgePageLocations(batch, edges, {});
	batch.publish(bootstrap);
	const Superblock published = batch.candidate();

	EXPECT_EQ(directory.findNodePageLocation(published, nodes.front().entity_id), nodes.front());
	EXPECT_EQ(directory.findNodePageLocationOptional(published, nodes.back().entity_id),
			  std::optional<EntityPageLocation>(nodes.back()));
	EXPECT_EQ(directory.findEdgePageLocation(published, edges.front().entity_id), edges.front());
	EXPECT_FALSE(directory.findEdgePageLocationOptional(published, 99U).has_value());

	std::vector<EntityPageLocation> visitedNodes;
	EXPECT_FALSE(directory.visitNodePageLocationsUntil(published, [&visitedNodes](const EntityPageLocation &location) {
		visitedNodes.push_back(location);
		return false;
	}));
	EXPECT_EQ(visitedNodes, (std::vector<EntityPageLocation>{nodes.front()}));
	visitedNodes.clear();
	EXPECT_TRUE(directory.visitNodePageLocationsUntil(published, [&visitedNodes](const EntityPageLocation &location) {
		visitedNodes.push_back(location);
		return true;
	}));
	EXPECT_EQ(visitedNodes, (std::vector<EntityPageLocation>{nodes.begin(), nodes.end()}));

	std::vector<EntityPageLocation> resumed;
	size_t metadataPages = 0U;
	EXPECT_TRUE(directory.visitNodePageLocationsAfter(
			published, nodes.front().entity_id, 4U,
			[&resumed](const EntityPageLocation &location) {
				resumed.push_back(location);
				return true;
			},
			&metadataPages));
	EXPECT_EQ(resumed, (std::vector<EntityPageLocation>{nodes.back()}));
	EXPECT_GT(metadataPages, 0U);

	EXPECT_FALSE(directory.visitEdgePageLocationsUntil(published, [](const EntityPageLocation &) { return false; }));
	expectFormatError([&] { static_cast<void>(directory.visitNodePageLocationsUntil(published, {})); },
				  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] {
		static_cast<void>(directory.visitNodePageLocationsAfter(published, 0U, 0U,
																							  [](const EntityPageLocation &) { return true; }));
	}, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableLocationDirectoryTest, ResumesSparseNodeDirectoryRoutesWithinAnExplicitPageBudget) {
	const Superblock initial = superblock();
	SuperblockFile bootstrap(dataIo);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);

	EXPECT_TRUE(directory.visitNodePageLocationsAfter(initial, std::numeric_limits<uint64_t>::max(), 1U,
																	 [](const EntityPageLocation &) { return true; }));
	EXPECT_TRUE(directory.visitNodePageLocationsAfter(initial, 0U, 1U, [](const EntityPageLocation &) { return true; }));
	const EntityPageLocation first{1U, PageId(10U), 0U, 1U};
	MetadataWriteBatch firstBatch(allocator, initial, LogSequenceNumber(2U));
	directory.prepareReplaceNodePageLocations(firstBatch, std::span(&first, 1U), {});
	firstBatch.publish(bootstrap);

	const uint64_t deepId = 1U + static_cast<uint64_t>(RSS_LOCATION_LEAF_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT;
	const std::array deep = {EntityPageLocation{deepId, PageId(20U), 1U, 2U},
							 EntityPageLocation{deepId + 1U, PageId(20U), 2U, 2U}};
	MetadataWriteBatch deepBatch(allocator, firstBatch.candidate(), LogSequenceNumber(3U));
	directory.prepareReplaceNodePageLocations(deepBatch, deep, {});
	deepBatch.publish(bootstrap);
	const Superblock published = deepBatch.candidate();

	std::vector<EntityPageLocation> visited;
	EXPECT_FALSE(directory.visitNodePageLocationsAfter(
			published, deepId - 1U, 4U, [&visited](const EntityPageLocation &location) {
		visited.push_back(location);
		return false;
	}));
	EXPECT_EQ(visited, (std::vector<EntityPageLocation>{deep[0]}));
	visited.clear();
	EXPECT_TRUE(directory.visitNodePageLocationsAfter(published, deepId, 3U, [&visited](const EntityPageLocation &location) {
		visited.push_back(location);
		return true;
	}));
	EXPECT_EQ(visited, (std::vector<EntityPageLocation>{deep[1]}));
	expectFormatError(
			[&] {
				static_cast<void>(directory.visitNodePageLocationsAfter(published, deepId - 1U, 3U,
																																	[](const EntityPageLocation &) { return true; }));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(directory.visitNodePageLocationsAfter(published, deepId - 1U, 0U,
																																	[](const EntityPageLocation &) { return true; }));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableLocationDirectoryTest, TraversesAndErasesDeepNodeRoutesWithinMetadataPageBounds) {
	const Superblock initial = superblock();
	SuperblockFile bootstrap(dataIo);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);
	const uint64_t firstDeepId = 1U + static_cast<uint64_t>(RSS_LOCATION_LEAF_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT;
	const std::array deep = {EntityPageLocation{firstDeepId, PageId(10), 0, 1},
							 EntityPageLocation{firstDeepId + RSS_LOCATION_LEAF_MAX_ENTRIES, PageId(11), 1, 1}};

	MetadataPagePoolAllocator foreignAllocator(dataIo, initial);
	MetadataWriteBatch foreignBatch(foreignAllocator, initial, LogSequenceNumber(2));
	expectFormatError([&] { directory.prepareReplaceNodePageLocations(foreignBatch, deep, {}); },
					  FormatError::FERR_INVALID_STATE);

	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
	directory.prepareReplaceNodePageLocations(batch, deep, {});
	batch.publish(bootstrap);
	const Superblock published = batch.candidate();
	std::vector<EntityPageLocation> visited;
	directory.visitNodePageLocations(published,
															[&visited](const EntityPageLocation &location) { visited.push_back(location); });
	EXPECT_EQ(visited, (std::vector<EntityPageLocation>{deep[0], deep[1]}));

	size_t visitedMetadataPages = std::numeric_limits<size_t>::max();
	EXPECT_TRUE(directory.visitNodePageLocationsAfter(
			published, 0U, 4U, [](const EntityPageLocation &) { return true; }, &visitedMetadataPages));
	EXPECT_EQ(visitedMetadataPages, 4U);
	visitedMetadataPages = std::numeric_limits<size_t>::max();
	EXPECT_TRUE(directory.visitNodePageLocationsAfter(
			initial, 0U, 1U, [](const EntityPageLocation &) { return true; }, &visitedMetadataPages));
	EXPECT_EQ(visitedMetadataPages, 0U);
	visitedMetadataPages = std::numeric_limits<size_t>::max();
	EXPECT_TRUE(directory.visitNodePageLocationsAfter(
			published, std::numeric_limits<uint64_t>::max(), 1U, [](const EntityPageLocation &) { return true; },
			&visitedMetadataPages));
	EXPECT_EQ(visitedMetadataPages, 0U);
	expectFormatError([&] { static_cast<void>(directory.visitNodePageLocationsAfter(published, 0U, 1U, {}, nullptr)); },
					  FormatError::FERR_INVALID_HEADER);

	const std::array invalidErase = {uint64_t{0}};
	MetadataWriteBatch invalidEraseBatch(allocator, published, LogSequenceNumber(3));
	expectFormatError([&] { directory.prepareReplaceNodePageLocations(invalidEraseBatch, {}, invalidErase); },
					  FormatError::FERR_INVALID_HEADER);
	const std::array unorderedErase = {firstDeepId + 1U, firstDeepId};
	MetadataWriteBatch unorderedEraseBatch(allocator, published, LogSequenceNumber(3));
	expectFormatError([&] { directory.prepareReplaceNodePageLocations(unorderedEraseBatch, {}, unorderedErase); },
					  FormatError::FERR_INVALID_HEADER);
	const std::array erased = {firstDeepId};
	MetadataWriteBatch eraseBatch(allocator, published, LogSequenceNumber(3));
	directory.prepareReplaceNodePageLocations(eraseBatch, {}, erased);
	eraseBatch.publish(bootstrap);
	expectFormatError([&] { static_cast<void>(directory.findNodePageLocation(eraseBatch.candidate(), firstDeepId)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(directory.findNodePageLocation(eraseBatch.candidate(), deep[1].entity_id), deep[1]);
}

TEST_F(RelocatableLocationDirectoryTest, RejectsInvalidEntityRangeAbsenceProofRequests) {
	const Superblock initial = superblock();
	SuperblockFile bootstrap(dataIo);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);

	expectFormatError([&] { directory.requireNodeRangeAbsent(initial, 0U, 1U); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { directory.requireEdgeRangeAbsent(initial, 1U, 0U); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { directory.requireNodeRangeAbsent(initial, std::numeric_limits<uint64_t>::max(), 2U); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableLocationDirectoryTest, ReplacesAndErasesPageLocationsInOneCowRootWithoutLostUpdates) {
	const Superblock initial = superblock();
	SuperblockFile bootstrap(dataIo);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);
	const std::array first = {page(1, 10, 1), page(2, 10, 2)};
	MetadataWriteBatch firstBatch(allocator, initial, LogSequenceNumber(2));
	directory.prepareReplacePages(firstBatch, first, {});
	firstBatch.publish(bootstrap);
	const Superblock firstPublished = firstBatch.candidate();
	EXPECT_EQ(directory.findPage(firstPublished, PageId(1)), first[0]);
	EXPECT_EQ(directory.findPage(firstPublished, PageId(2)), first[1]);

	const PageLocation replacement = page(2, 11, 4, 2, 128);
	const std::array erased = {uint64_t{1}};
	MetadataWriteBatch secondBatch(allocator, firstPublished, LogSequenceNumber(3));
	directory.prepareReplacePages(secondBatch, std::span(&replacement, 1), erased);
	secondBatch.publish(bootstrap);
	const Superblock secondPublished = secondBatch.candidate();
	expectFormatError([&] { static_cast<void>(directory.findPage(secondPublished, PageId(1))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(directory.findPage(secondPublished, PageId(2)), replacement);
	EXPECT_EQ(directory.findPage(firstPublished, PageId(1)), first[0]);

	MetadataWriteBatch invalidOverlap(allocator, secondPublished, LogSequenceNumber(4));
	const std::array overlap = {uint64_t{2}};
	expectFormatError([&] { directory.prepareReplacePages(invalidOverlap, std::span(&replacement, 1), overlap); },
					  FormatError::FERR_INVALID_HEADER);
	MetadataWriteBatch missingErase(allocator, secondPublished, LogSequenceNumber(4));
	const std::array missing = {uint64_t{99}};
	expectFormatError([&] { directory.prepareReplacePages(missingErase, {}, missing); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableLocationDirectoryTest, ReplacesAndErasesExtentLocationsInOneCowRootWithoutLostUpdates) {
	const Superblock initial = superblock();
	SuperblockFile bootstrap(dataIo);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);
	const std::array first = {extent(1, 10, RSS_PAGE_POOL_FIRST_DATA_FRAME, RSS_PAGE_POOL_DATA_FRAME_COUNT),
							  extent(2, 11, RSS_PAGE_POOL_FIRST_DATA_FRAME, RSS_PAGE_POOL_DATA_FRAME_COUNT)};
	MetadataWriteBatch firstBatch(allocator, initial, LogSequenceNumber(2));
	directory.prepareReplaceExtents(firstBatch, first, {});
	firstBatch.publish(bootstrap);
	const Superblock firstPublished = firstBatch.candidate();
	EXPECT_EQ(directory.findExtent(firstPublished, ExtentId(1)), first[0]);
	EXPECT_EQ(directory.findExtent(firstPublished, ExtentId(2)), first[1]);

	const ExtentLocation replacement = extent(2, 12, RSS_PAGE_POOL_FIRST_DATA_FRAME, RSS_PAGE_POOL_DATA_FRAME_COUNT, 2);
	const std::array erased = {uint64_t{1}};
	MetadataWriteBatch secondBatch(allocator, firstPublished, LogSequenceNumber(3));
	directory.prepareReplaceExtents(secondBatch, std::span(&replacement, 1), erased);
	secondBatch.publish(bootstrap);
	const Superblock secondPublished = secondBatch.candidate();
	expectFormatError([&] { static_cast<void>(directory.findExtent(secondPublished, ExtentId(1))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(directory.findExtent(secondPublished, ExtentId(2)), replacement);
	EXPECT_EQ(directory.findExtent(firstPublished, ExtentId(1)), first[0]);

	MetadataWriteBatch invalidOverlap(allocator, secondPublished, LogSequenceNumber(4));
	const std::array overlap = {uint64_t{2}};
	expectFormatError([&] { directory.prepareReplaceExtents(invalidOverlap, std::span(&replacement, 1), overlap); },
					  FormatError::FERR_INVALID_HEADER);
	MetadataWriteBatch missingErase(allocator, secondPublished, LogSequenceNumber(4));
	const std::array missing = {uint64_t{99}};
	expectFormatError([&] { directory.prepareReplaceExtents(missingErase, {}, missing); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableLocationDirectoryTest, ReplacesStateLocationsThroughTheirIndependentCowRoot) {
	const Superblock initial = superblock();
	SuperblockFile bootstrap(dataIo);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);
	const StateLocation first{17, PageId(91), 0, 4};
	MetadataWriteBatch firstBatch(allocator, initial, LogSequenceNumber(2));
	directory.prepareReplaceStates(firstBatch, std::span(&first, 1), {});
	firstBatch.publish(bootstrap);
	const Superblock firstPublished = firstBatch.candidate();
	EXPECT_TRUE(firstPublished.state_directory_root.isValid());
	EXPECT_EQ(directory.findState(firstPublished, first.state_id), first);

	const StateLocation moved{17, PageId(92), 3, 5};
	MetadataWriteBatch secondBatch(allocator, firstPublished, LogSequenceNumber(3));
	directory.prepareReplaceStates(secondBatch, std::span(&moved, 1), {});
	secondBatch.publish(bootstrap);
	const Superblock secondPublished = secondBatch.candidate();
	EXPECT_NE(firstPublished.state_directory_root, secondPublished.state_directory_root);
	EXPECT_EQ(directory.findState(firstPublished, first.state_id), first);
	EXPECT_EQ(directory.findState(secondPublished, moved.state_id), moved);
	EXPECT_EQ(directory.findStateOptional(secondPublished, moved.state_id), std::optional<StateLocation>(moved));
	EXPECT_FALSE(directory.findStateOptional(secondPublished, 99).has_value());
	std::vector<StateLocation> visited;
	directory.visitStates(secondPublished, [&visited](const StateLocation &location) { visited.push_back(location); });
	EXPECT_EQ(visited, std::vector<StateLocation>{moved});
	expectFormatError([&] { directory.visitStates(secondPublished, {}); }, FormatError::FERR_INVALID_HEADER);

	MetadataWriteBatch eraseBatch(allocator, secondPublished, LogSequenceNumber(4));
	const std::array erased = {moved.state_id};
	directory.prepareReplaceStates(eraseBatch, {}, erased);
	eraseBatch.publish(bootstrap);
	expectFormatError([&] { static_cast<void>(directory.findState(eraseBatch.candidate(), moved.state_id)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(directory.findState(secondPublished, moved.state_id), moved);
}

TEST_F(RelocatableLocationDirectoryTest, ReplacesVectorLocationsAndManifestsThroughIndependentCowRoots) {
	const Superblock initial = superblock();
	SuperblockFile bootstrap(dataIo);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);

	const VectorLocation firstVector{71, PageId(91), 3, 4};
	const VectorManifestLocation firstManifest{17, PageId(92), Generation(5)};
	MetadataWriteBatch firstBatch(allocator, initial, LogSequenceNumber(2));
	directory.prepareReplaceVectorLocations(firstBatch, std::span(&firstVector, 1), {});
	directory.prepareReplaceVectorManifests(firstBatch, std::span(&firstManifest, 1), {});
	firstBatch.publish(bootstrap);
	const Superblock firstPublished = firstBatch.candidate();
	EXPECT_TRUE(firstPublished.vector_location_directory_root.isValid());
	EXPECT_TRUE(firstPublished.vector_manifest_directory_root.isValid());
	EXPECT_EQ(directory.findVectorLocation(firstPublished, firstVector.vector_id), firstVector);
	EXPECT_EQ(directory.findVectorManifest(firstPublished, firstManifest.index_id), firstManifest);

	const VectorLocation movedVector{71, PageId(93), 1, 6};
	const VectorManifestLocation replacedManifest{17, PageId(94), Generation(7)};
	MetadataWriteBatch secondBatch(allocator, firstPublished, LogSequenceNumber(3));
	directory.prepareReplaceVectorLocations(secondBatch, std::span(&movedVector, 1), {});
	directory.prepareReplaceVectorManifests(secondBatch, std::span(&replacedManifest, 1), {});
	secondBatch.publish(bootstrap);
	const Superblock secondPublished = secondBatch.candidate();
	EXPECT_NE(firstPublished.vector_location_directory_root, secondPublished.vector_location_directory_root);
	EXPECT_NE(firstPublished.vector_manifest_directory_root, secondPublished.vector_manifest_directory_root);
	EXPECT_EQ(directory.findVectorLocation(firstPublished, firstVector.vector_id), firstVector);
	EXPECT_EQ(directory.findVectorManifest(firstPublished, firstManifest.index_id), firstManifest);
	EXPECT_EQ(directory.findVectorLocation(secondPublished, movedVector.vector_id), movedVector);
	EXPECT_EQ(directory.findVectorManifest(secondPublished, replacedManifest.index_id), replacedManifest);
	EXPECT_EQ(directory.findVectorLocationOptional(secondPublished, movedVector.vector_id),
			  std::optional<VectorLocation>(movedVector));
	EXPECT_EQ(directory.findVectorManifestOptional(secondPublished, replacedManifest.index_id),
			  std::optional<VectorManifestLocation>(replacedManifest));
	EXPECT_FALSE(directory.findVectorLocationOptional(secondPublished, 99).has_value());
	EXPECT_FALSE(directory.findVectorManifestOptional(secondPublished, 99).has_value());

	MetadataWriteBatch eraseBatch(allocator, secondPublished, LogSequenceNumber(4));
	const std::array erasedVector = {movedVector.vector_id};
	const std::array erasedIndex = {replacedManifest.index_id};
	directory.prepareReplaceVectorLocations(eraseBatch, {}, erasedVector);
	directory.prepareReplaceVectorManifests(eraseBatch, {}, erasedIndex);
	eraseBatch.publish(bootstrap);
	expectFormatError([&] { static_cast<void>(directory.findVectorLocation(eraseBatch.candidate(), 71)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(directory.findVectorManifest(eraseBatch.candidate(), 17)); },
					  FormatError::FERR_INVALID_STATE);

	const VectorLocation invalidVector{0, PageId(1), 0, 1};
	MetadataWriteBatch invalidVectorBatch(allocator, secondPublished, LogSequenceNumber(4));
	expectFormatError(
			[&] { directory.prepareReplaceVectorLocations(invalidVectorBatch, std::span(&invalidVector, 1), {}); },
			FormatError::FERR_INVALID_HEADER);
	const VectorManifestLocation invalidManifest{17, PageId{}, Generation(1)};
	MetadataWriteBatch invalidManifestBatch(allocator, secondPublished, LogSequenceNumber(4));
	expectFormatError(
			[&] { directory.prepareReplaceVectorManifests(invalidManifestBatch, std::span(&invalidManifest, 1), {}); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableLocationDirectoryTest, ReplacesLargeValueManifestsThroughAnIndependentCowRoot) {
	const Superblock initial = superblock();
	SuperblockFile bootstrap(dataIo);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);

	const LargeValueManifestLocation first{LargeValueId(71U), PageId(91U), Generation(5U)};
	MetadataWriteBatch firstBatch(allocator, initial, LogSequenceNumber(2U));
	directory.prepareReplaceLargeValueManifests(firstBatch, std::span(&first, 1U), {});
	firstBatch.publish(bootstrap);
	const Superblock firstPublished = firstBatch.candidate();
	EXPECT_TRUE(firstPublished.large_value_manifest_directory_root.isValid());
	EXPECT_EQ(directory.findLargeValueManifest(firstPublished, first.value_id), first);

	const LargeValueManifestLocation replacement{LargeValueId(71U), PageId(92U), Generation(7U)};
	MetadataWriteBatch secondBatch(allocator, firstPublished, LogSequenceNumber(3U));
	directory.prepareReplaceLargeValueManifests(secondBatch, std::span(&replacement, 1U), {});
	secondBatch.publish(bootstrap);
	const Superblock secondPublished = secondBatch.candidate();
	EXPECT_NE(firstPublished.large_value_manifest_directory_root, secondPublished.large_value_manifest_directory_root);
	EXPECT_EQ(directory.findLargeValueManifest(firstPublished, first.value_id), first);
	EXPECT_EQ(directory.findLargeValueManifest(secondPublished, replacement.value_id), replacement);
	EXPECT_EQ(directory.findLargeValueManifestOptional(secondPublished, replacement.value_id),
			  std::optional<LargeValueManifestLocation>(replacement));
	EXPECT_FALSE(directory.findLargeValueManifestOptional(secondPublished, LargeValueId(72U)).has_value());

	std::vector<LargeValueManifestLocation> visited;
	directory.visitLargeValueManifests(
			secondPublished, [&visited](const LargeValueManifestLocation &location) { visited.push_back(location); });
	EXPECT_EQ(visited, std::vector<LargeValueManifestLocation>{replacement});
	expectFormatError([&] { directory.visitLargeValueManifests(secondPublished, {}); },
					  FormatError::FERR_INVALID_HEADER);

	const std::array erased = {replacement.value_id};
	MetadataWriteBatch eraseBatch(allocator, secondPublished, LogSequenceNumber(4U));
	directory.prepareReplaceLargeValueManifests(eraseBatch, {}, erased);
	eraseBatch.publish(bootstrap);
	expectFormatError(
			[&] { static_cast<void>(directory.findLargeValueManifest(eraseBatch.candidate(), replacement.value_id)); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] { static_cast<void>(directory.findLargeValueManifestOptional(secondPublished, LargeValueId{})); },
			FormatError::FERR_INVALID_STATE);

	const LargeValueManifestLocation invalidId{LargeValueId{}, PageId(1U), Generation(1U)};
	MetadataWriteBatch invalidIdBatch(allocator, secondPublished, LogSequenceNumber(4U));
	expectFormatError(
			[&] { directory.prepareReplaceLargeValueManifests(invalidIdBatch, std::span(&invalidId, 1U), {}); },
			FormatError::FERR_INVALID_HEADER);
	const LargeValueManifestLocation invalidPage{LargeValueId(73U), PageId{}, Generation(1U)};
	MetadataWriteBatch invalidPageBatch(allocator, secondPublished, LogSequenceNumber(4U));
	expectFormatError(
			[&] { directory.prepareReplaceLargeValueManifests(invalidPageBatch, std::span(&invalidPage, 1U), {}); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableLocationDirectoryTest, SupportsEveryTypedDirectoryLookupAndTraversalWrapper) {
	const Superblock initial = superblock();
	SuperblockFile bootstrap(dataIo);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);

	const PageLocation pageLocation = page(1, 10, 1, 1, 512);
	const ExtentLocation extentLocation = extent(2, 11, 1, 2, 1);
	const EntityPageLocation nodeLocation{3, PageId(12), 0, 1};
	const EntityPageLocation edgeLocation{4, PageId(13), 1, 1};
	const IndexRootLocation indexRoot{5, PageId(14), Generation(1)};
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
	directory.prepareUpsertPages(batch, std::span(&pageLocation, 1));
	directory.prepareUpsertExtents(batch, std::span(&extentLocation, 1));
	directory.prepareReplaceNodePageLocations(batch, std::span(&nodeLocation, 1), {});
	directory.prepareReplaceEdgePageLocations(batch, std::span(&edgeLocation, 1), {});
	directory.prepareReplaceIndexRoots(batch, std::span(&indexRoot, 1), {});
	batch.publish(bootstrap);
	const Superblock published = batch.candidate();

	EXPECT_EQ(directory.findPageOptional(published, pageLocation.page_id), std::optional<PageLocation>(pageLocation));
	EXPECT_FALSE(directory.findPageOptional(published, PageId(99)).has_value());
	EXPECT_EQ(directory.findExtentOptional(published, extentLocation.extent_id),
			  std::optional<ExtentLocation>(extentLocation));
	EXPECT_FALSE(directory.findExtentOptional(published, ExtentId(99)).has_value());
	EXPECT_EQ(directory.findNodePageLocation(published, nodeLocation.entity_id), nodeLocation);
	EXPECT_EQ(directory.findNodePageLocationOptional(published, nodeLocation.entity_id),
			  std::optional<EntityPageLocation>(nodeLocation));
	EXPECT_FALSE(directory.findNodePageLocationOptional(published, 99).has_value());
	EXPECT_EQ(directory.findEdgePageLocation(published, edgeLocation.entity_id), edgeLocation);
	EXPECT_EQ(directory.findEdgePageLocationOptional(published, edgeLocation.entity_id),
			  std::optional<EntityPageLocation>(edgeLocation));
	EXPECT_FALSE(directory.findEdgePageLocationOptional(published, 99).has_value());
	EXPECT_EQ(directory.findIndexRoot(published, indexRoot.index_id), indexRoot);
	EXPECT_EQ(directory.findIndexRootOptional(published, indexRoot.index_id),
			  std::optional<IndexRootLocation>(indexRoot));
	EXPECT_FALSE(directory.findIndexRootOptional(published, 99).has_value());

	std::vector<PageLocation> visitedPages;
	std::vector<EntityPageLocation> visitedNodes;
	std::vector<EntityPageLocation> visitedEdges;
	directory.visitPages(published,
						 [&visitedPages](const PageLocation &location) { visitedPages.push_back(location); });
	directory.visitNodePageLocations(published,
															 [&visitedNodes](const EntityPageLocation &location) { visitedNodes.push_back(location); });
	directory.visitEdgePageLocations(published,
															 [&visitedEdges](const EntityPageLocation &location) { visitedEdges.push_back(location); });
	EXPECT_EQ(visitedPages, std::vector<PageLocation>{pageLocation});
	EXPECT_EQ(visitedNodes, std::vector<EntityPageLocation>{nodeLocation});
	EXPECT_EQ(visitedEdges, std::vector<EntityPageLocation>{edgeLocation});
	expectFormatError([&] { directory.visitPages(published, {}); }, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableLocationDirectoryTest, ResumesVectorManifestTraversalWithinAnExplicitMetadataPageBudget) {
	const Superblock initial = superblock();
	SuperblockFile bootstrap(dataIo);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);
	const std::array manifests = {VectorManifestLocation{9U, PageId(18U), Generation(1U)},
									 VectorManifestLocation{17U, PageId(19U), Generation(1U)}};
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2U));
	directory.prepareReplaceVectorManifests(batch, manifests, {});
	batch.publish(bootstrap);
	const Superblock published = batch.candidate();

	std::vector<VectorManifestLocation> resumed;
	size_t metadataPages = 0U;
	EXPECT_FALSE(directory.visitVectorManifestsAfter(
			published, manifests.front().index_id, 2U,
			[&resumed](const VectorManifestLocation &location) {
				resumed.push_back(location);
				return false;
			},
			&metadataPages));
	EXPECT_EQ(resumed, (std::vector<VectorManifestLocation>{manifests.back()}));
	EXPECT_EQ(metadataPages, 2U);

	resumed.clear();
	EXPECT_TRUE(directory.visitVectorManifestsAfter(
			published, manifests.back().index_id, 2U,
			[&resumed](const VectorManifestLocation &location) {
				resumed.push_back(location);
				return true;
			}));
	EXPECT_TRUE(resumed.empty());
	expectFormatError(
			[&] {
				static_cast<void>(directory.visitVectorManifestsAfter(
						published, 0U, 0U, [](const VectorManifestLocation &) { return true; }));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(directory.visitVectorManifestsAfter(published, 0U, 2U, {})); },
				  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableLocationDirectoryTest, TraversesEveryTypedDirectoryAndHonorsEarlyStopContracts) {
	const Superblock initial = superblock();
	SuperblockFile bootstrap(dataIo);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);

	const PageLocation pageLocation = page(1, 10, 1, 1, 512);
	const ExtentLocation extentLocation = extent(2, 11, 1, 2, 1);
	const EntityPageLocation nodeLocation{3, PageId(12), 0, 1};
	const EntityPageLocation edgeLocation{4, PageId(13), 1, 1};
	const StateLocation stateLocation{6, PageId(15), 3, 1};
	const IndexRootLocation indexRoot{7, PageId(16), Generation(1)};
	const VectorLocation vectorLocation{8, PageId(17), 4, 1};
	const VectorManifestLocation vectorManifest{9, PageId(18), Generation(1)};

	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
	directory.prepareReplacePages(batch, std::span(&pageLocation, 1), {});
	directory.prepareReplaceExtents(batch, std::span(&extentLocation, 1), {});
	directory.prepareReplaceNodePageLocations(batch, std::span(&nodeLocation, 1), {});
	directory.prepareReplaceEdgePageLocations(batch, std::span(&edgeLocation, 1), {});
	directory.prepareReplaceStates(batch, std::span(&stateLocation, 1), {});
	directory.prepareReplaceIndexRoots(batch, std::span(&indexRoot, 1), {});
	directory.prepareReplaceVectorLocations(batch, std::span(&vectorLocation, 1), {});
	directory.prepareReplaceVectorManifests(batch, std::span(&vectorManifest, 1), {});
	batch.publish(bootstrap);
	const Superblock published = batch.candidate();

	std::vector<ExtentLocation> extents;
	std::vector<StateLocation> states;
	std::vector<IndexRootLocation> indexRoots;
	std::vector<VectorLocation> vectorLocations;
	std::vector<VectorManifestLocation> vectorManifests;
	directory.visitExtents(published, [&extents](const ExtentLocation &location) { extents.push_back(location); });
	directory.visitStates(published, [&states](const StateLocation &location) { states.push_back(location); });
	directory.visitIndexRoots(published,
							  [&indexRoots](const IndexRootLocation &location) { indexRoots.push_back(location); });
	directory.visitVectorLocations(
			published, [&vectorLocations](const VectorLocation &location) { vectorLocations.push_back(location); });
	directory.visitVectorManifests(published, [&vectorManifests](const VectorManifestLocation &location) {
		vectorManifests.push_back(location);
	});
	EXPECT_EQ(extents, std::vector<ExtentLocation>{extentLocation});
	EXPECT_EQ(states, std::vector<StateLocation>{stateLocation});
	EXPECT_EQ(indexRoots, std::vector<IndexRootLocation>{indexRoot});
	EXPECT_EQ(vectorLocations, std::vector<VectorLocation>{vectorLocation});
	EXPECT_EQ(vectorManifests, std::vector<VectorManifestLocation>{vectorManifest});

	EXPECT_FALSE(directory.visitPagesUntil(published, [](const PageLocation &) { return false; }));
	EXPECT_TRUE(directory.visitPagesUntil(published, [](const PageLocation &) { return true; }));
	EXPECT_FALSE(directory.visitNodePageLocationsUntil(published, [](const EntityPageLocation &) { return false; }));
	EXPECT_TRUE(directory.visitNodePageLocationsUntil(published, [](const EntityPageLocation &) { return true; }));
	EXPECT_FALSE(directory.visitEdgePageLocationsUntil(published, [](const EntityPageLocation &) { return false; }));
	EXPECT_TRUE(directory.visitEdgePageLocationsUntil(published, [](const EntityPageLocation &) { return true; }));

	expectFormatError([&] { static_cast<void>(directory.visitPagesUntil(published, {})); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(directory.visitNodePageLocationsUntil(published, {})); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(directory.visitEdgePageLocationsUntil(published, {})); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { directory.visitExtents(published, {}); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { directory.visitIndexRoots(published, {}); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { directory.visitVectorLocations(published, {}); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { directory.visitVectorManifests(published, {}); }, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableLocationDirectoryTest, RejectsMalformedPublishedRootsAndRoutes) {
	const Superblock initial = superblock();
	SuperblockFile(dataIo).initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);
	LocationLeafPage validLeaf = leaf();
	validLeaf.generation = Generation(1);
	validLeaf.committed_lsn = LogSequenceNumber(1);

	auto writeRoot = [&](MetadataDirectoryType type, CatalogRadixPageKind kind, uint16_t level, PhysicalPageRef child,
						 uint32_t childIndex, LogSequenceNumber lsn = LogSequenceNumber(1)) {
		const PhysicalPageRef rootReference = allocator.allocatePage(Generation(1));
		CatalogRadixPage root;
		root.kind = kind;
		root.directory_type = type;
		root.level = level;
		root.page_id = allocator.allocatePageId();
		root.generation = Generation(1);
		root.committed_lsn = lsn;
		root.children[childIndex] = child;
		pages.writeMetadataRadix(rootReference, root);
		Superblock result = initial;
		result.page_directory_root = rootReference;
		return result;
	};

	const PhysicalPageRef validLeafReference = allocator.allocatePage(Generation(1));
	pages.writeLocationLeaf(validLeafReference, validLeaf);
	MetadataWriteBatch emptyBatch(allocator, initial, LogSequenceNumber(2));
	expectFormatError([&] { directory.prepareUpsertPages(emptyBatch, std::span<const PageLocation>{}); },
					  FormatError::FERR_INVALID_STATE);
	MetadataPagePoolAllocator foreignAllocator(dataIo, initial);
	MetadataWriteBatch foreignBatch(foreignAllocator, initial, LogSequenceNumber(2));
	const PageLocation foreignLocation = page(1, 10, 1);
	expectFormatError([&] { directory.prepareUpsertPages(foreignBatch, std::span(&foreignLocation, 1)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(directory.findPage(initial, PageId{})); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(
						directory.findPage(writeRoot(MetadataDirectoryType::MDT_EXTENT, CatalogRadixPageKind::CRPK_ROOT,
													 0, validLeafReference, 0),
										   PageId(1)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						directory.findPage(writeRoot(MetadataDirectoryType::MDT_PAGE,
													 CatalogRadixPageKind::CRPK_INTERNAL, 0, validLeafReference, 0),
										   PageId(1)));
			},
			FormatError::FERR_INVALID_HEADER);

	LocationLeafPage extentLeaf = leaf(MetadataDirectoryType::MDT_EXTENT);
	extentLeaf.generation = Generation(1);
	extentLeaf.committed_lsn = LogSequenceNumber(1);
	const PhysicalPageRef extentLeafReference = allocator.allocatePage(Generation(1));
	pages.writeLocationLeaf(extentLeafReference, extentLeaf);
	expectFormatError(
			[&] {
				static_cast<void>(
						directory.findPage(writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT,
													 0, extentLeafReference, 0),
										   PageId(1)));
			},
			FormatError::FERR_INVALID_HEADER);

	LocationLeafPage wrongRouteLeaf = validLeaf;
	wrongRouteLeaf.entries.front().logical_id = RSS_LOCATION_LEAF_MAX_ENTRIES + 1U;
	wrongRouteLeaf.entries.back().logical_id = 2U * RSS_LOCATION_LEAF_MAX_ENTRIES;
	const PhysicalPageRef wrongRouteReference = allocator.allocatePage(Generation(1));
	pages.writeLocationLeaf(wrongRouteReference, wrongRouteLeaf);
	expectFormatError(
			[&] {
				static_cast<void>(
						directory.findPage(writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT,
													 0, wrongRouteReference, 0),
										   PageId(1)));
			},
			FormatError::FERR_INVALID_HEADER);

	expectFormatError(
			[&] {
				static_cast<void>(
						directory.findPage(writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT,
													 0, validLeafReference, 0, LogSequenceNumber(2)),
										   PageId(1)));
			},
			FormatError::FERR_INVALID_STATE);

	const PhysicalPageRef malformedInternalReference = allocator.allocatePage(Generation(1));
	CatalogRadixPage malformedInternal;
	malformedInternal.kind = CatalogRadixPageKind::CRPK_ROOT;
	malformedInternal.directory_type = MetadataDirectoryType::MDT_PAGE;
	malformedInternal.level = 0;
	malformedInternal.page_id = allocator.allocatePageId();
	malformedInternal.generation = Generation(1);
	malformedInternal.committed_lsn = LogSequenceNumber(1);
	malformedInternal.children[0] = validLeafReference;
	pages.writeMetadataRadix(malformedInternalReference, malformedInternal);
	const uint64_t deepId = 1U + static_cast<uint64_t>(RSS_LOCATION_LEAF_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT;
	expectFormatError(
			[&] {
				static_cast<void>(
						directory.findPage(writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT,
													 1, malformedInternalReference, 1),
										   PageId(deepId)));
			},
			FormatError::FERR_INVALID_HEADER);

	auto expectPrepareError = [&](const Superblock &current, PageLocation update, FormatError expected) {
		MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2));
		expectFormatError([&] { directory.prepareUpsertPages(batch, std::span(&update, 1)); }, expected);
	};
	expectPrepareError(
			writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT, 0, extentLeafReference, 0),
			page(1, 10, 1), FormatError::FERR_INVALID_HEADER);

	const auto writeInternal = [&](CatalogRadixPageKind kind, MetadataDirectoryType type, uint16_t level,
								   Generation generation) {
		const PhysicalPageRef reference = allocator.allocatePage(generation);
		CatalogRadixPage internal;
		internal.kind = kind;
		internal.directory_type = type;
		internal.level = level;
		internal.page_id = allocator.allocatePageId();
		internal.generation = generation;
		internal.committed_lsn = LogSequenceNumber(1);
		internal.children[0] = validLeafReference;
		pages.writeMetadataRadix(reference, internal);
		return reference;
	};
	for (const PhysicalPageRef invalidInternal: {
				 writeInternal(CatalogRadixPageKind::CRPK_ROOT, MetadataDirectoryType::MDT_PAGE, 0, Generation(1)),
				 writeInternal(CatalogRadixPageKind::CRPK_INTERNAL, MetadataDirectoryType::MDT_EXTENT, 0,
							   Generation(1)),
				 writeInternal(CatalogRadixPageKind::CRPK_INTERNAL, MetadataDirectoryType::MDT_PAGE, 1, Generation(1)),
		 }) {
		expectPrepareError(
				writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT, 1, invalidInternal, 1),
				page(deepId, 10, 1), FormatError::FERR_INVALID_HEADER);
	}
	const PhysicalPageRef newerInternal =
			writeInternal(CatalogRadixPageKind::CRPK_INTERNAL, MetadataDirectoryType::MDT_PAGE, 0, Generation(2));
	expectPrepareError(writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT, 1, newerInternal, 1),
					   page(deepId, 10, 1), FormatError::FERR_INVALID_STATE);
	Superblock malformedForRecovery =
			writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT, 0, wrongRouteReference, 0);
	expectFormatError([&] { allocator.recover(malformedForRecovery); }, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableLocationDirectoryTest, RejectsInvalidMutationsAndHidesRootsBeforeSuperblockPublication) {
	auto store = RelocatableMetadataStore::initializeNew(dataIo, superblock());
	MetadataSnapshotPin empty = store->openSnapshot();
	expectFormatError([&] { static_cast<void>(store->findPageLocation(empty, PageId(1))); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findExtentLocation(empty, ExtentId(1))); },
					  FormatError::FERR_INVALID_STATE);
	empty.release();
	expectFormatError([&] { static_cast<void>(store->upsertLocations({}, {}, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						store->upsertLocations(std::vector{page(2, 10, 1), page(1, 11, 2)}, {}, LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->upsertLocations(std::vector{page(1, 10, 0)}, {}, LogSequenceNumber(2))); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->upsertLocations({}, std::vector{extent(1, 10, 31, 2)}, LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);

	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(
			static_cast<void>(store->upsertLocations(std::vector{page(1, 10, 1)}, {}, LogSequenceNumber(2), options)),
			BootstrapPublishInterrupted);
	auto recovered = RelocatableMetadataStore::open(dataIo);
	MetadataSnapshotPin snapshot = recovered->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(1));
	expectFormatError([&] { static_cast<void>(recovered->findPageLocation(snapshot, PageId(1))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableLocationDirectoryTest, RecoversRootsPublishedBeforeTheCallerObservesCompletion) {
	auto store = RelocatableMetadataStore::initializeNew(dataIo, superblock());
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(
			static_cast<void>(store->upsertLocations(std::vector{page(1, 10, 1)}, {}, LogSequenceNumber(2), options)),
			BootstrapPublishInterrupted);

	auto recovered = RelocatableMetadataStore::open(dataIo);
	MetadataSnapshotPin snapshot = recovered->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(2));
	EXPECT_EQ(recovered->findPageLocation(snapshot, PageId(1)), page(1, 10, 1));
}

TEST_F(RelocatableLocationDirectoryTest, PublishesAndReclaimsRetireQueueEntriesThroughItsOwnCowRoot) {
	const Superblock initial = superblock();
	SuperblockFile bootstrap(dataIo);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);

	const std::array first = {
			RetireQueueEntry{PhysicalSlotId(7), SegmentId(11), Generation(2),
							 RetireQueueDisposition::RQD_RELOCATED_SOURCE},
			RetireQueueEntry{PhysicalSlotId(9), SegmentId(12), Generation(2),
							 RetireQueueDisposition::RQD_RETIRED_SEGMENT},
	};
	MetadataWriteBatch firstBatch(allocator, initial, LogSequenceNumber(2));
	directory.prepareReplaceRetireQueue(firstBatch, first, {});
	firstBatch.publish(bootstrap);
	const Superblock firstPublished = firstBatch.candidate();
	EXPECT_TRUE(firstPublished.retire_queue_root.isValid());
	EXPECT_EQ(directory.findRetireQueueEntryOptional(firstPublished, PhysicalSlotId(7)),
			  std::optional<RetireQueueEntry>(first[0]));

	const RetireQueueEntry updated{PhysicalSlotId(7), SegmentId(11), Generation(3),
								   RetireQueueDisposition::RQD_RELOCATED_SOURCE};
	const std::array erased = {PhysicalSlotId(9)};
	MetadataWriteBatch secondBatch(allocator, firstPublished, LogSequenceNumber(3));
	directory.prepareReplaceRetireQueue(secondBatch, std::span(&updated, 1), erased);
	secondBatch.publish(bootstrap);
	const Superblock secondPublished = secondBatch.candidate();
	EXPECT_NE(firstPublished.retire_queue_root, secondPublished.retire_queue_root);
	EXPECT_EQ(directory.findRetireQueueEntryOptional(firstPublished, PhysicalSlotId(7)),
			  std::optional<RetireQueueEntry>(first[0]));
	EXPECT_EQ(directory.findRetireQueueEntryOptional(secondPublished, PhysicalSlotId(7)),
			  std::optional<RetireQueueEntry>(updated));
	EXPECT_FALSE(directory.findRetireQueueEntryOptional(secondPublished, PhysicalSlotId(9)).has_value());

	std::vector<RetireQueueEntry> visited;
	directory.visitRetireQueue(secondPublished,
							   [&visited](const RetireQueueEntry &entry) { visited.push_back(entry); });
	EXPECT_EQ(visited, std::vector<RetireQueueEntry>{updated});
	expectFormatError([&] { directory.visitRetireQueue(secondPublished, {}); }, FormatError::FERR_INVALID_HEADER);

	const std::array duplicate = {updated, updated};
	MetadataWriteBatch invalidBatch(allocator, secondPublished, LogSequenceNumber(4));
	expectFormatError([&] { directory.prepareReplaceRetireQueue(invalidBatch, duplicate, {}); },
					  FormatError::FERR_INVALID_HEADER);
}
