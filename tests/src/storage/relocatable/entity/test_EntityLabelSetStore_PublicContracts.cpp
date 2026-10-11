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
	invalidAppend = appendTo(31U, 0U);
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

TEST_F(RelocatableEntityLabelSetStoreContractTest, DiscoversTheCurrentEntityPoolWhenNoCandidateListIsSupplied) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, node(7U), LogSequenceNumber(2U))
				.has_value());
	SlottedPagePool pages(io, *metadata);
	static_cast<void>(pages.appendEmptyPage(labelPage(31U, 901U, 102U), LogSequenceNumber(3U)));
	EntityLabelSetStore store(io, *metadata, pages);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	// An empty candidate list is the public default: use the current sealed
	// Entity pools rather than requiring callers to duplicate catalog state.
	const EntityLabelSetInitialNodePlan planned = store.planInitialNodeLabels(
			snapshot, node(101U), labels(101U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U), {}, LogSequenceNumber(4U));

	ASSERT_EQ(planned.auxiliary_pages.replacements.size(), 1U);
	EXPECT_TRUE(planned.auxiliary_pages.appends.empty());
	EXPECT_TRUE(planned.auxiliary_pages.new_pools.empty());
	EXPECT_EQ(planned.overflow.page_id, PageId(102U));
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

TEST_F(RelocatableEntityLabelSetStoreContractTest,
		FallsBackToANewPoolWhenTheRequestedAppendPageIsAlreadyPublished) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, node(7U), LogSequenceNumber(2U)).has_value());
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore store(io, *metadata, pages);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	EntityLabelSetWritePlan plan = appendTo(31U, 101U);
	plan.new_pool = EntityLabelSetNewPoolRequest{
			SegmentId(32U), 902U, Generation(1U), PageId(102U), PhysicalSlotId(41U), PhysicalSlotId(41U)};
	const EntityLabelSetInitialNodePlan planned = store.planInitialNodeLabels(
			snapshot, node(101U), labels(101U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U), plan, LogSequenceNumber(3U));

	// The existing Entity page cannot be reused as a LabelSet append frame. The
	// supplied new-pool fallback remains a valid, unpublished destination.
	EXPECT_TRUE(planned.auxiliary_pages.appends.empty());
	ASSERT_EQ(planned.auxiliary_pages.new_pools.size(), 1U);
	EXPECT_EQ(planned.auxiliary_pages.new_pools.front().segment_id, SegmentId(32U));
	EXPECT_EQ(planned.overflow.page_id, PageId(102U));
	EXPECT_FALSE(metadata->findPageLocationOptional(snapshot, PageId(102U)).has_value());
	snapshot.release();
}

TEST_F(RelocatableEntityLabelSetStoreContractTest, RejectsAnOverflowReferenceWithAPartialFixedPrefix) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, node(7U), LogSequenceNumber(2U)).has_value());
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore store(io, *metadata, pages);
	const std::vector<uint64_t> complete = labels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);

	MetadataSnapshotPin writeSnapshot = metadata->openSnapshot();
	static_cast<void>(store.updateNodeLabels(writeSnapshot, 1001U, complete, appendTo(31U, 102U), LogSequenceNumber(3U)));
	writeSnapshot.release();

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const NodeEntityRecord persisted = entities.readNode(snapshot, 1001U);
	ASSERT_TRUE(persisted.label_overflow.isValid());
	size_t pageRequested = 0U;
	EXPECT_THROW(static_cast<void>(store.read(snapshot, persisted.label_overflow, [&](size_t bytes) {
		pageRequested = bytes; throw std::length_error("point page budget");
	})), std::length_error);
	EXPECT_EQ(pageRequested, RSS_PAGE_SIZE);
	bool rejected = false;
	EXPECT_THROW(static_cast<void>(store.read(snapshot, persisted.label_overflow, [&](size_t) {
		rejected = true; throw std::length_error("label decode budget");
	})), std::length_error);
	EXPECT_TRUE(rejected);
	for (int retry = 0; retry < 2; ++retry) {
		size_t retained = 0U;
		const auto overflow = store.read(snapshot, persisted.label_overflow,
			[&](size_t bytes) { retained = bytes; });
		EXPECT_EQ(overflow, (std::vector<uint64_t>(complete.begin() + RSS_ENTITY_INLINE_LABEL_CAPACITY, complete.end())));
		EXPECT_EQ(retained, overflow.capacity() * sizeof(uint64_t));
	}

	NodeEntityRecord malformed = persisted;
	--malformed.inline_label_count;
	expectFormatError([&] { static_cast<void>(store.readNodeLabels(snapshot, malformed)); },
				  FormatError::FERR_INVALID_STATE);
	size_t membershipRetained = 1U;
	EXPECT_TRUE(store.containsNodeLabel(snapshot, persisted, complete.back(),
		[&](size_t bytes) { membershipRetained = bytes; }));
	EXPECT_EQ(membershipRetained, 0U);
	EXPECT_THROW(static_cast<void>(store.readNodeLabels(snapshot, persisted, [&](size_t) {
		throw std::length_error("combined label budget");
	})), std::length_error);
	const auto overflowForBudget = store.read(snapshot, persisted.label_overflow);
	const size_t overflowBytes = overflowForBudget.capacity() * sizeof(uint64_t);
	bool decodedOverflow = false;
	size_t mergeRequested = 0U;
	EXPECT_THROW(static_cast<void>(store.readNodeLabels(snapshot, persisted, [&](size_t bytes) {
		if (decodedOverflow && bytes > overflowBytes) {
			mergeRequested = bytes; throw std::length_error("label merge budget");
		}
		if (bytes == overflowBytes) decodedOverflow = true;
	})), std::length_error);
	EXPECT_EQ(mergeRequested, overflowBytes + complete.size() * sizeof(uint64_t));
	for (int retry = 0; retry < 2; ++retry) {
		size_t retained = 0U, peak = 0U;
		const auto combined = store.readNodeLabels(snapshot, persisted, [&](size_t bytes) {
			retained = bytes; peak = std::max(peak, bytes);
		});
		EXPECT_EQ(combined, complete);
		EXPECT_EQ(retained, combined.capacity() * sizeof(uint64_t));
		EXPECT_GE(peak, overflowBytes + retained);
	}
	const auto decoded = store.readNodeLabels(snapshot, persisted);
	EXPECT_EQ(decoded, complete);
	EXPECT_LT(decoded.capacity(), RSS_ENTITY_MAX_LABELS);
	const auto batch = store.readNodeLabelsSorted(snapshot, std::array{persisted});
	ASSERT_EQ(batch.size(), 1U);
	EXPECT_EQ(batch[0], complete);
	EXPECT_LT(batch[0].capacity(), RSS_ENTITY_MAX_LABELS);
	const std::array repeatedRecords{persisted, persisted, persisted};
	size_t batchRequested = 0U;
	EXPECT_THROW(static_cast<void>(store.readNodeLabelsSorted(snapshot, repeatedRecords, [&](size_t bytes) {
		batchRequested = bytes; throw std::length_error("batch result slots");
	})), std::length_error);
	EXPECT_EQ(batchRequested, repeatedRecords.size() * sizeof(std::vector<uint64_t>));
	const size_t batchSlots = repeatedRecords.size() * sizeof(std::vector<uint64_t>);
	EXPECT_THROW(static_cast<void>(store.readNodeLabelsSorted(snapshot, repeatedRecords, [&](size_t bytes) {
		if (bytes > batchSlots) { batchRequested = bytes; throw std::length_error("page view slots"); }
	})), std::length_error);
	EXPECT_GT(batchRequested, batchSlots);
	bool pageRejected = false;
	EXPECT_THROW(static_cast<void>(store.readNodeLabelsSorted(snapshot, repeatedRecords, [&](size_t bytes) {
		if (bytes >= RSS_PAGE_SIZE) { pageRejected = true; throw std::length_error("label page budget"); }
	})), std::length_error);
	EXPECT_TRUE(pageRejected);
	for (int retry = 0; retry < 2; ++retry) {
		size_t retained = 0U;
		size_t peak = 0U;
		const auto admitted = store.readNodeLabelsSorted(snapshot, repeatedRecords, [&](size_t bytes) {
			retained = bytes; peak = std::max(peak, bytes);
		});
		EXPECT_GE(peak, RSS_PAGE_SIZE);
		size_t expectedBytes = admitted.capacity() * sizeof(std::vector<uint64_t>);
		for (const auto &entry: admitted) expectedBytes += entry.capacity() * sizeof(uint64_t);
		EXPECT_EQ(retained, expectedBytes);
		EXPECT_EQ(admitted, (std::vector<std::vector<uint64_t>>(3U, complete)));
	}
	NodeEntityRecord inlineOnly;
	inlineOnly.inline_label_ids = {3U, 7U, 11U}; inlineOnly.inline_label_count = 3U;
	std::vector<NodeEntityRecord> mixedRecords(1024U, inlineOnly);
	mixedRecords[1U] = persisted; mixedRecords[511U] = persisted; mixedRecords.back() = persisted;
	const auto mixed = store.readNodeLabelsSorted(snapshot, mixedRecords);
	ASSERT_EQ(mixed.size(), mixedRecords.size());
	for (size_t index = 0U; index < mixed.size(); ++index) {
		EXPECT_EQ(mixed[index], (index == 1U || index == 511U || index == 1023U)
			? complete : (std::vector<uint64_t>{3U, 7U, 11U}));
	}
	for (int retry = 0; retry < 2; ++retry) {
		const auto repeated = store.readNodeLabelsSorted(snapshot, repeatedRecords);
		EXPECT_EQ(repeated, (std::vector<std::vector<uint64_t>>(3U, complete)));
	}
	io->enableBufferPool(64U * RSS_PAGE_SIZE);
	for (int retry = 0; retry < 2; ++retry) {
		EXPECT_EQ(store.readNodeLabels(snapshot, persisted), complete);
		EXPECT_TRUE(store.containsNodeLabel(snapshot, persisted, complete.back()));
		EXPECT_EQ(store.readNodeLabelsSorted(snapshot, repeatedRecords),
			(std::vector<std::vector<uint64_t>>(3U, complete)));
	}
	NodeEntityRecord stale = persisted;
	++stale.label_overflow.slot_generation;
	EXPECT_THROW(static_cast<void>(store.readNodeLabelsSorted(snapshot, std::array{persisted, stale})), FormatException);
	snapshot.release();
}

TEST_F(RelocatableEntityLabelSetStoreContractTest, InlineMembershipValidatesAllLabelsAndDoesNotSkipOverflow) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata); EntityLabelSetStore store(io, *metadata, pages);
	auto snapshot = metadata->openSnapshot(); NodeEntityRecord record;
	EXPECT_FALSE(store.containsNodeLabel(snapshot, record, 7U));
	record.inline_label_count = 3U; record.inline_label_ids = {3U, 7U, 11U};
	EXPECT_TRUE(store.containsNodeLabel(snapshot, record, 3U));
	EXPECT_TRUE(store.containsNodeLabel(snapshot, record, 11U));
	EXPECT_FALSE(store.containsNodeLabel(snapshot, record, 0U));
	EXPECT_FALSE(store.containsNodeLabel(snapshot, record, 8U));
	const auto inlineLabels = store.readNodeLabels(snapshot, record);
	EXPECT_EQ(inlineLabels, (std::vector<uint64_t>{3U, 7U, 11U}));
	EXPECT_LT(inlineLabels.capacity(), RSS_ENTITY_MAX_LABELS);
	const auto inlineBatch = store.readNodeLabelsSorted(snapshot, std::array{record});
	ASSERT_EQ(inlineBatch.size(), 1U);
	EXPECT_EQ(inlineBatch[0], inlineLabels);
	EXPECT_LT(inlineBatch[0].capacity(), RSS_ENTITY_MAX_LABELS);
	record.inline_label_ids[2] = 7U;
	EXPECT_THROW(static_cast<void>(store.containsNodeLabel(snapshot, record, 3U)), FormatException);
	record.inline_label_ids[2] = 11U;
	record.label_overflow = {PageId(999U), 0U, 0U};
	EXPECT_THROW(static_cast<void>(store.containsNodeLabel(snapshot, record, 3U)), FormatException);
}

TEST_F(RelocatableEntityLabelSetStoreContractTest, BatchRejectsInvalidCountsBeforeAdmissionAndInlineAllocationBeforeDecode) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata); EntityLabelSetStore store(io, *metadata, pages);
	auto snapshot = metadata->openSnapshot();
	NodeEntityRecord valid; valid.inline_label_ids = {3U, 7U, 11U}; valid.inline_label_count = 3U;
	NodeEntityRecord invalid = valid; invalid.inline_label_count = RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U;
	size_t calls = 0U;
	EXPECT_THROW(static_cast<void>(store.readNodeLabelsSorted(snapshot, std::array{valid, invalid},
		[&](size_t) { ++calls; })), FormatException);
	EXPECT_EQ(calls, 0U);
	for (const auto badLabels: {std::array<uint64_t, 3U>{3U, 7U, 7U},
		std::array<uint64_t, 3U>{3U, 0U, 11U}, std::array<uint64_t, 3U>{11U, 7U, 3U}}) {
		invalid = valid; invalid.inline_label_ids = badLabels;
		EXPECT_THROW(static_cast<void>(store.readNodeLabelsSorted(snapshot, std::array{valid, invalid},
			[&](size_t) { ++calls; throw std::length_error("must not mask corrupt labels"); })), FormatException);
		EXPECT_EQ(calls, 0U);
	}
	invalid = valid; invalid.label_overflow = {PageId(999U), 0U, 0U};
	EXPECT_THROW(static_cast<void>(store.readNodeLabelsSorted(snapshot, std::array{valid, invalid},
		[&](size_t) { ++calls; throw std::length_error("partial overflow"); })), FormatException);
	EXPECT_EQ(calls, 0U);
	invalid.label_overflow.slot_generation = 1U; invalid.inline_label_count = 2U;
	EXPECT_THROW(static_cast<void>(store.readNodeLabelsSorted(snapshot, std::array{valid, invalid},
		[&](size_t) { ++calls; throw std::length_error("partial prefix"); })), FormatException);
	EXPECT_EQ(calls, 0U);
	for (auto malformed: {valid, invalid}) {
		if (malformed.label_overflow.isEmpty()) malformed.inline_label_ids[1] = 0U;
		EXPECT_THROW(static_cast<void>(store.readNodeLabels(snapshot, malformed, [&](size_t) {
			++calls; throw std::length_error("must not mask invalid point labels");
		})), FormatException);
		EXPECT_EQ(calls, 0U);
	}
	const size_t slots = sizeof(std::vector<uint64_t>);
	size_t requested = 0U;
	EXPECT_THROW(static_cast<void>(store.readNodeLabelsSorted(snapshot, std::array{valid}, [&](size_t bytes) {
		if (bytes > slots) { requested = bytes; throw std::length_error("inline result budget"); }
	})), std::length_error);
	EXPECT_EQ(requested, slots + 3U * sizeof(uint64_t));
	for (int retry = 0; retry < 2; ++retry) {
		size_t retained = 0U;
		const auto result = store.readNodeLabelsSorted(snapshot, std::array{valid}, [&](size_t bytes) { retained = bytes; });
		ASSERT_EQ(result.size(), 1U);
		EXPECT_EQ(result[0], (std::vector<uint64_t>{3U, 7U, 11U}));
		EXPECT_EQ(retained, result.capacity() * sizeof(std::vector<uint64_t>) + result[0].capacity() * sizeof(uint64_t));
	}
}
