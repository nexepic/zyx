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
#include <system_error>
#include <vector>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
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
		page.page_id = PageId(1U);
		page.generation = Generation(1U);
		page.committed_lsn = LogSequenceNumber(1U);
		page.entries = {{1U, SegmentId(10U), Generation(1U), 0U, 1U, 0U}};
		return page;
	}

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
