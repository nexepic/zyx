/**
 * @file test_PropertyCollectionRootStoreCache.cpp
 * @brief Buffer Pool admission tests for owner-keyed Property roots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <system_error>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Node.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	EntityPageNewPoolRequest nodePool() {
		return {.segment_id = SegmentId(31U),
				.pool_owner_id = 901U,
				.generation = Generation(1U),
				.page_id = PageId(101U),
				.first_slot = PhysicalSlotId(40U),
				.last_slot = PhysicalSlotId(40U)};
	}

	PropertyCollectionRoot root(uint64_t nodeId, uint32_t token, const char *value) {
		return {nodeId, graph::Node::typeId, {{token, graph::PropertyValue(value)}}};
	}

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

} // namespace

class PropertyCollectionRootStoreCacheTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<EntityPageStore> entities_;
	std::unique_ptr<SlottedPagePool> pages_;
	std::unique_ptr<PropertyCollectionRootStore> properties_;
	MetadataSnapshotPin snapshot_;
	PropertyCollectionRoot first_root_ = root(1001U, 3U, "first shared Property root");
	PropertyCollectionRoot second_root_ = root(1002U, 5U, "second shared Property root");
	EntityPropertyKey first_key_{EntityPropertyOwnerKind::EPOK_NODE, 1001U};
	EntityPropertyKey second_key_{EntityPropertyOwnerKind::EPOK_NODE, 1002U};
	PageId property_page_id_{};
	SegmentId property_segment_id_{};

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path_ = std::filesystem::temp_directory_path() / ("property_root_cache_" + to_string(uuid));
		file_ = std::make_shared<std::fstream>(path_,
											   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
		entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
		pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*pages_, *metadata_);

		const PropertyCollectionRootStoreOptions options{PhysicalSlotId(41U), PhysicalSlotId(41U), 902U};
		MetadataSnapshotPin empty = metadata_->openSnapshot();
		const std::array<PropertyCollectionRoot, 2U> roots{first_root_, second_root_};
		const std::vector<EntityPropertyRootMutationPlan> propertyPlans =
				properties_->planEntityRootReplacements(empty, roots, options);
		ASSERT_EQ(propertyPlans.size(), roots.size());
		const std::array<EntityPageBatchNewPool, 1U> entityPools{{
				{.request = nodePool(),
				 .record_kind = EntityPageRecordKind::EPRK_NODE,
				 .node_insertions = {{1001U, NodeEntityRecord{}}, {1002U, NodeEntityRecord{}}}},
		}};
		const EntityPageBatchResult published = entities_->applyNativeGraphBatchTransaction(
				empty, {}, {}, entityPools, propertyPlans, *properties_, options, LogSequenceNumber(2U));
		ASSERT_EQ(published.properties.size(), roots.size());
		ASSERT_EQ(published.properties[0].page_id, published.properties[1].page_id);
		property_page_id_ = published.properties.front().page_id;
		empty.release();
		io_->sync();
		io_->enableBufferPool(64U * RSS_PAGE_SIZE);
		snapshot_ = metadata_->openSnapshot();

		const EntityPropertyLocation first = metadata_->findEntityPropertyLocation(snapshot_, first_key_);
		const EntityPropertyLocation second = metadata_->findEntityPropertyLocation(snapshot_, second_key_);
		ASSERT_EQ(first.page_id, property_page_id_);
		ASSERT_EQ(second.page_id, property_page_id_);
		const PageLocation page = metadata_->findPageLocation(snapshot_, property_page_id_);
		property_segment_id_ = page.segment_id;
		const CatalogEntry pool = metadata_->findCatalog(snapshot_, property_segment_id_);
		ASSERT_TRUE(metadata_->isPhysicalSlotAllocated(snapshot_, pool.physical_slot));
		static_cast<void>(pages_->availableFrameCount(snapshot_, property_segment_id_));
	}

	void TearDown() override {
		snapshot_.release();
		properties_.reset();
		pages_.reset();
		entities_.reset();
		metadata_.reset();
		io_.reset();
		if (file_ && file_->is_open()) {
			file_->close();
		}
		file_.reset();
		std::error_code error;
		std::filesystem::remove(path_, error);
	}
};

TEST_F(PropertyCollectionRootStoreCacheTest, PointReadsAdmitOneSharedOwnedFrame) {
	io_->resetBufferPoolStats();
	const BufferPoolStats before = io_->bufferPoolStats();

	EXPECT_EQ(properties_->readEntityOptional(snapshot_, first_key_), std::optional(first_root_));
	const BufferPoolStats afterFirst = io_->bufferPoolStats();
	EXPECT_EQ(afterFirst.disk_read_bytes, before.disk_read_bytes + RSS_PAGE_SIZE);
	EXPECT_EQ(afterFirst.misses, before.misses + 1U);
	EXPECT_EQ(afterFirst.resident_bytes, before.resident_bytes + RSS_PAGE_SIZE);

	EXPECT_EQ(properties_->readEntityOptional(snapshot_, second_key_), std::optional(second_root_));
	const BufferPoolStats afterSecond = io_->bufferPoolStats();
	EXPECT_EQ(afterSecond.disk_read_bytes, afterFirst.disk_read_bytes);
	EXPECT_EQ(afterSecond.misses, afterFirst.misses);
	EXPECT_EQ(afterSecond.resident_bytes, afterFirst.resident_bytes);
	EXPECT_GT(afterSecond.hits, afterFirst.hits);

	EXPECT_EQ(properties_->readEntity(snapshot_, first_key_), first_root_);
	const BufferPoolStats afterRepeat = io_->bufferPoolStats();
	EXPECT_EQ(afterRepeat.disk_read_bytes, afterSecond.disk_read_bytes);
	EXPECT_EQ(afterRepeat.misses, afterSecond.misses);
	EXPECT_EQ(afterRepeat.resident_bytes, afterSecond.resident_bytes);
	EXPECT_GT(afterRepeat.hits, afterSecond.hits);

	const EntityPropertyKey missing{EntityPropertyOwnerKind::EPOK_NODE, 1003U};
	EXPECT_EQ(properties_->readEntityOptional(snapshot_, missing), std::nullopt);
	EXPECT_EQ(io_->bufferPoolStats().disk_read_bytes, afterRepeat.disk_read_bytes);
}

TEST_F(PropertyCollectionRootStoreCacheTest, ScanCursorRetainsOneDecodedPageWithoutResidentPollution) {
	io_->resetBufferPoolStats();
	const BufferPoolStats before = io_->bufferPoolStats();
	{
		PropertyCollectionRootScanCursor cursor = properties_->openScanCursor(snapshot_);
		const BufferPoolStats afterOpen = io_->bufferPoolStats();
		EXPECT_EQ(afterOpen.disk_read_bytes, before.disk_read_bytes);
		EXPECT_EQ(afterOpen.misses, before.misses);
		EXPECT_EQ(afterOpen.resident_bytes, before.resident_bytes);

		EXPECT_EQ(cursor.readEntity(snapshot_, first_key_), first_root_);
		const BufferPoolStats afterFirst = io_->bufferPoolStats();
		EXPECT_EQ(afterFirst.disk_read_bytes, before.disk_read_bytes + RSS_PAGE_SIZE);
		EXPECT_EQ(afterFirst.misses, before.misses + 1U);
		EXPECT_EQ(afterFirst.resident_bytes, before.resident_bytes);

		EXPECT_EQ(cursor.readEntityOptional(snapshot_, second_key_), std::optional(second_root_));
		const BufferPoolStats afterSecond = io_->bufferPoolStats();
		EXPECT_EQ(afterSecond.disk_read_bytes, afterFirst.disk_read_bytes);
		EXPECT_EQ(afterSecond.misses, afterFirst.misses);
		EXPECT_EQ(afterSecond.resident_bytes, before.resident_bytes);
	}

	PropertyCollectionRootScanCursor repeated = properties_->openScanCursor(snapshot_);
	EXPECT_EQ(repeated.readEntity(snapshot_, second_key_), second_root_);
	const BufferPoolStats afterRepeated = io_->bufferPoolStats();
	EXPECT_EQ(afterRepeated.disk_read_bytes, before.disk_read_bytes + 2U * RSS_PAGE_SIZE);
	EXPECT_EQ(afterRepeated.misses, before.misses + 2U);
	EXPECT_EQ(afterRepeated.resident_bytes, before.resident_bytes);
}

TEST_F(PropertyCollectionRootStoreCacheTest, CursorRejectsInvalidKeysAndForeignOrNewerSnapshotsBeforeDataIo) {
	PropertyCollectionRootScanCursor cursor = properties_->openScanCursor(snapshot_);
	io_->resetBufferPoolStats();
	expectFormatError(
			[&] { static_cast<void>(cursor.readEntity(snapshot_, {EntityPropertyOwnerKind::EPOK_NODE, 0U})); },
			FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(io_->bufferPoolStats().disk_read_bytes, 0U);

	MetadataSnapshotPin invalid;
	expectFormatError([&] { static_cast<void>(properties_->openScanCursor(invalid)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(cursor.readEntity(invalid, first_key_)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(io_->bufferPoolStats().disk_read_bytes, 0U);

	const PropertyCollectionRoot third = root(1003U, 7U, "generation fence");
	const PropertyCollectionRootStoreOptions options{PhysicalSlotId(41U), PhysicalSlotId(41U), 902U};
	const EntityPropertyRootMutationPlan thirdPlan = properties_->planEntityRootReplacement(snapshot_, third, options);
	const std::array propertyPlans{thirdPlan};
	const std::array<EntityPageBatchMutation, 1U> entityMutations{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1003U, NodeEntityRecord{}}}},
	}};
	static_cast<void>(entities_->applyNativeGraphBatchTransaction(snapshot_, entityMutations, {}, {}, propertyPlans,
																  *properties_, options, LogSequenceNumber(3U)));
	MetadataSnapshotPin newer = metadata_->openSnapshot();
	io_->resetBufferPoolStats();
	expectFormatError([&] { static_cast<void>(cursor.readEntity(newer, first_key_)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(io_->bufferPoolStats().disk_read_bytes, 0U);
	newer.release();
}
