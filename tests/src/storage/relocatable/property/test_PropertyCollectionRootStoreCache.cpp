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
#include <stdexcept>
#include <system_error>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Node.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeReadSession.hpp"
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

	class ThrowOnCancellationCheck final : public CancellationCheckpoint {
	public:
		void check() const override { throw std::runtime_error("cancelled before Property scan I/O"); }
	};

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

TEST_F(PropertyCollectionRootStoreCacheTest, ReadsSortedSelectedPropertiesThroughOneDirectoryMergeAndPageRead) {
	const EntityPropertyKey missing{EntityPropertyOwnerKind::EPOK_NODE, 1003U};
	const std::array<EntityPropertyKey, 3U> keys{first_key_, second_key_, missing};
	std::array<std::optional<PropertyCollectionRootEntry>, 3U> values;
	values[2] = PropertyCollectionRootEntry{3U, graph::PropertyValue("stale output")};
	io_->resetBufferPoolStats();
	const BufferPoolStats before = io_->bufferPoolStats();

	properties_->readEntityPropertiesOptionalSorted(snapshot_, keys, 3U, values);

	ASSERT_TRUE(values[0].has_value());
	EXPECT_EQ(values[0]->key_token, 3U);
	EXPECT_EQ(values[0]->value, graph::PropertyValue("first shared Property root"));
	EXPECT_FALSE(values[1].has_value());
	EXPECT_FALSE(values[2].has_value());
	const BufferPoolStats after = io_->bufferPoolStats();
	EXPECT_EQ(after.disk_read_bytes, before.disk_read_bytes + RSS_PAGE_SIZE);
	EXPECT_EQ(after.misses, before.misses + 1U);

	const std::array<EntityPropertyKey, 2U> unordered{second_key_, first_key_};
	std::array<std::optional<PropertyCollectionRootEntry>, 2U> unorderedValues;
	expectFormatError(
			[&] { properties_->readEntityPropertiesOptionalSorted(snapshot_, unordered, 3U, unorderedValues); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(PropertyCollectionRootStoreCacheTest, ReadsMultipleSelectedColumnsThroughOneDirectoryMergeAndPageRead) {
	const EntityPropertyKey missing{EntityPropertyOwnerKind::EPOK_NODE, 1003U};
	const std::array<EntityPropertyKey, 3U> keys{first_key_, second_key_, missing};
	const std::array<uint32_t, 2U> tokens{3U, 5U};
	std::array<std::optional<PropertyCollectionRootEntry>, 6U> values;
	io_->resetBufferPoolStats();
	const BufferPoolStats before = io_->bufferPoolStats();

	properties_->readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values);

	ASSERT_TRUE(values[0].has_value());
	EXPECT_EQ(values[0]->value, graph::PropertyValue("first shared Property root"));
	EXPECT_FALSE(values[1].has_value());
	EXPECT_FALSE(values[2].has_value());
	ASSERT_TRUE(values[3].has_value());
	EXPECT_EQ(values[3]->value, graph::PropertyValue("second shared Property root"));
	EXPECT_FALSE(values[4].has_value());
	EXPECT_FALSE(values[5].has_value());
	const BufferPoolStats after = io_->bufferPoolStats();
	EXPECT_EQ(after.disk_read_bytes, before.disk_read_bytes + RSS_PAGE_SIZE);
	EXPECT_EQ(after.misses, before.misses + 1U);

	const std::array<uint32_t, 2U> unorderedTokens{5U, 3U};
	expectFormatError(
			[&] { properties_->readEntityPropertiesOptionalSorted(snapshot_, keys, unorderedTokens, values); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(PropertyCollectionRootStoreCacheTest, ReadsPropertyCountsThroughOneDirectoryMergeAndPageRead) {
	const EntityPropertyKey missing{EntityPropertyOwnerKind::EPOK_NODE, 1003U};
	const std::array<EntityPropertyKey, 3U> keys{first_key_, second_key_, missing};
	std::array<std::optional<size_t>, 3U> counts;
	io_->resetBufferPoolStats();
	const BufferPoolStats before = io_->bufferPoolStats();

	properties_->readEntityPropertyCountsSorted(snapshot_, keys, counts);

	ASSERT_TRUE(counts[0].has_value());
	EXPECT_EQ(*counts[0], 1U);
	ASSERT_TRUE(counts[1].has_value());
	EXPECT_EQ(*counts[1], 1U);
	ASSERT_TRUE(counts[2].has_value());
	EXPECT_EQ(*counts[2], 0U);
	const BufferPoolStats after = io_->bufferPoolStats();
	EXPECT_EQ(after.disk_read_bytes, before.disk_read_bytes + RSS_PAGE_SIZE);
	EXPECT_EQ(after.misses, before.misses + 1U);

	const std::array<EntityPropertyKey, 2U> unordered{second_key_, first_key_};
	std::array<std::optional<size_t>, 2U> unorderedCounts;
	expectFormatError(
			[&] { properties_->readEntityPropertyCountsSorted(snapshot_, unordered, unorderedCounts); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(PropertyCollectionRootStoreCacheTest, ReadsSelectedValuesAndCountsThroughOneDirectoryMerge) {
	const EntityPropertyKey missing{EntityPropertyOwnerKind::EPOK_NODE, 1003U};
	const std::array<EntityPropertyKey, 3U> keys{first_key_, second_key_, missing};
	const std::array<uint32_t, 2U> tokens{3U, 5U};
	std::array<std::optional<PropertyCollectionRootEntry>, 6U> values;
	std::array<std::optional<size_t>, 3U> counts;
	io_->resetBufferPoolStats();
	const BufferPoolStats before = io_->bufferPoolStats();

	properties_->readEntityPropertiesAndCountsOptionalSorted(snapshot_, keys, tokens, values, counts);

	ASSERT_TRUE(values[0].has_value());
	EXPECT_EQ(values[0]->value, graph::PropertyValue("first shared Property root"));
	EXPECT_FALSE(values[1].has_value());
	EXPECT_FALSE(values[2].has_value());
	ASSERT_TRUE(values[3].has_value());
	EXPECT_EQ(values[3]->value, graph::PropertyValue("second shared Property root"));
	EXPECT_FALSE(values[4].has_value());
	EXPECT_FALSE(values[5].has_value());
	ASSERT_TRUE(counts[0].has_value());
	ASSERT_TRUE(counts[1].has_value());
	ASSERT_TRUE(counts[2].has_value());
	EXPECT_EQ(*counts[0], 1U);
	EXPECT_EQ(*counts[1], 1U);
	EXPECT_EQ(*counts[2], 0U);
	const BufferPoolStats after = io_->bufferPoolStats();
	EXPECT_EQ(after.disk_read_bytes, before.disk_read_bytes + RSS_PAGE_SIZE);
	EXPECT_EQ(after.misses, before.misses + 1U);
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

TEST_F(PropertyCollectionRootStoreCacheTest, NativeReadSessionReusesOneCursorForOrderedSelectedProperties) {
	io_->resetBufferPoolStats();
	const BufferPoolStats before = io_->bufferPoolStats();
	NativeReadSession session;
	PropertyCollectionRootScanCursor &cursor = session.propertyCursor(*properties_, snapshot_);
	EXPECT_EQ(cursor.readEntityPropertyOptional(snapshot_, first_key_, 3U),
			  std::optional<PropertyCollectionRootEntry>(first_root_.entries.front()));
	const BufferPoolStats afterFirst = io_->bufferPoolStats();
	EXPECT_EQ(afterFirst.disk_read_bytes, before.disk_read_bytes + RSS_PAGE_SIZE);
	EXPECT_EQ(afterFirst.misses, before.misses + 1U);
	EXPECT_EQ(afterFirst.resident_bytes, before.resident_bytes);

	EXPECT_EQ(session.propertyCursor(*properties_, snapshot_).readEntityPropertyOptional(snapshot_, second_key_, 5U),
			  std::optional<PropertyCollectionRootEntry>(second_root_.entries.front()));
	const BufferPoolStats afterSecond = io_->bufferPoolStats();
	EXPECT_EQ(afterSecond.disk_read_bytes, afterFirst.disk_read_bytes);
	EXPECT_EQ(afterSecond.misses, afterFirst.misses);
	EXPECT_EQ(afterSecond.resident_bytes, before.resident_bytes);
	EXPECT_FALSE(session.propertyCursor(*properties_, snapshot_)
					.readEntityPropertyOptional(snapshot_, second_key_, 99U)
					.has_value());
}

	TEST_F(PropertyCollectionRootStoreCacheTest, CursorBatchDecodesMultipleTokensWithoutReopeningSharedPage) {
	NativeReadSession session;
	PropertyCollectionRootScanCursor &cursor = session.propertyCursor(*properties_, snapshot_);
	const std::array<EntityPropertyKey, 2U> keys{first_key_, second_key_};
	const std::array<uint32_t, 2U> tokens{3U, 5U};
	std::array<std::optional<PropertyCollectionRootEntry>, 4U> values;
	io_->resetBufferPoolStats();
	const BufferPoolStats before = io_->bufferPoolStats();

	cursor.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values);

	ASSERT_TRUE(values[0].has_value());
	EXPECT_EQ(values[0]->value, graph::PropertyValue("first shared Property root"));
	EXPECT_FALSE(values[1].has_value());
	EXPECT_FALSE(values[2].has_value());
	ASSERT_TRUE(values[3].has_value());
	EXPECT_EQ(values[3]->value, graph::PropertyValue("second shared Property root"));
	const BufferPoolStats after = io_->bufferPoolStats();
	EXPECT_EQ(after.disk_read_bytes, before.disk_read_bytes + RSS_PAGE_SIZE);
	EXPECT_EQ(after.misses, before.misses + 1U);
}

	TEST_F(PropertyCollectionRootStoreCacheTest, CursorCountBatchReusesTheOrderedDirectoryAndPageRoute) {
		NativeReadSession session;
		PropertyCollectionRootScanCursor &cursor = session.propertyCursor(*properties_, snapshot_);
		const std::array<EntityPropertyKey, 2U> keys{first_key_, second_key_};
		std::array<std::optional<size_t>, 2U> counts;
		io_->resetBufferPoolStats();
		const BufferPoolStats before = io_->bufferPoolStats();

		cursor.readEntityPropertyCountsSorted(snapshot_, keys, counts);

		ASSERT_TRUE(counts[0].has_value());
		ASSERT_TRUE(counts[1].has_value());
		EXPECT_EQ(*counts[0], 1U);
		EXPECT_EQ(*counts[1], 1U);
		const BufferPoolStats afterFirst = io_->bufferPoolStats();
		EXPECT_EQ(afterFirst.disk_read_bytes, before.disk_read_bytes + RSS_PAGE_SIZE);
		EXPECT_EQ(afterFirst.misses, before.misses + 1U);

		std::array<std::optional<size_t>, 2U> repeated;
		cursor.readEntityPropertyCountsSorted(snapshot_, keys, repeated);
		EXPECT_EQ(repeated, counts);
		const BufferPoolStats afterRepeat = io_->bufferPoolStats();
		EXPECT_EQ(afterRepeat.disk_read_bytes, afterFirst.disk_read_bytes);
		EXPECT_EQ(afterRepeat.misses, afterFirst.misses);
		EXPECT_GT(afterRepeat.hits, afterFirst.hits);
	}

	TEST_F(PropertyCollectionRootStoreCacheTest, ScanCursorStreamsOrderedOwnersAndFallsBackForOutOfOrderReads) {
	PropertyCollectionRootScanCursor cursor = properties_->openScanCursor(snapshot_);
	const EntityPropertyKey beforeFirst{EntityPropertyOwnerKind::EPOK_NODE, 1000U};

	// A normal Entity scan encounters missing and present owners in increasing
	// logical-ID order. The cursor must preserve that exact optional-read
	// contract while advancing one bounded directory stream.
	EXPECT_EQ(cursor.readEntityOptional(snapshot_, beforeFirst), std::nullopt);
	EXPECT_EQ(cursor.readEntityOptional(snapshot_, first_key_), std::optional(first_root_));
	EXPECT_EQ(cursor.readEntityOptional(snapshot_, second_key_), std::optional(second_root_));

	// Direct callers are allowed to reuse a cursor in a different order. They do
	// not inherit an ambiguous partially consumed stream; correctness wins over
	// the scan optimization and the ordinary point lookup remains available.
	EXPECT_EQ(cursor.readEntityOptional(snapshot_, first_key_), std::optional(first_root_));
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

TEST_F(PropertyCollectionRootStoreCacheTest, CursorPropagatesCancellationBeforeDirectoryOrPropertyIo) {
	ThrowOnCancellationCheck cancellation;
	io_->resetBufferPoolStats();
	PropertyCollectionRootScanCursor cursor = properties_->openScanCursor(snapshot_, &cancellation);
	EXPECT_THROW(static_cast<void>(cursor.readEntityOptional(snapshot_, first_key_)), std::runtime_error);
	EXPECT_EQ(io_->bufferPoolStats().disk_read_bytes, 0U);
}
