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
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
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
        // Tests below measure a cold property data frame after warm control
        // metadata. Read-side mutable control images now include the snapshot
        // publication context; availableFrameCount warms the writer identity.
        std::array<std::byte, RSS_PAGE_SIZE> control{};
        const auto controlOffset = PhysicalSegmentAllocator::checkedSlotOffset(
            snapshot_.superblock().data_region_offset, pool.physical_slot);
        ASSERT_EQ(io_->readAt(BufferPoolPageKey{controlOffset, snapshot_.superblock().generation.value()},
            control.data(), control.size()), RSS_PAGE_SIZE);

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
		// The retained decoded directory and owned Property image avoid even
		// a Buffer Pool lookup when this unchanged snapshot is read again.
		EXPECT_EQ(afterRepeat.hits, afterFirst.hits);
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

TEST_F(PropertyCollectionRootStoreCacheTest, CursorAdmissionRejectsAndCanRetryWithoutStaleOutput) {
	const std::array<EntityPropertyKey, 3U> keys{first_key_, second_key_,
		{EntityPropertyOwnerKind::EPOK_NODE, 1003U}};
	const std::array<uint32_t, 2U> tokens{3U, 5U};
	std::array<std::optional<PropertyCollectionRootEntry>, 6U> values;
	auto cursor = properties_->openScanCursor(snapshot_);
	size_t calls = 0U;
	const SelectedPropertyReadAdmission reject = [&](size_t) {
		if (++calls == 2U) throw std::runtime_error("selected batch budget exhausted");
	};
	EXPECT_THROW(cursor.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values, true, &reject), std::runtime_error);
	EXPECT_EQ(calls, 2U);
	size_t accepted = 0U;
	const SelectedPropertyReadAdmission accept = [&](size_t bytes) { accepted += bytes; };
	cursor.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values, true, &accept);
	ASSERT_TRUE(values[0].has_value());
	ASSERT_TRUE(values[3].has_value());
	EXPECT_EQ(*values[0], first_root_.entries.front());
	EXPECT_EQ(*values[3], second_root_.entries.front());
	for (size_t index: {1U, 2U, 4U, 5U}) EXPECT_FALSE(values[index].has_value());
	EXPECT_GT(accepted, 0U);
}

TEST_F(PropertyCollectionRootStoreCacheTest, FullPointAdmissionRejectsAndRetriesThroughStoreAndCursor) {
	auto cursor = properties_->openScanCursor(snapshot_);
	for (bool useCursor : {false, true}) {
		SCOPED_TRACE(useCursor);
		size_t calls = 0U;
		size_t requested = 0U;
		const SelectedPropertyReadAdmission reject = [&](size_t bytes) {
			++calls;
			requested = bytes;
			throw std::length_error("full Property decode allowance exhausted");
		};
		const auto read = [&](const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slots = nullptr) {
			return useCursor ? cursor.readEntity(snapshot_, first_key_, admission, slots)
						 : properties_->readEntity(snapshot_, first_key_, admission, slots);
		};
		EXPECT_THROW(static_cast<void>(read(&reject)), std::length_error);
		EXPECT_EQ(calls, 1U);
		EXPECT_GT(requested, std::string("first shared Property root").size());
		const SelectedPropertyReadAdmission accept = [&](size_t bytes) {
			++calls;
			EXPECT_EQ(bytes, requested);
		};
		EXPECT_EQ(read(&accept), first_root_);
		EXPECT_EQ(calls, 2U);
		const EntityPropertyKey missing{EntityPropertyOwnerKind::EPOK_NODE, 1003U};
		const auto optional = [&](EntityPropertyKey key, const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slots = nullptr) {
			return useCursor ? cursor.readEntityOptional(snapshot_, key, admission, slots)
						 : properties_->readEntityOptional(snapshot_, key, admission, slots);
		};
		EXPECT_FALSE(optional(missing, &reject));
		EXPECT_EQ(calls, 2U);
		EXPECT_THROW(static_cast<void>(optional(first_key_, &reject)), std::length_error);
		EXPECT_EQ(optional(first_key_, &accept), std::optional(first_root_));
		EXPECT_EQ(calls, 4U);
		size_t slotCalls = 0U, retainedSlots = 0U;
		const SelectedPropertyReadAdmission rejectSlots = [&](size_t bytes) {
			++slotCalls; retainedSlots = bytes; throw std::length_error("root slots rejected");
		};
		EXPECT_FALSE(optional(missing, nullptr, &rejectSlots));
		EXPECT_EQ(slotCalls, 0U);
		EXPECT_THROW(static_cast<void>(optional(first_key_, nullptr, &rejectSlots)), std::length_error);
		EXPECT_EQ(slotCalls, 1U);
		EXPECT_EQ(retainedSlots, useCursor
				? RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + EntityPropertyCollectionRootCodec::encode(first_root_).size()
				: first_root_.entries.size() * sizeof(PropertyCollectionRootEntry));
		EXPECT_THROW(static_cast<void>(read(nullptr, &rejectSlots)), std::length_error);
		EXPECT_EQ(slotCalls, 2U);
		const SelectedPropertyReadAdmission acceptSlots = [&](size_t bytes) { retainedSlots = bytes; };
		for (int retry = 0; retry < 2; ++retry) {
			const auto required = read(nullptr, &acceptSlots);
			EXPECT_EQ(required, first_root_);
			EXPECT_EQ(retainedSlots, required.entries.capacity() * sizeof(PropertyCollectionRootEntry));
			const auto output = optional(first_key_, nullptr, &acceptSlots);
			ASSERT_TRUE(output);
			EXPECT_EQ(*output, first_root_);
			EXPECT_EQ(retainedSlots, output->entries.capacity() * sizeof(PropertyCollectionRootEntry));
		}
	}
}

TEST_F(PropertyCollectionRootStoreCacheTest, CancellationDuringDecodePreventsPublicationAndAllowsSameCursorRetry) {
	class Switch final : public CancellationCheckpoint {
	public:
		bool cancelled = false;
		void check() const override { if (cancelled) throw std::runtime_error("cancelled during decode"); }
	} cancellation;
	const std::array<EntityPropertyKey, 1U> keys{first_key_};
	const std::array<uint32_t, 1U> tokens{3U};
	const SelectedPropertyReadAdmission cancel = [&](size_t) { cancellation.cancelled = true; };
	for (const int mode: {0, 1, 2}) {
		const bool combined = mode != 0;
		cancellation.cancelled = false;
		auto cursor = properties_->openScanCursor(snapshot_, &cancellation);
		std::array<std::optional<PropertyCollectionRootEntry>, 1U> values{second_root_.entries.front()};
		std::array<std::optional<size_t>, 1U> counts{999U};
		const auto read = [&](const SelectedPropertyReadAdmission *admission) {
			if (mode == 2) properties_->readEntityPropertiesAndCountsOptionalSorted(snapshot_, keys, tokens, values, counts, &cancellation, admission);
			else if (combined) cursor.readEntityPropertiesAndCountsOptionalSorted(snapshot_, keys, tokens, values, counts, admission);
			else cursor.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values, true, admission);
		};
		EXPECT_THROW(read(&cancel), std::runtime_error);
		EXPECT_FALSE(values[0].has_value());
		if (combined) EXPECT_FALSE(counts[0].has_value());
		cancellation.cancelled = false;
		read(nullptr);
		ASSERT_TRUE(values[0].has_value());
		EXPECT_EQ(*values[0], first_root_.entries.front());
		if (combined) EXPECT_EQ(counts[0], first_root_.entries.size());
	}
	cancellation.cancelled = false;
	auto cursor = properties_->openScanCursor(snapshot_, &cancellation);
	EXPECT_THROW(static_cast<void>(cursor.readEntity(snapshot_, first_key_, &cancel)), std::runtime_error);
	cancellation.cancelled = false;
	EXPECT_EQ(cursor.readEntity(snapshot_, first_key_).entries, first_root_.entries);
}

TEST_F(PropertyCollectionRootStoreCacheTest, CountOnlyOwnerArraysAdmitBeforeLookupAndRetryWithSameCursor) {
	auto cursor = properties_->openScanCursor(snapshot_);
	const std::array keys{first_key_};
	std::array<std::optional<size_t>, 1U> counts{999U};
	size_t requested = 0U;
	const SelectedPropertyReadAdmission rejectLocations = [&](size_t bytes) {
		requested = bytes; throw std::length_error("count owner locations rejected");
	};
	EXPECT_THROW(cursor.readEntityPropertyCountsSorted(snapshot_, keys, counts, &rejectLocations), std::length_error);
	EXPECT_FALSE(counts[0]);
	EXPECT_EQ(requested, sizeof(std::optional<EntityPropertyLocation>));
	const SelectedPropertyReadAdmission rejectOrder = [&](size_t bytes) {
		if (bytes > sizeof(std::optional<EntityPropertyLocation>)) throw std::length_error("count owner order rejected");
	};
	EXPECT_THROW(cursor.readEntityPropertyCountsSorted(snapshot_, keys, counts, &rejectOrder), std::length_error);
	EXPECT_FALSE(counts[0]);
	for (int retry = 0; retry < 2; ++retry) {
		size_t retained = 0U, peak = 0U;
		const SelectedPropertyReadAdmission accept = [&](size_t bytes) { retained = bytes; peak = std::max(peak, bytes); };
		EXPECT_NO_THROW(cursor.readEntityPropertyCountsSorted(snapshot_, keys, counts, &accept));
		EXPECT_EQ(counts[0], first_root_.entries.size());
		EXPECT_GE(peak, sizeof(std::optional<EntityPropertyLocation>) + sizeof(size_t));
		EXPECT_EQ(retained, 0U);
	}
}

TEST_F(PropertyCollectionRootStoreCacheTest, CombinedCursorReusesAnAlreadyAuthenticatedPropertyPage) {
	NativeReadSession session;
	auto &cursor = session.propertyCursor(*properties_, snapshot_);
	EXPECT_TRUE(cursor.readEntityPropertyOptional(snapshot_, first_key_, 3U));
	const auto before = io_->bufferPoolStats();
	const bool enabled = graph::debug::PerfTrace::isEnabled();
	const bool valuesEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	const std::array keys{first_key_, second_key_};
	const std::array<uint32_t, 2U> tokens{3U, 5U};
	std::array<std::optional<PropertyCollectionRootEntry>, 4U> values;
	std::array<std::optional<size_t>, 2U> counts;
	for (int retry = 0; retry < 2; ++retry) {
		cursor.readEntityPropertiesAndCountsOptionalSorted(snapshot_, keys, tokens, values, counts);
		EXPECT_EQ(values[0], first_root_.entries.front());
		EXPECT_EQ(values[3], second_root_.entries.front());
		EXPECT_FALSE(values[1]);
		EXPECT_FALSE(values[2]);
		EXPECT_EQ(counts[0], first_root_.entries.size());
		EXPECT_EQ(counts[1], second_root_.entries.size());
		const auto after = io_->bufferPoolStats();
		EXPECT_EQ(after.disk_read_bytes, before.disk_read_bytes);
		EXPECT_EQ(after.misses, before.misses);

	}
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesEnabled);
	graph::debug::PerfTrace::setEnabled(enabled);
	EXPECT_FALSE(trace.contains("native_graph.read.property_combined_page_acquisition"));
}

TEST_F(PropertyCollectionRootStoreCacheTest, CombinedPinnedReadRetainsOwnedImageForSubsequentCalls) {
	auto cursor = properties_->openScanCursor(snapshot_);
	const std::array keys{first_key_, second_key_};
	const std::array<uint32_t, 2U> tokens{3U, 5U};
	std::array<std::optional<PropertyCollectionRootEntry>, 4U> values;
	std::array<std::optional<size_t>, 2U> counts;
	const SelectedPropertyReadAdmission reject = [](size_t bytes) {
		if (bytes >= RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) throw std::length_error("retained image budget");
	};
	// Borrowing the pool image must not require a full-page cache copy.
	for (int retry = 0; retry < 2; ++retry) {
		EXPECT_NO_THROW(cursor.readEntityPropertiesAndCountsOptionalSorted(snapshot_, keys, tokens, values, counts,
				nullptr, &reject));
		EXPECT_EQ(values[0], first_root_.entries.front());
		EXPECT_EQ(values[3], second_root_.entries.front());
		EXPECT_EQ(counts[0], first_root_.entries.size());
		EXPECT_EQ(counts[1], second_root_.entries.size());
	}
	cursor.readEntityPropertiesAndCountsOptionalSorted(snapshot_, keys, tokens, values, counts);
	const auto before = io_->bufferPoolStats();
	const bool enabled = graph::debug::PerfTrace::isEnabled();
	const bool valuesEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	for (int retry = 0; retry < 2; ++retry) {
		cursor.readEntityPropertiesAndCountsOptionalSorted(snapshot_, keys, tokens, values, counts);
		EXPECT_EQ(values[0], first_root_.entries.front());
		EXPECT_EQ(values[3], second_root_.entries.front());
		EXPECT_EQ(counts[0], first_root_.entries.size());
		EXPECT_EQ(counts[1], second_root_.entries.size());
		EXPECT_EQ(io_->bufferPoolStats().disk_read_bytes, before.disk_read_bytes);
	}
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesEnabled);
	graph::debug::PerfTrace::setEnabled(enabled);
	EXPECT_FALSE(trace.contains("native_graph.read.property_combined_page_acquisition"));
}

TEST_F(PropertyCollectionRootStoreCacheTest, SelectedColumnBatchBorrowsSlotsAndRetainsOnlyLastPageForRetry) {
 auto cursor = properties_->openScanCursor(snapshot_);
 const std::array keys{first_key_, second_key_};
 const std::array<uint32_t, 2> tokens{3U, 5U};
 std::array<std::optional<PropertyCollectionRootEntry>, 4> values;
 const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
 graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
 cursor.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values, true);
 const auto trace = graph::debug::PerfTrace::snapshotAndReset();
 graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
 EXPECT_EQ(values[0], first_root_.entries[0]); EXPECT_EQ(values[3], second_root_.entries[0]);
 EXPECT_FALSE(values[1]); EXPECT_FALSE(values[2]);
 ASSERT_TRUE(trace.contains("native_graph.read.property_selected_borrowed_owners"));
 EXPECT_EQ(trace.at("native_graph.read.property_selected_borrowed_owners").totalValue, 2);
 EXPECT_FALSE(trace.contains("native_graph.read.property_selected_slot_copy_bytes"));
 EXPECT_FALSE(trace.contains("native_graph.read.shared_page_known_location_copy"));
 EXPECT_FALSE(trace.contains("native_graph.read.property_selected_last_page_cache_copy_bytes"));
 EXPECT_EQ(io_->bufferPoolStats().pinned_bytes, RSS_PAGE_SIZE);
 const auto before = io_->bufferPoolStats();
 graph::debug::PerfTrace::setValueCollectionEnabled(true);
 graph::debug::PerfTrace::reset();
 cursor.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values, true);
 const auto repeated = graph::debug::PerfTrace::snapshotAndReset();
 graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
 EXPECT_FALSE(repeated.contains("native_graph.read.property_selected_page_acquisition"));
 ASSERT_TRUE(repeated.contains("native_graph.read.property_selected_page_reuse"));
 EXPECT_EQ(repeated.at("native_graph.read.property_selected_page_reuse").totalValue, 1);
 EXPECT_EQ(io_->bufferPoolStats().disk_read_bytes, before.disk_read_bytes);
 EXPECT_EQ(values[0], first_root_.entries[0]); EXPECT_EQ(values[3], second_root_.entries[0]);
}

TEST_F(PropertyCollectionRootStoreCacheTest, SelectedColumnBorrowingFallsBackWithoutAPoolAndRetriesAdmission) {
 io_->disableBufferPool();
 auto cursor = properties_->openScanCursor(snapshot_);
 const std::array keys{first_key_, second_key_};
 const std::array<uint32_t, 2> tokens{3U, 5U};
 std::array<std::optional<PropertyCollectionRootEntry>, 4> values;
 const SelectedPropertyReadAdmission reject = [](size_t) { throw std::length_error("selected output budget"); };
 EXPECT_THROW(cursor.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values, true, &reject), std::length_error);
 EXPECT_NO_THROW(cursor.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values, true));
 EXPECT_EQ(values[0], first_root_.entries[0]); EXPECT_EQ(values[3], second_root_.entries[0]);
 EXPECT_FALSE(values[1]); EXPECT_FALSE(values[2]);
}

TEST_F(PropertyCollectionRootStoreCacheTest, PointScanAndGatherRecordAuthenticatedWorkWithoutPayloadChanges) {
    struct TraceReset {
        bool enabled = graph::debug::PerfTrace::areValuesEnabled();
        ~TraceReset() { graph::debug::PerfTrace::setValueCollectionEnabled(enabled); graph::debug::PerfTrace::reset(); }
    } reset;
    graph::debug::PerfTrace::setValueCollectionEnabled(true);
    const std::array<EntityPropertyKey, 2U> keys{first_key_, second_key_};
    const std::array<uint32_t, 2U> tokens{3U, 5U};
    const auto record = [&](const std::string &path) {
        const auto trace = graph::debug::PerfTrace::snapshotAndReset();
        std::map<std::string, int64_t> sorted;
        for (const auto &[key, entry] : trace) if (entry.valueCalls) sorted.emplace(key, entry.totalValue);
        std::string json = "{";
        for (const auto &[key, value] : sorted) {
            if (json.size() > 1U) json += ",";
            json += "\"" + key + "\":" + std::to_string(value);
        }
        RecordProperty(path, json + "}");
        return trace;
    };
    graph::debug::PerfTrace::reset();
    for (size_t i = 0; i < 2U; ++i) {
        const auto value = properties_->readEntityPropertyOptional(snapshot_, first_key_, 3U);
        ASSERT_TRUE(value); EXPECT_EQ(value->value, first_root_.entries[0].value);
    }
    record("point_twice");
    NativeReadSession session;
    auto &cursor = session.propertyCursor(*properties_, snapshot_);
    for (size_t i = 0; i < 2U; ++i) {
        std::array<std::optional<PropertyCollectionRootEntry>, 4U> values;
        cursor.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values);
        ASSERT_TRUE(values[0]); ASSERT_TRUE(values[3]);
        EXPECT_EQ(values[0]->value, first_root_.entries[0].value);
        EXPECT_EQ(values[3]->value, second_root_.entries[0].value);
        EXPECT_FALSE(values[1]); EXPECT_FALSE(values[2]);
    }
    const auto scan = record("scan_twice");
    ASSERT_TRUE(scan.contains("native_graph.read.decoded_metadata_borrow_fixed_bytes"));
    EXPECT_GT(scan.at("native_graph.read.decoded_metadata_borrow_fixed_bytes").totalValue, 0);
    for (size_t i = 0; i < 2U; ++i) {
        std::array<std::optional<EntityPropertyLocation>, 2U> values;
        metadata_->findEntityPropertyLocationsSorted(snapshot_, keys, values);
        ASSERT_TRUE(values[0]); ASSERT_TRUE(values[1]);
        EXPECT_EQ(values[0]->key, first_key_); EXPECT_EQ(values[1]->key, second_key_);
    }
    const auto gather = record("gather_locations_twice");
    ASSERT_TRUE(gather.contains("native_graph.read.decoded_metadata_borrow_fixed_bytes"));
    EXPECT_GT(gather.at("native_graph.read.decoded_metadata_borrow_fixed_bytes").totalValue, 0);
}

TEST_F(PropertyCollectionRootStoreCacheTest, SelectedPageRetentionReleasesOnFailureAndOtherReadModes) {
    auto cursor = properties_->openScanCursor(snapshot_);
    const std::array keys{first_key_, second_key_};
    const std::array<uint32_t, 2> tokens{3U, 5U};
    std::array<std::optional<PropertyCollectionRootEntry>, 4> values;
    const auto read = [&](const SelectedPropertyReadAdmission *admission = nullptr) {
        cursor.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values, true, admission);
    };
    read(); EXPECT_EQ(io_->bufferPoolStats().pinned_bytes, RSS_PAGE_SIZE);
    const SelectedPropertyReadAdmission reject = [](size_t) { throw std::length_error("output rejected"); };
    EXPECT_THROW(read(&reject), std::length_error);
    EXPECT_EQ(io_->bufferPoolStats().pinned_bytes, 0U);
    read(); EXPECT_EQ(values[0], first_root_.entries[0]);
    EXPECT_EQ(io_->bufferPoolStats().pinned_bytes, RSS_PAGE_SIZE);
    EXPECT_EQ(cursor.readEntity(snapshot_, first_key_), first_root_);
    EXPECT_EQ(io_->bufferPoolStats().pinned_bytes, 0U);
    read(); // The full-read mode already retained an owned image of this page.
    EXPECT_EQ(values[0], first_root_.entries[0]);
    EXPECT_EQ(io_->bufferPoolStats().pinned_bytes, 0U);
    cursor = properties_->openScanCursor(snapshot_); // Exercise moving a pin separately.
    read();
    auto moved = std::move(cursor);
    EXPECT_EQ(io_->bufferPoolStats().pinned_bytes, RSS_PAGE_SIZE);
    moved.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values);
    EXPECT_EQ(values[3], second_root_.entries[0]);
    EXPECT_EQ(io_->bufferPoolStats().pinned_bytes, RSS_PAGE_SIZE);
    const std::array<uint32_t, 1> invalidTokens{0U};
    EXPECT_THROW(moved.readEntityPropertiesOptionalSorted(snapshot_, keys, invalidTokens, {}), FormatException);
    EXPECT_EQ(io_->bufferPoolStats().pinned_bytes, 0U);
}

TEST_F(PropertyCollectionRootStoreCacheTest, SelectedPageRetentionInvalidatesAfterWriteAttempt) {
    auto cursor = properties_->openScanCursor(snapshot_);
    const std::array keys{first_key_}; const std::array<uint32_t, 1> tokens{3U};
    std::array<std::optional<PropertyCollectionRootEntry>, 1> values;
    cursor.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values);
    const auto epoch = metadata_->dataMutationEpoch();
    // Even an aborted/empty write attempt invalidates decoded source images.
    io_->writeAt(0U, nullptr, 0U);
    ASSERT_GT(metadata_->dataMutationEpoch(), epoch);
    const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
    graph::debug::PerfTrace::setValueCollectionEnabled(true); graph::debug::PerfTrace::reset();
    cursor.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values);
    const auto trace = graph::debug::PerfTrace::snapshotAndReset();
    graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
    EXPECT_FALSE(trace.contains("native_graph.read.property_selected_page_reuse"));
    ASSERT_TRUE(trace.contains("native_graph.read.property_selected_page_acquisition"));
    EXPECT_EQ(trace.at("native_graph.read.property_selected_page_acquisition").totalValue, 1);
    EXPECT_EQ(values[0], first_root_.entries[0]);
}

TEST_F(PropertyCollectionRootStoreCacheTest, SelectedPinSurvivesPoolDisableAndMoveAssignmentWithoutDanglingView) {
    const std::array keys{first_key_}; const std::array<uint32_t, 1> tokens{3U};
    std::array<std::optional<PropertyCollectionRootEntry>, 1> values;
    auto first = properties_->openScanCursor(snapshot_);
    first.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values);
    io_->disableBufferPool();
    io_->enableBufferPool(64U * RSS_PAGE_SIZE);
    auto second = properties_->openScanCursor(snapshot_);
    second.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values);
    // Assignment releases the old pool pin before its last owning reference.
    second = std::move(first);
    second.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, values);
    EXPECT_EQ(values[0], first_root_.entries[0]);
    io_->disableBufferPool();
    EXPECT_EQ(second.readEntity(snapshot_, first_key_), first_root_);
}

TEST_F(PropertyCollectionRootStoreCacheTest, SelectedBatchReacquiresAfterMutationWithoutReplayingDeliveredOwners) {
    auto cursor = properties_->openScanCursor(snapshot_);
    const std::array keys{first_key_, second_key_}; const std::array<uint32_t, 2> tokens{3U, 5U};
    std::array<std::optional<PropertyCollectionRootEntry>, 4> actual;
    std::array<size_t, 4> delivered{};
    const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> sink =
        [&](size_t row, size_t column, std::optional<PropertyCollectionRootEntry> value) {
            const size_t index = row * tokens.size() + column;
            ++delivered.at(index); actual.at(index) = std::move(value);
            if (index == 0U) io_->writeAt(0U, nullptr, 0U);
        };
    EXPECT_NO_THROW(cursor.readEntityPropertiesOptionalSorted(snapshot_, keys, tokens, {}, true, nullptr, &sink));
    EXPECT_EQ(actual[0], first_root_.entries[0]); EXPECT_EQ(actual[3], second_root_.entries[0]);
    EXPECT_FALSE(actual[1]); EXPECT_FALSE(actual[2]);
    for (const auto count : delivered) EXPECT_EQ(count, 1U);
    EXPECT_EQ(io_->bufferPoolStats().pinned_bytes, RSS_PAGE_SIZE);
}
