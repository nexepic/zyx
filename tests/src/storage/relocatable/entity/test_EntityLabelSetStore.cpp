/**
 * @file test_EntityLabelSetStore.cpp
 * @brief Durable atomic Node LabelSet tests for native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
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

} // namespace

class RelocatableEntityLabelSetStoreTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_entity_label_set_store_" + to_string(uuid));
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

TEST_F(RelocatableEntityLabelSetStoreTest, PublishesOverflowWithNodeThenPreservesReferenceAndRetiresOnShrink) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	const NodeEntityRecord initial = node(7U);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, initial, LogSequenceNumber(2U)).has_value());
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore labelsStore(io, *metadata, pages);

	MetadataSnapshotPin beforeOverflow = metadata->openSnapshot();
	MetadataSnapshotPin writeSnapshot = metadata->openSnapshot();
	const std::vector<uint64_t> first = labels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);
	const EntityLabelSetUpdateResult firstUpdate =
			labelsStore.updateNodeLabels(writeSnapshot, 1001U, first, appendTo(31U, 102U), LogSequenceNumber(3U));
	writeSnapshot.release();
	ASSERT_TRUE(firstUpdate.current_overflow.isValid());
	EXPECT_TRUE(firstUpdate.previous_overflow.isEmpty());
	EXPECT_EQ(entities.readNode(beforeOverflow, 1001U), initial);
	EXPECT_FALSE(metadata->findPageLocationOptional(beforeOverflow, PageId(102U)).has_value());

	MetadataSnapshotPin firstSnapshot = metadata->openSnapshot();
	const NodeEntityRecord firstNode = entities.readNode(firstSnapshot, 1001U);
	EXPECT_EQ(firstNode.label_overflow, firstUpdate.current_overflow);
	EXPECT_EQ(labelsStore.readNodeLabels(firstSnapshot, firstNode), first);

	const std::vector<uint64_t> grown = labels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 3U);
	EntityLabelSetWritePlan existingOnly;
	existingOnly.candidate_pool_segments = {SegmentId(31U)};
	const EntityLabelSetUpdateResult grownUpdate =
			labelsStore.updateNodeLabels(firstSnapshot, 1001U, grown, existingOnly, LogSequenceNumber(4U));
	EXPECT_EQ(grownUpdate.current_overflow, firstUpdate.current_overflow);
	EXPECT_EQ(labelsStore.readNodeLabels(firstSnapshot, firstNode), first);

	MetadataSnapshotPin grownSnapshot = metadata->openSnapshot();
	const NodeEntityRecord grownNode = entities.readNode(grownSnapshot, 1001U);
	EXPECT_EQ(grownNode.label_overflow, firstUpdate.current_overflow);
	EXPECT_EQ(labelsStore.readNodeLabels(grownSnapshot, grownNode), grown);

	const std::vector<uint64_t> inlineOnly = labels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY);
	const EntityLabelSetUpdateResult shrunk = labelsStore.updateNodeLabels(
			grownSnapshot, 1001U, inlineOnly, EntityLabelSetWritePlan{}, LogSequenceNumber(5U));
	EXPECT_TRUE(shrunk.current_overflow.isEmpty());
	EXPECT_TRUE(shrunk.retired_empty_label_page);
	EXPECT_EQ(labelsStore.readNodeLabels(grownSnapshot, grownNode), grown);

	MetadataSnapshotPin current = metadata->openSnapshot();
	const NodeEntityRecord currentNode = entities.readNode(current, 1001U);
	EXPECT_TRUE(currentNode.label_overflow.isEmpty());
	EXPECT_EQ(labelsStore.readNodeLabels(current, currentNode), inlineOnly);
	EXPECT_FALSE(metadata->findPageLocationOptional(current, PageId(102U)).has_value());
	beforeOverflow.release();
	firstSnapshot.release();
	grownSnapshot.release();
	current.release();
	pages.reclaimRetiredFrames(SegmentId(31U));

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	EntityPageStore reopenedEntities(io, *reopened);
	SlottedPagePool reopenedPages(io, *reopened);
	EntityLabelSetStore reopenedLabels(io, *reopened, reopenedPages);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedLabels.readNodeLabels(reopenedSnapshot, reopenedEntities.readNode(reopenedSnapshot, 1001U)),
			  inlineOnly);
}

TEST_F(RelocatableEntityLabelSetStoreTest, PlansLabelSetWithoutPublicationAndRequiresItsExactNodeReplacement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	const NodeEntityRecord initial = node(7U);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, initial, LogSequenceNumber(2U)).has_value());
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore labelsStore(io, *metadata, pages);
	const std::vector<uint64_t> completeLabels = labels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);

	MetadataSnapshotPin before = metadata->openSnapshot();
	const EntityLabelSetMutationPlan planned =
			labelsStore.planNodeLabels(before, 1001U, completeLabels, appendTo(31U, 102U), LogSequenceNumber(3U));
	EXPECT_EQ(planned.node_id, 1001U);
	EXPECT_EQ(planned.node_location, metadata->findNodePageLocation(before, 1001U));
	EXPECT_TRUE(planned.previous_overflow.isEmpty());
	EXPECT_TRUE(planned.current_overflow.isValid());
	ASSERT_EQ(planned.auxiliary_pages.appends.size(), 1U);
	ASSERT_EQ(planned.auxiliary_pages.node_update_bindings.size(), 1U);
	EXPECT_EQ(planned.auxiliary_pages.node_update_bindings.front().replacement_record, planned.replacement_node);
	EXPECT_EQ(entities.readNode(before, 1001U), initial);
	EXPECT_FALSE(metadata->findPageLocationOptional(before, PageId(102U)).has_value());

	const std::array<EntityPageBatchMutation, 1U> wrongMutation{{
			{.page_id = planned.node_location.page_id,
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, initial}}},
	}};
	expectFormatError(
			[&] {
				static_cast<void>(entities.applyAuxiliaryEntityBatchTransaction(
						before, wrongMutation, {}, {}, planned.auxiliary_pages, LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(entities.readNode(before, 1001U), initial);
	EXPECT_FALSE(metadata->findPageLocationOptional(before, PageId(102U)).has_value());

	const std::array<EntityPageBatchMutation, 1U> matchingMutation{{
			{.page_id = planned.node_location.page_id,
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, planned.replacement_node}}},
	}};
	const EntityPageBatchResult published = entities.applyAuxiliaryEntityBatchTransaction(
			before, matchingMutation, {}, {}, planned.auxiliary_pages, LogSequenceNumber(3U));
	ASSERT_EQ(published.pages.size(), 1U);
	EXPECT_TRUE(published.appended_pages.empty());
	EXPECT_TRUE(published.new_pool_pages.empty());
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(entities.readNode(before, 1001U), initial);
	EXPECT_EQ(labelsStore.readNodeLabels(current, entities.readNode(current, 1001U)), completeLabels);
	before.release();
	current.release();
}

TEST_F(RelocatableEntityLabelSetStoreTest, PlansAndPublishesInitialOverflowLabelsWithOneNodeInsertion) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, node(7U), LogSequenceNumber(2U)).has_value());
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore labelsStore(io, *metadata, pages);
	const std::vector<uint64_t> inlineLabels = labels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const EntityLabelSetInitialNodePlan inlinePlan =
			labelsStore.planInitialNodeLabels(snapshot, node(7U), inlineLabels, {}, LogSequenceNumber(3U));
	EXPECT_TRUE(inlinePlan.overflow.isEmpty());
	EXPECT_TRUE(inlinePlan.auxiliary_pages.empty());
	EXPECT_EQ(inlinePlan.node.inline_label_count, RSS_ENTITY_INLINE_LABEL_CAPACITY);

	const std::vector<uint64_t> completeLabels = labels(101U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);
	const EntityLabelSetInitialNodePlan overflowPlan = labelsStore.planInitialNodeLabels(
			snapshot, node(101U), completeLabels, appendTo(31U, 102U), LogSequenceNumber(3U));
	ASSERT_TRUE(overflowPlan.overflow.isValid());
	ASSERT_EQ(overflowPlan.auxiliary_pages.appends.size(), 1U);
	EXPECT_TRUE(overflowPlan.auxiliary_pages.node_update_bindings.empty());
	EXPECT_TRUE(overflowPlan.auxiliary_pages.node_insertion_bindings.empty());
	EXPECT_FALSE(metadata->findPageLocationOptional(snapshot, PageId(102U)).has_value());

	const CatalogEntry pool = metadata->findCatalog(snapshot, SegmentId(31U));
	EntityPageBatchAppend append{
			.request = {pool, PageId(103U)},
			.record_kind = EntityPageRecordKind::EPRK_NODE,
			.node_insertions = {{1002U, overflowPlan.node}},
	};
	EntityPageAuxiliaryMutationBatch wrongBinding = overflowPlan.auxiliary_pages;
	NodeEntityRecord mismatched = overflowPlan.node;
	mismatched.inline_label_ids[0U] = 100U;
	wrongBinding.node_insertion_bindings.push_back({1002U, mismatched});
	expectFormatError(
			[&] {
				static_cast<void>(entities.applyAuxiliaryEntityBatchTransaction(
						snapshot, {}, std::span(&append, 1U), {}, wrongBinding, LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(metadata->findPageLocationOptional(snapshot, PageId(102U)).has_value());
	EXPECT_FALSE(metadata->findNodePageLocationOptional(snapshot, 1002U).has_value());

	EntityPageAuxiliaryMutationBatch binding = overflowPlan.auxiliary_pages;
	binding.node_insertion_bindings.push_back({1002U, overflowPlan.node});
	const EntityPageBatchResult published = entities.applyAuxiliaryEntityBatchTransaction(
			snapshot, {}, std::span(&append, 1U), {}, binding, LogSequenceNumber(3U));
	ASSERT_EQ(published.appended_pages.size(), 1U);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(labelsStore.readNodeLabels(current, entities.readNode(current, 1002U)), completeLabels);
	EXPECT_EQ(entities.readNode(snapshot, 1001U), node(7U));
	snapshot.release();
	current.release();
}

TEST_F(RelocatableEntityLabelSetStoreTest, PacksInitialNodeAndLabelSetPagesIntoOneFreshEntityPool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore labelsStore(io, *metadata, pages);
	const std::vector<uint64_t> completeLabels = labels(101U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);
	EntityLabelSetWritePlan labelPlan;
	labelPlan.new_pool = EntityLabelSetNewPoolRequest{
			SegmentId(31U), 901U, Generation(1U), PageId(102U), PhysicalSlotId(40U), PhysicalSlotId(40U)};
	MetadataSnapshotPin before = metadata->openSnapshot();
	const EntityLabelSetInitialNodePlan initialPlan =
			labelsStore.planInitialNodeLabels(before, node(101U), completeLabels, labelPlan, LogSequenceNumber(2U));
	ASSERT_TRUE(initialPlan.overflow.isValid());
	ASSERT_EQ(initialPlan.auxiliary_pages.new_pools.size(), 1U);

	EntityPageBatchNewPool nodePage{
			.request = nodePool(31U, 901U, 101U, 40U),
			.record_kind = EntityPageRecordKind::EPRK_NODE,
			.node_insertions = {{1001U, initialPlan.node}},
	};
	EntityPageAuxiliaryMutationBatch auxiliary = initialPlan.auxiliary_pages;
	auxiliary.node_insertion_bindings.push_back({1001U, initialPlan.node});
	const EntityPageBatchResult result =
			entities.applyAuxiliaryEntityBatchTransaction(before, {}, std::span<const EntityPageBatchAppend>{},
														  std::span(&nodePage, 1U), auxiliary, LogSequenceNumber(2U));
	ASSERT_EQ(result.new_pool_pages.size(), 1U);
	EXPECT_EQ(result.new_pool_pages.front().pool.physical_slot, PhysicalSlotId(40U));

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(entities.currentPools(current).size(), 1U);
	const PageLocation nodeLocation = metadata->findPageLocation(current, PageId(101U));
	const PageLocation labelLocation = metadata->findPageLocation(current, PageId(102U));
	EXPECT_EQ(nodeLocation.segment_id, SegmentId(31U));
	EXPECT_EQ(labelLocation.segment_id, SegmentId(31U));
	EXPECT_NE(nodeLocation.page_index, labelLocation.page_index);
	EXPECT_EQ(labelsStore.readNodeLabels(current, entities.readNode(current, 1001U)), completeLabels);
	before.release();
	current.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	EntityPageStore reopenedEntities(io, *reopened);
	SlottedPagePool reopenedPages(io, *reopened);
	EntityLabelSetStore reopenedLabels(io, *reopened, reopenedPages);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedEntities.currentPools(reopenedSnapshot).size(), 1U);
	EXPECT_EQ(reopenedLabels.readNodeLabels(reopenedSnapshot, reopenedEntities.readNode(reopenedSnapshot, 1001U)),
			  completeLabels);
}

TEST_F(RelocatableEntityLabelSetStoreTest, SharesLabelPageAndMovesOnlyTheGrowingNodeToAnotherPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, node(7U), LogSequenceNumber(2U)).has_value());
	MetadataSnapshotPin insertSnapshot = metadata->openSnapshot();
	static_cast<void>(entities.insertNode(insertSnapshot, PageId(101U), 1002U, node(1001U), LogSequenceNumber(3U)));
	insertSnapshot.release();
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore labelsStore(io, *metadata, pages);

	MetadataSnapshotPin firstWrite = metadata->openSnapshot();
	const std::vector<uint64_t> nodeALabels = labels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);
	const EntityLabelSetUpdateResult nodeA =
			labelsStore.updateNodeLabels(firstWrite, 1001U, nodeALabels, appendTo(31U, 102U), LogSequenceNumber(4U));
	firstWrite.release();
	ASSERT_TRUE(nodeA.current_overflow.isValid());

	MetadataSnapshotPin secondWrite = metadata->openSnapshot();
	const std::vector<uint64_t> nodeBLabels = labels(1001U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);
	EntityLabelSetWritePlan existingOnly;
	existingOnly.candidate_pool_segments = {SegmentId(31U)};
	const EntityLabelSetUpdateResult nodeB =
			labelsStore.updateNodeLabels(secondWrite, 1002U, nodeBLabels, existingOnly, LogSequenceNumber(5U));
	secondWrite.release();
	ASSERT_EQ(nodeA.current_overflow.page_id, nodeB.current_overflow.page_id);
	EXPECT_NE(nodeA.current_overflow.slot_id, nodeB.current_overflow.slot_id);

	MetadataSnapshotPin moveWrite = metadata->openSnapshot();
	const std::vector<uint64_t> maximum = labels(7U, RSS_ENTITY_MAX_LABELS);
	EntityLabelSetWritePlan movePlan;
	movePlan.candidate_pool_segments = {SegmentId(31U)};
	movePlan.append_page = EntityLabelSetAppendRequest{SegmentId(31U), PageId(103U)};
	const EntityLabelSetUpdateResult moved =
			labelsStore.updateNodeLabels(moveWrite, 1001U, maximum, movePlan, LogSequenceNumber(6U));
	moveWrite.release();
	EXPECT_NE(moved.current_overflow.page_id, nodeA.current_overflow.page_id);

	MetadataSnapshotPin current = metadata->openSnapshot();
	const NodeEntityRecord currentA = entities.readNode(current, 1001U);
	const NodeEntityRecord currentB = entities.readNode(current, 1002U);
	EXPECT_EQ(currentA.label_overflow, moved.current_overflow);
	EXPECT_EQ(currentB.label_overflow, nodeB.current_overflow);
	EXPECT_EQ(labelsStore.readNodeLabels(current, currentA), maximum);
	EXPECT_EQ(labelsStore.readNodeLabels(current, currentB), nodeBLabels);
}

TEST_F(RelocatableEntityLabelSetStoreTest, SharedLabelPageRemainsResidentAcrossNodePointReads) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, node(7U), LogSequenceNumber(2U)).has_value());
	MetadataSnapshotPin insertSnapshot = metadata->openSnapshot();
	static_cast<void>(entities.insertNode(insertSnapshot, PageId(101U), 1002U, node(1001U), LogSequenceNumber(3U)));
	insertSnapshot.release();
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore labelsStore(io, *metadata, pages);

	const std::vector<uint64_t> nodeALabels = labels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);
	MetadataSnapshotPin firstWrite = metadata->openSnapshot();
	const EntityLabelSetUpdateResult nodeA =
			labelsStore.updateNodeLabels(firstWrite, 1001U, nodeALabels, appendTo(31U, 102U), LogSequenceNumber(4U));
	firstWrite.release();

	const std::vector<uint64_t> nodeBLabels = labels(1001U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);
	MetadataSnapshotPin secondWrite = metadata->openSnapshot();
	EntityLabelSetWritePlan existingOnly;
	existingOnly.candidate_pool_segments = {SegmentId(31U)};
	const EntityLabelSetUpdateResult nodeB =
			labelsStore.updateNodeLabels(secondWrite, 1002U, nodeBLabels, existingOnly, LogSequenceNumber(5U));
	secondWrite.release();
	ASSERT_EQ(nodeA.current_overflow.page_id, nodeB.current_overflow.page_id);
	ASSERT_NE(nodeA.current_overflow.slot_id, nodeB.current_overflow.slot_id);

	io->sync();
	io->enableBufferPool(32U * RSS_PAGE_SIZE);
	MetadataSnapshotPin current = metadata->openSnapshot();
	const NodeEntityRecord currentA = entities.readNode(current, 1001U);
	const NodeEntityRecord currentB = entities.readNode(current, 1002U);
	const PageLocation labelLocation = metadata->findPageLocation(current, nodeA.current_overflow.page_id);
	EXPECT_EQ(labelLocation.segment_id, SegmentId(31U));
	EXPECT_GT(pages.availableFrameCount(current, labelLocation.segment_id), 0U);

	io->resetBufferPoolStats();
	const BufferPoolStats before = io->bufferPoolStats();
	EXPECT_EQ(labelsStore.readNodeLabels(current, currentA), nodeALabels);
	const BufferPoolStats afterFirst = io->bufferPoolStats();
	EXPECT_EQ(afterFirst.disk_read_bytes, before.disk_read_bytes + RSS_PAGE_SIZE);
	EXPECT_EQ(afterFirst.misses, before.misses + 1U);
	EXPECT_EQ(afterFirst.resident_bytes, before.resident_bytes + RSS_PAGE_SIZE);

	EXPECT_EQ(labelsStore.readNodeLabels(current, currentB), nodeBLabels);
	const BufferPoolStats afterSecond = io->bufferPoolStats();
	EXPECT_EQ(afterSecond.disk_read_bytes, afterFirst.disk_read_bytes);
	EXPECT_EQ(afterSecond.misses, afterFirst.misses);
	EXPECT_EQ(afterSecond.resident_bytes, afterFirst.resident_bytes);
	EXPECT_GT(afterSecond.hits, afterFirst.hits);

	EXPECT_EQ(labelsStore.readNodeLabels(current, currentA), nodeALabels);
	const BufferPoolStats afterRepeat = io->bufferPoolStats();
	EXPECT_EQ(afterRepeat.disk_read_bytes, afterSecond.disk_read_bytes);
	EXPECT_EQ(afterRepeat.misses, afterSecond.misses);
	EXPECT_EQ(afterRepeat.resident_bytes, afterSecond.resident_bytes);
	EXPECT_GT(afterRepeat.hits, afterSecond.hits);
}

TEST_F(RelocatableEntityLabelSetStoreTest, ReusesDeadSlotWithoutChargingAnotherDirectoryEntry) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, node(7U), LogSequenceNumber(2U)).has_value());
	SlottedPagePool pages(io, *metadata);
	SlottedPagePoolAppendRequest labelPageRequest;
	labelPageRequest.segment_id = SegmentId(31U);
	labelPageRequest.generation = Generation(1U);
	labelPageRequest.page_id = PageId(102U);
	labelPageRequest.page_owner_id = 901U;
	labelPageRequest.page_owner_kind = PageOwnerKind::POK_ENTITY_LABEL;
	static_cast<void>(pages.appendEmptyPage(labelPageRequest, LogSequenceNumber(3U)));

	MetadataSnapshotPin seedSnapshot = metadata->openSnapshot();
	SlottedPage seed = pages.read(seedSnapshot, PageId(102U));
	const std::vector<std::byte> emptyValue;
	const SlottedSlotReference deadSlot = seed.insert(emptyValue);
	seed.erase(deadSlot);
	EXPECT_EQ(seed.stats().reusable_free_slot_count, 1U);
	seedSnapshot.release();
	static_cast<void>(pages.replacePage(seed, LogSequenceNumber(4U)));

	EntityLabelSetStore labelsStore(io, *metadata, pages);
	MetadataSnapshotPin writeSnapshot = metadata->openSnapshot();
	EntityLabelSetWritePlan plan;
	plan.candidate_pool_segments = {SegmentId(31U)};
	const std::vector<uint64_t> maximum = labels(7U, RSS_ENTITY_MAX_LABELS);
	const EntityLabelSetUpdateResult updated =
			labelsStore.updateNodeLabels(writeSnapshot, 1001U, maximum, plan, LogSequenceNumber(5U));
	writeSnapshot.release();

	EXPECT_EQ(updated.current_overflow.page_id, PageId(102U));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(labelsStore.readNodeLabels(current, entities.readNode(current, 1001U)), maximum);
	current.release();
}

TEST_F(RelocatableEntityLabelSetStoreTest,
	   RelocatesReusedLabelSlotWhileOldSnapshotKeepsItsOriginalGenerationReadable) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, node(7U), LogSequenceNumber(2U)).has_value());
	MetadataSnapshotPin insertSnapshot = metadata->openSnapshot();
	static_cast<void>(entities.insertNode(insertSnapshot, PageId(101U), 1002U, node(1001U), LogSequenceNumber(3U)));
	insertSnapshot.release();
	MetadataSnapshotPin secondInsertSnapshot = metadata->openSnapshot();
	static_cast<void>(
			entities.insertNode(secondInsertSnapshot, PageId(101U), 1003U, node(2001U), LogSequenceNumber(4U)));
	secondInsertSnapshot.release();
	// The destination is a live pool, so relocation never depends on an orphaned frame.
	ASSERT_TRUE(
			entities.allocateNode(nodePool(32U, 901U, 201U, 41U), 1004U, node(3001U), LogSequenceNumber(5U)).has_value());
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore labelsStore(io, *metadata, pages);

	const std::vector<uint64_t> firstLabels = labels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);
	MetadataSnapshotPin firstWrite = metadata->openSnapshot();
	const EntityLabelSetUpdateResult first =
			labelsStore.updateNodeLabels(firstWrite, 1001U, firstLabels, appendTo(31U, 102U), LogSequenceNumber(6U));
	firstWrite.release();
	ASSERT_TRUE(first.current_overflow.isValid());
	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	EXPECT_EQ(labelsStore.readNodeLabels(oldSnapshot, entities.readNode(oldSnapshot, 1001U)), firstLabels);

	MetadataSnapshotPin sharedWrite = metadata->openSnapshot();
	EntityLabelSetWritePlan existingOnly;
	existingOnly.candidate_pool_segments = {SegmentId(31U)};
	const std::vector<uint64_t> sharedLabels = labels(1001U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);
	const EntityLabelSetUpdateResult shared = labelsStore.updateNodeLabels(
			sharedWrite, 1002U, sharedLabels, existingOnly, LogSequenceNumber(7U));
	sharedWrite.release();
	ASSERT_EQ(shared.current_overflow.page_id, first.current_overflow.page_id);
	ASSERT_NE(shared.current_overflow.slot_id, first.current_overflow.slot_id);
	MetadataSnapshotPin shrinkWrite = metadata->openSnapshot();
	static_cast<void>(labelsStore.updateNodeLabels(
			shrinkWrite, 1001U, labels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY), {}, LogSequenceNumber(8U)));
	shrinkWrite.release();
	MetadataSnapshotPin reuseWrite = metadata->openSnapshot();
	const std::vector<uint64_t> replacementLabels = labels(1001U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);
	const EntityLabelSetUpdateResult replacement = labelsStore.updateNodeLabels(
			reuseWrite, 1003U, replacementLabels, existingOnly, LogSequenceNumber(9U));
	reuseWrite.release();
	ASSERT_EQ(replacement.current_overflow.page_id, first.current_overflow.page_id);
	EXPECT_EQ(replacement.current_overflow.slot_id, first.current_overflow.slot_id);
	EXPECT_EQ(replacement.current_overflow.slot_generation, first.current_overflow.slot_generation + 1U);

	MetadataSnapshotPin relocationSnapshot = metadata->openSnapshot();
	const EntityPageMutationResult relocated = entities.evacuatePoolPage(
			relocationSnapshot, first.current_overflow.page_id, SegmentId(32U), LogSequenceNumber(10U));
	EXPECT_EQ(relocated.pool.segment_id, SegmentId(32U));
	EXPECT_EQ(metadata->findPageLocation(oldSnapshot, first.current_overflow.page_id).segment_id, SegmentId(31U));
	EXPECT_EQ(labelsStore.readNodeLabels(oldSnapshot, entities.readNode(oldSnapshot, 1001U)), firstLabels);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(current, replacement.current_overflow.page_id).segment_id, SegmentId(32U));
	EXPECT_EQ(labelsStore.readNodeLabels(current, entities.readNode(current, 1002U)), sharedLabels);
	EXPECT_EQ(labelsStore.readNodeLabels(current, entities.readNode(current, 1003U)), replacementLabels);
	expectFormatError([&] { static_cast<void>(labelsStore.read(current, first.current_overflow)); },
				  FormatError::FERR_INVALID_STATE);
	relocationSnapshot.release();
	oldSnapshot.release();
	current.release();
	entities.reclaimRetiredFrames(SegmentId(31U));

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	EntityPageStore reopenedEntities(io, *reopened);
	SlottedPagePool reopenedPages(io, *reopened);
	EntityLabelSetStore reopenedLabels(io, *reopened, reopenedPages);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopened->findPageLocation(reopenedSnapshot, replacement.current_overflow.page_id).segment_id,
			  SegmentId(32U));
	EXPECT_EQ(reopenedLabels.readNodeLabels(reopenedSnapshot, reopenedEntities.readNode(reopenedSnapshot, 1003U)),
			  replacementLabels);
	expectFormatError([&] { static_cast<void>(reopenedLabels.read(reopenedSnapshot, first.current_overflow)); },
				  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableEntityLabelSetStoreTest, UsesNewPoolFallbackAndRejectsOversizedOrPartialLabelsBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	const NodeEntityRecord initial = node(7U);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, initial, LogSequenceNumber(2U)).has_value());
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore labelsStore(io, *metadata, pages);

	MetadataSnapshotPin before = metadata->openSnapshot();
	std::vector<uint64_t> oversized = labels(7U, RSS_ENTITY_MAX_LABELS + 1U);
	expectFormatError(
			[&] {
				static_cast<void>(labelsStore.updateNodeLabels(before, 1001U, oversized, EntityLabelSetWritePlan{},
															   LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(entities.readNode(before, 1001U), initial);

	EntityLabelSetWritePlan newPoolPlan;
	newPoolPlan.new_pool = EntityLabelSetNewPoolRequest{
			SegmentId(32U), 902U, Generation(1U), PageId(201U), PhysicalSlotId(41U), PhysicalSlotId(41U)};
	const std::vector<uint64_t> overflow = labels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);
	const EntityLabelSetUpdateResult updated =
			labelsStore.updateNodeLabels(before, 1001U, overflow, newPoolPlan, LogSequenceNumber(3U));
	EXPECT_EQ(updated.current_overflow.page_id, PageId(201U));

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(current, SegmentId(32U)).kind, SegmentKind::SGK_ENTITY_PAGE_POOL);
	EXPECT_EQ(labelsStore.readNodeLabels(current, entities.readNode(current, 1001U)), overflow);
	NodeEntityRecord malformed = entities.readNode(current, 1001U);
	malformed.inline_label_count = static_cast<uint8_t>(RSS_ENTITY_INLINE_LABEL_CAPACITY - 1U);
	malformed.inline_label_ids[RSS_ENTITY_INLINE_LABEL_CAPACITY - 1U] = 0U;
	expectFormatError([&] { static_cast<void>(NodeEntityRecordCodec::encode(malformed)); },
					  FormatError::FERR_INVALID_HEADER);
	before.release();
	current.release();
}

TEST_F(RelocatableEntityLabelSetStoreTest, RecoversInterruptedAtomicPublicationWithoutExposingAnOrphanLabelPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	const NodeEntityRecord initial = node(7U);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, initial, LogSequenceNumber(2U)).has_value());
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore labelsStore(io, *metadata, pages);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	const std::vector<uint64_t> overflow = labels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);
	EXPECT_THROW(static_cast<void>(labelsStore.updateNodeLabels(snapshot, 1001U, overflow, appendTo(31U, 102U),
																LogSequenceNumber(3U), interrupted)),
				 BootstrapPublishInterrupted);
	EXPECT_EQ(entities.readNode(snapshot, 1001U), initial);
	EXPECT_FALSE(metadata->findPageLocationOptional(snapshot, PageId(102U)).has_value());
	snapshot.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	EntityPageStore reopenedEntities(io, *reopened);
	SlottedPagePool reopenedPages(io, *reopened);
	EntityLabelSetStore reopenedLabels(io, *reopened, reopenedPages);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(reopenedEntities.readNode(recovered, 1001U), initial);
	EXPECT_FALSE(reopened->findPageLocationOptional(recovered, PageId(102U)).has_value());
	const EntityLabelSetUpdateResult retried =
			reopenedLabels.updateNodeLabels(recovered, 1001U, overflow, appendTo(31U, 102U), LogSequenceNumber(3U));
	EXPECT_TRUE(retried.current_overflow.isValid());
	MetadataSnapshotPin current = reopened->openSnapshot();
	EXPECT_EQ(reopenedLabels.readNodeLabels(current, reopenedEntities.readNode(current, 1001U)), overflow);
	current.release();
	recovered.release();
}

TEST_F(RelocatableEntityLabelSetStoreTest, RejectsStaleForeignAndNoncanonicalLabelReferencesWithoutMutatingNode) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, node(7U), LogSequenceNumber(2U)).has_value());
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore labelsStore(io, *metadata, pages);
	MetadataSnapshotPin writeSnapshot = metadata->openSnapshot();
	const std::vector<uint64_t> overflow = labels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);
	static_cast<void>(
			labelsStore.updateNodeLabels(writeSnapshot, 1001U, overflow, appendTo(31U, 102U), LogSequenceNumber(3U)));
	writeSnapshot.release();

	MetadataSnapshotPin current = metadata->openSnapshot();
	const NodeEntityRecord persisted = entities.readNode(current, 1001U);
	EntityLabelOverflowReference stale = persisted.label_overflow;
	++stale.slot_generation;
	expectFormatError([&] { static_cast<void>(labelsStore.read(current, stale)); }, FormatError::FERR_INVALID_STATE);
	const EntityLabelOverflowReference foreign{PageId(101U), 0U, 1U};
	expectFormatError([&] { static_cast<void>(labelsStore.read(current, foreign)); }, FormatError::FERR_INVALID_STATE);
	const std::vector<uint64_t> unordered = {7U, 5U};
	expectFormatError(
			[&] {
				static_cast<void>(labelsStore.updateNodeLabels(current, 1001U, unordered, EntityLabelSetWritePlan{},
															   LogSequenceNumber(4U)));
			},
			FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(entities.readNode(current, 1001U), persisted);
	EXPECT_EQ(labelsStore.readNodeLabels(current, persisted), overflow);
	current.release();
}
