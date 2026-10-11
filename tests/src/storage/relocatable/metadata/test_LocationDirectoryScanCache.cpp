/**
 * @file test_LocationDirectoryScanCache.cpp
 * @brief Buffer Pool admission tests for deep logical-directory scans.
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
#include <stdexcept>
#include <system_error>
#include <vector>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	LocationLeafPage singleEntryPage(MetadataDirectoryType type) {
		LocationLeafPage page;
		page.directory_type = type;
		page.page_id = MetadataPageId(1U);
		page.generation = Generation(1U);
		page.committed_lsn = LogSequenceNumber(1U);
		page.entries = {{1U, SegmentId(10U), Generation(1U), 0U, 1U, 0U}};
		return page;
	}

	class ThrowOnCancellationCheck final : public CancellationCheckpoint {
	public:
		void check() const override { throw std::runtime_error("cancelled before directory I/O"); }
	};

} // namespace

class LocationDirectoryScanCacheTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path_ = std::filesystem::temp_directory_path() / ("location_scan_cache_" + to_string(uuid));
		file_ = std::make_shared<std::fstream>(path_,
											 std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io_.reset();
		if (file_ && file_->is_open()) {
			file_->close();
		}
		file_.reset();
		std::error_code error;
		std::filesystem::remove(path_, error);
	}
};

TEST_F(LocationDirectoryScanCacheTest, KeepsRootResidentAndStreamsDeepDescendantsTransiently) {
	const Superblock initial = initialSuperblock();
	SuperblockFile bootstrap(io_);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io_, initial);
	MetadataPagePoolAllocator allocator(io_, initial);
	LocationDirectory directory(pages, allocator);
	const uint64_t deepId =
			1U + static_cast<uint64_t>(RSS_LOCATION_LEAF_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT;
	const std::array mappings = {
			EntityPageLocation{1U, PageId(101U), 0U, 1U},
			EntityPageLocation{1U + RSS_LOCATION_LEAF_MAX_ENTRIES, PageId(102U), 1U, 1U},
			EntityPageLocation{deepId, PageId(201U), 2U, 2U},
			EntityPageLocation{deepId + RSS_LOCATION_LEAF_MAX_ENTRIES, PageId(202U), 3U, 2U},
	};
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2U));
	directory.prepareReplaceNodePageLocations(batch, mappings, {});
	batch.publish(bootstrap);
	const Superblock published = batch.candidate();
	io_->sync();
	io_->enableBufferPool(16U * RSS_PAGE_SIZE);
	io_->resetBufferPoolStats();

	std::vector<uint64_t> firstIds;
	EXPECT_TRUE(directory.visitNodePageLocationsUntil(published, [&](const EntityPageLocation &location) {
		firstIds.push_back(location.entity_id);
		return true;
	}));
	EXPECT_EQ(firstIds, (std::vector<uint64_t>{mappings[0].entity_id, mappings[1].entity_id, mappings[2].entity_id,
											 mappings[3].entity_id}));
	const BufferPoolStats afterFirst = io_->bufferPoolStats();
	EXPECT_EQ(afterFirst.misses, 7U);
	EXPECT_EQ(afterFirst.disk_read_bytes, 7U * RSS_PAGE_SIZE);
	EXPECT_EQ(afterFirst.resident_bytes, RSS_PAGE_SIZE);

	std::vector<uint64_t> secondIds;
	EXPECT_TRUE(directory.visitNodePageLocationsUntil(published, [&](const EntityPageLocation &location) {
		secondIds.push_back(location.entity_id);
		return true;
	}));
	EXPECT_EQ(secondIds, firstIds);
	const BufferPoolStats afterSecond = io_->bufferPoolStats();
	EXPECT_EQ(afterSecond.misses, afterFirst.misses + 6U);
	EXPECT_EQ(afterSecond.disk_read_bytes, afterFirst.disk_read_bytes + 6U * RSS_PAGE_SIZE);
	EXPECT_EQ(afterSecond.resident_bytes, afterFirst.resident_bytes);
	EXPECT_GT(afterSecond.hits, afterFirst.hits);

	std::vector<uint64_t> resumedIds;
	size_t visitedMetadataPages = 0U;
	EXPECT_TRUE(directory.visitNodePageLocationsAfter(
			published, 0U, 7U,
			[&](const EntityPageLocation &location) {
				resumedIds.push_back(location.entity_id);
				return true;
			},
			&visitedMetadataPages));
	EXPECT_EQ(resumedIds, firstIds);
	EXPECT_EQ(visitedMetadataPages, 7U);
	const BufferPoolStats afterResumed = io_->bufferPoolStats();
	EXPECT_EQ(afterResumed.misses, afterSecond.misses + 6U);
	EXPECT_EQ(afterResumed.disk_read_bytes, afterSecond.disk_read_bytes + 6U * RSS_PAGE_SIZE);
	EXPECT_EQ(afterResumed.resident_bytes, afterSecond.resident_bytes);

	EXPECT_EQ(directory.findNodePageLocation(published, deepId), mappings[2]);
	const BufferPoolStats afterFirstPoint = io_->bufferPoolStats();
	EXPECT_EQ(afterFirstPoint.misses, afterResumed.misses + 2U);
	EXPECT_EQ(afterFirstPoint.disk_read_bytes, afterResumed.disk_read_bytes + 2U * RSS_PAGE_SIZE);
	EXPECT_EQ(afterFirstPoint.resident_bytes, afterResumed.resident_bytes + 2U * RSS_PAGE_SIZE);

	EXPECT_EQ(directory.findNodePageLocation(published, deepId), mappings[2]);
	const BufferPoolStats afterSecondPoint = io_->bufferPoolStats();
	EXPECT_EQ(afterSecondPoint.misses, afterFirstPoint.misses);
	EXPECT_EQ(afterSecondPoint.disk_read_bytes, afterFirstPoint.disk_read_bytes);
	EXPECT_GT(afterSecondPoint.hits, afterFirstPoint.hits);
}

TEST_F(LocationDirectoryScanCacheTest, RangeCursorRetainsRouteAndSupportsSparseMonotonicSeek) {
	const Superblock initial = initialSuperblock();
	SuperblockFile bootstrap(io_);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io_, initial);
	MetadataPagePoolAllocator allocator(io_, initial);
	LocationDirectory directory(pages, allocator);
	const uint64_t nextLeafId = 1U + RSS_LOCATION_LEAF_MAX_ENTRIES;
	const uint64_t deepId =
			1U + static_cast<uint64_t>(RSS_LOCATION_LEAF_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT;
	const std::array mappings = {
			EntityPageLocation{1U, PageId(101U), 0U, 1U},
			EntityPageLocation{nextLeafId, PageId(102U), 1U, 1U},
			EntityPageLocation{deepId, PageId(201U), 2U, 2U},
			EntityPageLocation{deepId + RSS_LOCATION_LEAF_MAX_ENTRIES, PageId(202U), 3U, 2U},
	};
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2U));
	directory.prepareReplaceNodePageLocations(batch, mappings, {});
	batch.publish(bootstrap);
	const Superblock published = batch.candidate();

	using graph::debug::PerfTrace;
	struct Cleanup { ~Cleanup() { PerfTrace::setEnabled(false); PerfTrace::reset(); } } cleanup;
	PerfTrace::setEnabled(false); PerfTrace::setValueCollectionEnabled(true); PerfTrace::reset();
	LocationDirectoryRangeCursor cursor = directory.openRangeCursor(
			published, MetadataDirectoryType::MDT_NODE_ENTITY, 1U, std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(cursor.metadataPagesRead(), 0U);
	ASSERT_EQ(cursor.next()->logical_id, mappings[0].entity_id);
	EXPECT_EQ(cursor.metadataPagesRead(), 3U); // Root, first internal, first leaf.
	ASSERT_EQ(cursor.next()->logical_id, mappings[1].entity_id);
	EXPECT_EQ(cursor.metadataPagesRead(), 4U); // The retained route reads only the next leaf.
	ASSERT_EQ(cursor.nextAtOrAfter(deepId)->logical_id, mappings[2].entity_id);
	EXPECT_EQ(cursor.metadataPagesRead(), 6U); // Second internal and its first leaf; no root reread.
	expectFormatError([&] { static_cast<void>(cursor.nextAtOrAfter(deepId)); },
					  FormatError::FERR_INVALID_STATE);

	LocationDirectoryRangeCursor moved = std::move(cursor);
	expectFormatError([&] { static_cast<void>(cursor.next()); }, FormatError::FERR_INVALID_STATE);
	ASSERT_EQ(moved.next()->logical_id, mappings[3].entity_id);
	EXPECT_EQ(moved.metadataPagesRead(), 7U);
	EXPECT_FALSE(moved.next().has_value());
	EXPECT_TRUE(moved.exhausted());
	const auto trace = PerfTrace::snapshotAndReset();
	EXPECT_EQ(trace.at("native_graph.read.directory_range_calls").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.root_descents").totalValue, 1);
	EXPECT_GE(trace.at("native_graph.read.metadata_pages_read").totalValue, 7);
}

TEST_F(LocationDirectoryScanCacheTest, RangeCursorChecksCancellationBeforeStorageIo) {
	const Superblock initial = initialSuperblock();
	SuperblockFile bootstrap(io_);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io_, initial);
	MetadataPagePoolAllocator allocator(io_, initial);
	LocationDirectory directory(pages, allocator);
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2U));
	const std::array mappings{EntityPageLocation{1U, PageId(101U), 0U, 1U}};
	directory.prepareReplaceNodePageLocations(batch, mappings, {});
	batch.publish(bootstrap);
	io_->sync();
	io_->enableBufferPool(8U * RSS_PAGE_SIZE);
	io_->resetBufferPoolStats();

	ThrowOnCancellationCheck cancellation;
	LocationDirectoryRangeCursor cursor = directory.openRangeCursor(
			batch.candidate(), MetadataDirectoryType::MDT_NODE_ENTITY, 1U,
			std::numeric_limits<uint64_t>::max(), &cancellation);
	EXPECT_THROW(static_cast<void>(cursor.next()), std::runtime_error);
	EXPECT_EQ(cursor.metadataPagesRead(), 0U);
	EXPECT_EQ(io_->bufferPoolStats().disk_read_bytes, 0U);
}

TEST_F(LocationDirectoryScanCacheTest, SortedPropertyMultiLookupStreamsOnlyRequestedRange) {
	const Superblock initial = initialSuperblock();
	SuperblockFile bootstrap(io_);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io_, initial);
	MetadataPagePoolAllocator allocator(io_, initial);
	LocationDirectory directory(pages, allocator);
	const uint64_t nextLeafId = 1U + RSS_LOCATION_LEAF_MAX_ENTRIES;
	const uint64_t deepId =
			1U + static_cast<uint64_t>(RSS_LOCATION_LEAF_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT;
	const std::array locations = {
			EntityPropertyLocation{{EntityPropertyOwnerKind::EPOK_NODE, 1U}, PageId(101U), 0U, 1U},
			EntityPropertyLocation{{EntityPropertyOwnerKind::EPOK_NODE, nextLeafId}, PageId(102U), 1U, 2U},
			EntityPropertyLocation{{EntityPropertyOwnerKind::EPOK_NODE, deepId}, PageId(201U), 2U, 3U},
	};
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2U));
	directory.prepareReplaceEntityProperties(batch, locations, {});
	batch.publish(bootstrap);
	const std::array keys = {
			EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1U},
			EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 2U},
			EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, nextLeafId},
			EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, deepId},
	};
	std::array<std::optional<EntityPropertyLocation>, keys.size()> results;
	directory.findEntityPropertiesSorted(batch.candidate(), keys, results);
	EXPECT_EQ(results[0], std::optional(locations[0]));
	EXPECT_EQ(results[1], std::nullopt);
	EXPECT_EQ(results[2], std::optional(locations[1]));
	EXPECT_EQ(results[3], std::optional(locations[2]));

	const std::array duplicateKeys{keys[0], keys[0]};
	std::array<std::optional<EntityPropertyLocation>, duplicateKeys.size()> duplicateResults;
	expectFormatError(
			[&] { directory.findEntityPropertiesSorted(batch.candidate(), duplicateKeys, duplicateResults); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { directory.findEntityPropertiesSorted(batch.candidate(), keys, duplicateResults); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(LocationDirectoryScanCacheTest, StreamsEntityRangeAbsenceProofDescendantsTransiently) {
	const Superblock initial = initialSuperblock();
	SuperblockFile bootstrap(io_);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io_, initial);
	MetadataPagePoolAllocator allocator(io_, initial);
	LocationDirectory directory(pages, allocator);
	const uint64_t deepId =
			1U + static_cast<uint64_t>(RSS_LOCATION_LEAF_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT;
	const std::array mappings = {EntityPageLocation{1U, PageId(101U), 0U, 1U},
								 EntityPageLocation{deepId, PageId(201U), 1U, 1U}};
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2U));
	directory.prepareReplaceNodePageLocations(batch, mappings, {});
	batch.publish(bootstrap);
	const Superblock published = batch.candidate();
	io_->sync();
	io_->enableBufferPool(8U * RSS_PAGE_SIZE);
	io_->resetBufferPoolStats();

	EXPECT_NO_THROW(directory.requireNodeRangeAbsent(published, 2U, RSS_LOCATION_LEAF_MAX_ENTRIES));
	const BufferPoolStats afterFirst = io_->bufferPoolStats();
	EXPECT_EQ(afterFirst.misses, 3U);
	EXPECT_EQ(afterFirst.disk_read_bytes, 3U * RSS_PAGE_SIZE);
	EXPECT_EQ(afterFirst.resident_bytes, RSS_PAGE_SIZE);

	EXPECT_NO_THROW(directory.requireNodeRangeAbsent(published, 2U, RSS_LOCATION_LEAF_MAX_ENTRIES));
	const BufferPoolStats afterSecond = io_->bufferPoolStats();
	EXPECT_EQ(afterSecond.misses, afterFirst.misses + 2U);
	EXPECT_EQ(afterSecond.disk_read_bytes, afterFirst.disk_read_bytes + 2U * RSS_PAGE_SIZE);
	EXPECT_EQ(afterSecond.resident_bytes, afterFirst.resident_bytes);
}

TEST_F(LocationDirectoryScanCacheTest, RejectsInvalidTypedEntryWidthsAndPublicTraversalArguments) {
	LocationLeafPage property = singleEntryPage(MetadataDirectoryType::MDT_ENTITY_PROPERTY);
	property.entries.front().page_count = 2U;
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(property)); },
					  FormatError::FERR_INVALID_HEADER);
	property.entries.front().page_count = 1U;
	property.entries.front().first_page = std::numeric_limits<uint16_t>::max();
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(property)); },
					  FormatError::FERR_INVALID_HEADER);
	property.entries.front().first_page = 0U;
	property.entries.front().generation = Generation(static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1U);
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(property)); },
					  FormatError::FERR_INVALID_HEADER);

	LocationLeafPage retirement = singleEntryPage(MetadataDirectoryType::MDT_RETIRE_QUEUE);
	retirement.entries.front().first_page =
			static_cast<uint16_t>(RetireQueueDisposition::RQD_RELOCATED_SOURCE);
	retirement.entries.front().page_count = 2U;
	expectFormatError([&] { static_cast<void>(LocationLeafPageCodec::encode(retirement)); },
					  FormatError::FERR_INVALID_HEADER);

	const Superblock initial = initialSuperblock();
	SuperblockFile bootstrap(io_);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io_, initial);
	MetadataPagePoolAllocator allocator(io_, initial);
	LocationDirectory directory(pages, allocator);
	expectFormatError([&] { static_cast<void>(directory.findRetireQueueEntryOptional(initial, PhysicalSlotId{})); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { directory.visitEntityProperties(initial, {}); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_NO_THROW(directory.requireNodeRangeAbsent(initial, 1U, 1U));
}
