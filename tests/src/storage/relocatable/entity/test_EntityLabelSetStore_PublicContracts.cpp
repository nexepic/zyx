/**
 * @file test_EntityLabelSetStore_PublicContracts.cpp
 * @brief Public validation and placement contracts for native Node LabelSets.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
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

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	EntityPageNewPoolRequest nodePool(uint64_t segmentId, uint64_t ownerId, uint64_t pageId, uint64_t physicalSlot) {
		return {.segment_id = SegmentId(segmentId),
				.pool_owner_id = ownerId,
				.generation = Generation(1U),
				.page_id = PageId(pageId),
				.first_slot = PhysicalSlotId(physicalSlot),
				.last_slot = PhysicalSlotId(physicalSlot)};
	}

	NodeEntityRecord node(uint64_t firstLabel) {
		NodeEntityRecord result;
		result.inline_label_ids = {firstLabel, firstLabel + 2U, firstLabel + 4U};
		result.inline_label_count = static_cast<uint8_t>(RSS_ENTITY_INLINE_LABEL_CAPACITY);
		return result;
	}

	std::vector<uint64_t> labels(uint64_t first, size_t count) {
		std::vector<uint64_t> result;
		result.reserve(count);
		for (size_t index = 0U; index < count; ++index) {
			result.push_back(first + static_cast<uint64_t>(index) * 2U);
		}
		return result;
	}

	EntityLabelSetWritePlan appendTo(uint64_t segmentId, uint64_t pageId) {
		EntityLabelSetWritePlan result;
		result.append_page = EntityLabelSetAppendRequest{SegmentId(segmentId), PageId(pageId)};
		return result;
	}

	SlottedPagePoolAppendRequest labelPage(uint64_t segmentId, uint64_t ownerId, uint64_t pageId) {
		SlottedPagePoolAppendRequest result;
		result.segment_id = SegmentId(segmentId);
		result.generation = Generation(1U);
		result.page_id = PageId(pageId);
		result.page_owner_id = ownerId;
		result.page_owner_kind = PageOwnerKind::POK_ENTITY_LABEL;
		return result;
	}

} // namespace

class RelocatableEntityLabelSetStoreContractTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_entity_label_contract_" + to_string(uuid));
		file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		if (file && file->is_open()) {
			file->close();
		}
		file.reset();
		std::error_code error;
		std::filesystem::remove(path, error);
	}
};

TEST_F(RelocatableEntityLabelSetStoreContractTest, RejectsMismatchedPoolAndMalformedReadInputs) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore store(io, *metadata, pages);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.read(snapshot, {})); }, FormatError::FERR_INVALID_HEADER);

	NodeEntityRecord malformed;
	malformed.inline_label_count = static_cast<uint8_t>(RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);
	expectFormatError([&] { static_cast<void>(store.readNodeLabels(snapshot, malformed)); },
					  FormatError::FERR_INVALID_STATE);

	const auto foreignPath = path.string() + "_foreign";
	auto foreignFile = std::make_shared<std::fstream>(foreignPath,
														  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(foreignFile->is_open());
	auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, initialSuperblock());
	SlottedPagePool foreignPages(foreignIo, *foreignMetadata);
	EXPECT_THROW({ EntityLabelSetStore mismatch(io, *metadata, foreignPages); }, std::invalid_argument);
	snapshot.release();
	foreignIo.reset();
	foreignFile->close();
	foreignFile.reset();
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
}

TEST_F(RelocatableEntityLabelSetStoreContractTest, RejectsInvalidUpdatePlansBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	const NodeEntityRecord initial = node(7U);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, initial, LogSequenceNumber(2U)).has_value());
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore store(io, *metadata, pages);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::vector<uint64_t> overflow = labels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);

	expectFormatError(
			[&] { static_cast<void>(store.planNodeLabels(snapshot, 0U, overflow, {}, LogSequenceNumber(3U))); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store.planNodeLabels(snapshot, 1001U, overflow, {}, {})); },
			FormatError::FERR_INVALID_HEADER);
	for (const std::vector<uint64_t> &invalid: {std::vector<uint64_t>{0U}, std::vector<uint64_t>{7U, 5U},
																	 std::vector<uint64_t>{7U, 7U}}) {
		expectFormatError(
					[&] { static_cast<void>(store.planNodeLabels(snapshot, 1001U, invalid, {}, LogSequenceNumber(3U))); },
					FormatError::FERR_INVALID_HEADER);
	}

	for (const std::vector<PageId> &invalid: {std::vector<PageId>{PageId{}},
														std::vector<PageId>{PageId(103U), PageId(102U)},
														std::vector<PageId>{PageId(102U), PageId(102U)}}) {
		EntityLabelSetWritePlan plan;
		plan.excluded_page_ids = invalid;
		expectFormatError(
					[&] { static_cast<void>(store.planNodeLabels(snapshot, 1001U, overflow, plan, LogSequenceNumber(3U))); },
					FormatError::FERR_INVALID_HEADER);
	}
	for (const std::vector<SegmentId> &invalid: {std::vector<SegmentId>{SegmentId{}},
															   std::vector<SegmentId>{SegmentId(31U), SegmentId(31U)}}) {
		EntityLabelSetWritePlan plan;
		plan.candidate_pool_segments = invalid;
		expectFormatError(
					[&] { static_cast<void>(store.planNodeLabels(snapshot, 1001U, overflow, plan, LogSequenceNumber(3U))); },
					FormatError::FERR_INVALID_HEADER);
	}
	EntityLabelSetWritePlan invalidAppend = appendTo(0U, 102U);
	expectFormatError(
			[&] { static_cast<void>(store.planNodeLabels(snapshot, 1001U, overflow, invalidAppend, LogSequenceNumber(3U))); },
			FormatError::FERR_INVALID_HEADER);
	EntityLabelSetWritePlan invalidNewPool;
	invalidNewPool.new_pool = EntityLabelSetNewPoolRequest{};
	expectFormatError(
			[&] { static_cast<void>(store.planNodeLabels(snapshot, 1001U, overflow, invalidNewPool, LogSequenceNumber(3U))); },
			FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(entities.readNode(snapshot, 1001U), initial);
	snapshot.release();
}

TEST_F(RelocatableEntityLabelSetStoreContractTest, UsesExistingTypedPageForInitialNodeAndHonorsExcludedPages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, node(7U), LogSequenceNumber(2U))
					.has_value());
	SlottedPagePool pages(io, *metadata);
	static_cast<void>(pages.appendEmptyPage(labelPage(31U, 901U, 102U), LogSequenceNumber(3U)));
	EntityLabelSetStore store(io, *metadata, pages);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::vector<uint64_t> complete = labels(101U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);

	EntityLabelSetWritePlan existing;
	existing.candidate_pool_segments = {SegmentId(31U)};
	const EntityLabelSetInitialNodePlan onExisting =
			store.planInitialNodeLabels(snapshot, node(101U), complete, existing, LogSequenceNumber(4U));
	ASSERT_EQ(onExisting.auxiliary_pages.replacements.size(), 1U);
	EXPECT_TRUE(onExisting.auxiliary_pages.appends.empty());
	EXPECT_TRUE(onExisting.auxiliary_pages.new_pools.empty());
	EXPECT_EQ(onExisting.overflow.page_id, PageId(102U));

	EntityLabelSetWritePlan excluded = appendTo(31U, 103U);
	excluded.candidate_pool_segments = {SegmentId(31U)};
	excluded.excluded_page_ids = {PageId(102U)};
	const EntityLabelSetInitialNodePlan onAppend =
			store.planInitialNodeLabels(snapshot, node(201U), complete, excluded, LogSequenceNumber(4U));
	EXPECT_TRUE(onAppend.auxiliary_pages.replacements.empty());
	ASSERT_EQ(onAppend.auxiliary_pages.appends.size(), 1U);
	EXPECT_EQ(onAppend.overflow.page_id, PageId(103U));
	EXPECT_FALSE(metadata->findPageLocationOptional(snapshot, PageId(103U)).has_value());
	snapshot.release();
}

TEST_F(RelocatableEntityLabelSetStoreContractTest, RejectsPageIdCollisionsForUpdateAndInitialFallbacks) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	const NodeEntityRecord initial = node(7U);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, initial, LogSequenceNumber(2U)).has_value());
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore store(io, *metadata, pages);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::vector<uint64_t> overflow = labels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);

	expectFormatError(
			[&] { static_cast<void>(store.planNodeLabels(snapshot, 1001U, overflow, appendTo(31U, 101U), LogSequenceNumber(3U))); },
			FormatError::FERR_INVALID_STATE);
	EntityLabelSetWritePlan reusedNewPool;
	reusedNewPool.new_pool = EntityLabelSetNewPoolRequest{
			SegmentId(32U), 902U, Generation(1U), PageId(101U), PhysicalSlotId(41U), PhysicalSlotId(41U)};
	expectFormatError(
			[&] { static_cast<void>(store.planNodeLabels(snapshot, 1001U, overflow, reusedNewPool, LogSequenceNumber(3U))); },
			FormatError::FERR_INVALID_STATE);

	expectFormatError(
			[&] { static_cast<void>(store.planInitialNodeLabels(snapshot, node(101U), overflow, appendTo(31U, 101U),
																			 LogSequenceNumber(3U))); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] { static_cast<void>(store.planInitialNodeLabels(snapshot, node(101U), overflow, reusedNewPool,
																			 LogSequenceNumber(3U))); },
			FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(entities.readNode(snapshot, 1001U), initial);
	snapshot.release();
}
