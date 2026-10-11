/**
 * @file test_EntityPage.cpp
 * @brief Native fixed Node/Edge page codec tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <functional>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/utils/ChecksumUtils.hpp"

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

	PageOwnerHeader owner(PageId pageId = PageId(41U), Generation generation = Generation(3U),
						  LogSequenceNumber lsn = LogSequenceNumber(7U)) {
		return {.page_id = pageId,
				.owner_id = 99U,
				.kind = PageOwnerKind::POK_ENTITY,
				.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				.generation = generation,
				.committed_lsn = lsn};
	}

	NodeEntityRecord node() {
		NodeEntityRecord record;
		record.inline_label_ids = {3U, 5U, 7U};
		record.inline_label_count = 3U;
		record.label_overflow = {PageId(81U), 4U, 2U};
		return record;
	}

	EdgeEntityRecord edge() {
		return {.source_node_id = 10U,
				.target_node_id = 11U,
				.type_id = 16U};
	}

	void refreshPageChecksums(std::vector<std::byte> &bytes) {
		constexpr size_t headerBytes = RSS_ENTITY_PAGE_HEADER_BYTES;
		constexpr size_t payloadCrcOffset = 40U;
		constexpr size_t headerCrcOffset = 44U;
		LittleEndianCodec::writeU32(bytes, payloadCrcOffset,
								graph::utils::calculateCrc(bytes.data() + headerBytes, bytes.size() - headerBytes));
		LittleEndianCodec::writeU32(bytes, headerCrcOffset, 0U);
	LittleEndianCodec::writeU32(bytes, headerCrcOffset, graph::utils::calculateCrc(bytes.data(), headerBytes));
	}

} // namespace

TEST(EntityLabelOverflowReferenceTest, DistinguishesTheCanonicalEmptyReferenceFromValidAndMalformedReferences) {
	const EntityLabelOverflowReference empty;
	const EntityLabelOverflowReference valid{PageId(81U), 4U, 2U};
	const EntityLabelOverflowReference missingPage{{}, 4U, 2U};
	const EntityLabelOverflowReference missingGeneration{PageId(81U), 4U, 0U};
	const EntityLabelOverflowReference pageOnly{PageId(81U), 0U, 0U};
	const EntityLabelOverflowReference slotOnly{{}, 4U, 0U};
	const EntityLabelOverflowReference generationOnly{{}, 0U, 2U};

	EXPECT_TRUE(empty.isEmpty());
	EXPECT_FALSE(pageOnly.isEmpty());
	EXPECT_FALSE(slotOnly.isEmpty());
	EXPECT_FALSE(generationOnly.isEmpty());
	EXPECT_TRUE(valid.isValid());
	EXPECT_FALSE(missingPage.isValid());
	EXPECT_FALSE(missingGeneration.isValid());
}

TEST(RelocatableEntityPageTest, NodeAndEdgeCodecsRejectNonCanonicalRecords) {
	const NodeEntityRecord inputNode = node();
	const NodeEntityRecordCodec::Bytes encodedNode = NodeEntityRecordCodec::encode(inputNode);
	EXPECT_EQ(NodeEntityRecordCodec::decode(encodedNode), inputNode);
	expectFormatError(
			[&] { static_cast<void>(NodeEntityRecordCodec::decode(std::vector<std::byte>(RSS_ENTITY_NODE_RECORD_BYTES - 1U))); },
			FormatError::FERR_BUFFER_TOO_SMALL);
	expectFormatError(
			[&] { static_cast<void>(NodeEntityRecordCodec::decode(std::vector<std::byte>(RSS_ENTITY_NODE_RECORD_BYTES + 1U))); },
			FormatError::FERR_INVALID_HEADER);

	NodeEntityRecord invalidNode = inputNode;
	invalidNode.inline_label_ids[1] = invalidNode.inline_label_ids[0];
	expectFormatError([&] { static_cast<void>(NodeEntityRecordCodec::encode(invalidNode)); },
					  FormatError::FERR_INVALID_HEADER);
	invalidNode = inputNode;
	invalidNode.inline_label_count = 2U;
	expectFormatError([&] { static_cast<void>(NodeEntityRecordCodec::encode(invalidNode)); },
					  FormatError::FERR_RESERVED_BITS);
	invalidNode = inputNode;
	invalidNode.inline_label_count = 4U;
	expectFormatError([&] { static_cast<void>(NodeEntityRecordCodec::encode(invalidNode)); },
					  FormatError::FERR_INVALID_HEADER);
	invalidNode = inputNode;
	invalidNode.inline_label_ids[0] = 0U;
	expectFormatError([&] { static_cast<void>(NodeEntityRecordCodec::encode(invalidNode)); },
					  FormatError::FERR_INVALID_HEADER);
	invalidNode = inputNode;
	invalidNode.label_overflow = {PageId(81U), 4U, 0U};
	expectFormatError([&] { static_cast<void>(NodeEntityRecordCodec::encode(invalidNode)); },
					  FormatError::FERR_INVALID_HEADER);
	invalidNode = inputNode;
	invalidNode.inline_label_count = 2U;
	invalidNode.inline_label_ids[2] = 0U;
	invalidNode.label_overflow = {PageId(81U), 4U, 2U};
	expectFormatError([&] { static_cast<void>(NodeEntityRecordCodec::encode(invalidNode)); },
					  FormatError::FERR_INVALID_HEADER);
	invalidNode = inputNode;
	invalidNode.label_overflow = {{}, 0U, 1U};
	expectFormatError([&] { static_cast<void>(NodeEntityRecordCodec::encode(invalidNode)); },
					  FormatError::FERR_INVALID_HEADER);
	auto corruptNode = encodedNode;
	corruptNode[41] = std::byte{1};
	expectFormatError([&] { static_cast<void>(NodeEntityRecordCodec::decode(corruptNode)); },
					  FormatError::FERR_RESERVED_BITS);

	const EdgeEntityRecord inputEdge = edge();
	const EdgeEntityRecordCodec::Bytes encodedEdge = EdgeEntityRecordCodec::encode(inputEdge);
	EXPECT_EQ(EdgeEntityRecordCodec::decode(encodedEdge), inputEdge);
	EdgeEntityRecord invalidEdge = inputEdge;
	invalidEdge.source_node_id = 0U;
	expectFormatError([&] { static_cast<void>(EdgeEntityRecordCodec::encode(invalidEdge)); },
					  FormatError::FERR_INVALID_HEADER);
	invalidEdge = inputEdge;
	invalidEdge.target_node_id = 0U;
	expectFormatError([&] { static_cast<void>(EdgeEntityRecordCodec::encode(invalidEdge)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(EdgeEntityRecordCodec::decode(std::vector<std::byte>(RSS_ENTITY_EDGE_RECORD_BYTES - 1U))); },
			FormatError::FERR_BUFFER_TOO_SMALL);
	expectFormatError(
			[&] { static_cast<void>(EdgeEntityRecordCodec::decode(std::vector<std::byte>(RSS_ENTITY_EDGE_RECORD_BYTES + 1U))); },
			FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableEntityPageTest, NodePageReusesSlotsWithDurableGenerationsAndSurvivesCowRebind) {
	EntityPage page = EntityPage::create(owner(), EntityPageRecordKind::EPRK_NODE);
	const EntityPageHeader initial = page.header();
	EXPECT_EQ(initial.record_bytes, RSS_ENTITY_NODE_RECORD_BYTES);
	EXPECT_EQ(initial.live_count, 0U);
	EXPECT_GT(initial.slot_capacity, 1U);
	EXPECT_EQ(initial.bitmap_bytes, (initial.slot_capacity + 7U) / 8U);
	expectFormatError([&] { static_cast<void>(page.insert(1001U, edge())); }, FormatError::FERR_INVALID_STATE);

	const uint16_t first = page.insert(1001U, node());
	EXPECT_EQ(first, 0U);
	EXPECT_EQ(page.slotGeneration(first), 1U);
	ASSERT_TRUE(page.findNode(first).has_value());
	EXPECT_EQ(*page.findNode(first, 1U), node());

	NodeEntityRecord second = node();
	second.inline_label_ids = {11U, 13U, 17U};
	const uint16_t secondSlot = page.insert(1002U, second);
	EXPECT_EQ(secondSlot, 1U);
	page.erase(first, 1U);
	EXPECT_FALSE(page.findNode(first).has_value());
	expectFormatError([&] { static_cast<void>(page.findNode(first, 1U)); }, FormatError::FERR_INVALID_STATE);
	const uint16_t reused = page.insert(1003U, node());
	EXPECT_EQ(reused, first);
	EXPECT_EQ(page.slotGeneration(reused), 2U);
	EXPECT_EQ(page.entityId(reused), 1003U);
	expectFormatError([&] { page.replace(reused, 1U, second); }, FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(*page.findNode(reused, 2U), node());

	PageOwnerHeader replacementOwner = owner(PageId(41U), Generation(4U), LogSequenceNumber(8U));
	EntityPage rebound = page.rebindOwner(replacementOwner);
	EXPECT_EQ(rebound.header().generation, Generation(4U));
	EXPECT_EQ(rebound.header().committed_lsn, LogSequenceNumber(8U));
	EXPECT_EQ(*rebound.findNode(reused, 2U), node());
	EXPECT_EQ(*rebound.findNode(secondSlot, 1U), second);

	EntityPage reopened = EntityPage::open(replacementOwner, rebound.serialize());
	EXPECT_EQ(*reopened.findNode(reused, 2U), node());
	EXPECT_EQ(*reopened.findNode(secondSlot, 1U), second);
	EXPECT_FALSE(reopened.findNode(reopened.header().slot_capacity).has_value());
}

TEST(RelocatableEntityPageTest, ReadViewAuthenticatesPinnedBytesAndDecodesOnlyRequestedSlots) {
	EntityPage page = EntityPage::create(owner(), EntityPageRecordKind::EPRK_NODE);
	const uint16_t slot = page.insert(1001U, node());
	const std::vector<std::byte> bytes = page.serialize();

	const EntityPageReadView view = EntityPageReadView::open(owner(), bytes);
	EXPECT_EQ(view.header(), page.header());
	ASSERT_TRUE(view.findNode(slot, 1U).has_value());
	EXPECT_EQ(*view.findNode(slot, 1U), node());
	EXPECT_EQ(*view.findNode(slot, 1U, 1001U), node());
	EXPECT_EQ(view.entityId(slot), 1001U);
	EXPECT_EQ(view.slotGeneration(slot), 1U);
	expectFormatError([&] { static_cast<void>(view.findNode(slot, 1U, 1002U)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(view.findNode(slot, 2U)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(view.findEdge(slot, 1U)); }, FormatError::FERR_INVALID_STATE);

	std::vector<std::byte> corrupt = bytes;
	corrupt.back() = std::byte{1};
	expectFormatError([&] { static_cast<void>(EntityPageReadView::open(owner(), corrupt)); },
				  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST(RelocatableEntityPageTest, SlotBoundaryChecksDoNotMutatePagesAndMoveOperationsPreserveRecords) {
	EntityPage page = EntityPage::create(owner(), EntityPageRecordKind::EPRK_NODE);
	const uint16_t slot = page.insert(1001U, node());
	NodeEntityRecord replacement = node();
	replacement.inline_label_ids = {11U, 13U, 17U};
	expectFormatError([&] { page.replace(slot, 2U, replacement); }, FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(*page.findNode(slot, 1U), node());
	expectFormatError([&] { page.replace(page.header().slot_capacity, replacement); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { page.erase(page.header().slot_capacity); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(page.slotGeneration(page.header().slot_capacity)); },
					  FormatError::FERR_INVALID_HEADER);
	page.erase(slot);
	EXPECT_EQ(page.entityId(slot), 0U);
	expectFormatError([&] { page.replace(slot, replacement); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { page.erase(slot, 1U); }, FormatError::FERR_INVALID_STATE);

	uint64_t nextNodeId = 1002U;
	while (page.header().live_count < page.header().slot_capacity) {
		static_cast<void>(page.insert(nextNodeId++, node()));
	}
	expectFormatError([&] { static_cast<void>(page.insert(nextNodeId, node())); }, FormatError::FERR_INVALID_STATE);
	EntityPage moved(std::move(page));
	EXPECT_TRUE(moved.findNode(0U).has_value());
	EntityPage assigned = EntityPage::create(owner(PageId(42U)), EntityPageRecordKind::EPRK_NODE);
	assigned = std::move(moved);
	EXPECT_TRUE(assigned.findNode(0U).has_value());
	EntityPage *same = &assigned;
	assigned = std::move(*same);
	EXPECT_TRUE(assigned.findNode(0U).has_value());
}

TEST(RelocatableEntityPageTest, EdgePageUsesFixedCapacityAndRejectsCrossKindOperations) {
	EntityPage page = EntityPage::create(owner(), EntityPageRecordKind::EPRK_EDGE);
	EXPECT_EQ(page.header().record_bytes, RSS_ENTITY_EDGE_RECORD_BYTES);
	EXPECT_GT(page.header().slot_capacity, 1U);
	const uint16_t first = page.insert(2001U, edge());
	EXPECT_EQ(first, 0U);
	EXPECT_EQ(*page.findEdge(first, 1U), edge());
	expectFormatError([&] { static_cast<void>(page.findEdge(first, 0U)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(page.insert(2002U, node())); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(page.findNode(first)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { page.replace(first, node()); }, FormatError::FERR_INVALID_STATE);

	EdgeEntityRecord replacement = edge();
	replacement.type_id = 101U;
	expectFormatError([&] { static_cast<void>(page.findEdge(first, 1U, 2002U)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { page.replace(first, 1U, 2002U, replacement); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { page.erase(first, 1U, 2002U); }, FormatError::FERR_INVALID_STATE);
	page.replace(first, 1U, replacement);
	EXPECT_EQ(*page.findEdge(first, 1U), replacement);
	page.erase(first, 1U);
	EXPECT_FALSE(page.findEdge(first).has_value());
	EXPECT_FALSE(page.findEdge(page.header().slot_capacity).has_value());
	expectFormatError([&] { static_cast<void>(page.findEdge(first, 1U)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { page.replace(first, replacement); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { page.erase(first); }, FormatError::FERR_INVALID_STATE);

	uint64_t nextEdgeId = 2002U;
	while (page.header().live_count < page.header().slot_capacity) {
		static_cast<void>(page.insert(nextEdgeId++, edge()));
	}
	expectFormatError([&] { static_cast<void>(page.insert(nextEdgeId, edge())); }, FormatError::FERR_INVALID_STATE);
}

TEST(RelocatableEntityPageTest, EdgeBatchInsertIsAtomicAndPreservesDurableSlotGenerations) {
	EntityPage page = EntityPage::create(owner(), EntityPageRecordKind::EPRK_EDGE);
	std::vector<IdentifiedEdgeEntityRecord> records{
			{2001U, edge()},
			{2002U, {.source_node_id = 12U, .target_node_id = 13U, .type_id = 17U}},
			{2003U, {.source_node_id = 14U, .target_node_id = 15U, .type_id = 18U}}};
	const std::vector<uint16_t> slots = page.insertEdges(records);
	ASSERT_EQ(slots.size(), records.size());
	for (size_t index = 0U; index < records.size(); ++index) {
		EXPECT_EQ(slots[index], index);
		EXPECT_EQ(page.slotGeneration(slots[index]), 1U);
		EXPECT_EQ(page.entityId(slots[index]), records[index].entity_id);
		EXPECT_EQ(*page.findEdge(slots[index], 1U), records[index].record);
	}
	const std::vector<std::byte> beforeFailure = page.serialize();
	std::vector<IdentifiedEdgeEntityRecord> invalid{
			{2004U, {.source_node_id = 16U, .target_node_id = 17U, .type_id = 19U}},
			{2005U, {.source_node_id = 0U, .target_node_id = 18U, .type_id = 20U}}};
	expectFormatError([&] { static_cast<void>(page.insertEdges(invalid)); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(page.serialize(), beforeFailure);
	const std::array<IdentifiedEdgeEntityRecord, 1U> zeroId{{{0U, edge()}}};
	expectFormatError([&] { static_cast<void>(page.insertEdges(zeroId)); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(page.serialize(), beforeFailure);
	const std::array<IdentifiedEdgeEntityRecord, 2U> duplicateIds{{{2004U, edge()}, {2004U, edge()}}};
	expectFormatError([&] { static_cast<void>(page.insertEdges(duplicateIds)); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(page.serialize(), beforeFailure);
	const std::array<IdentifiedEdgeEntityRecord, 1U> existingId{{{2001U, edge()}}};
	expectFormatError([&] { static_cast<void>(page.insertEdges(existingId)); }, FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(page.serialize(), beforeFailure);

	EntityPage reopened = EntityPage::open(owner(), page.serialize());
	for (size_t index = 0U; index < records.size(); ++index) {
		EXPECT_EQ(*reopened.findEdge(slots[index], 1U), records[index].record);
	}
}

TEST(RelocatableEntityPageTest, EdgeBatchInsertHandlesEmptyBatchesAndRejectsNodePages) {
	EntityPage edgePage = EntityPage::create(owner(), EntityPageRecordKind::EPRK_EDGE);
	const std::vector<IdentifiedEdgeEntityRecord> empty;
	EXPECT_TRUE(edgePage.insertEdges(empty).empty());
	EXPECT_EQ(edgePage.header().live_count, 0U);

	EntityPage nodePage = EntityPage::create(owner(), EntityPageRecordKind::EPRK_NODE);
	expectFormatError([&] { static_cast<void>(nodePage.insertEdges(empty)); }, FormatError::FERR_INVALID_STATE);
}

TEST(RelocatableEntityPageTest, EdgeBatchInsertSkipsLiveSlotsAndPreflightsCapacity) {
	EntityPage page = EntityPage::create(owner(), EntityPageRecordKind::EPRK_EDGE);
	ASSERT_EQ(page.insert(2001U, edge()), 0U);
	const std::array<IdentifiedEdgeEntityRecord, 2U> records{
			IdentifiedEdgeEntityRecord{2002U, edge()},
			IdentifiedEdgeEntityRecord{2003U, {.source_node_id = 12U, .target_node_id = 13U, .type_id = 17U}}};
	const std::vector<uint16_t> slots = page.insertEdges(records);
	ASSERT_EQ(slots, (std::vector<uint16_t>{1U, 2U}));

	std::vector<IdentifiedEdgeEntityRecord> overCapacity;
	overCapacity.reserve(page.header().slot_capacity);
	for (uint64_t edgeId = 3000U; edgeId < 3000U + page.header().slot_capacity; ++edgeId) {
		overCapacity.push_back({edgeId, edge()});
	}
	expectFormatError([&] { static_cast<void>(page.insertEdges(overCapacity)); }, FormatError::FERR_INVALID_STATE);
}

TEST(RelocatableEntityPageTest, HeaderAndOwnerValidationFailClosed) {
	EntityPage page = EntityPage::create(owner(), EntityPageRecordKind::EPRK_NODE);
	EntityPageHeader header = page.header();
	EXPECT_EQ(EntityPageHeaderCodec::decode(EntityPageHeaderCodec::encode(header)), header);
	expectFormatError(
			[&] { static_cast<void>(EntityPageHeaderCodec::decode(std::vector<std::byte>(RSS_ENTITY_PAGE_HEADER_BYTES - 1U))); },
			FormatError::FERR_BUFFER_TOO_SMALL);
	header.record_bytes = 1U;
	expectFormatError([&] { static_cast<void>(EntityPageHeaderCodec::encode(header)); }, FormatError::FERR_INVALID_HEADER);
	header = page.header();
	header.record_kind = static_cast<EntityPageRecordKind>(99U);
	expectFormatError([&] { static_cast<void>(EntityPageHeaderCodec::encode(header)); }, FormatError::FERR_INVALID_ENUM);
	auto corruptHeader = EntityPageHeaderCodec::encode(page.header());
	corruptHeader[10] = std::byte{1};
	expectFormatError([&] { static_cast<void>(EntityPageHeaderCodec::decode(corruptHeader)); },
					  FormatError::FERR_RESERVED_BITS);
	expectFormatError(
			[&] { static_cast<void>(EntityPageHeaderCodec::decode(std::vector<std::byte>(RSS_ENTITY_PAGE_HEADER_BYTES + 1U))); },
			FormatError::FERR_INVALID_HEADER);

	PageOwnerHeader wrongKind = owner();
	wrongKind.kind = PageOwnerKind::POK_INDEX;
	expectFormatError([&] { static_cast<void>(EntityPage::create(wrongKind, EntityPageRecordKind::EPRK_NODE)); },
					  FormatError::FERR_INVALID_STATE);
	PageOwnerHeader wrongPayload = owner();
	wrongPayload.payload_bytes -= 1U;
	expectFormatError([&] { static_cast<void>(EntityPage::create(wrongPayload, EntityPageRecordKind::EPRK_NODE)); },
					  FormatError::FERR_INVALID_STATE);
	PageOwnerHeader unknownFlags = owner();
	unknownFlags.flags = static_cast<uint16_t>(RSS_PAGE_OWNER_KNOWN_FLAGS << 1U);
	expectFormatError([&] { static_cast<void>(EntityPage::create(unknownFlags, EntityPageRecordKind::EPRK_NODE)); },
					  FormatError::FERR_INVALID_STATE);
	PageOwnerHeader retired = owner();
	retired.flags = RSS_PAGE_OWNER_FLAG_RETIRED;
	expectFormatError([&] { static_cast<void>(EntityPage::create(retired, EntityPageRecordKind::EPRK_NODE)); },
					  FormatError::FERR_INVALID_STATE);

	const std::vector<std::byte> bytes = page.serialize();
	expectFormatError([&] { static_cast<void>(EntityPage::open(owner(), std::span(bytes).first(bytes.size() - 1U))); },
			FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> oversized = bytes;
	oversized.push_back(std::byte{});
	expectFormatError([&] { static_cast<void>(EntityPage::open(owner(), oversized)); }, FormatError::FERR_INVALID_HEADER);
	PageOwnerHeader staleOwner = owner(PageId(41U), Generation(4U), LogSequenceNumber(7U));
	expectFormatError([&] { static_cast<void>(EntityPage::open(staleOwner, bytes)); }, FormatError::FERR_INVALID_STATE);
	PageOwnerHeader wrongPage = owner(PageId(42U));
	expectFormatError([&] { static_cast<void>(page.rebindOwner(wrongPage)); }, FormatError::FERR_INVALID_STATE);
	retired = owner();
	retired.flags = RSS_PAGE_OWNER_FLAG_RETIRED;
	EXPECT_NO_THROW(static_cast<void>(EntityPage::open(retired, bytes)));
}

TEST(RelocatableEntityPageTest, OpenRejectsChecksumAndLayoutCorruption) {
	EntityPage page = EntityPage::create(owner(), EntityPageRecordKind::EPRK_NODE);
	ASSERT_EQ(page.insert(1001U, node()), 0U);
	const std::vector<std::byte> bytes = page.serialize();
	auto corrupt = bytes;
	corrupt.back() = std::byte{1};
	expectFormatError([&] { static_cast<void>(EntityPage::open(owner(), corrupt)); }, FormatError::FERR_CHECKSUM_MISMATCH);
	corrupt = bytes;
	corrupt[44] = std::byte{1};
	expectFormatError([&] { static_cast<void>(EntityPage::open(owner(), corrupt)); }, FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = bytes;
	const EntityPageHeader header = page.header();
	const size_t firstGenerationOffset = RSS_ENTITY_PAGE_HEADER_BYTES + header.bitmap_bytes;
	LittleEndianCodec::writeU32(corrupt, firstGenerationOffset, 0U);
	refreshPageChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(EntityPage::open(owner(), corrupt)); }, FormatError::FERR_INVALID_HEADER);
	corrupt = bytes;
	const size_t bitmapOffset = RSS_ENTITY_PAGE_HEADER_BYTES;
	corrupt[bitmapOffset] = static_cast<std::byte>(
				std::to_integer<uint8_t>(corrupt[bitmapOffset]) | static_cast<uint8_t>(0x80U));
	refreshPageChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(EntityPage::open(owner(), corrupt)); }, FormatError::FERR_INVALID_HEADER);
	corrupt = bytes;
	LittleEndianCodec::writeU16(corrupt, 18U, 2U);
	refreshPageChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(EntityPage::open(owner(), corrupt)); }, FormatError::FERR_INVALID_HEADER);

	corrupt = bytes;
	corrupt[56] = std::byte{1};
	refreshPageChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(EntityPage::open(owner(), corrupt)); }, FormatError::FERR_RESERVED_BITS);
	corrupt = bytes;
	const size_t recordOffset = RSS_ENTITY_PAGE_HEADER_BYTES + header.bitmap_bytes +
								  static_cast<size_t>(header.slot_capacity) * sizeof(uint32_t) +
								  static_cast<size_t>(header.slot_capacity) * sizeof(uint64_t);
	corrupt[recordOffset + header.record_bytes] = std::byte{1};
	refreshPageChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(EntityPage::open(owner(), corrupt)); }, FormatError::FERR_INVALID_HEADER);
	corrupt = bytes;
	corrupt[recordOffset + 56U] = std::byte{4};
	refreshPageChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(EntityPage::open(owner(), corrupt)); }, FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableEntityPageTest, EnumeratesLiveSlotsWithPageLocalIdsAndRejectsDuplicateIdentities) {
	EntityPage nodePage = EntityPage::create(owner(), EntityPageRecordKind::EPRK_NODE);
	const uint16_t first = nodePage.insert(1003U, node());
	NodeEntityRecord secondRecord = node();
	secondRecord.inline_label_ids = {11U, 13U, 17U};
	const uint16_t second = nodePage.insert(1001U, secondRecord);
	nodePage.erase(first, 1U, 1003U);
	const uint16_t reused = nodePage.insert(1005U, node());
	ASSERT_EQ(reused, first);

	const EntityPageReadView nodeView = EntityPageReadView::open(owner(), nodePage.serialize());
	std::vector<uint64_t> nodeIds;
	EXPECT_TRUE(nodeView.visitNodesUntil(
			4U, [&](const uint64_t entityId, const uint16_t slot, const uint32_t generation,
						const NodeEntityRecord &) {
				nodeIds.push_back(entityId);
				EXPECT_EQ(nodeView.entityId(slot), entityId);
				EXPECT_EQ(nodeView.slotGeneration(slot), generation);
				return true;
			}));
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1005U, 1001U}));
	EXPECT_FALSE(nodeView.visitNodesUntil(1U, [](uint64_t, uint16_t, uint32_t, const NodeEntityRecord &) {
		return true;
	}));
	expectFormatError(
			[&] { static_cast<void>(nodeView.visitEdgesUntil(1U, [](uint64_t, uint16_t, uint32_t,
																 const EdgeEntityRecord &) { return true; })); },
			FormatError::FERR_INVALID_STATE);

	expectFormatError([&] { static_cast<void>(nodePage.insert(0U, node())); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(nodePage.insert(1001U, node())); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { nodePage.replace(second, 1U, 1005U, node()); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { nodePage.erase(second, 1U, 1005U); }, FormatError::FERR_INVALID_STATE);

	std::vector<std::byte> duplicate = nodePage.serialize();
	const EntityPageHeader header = nodePage.header();
	const size_t entityIdsOffset = RSS_ENTITY_PAGE_HEADER_BYTES + header.bitmap_bytes +
									static_cast<size_t>(header.slot_capacity) * sizeof(uint32_t);
	LittleEndianCodec::writeU64(duplicate, entityIdsOffset + static_cast<size_t>(second) * sizeof(uint64_t), 1005U);
	refreshPageChecksums(duplicate);
	expectFormatError([&] { static_cast<void>(EntityPage::open(owner(), duplicate)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableEntityPageTest, RejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	EntityPage nodePage = EntityPage::create(owner(), EntityPageRecordKind::EPRK_NODE);
	ASSERT_EQ(nodePage.insert(1001U, node()), 0U);
	EntityPage edgePage = EntityPage::create(owner(), EntityPageRecordKind::EPRK_EDGE);
	ASSERT_EQ(edgePage.insert(2001U, edge()), 0U);
	ASSERT_EQ(edgePage.insert(2002U, {.source_node_id = 12U, .target_node_id = 13U, .type_id = 17U}), 1U);
	const std::array<std::vector<std::byte>, 2> encodedPages{nodePage.serialize(), edgePage.serialize()};

	for (size_t pageIndex = 0U; pageIndex < encodedPages.size(); ++pageIndex) {
		const std::vector<std::byte> &encoded = encodedPages[pageIndex];
		for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
			for (uint8_t bit = 0U; bit < 8U; ++bit) {
				auto mutation = encoded;
				mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
				refreshPageChecksums(mutation);
				try {
					EntityPage decoded = EntityPage::open(owner(), mutation);
					if (decoded.serialize() != mutation) {
						ADD_FAILURE() << "Entity page mutation was silently normalized for page " << pageIndex
										 << " at byte " << byteOffset << ", bit " << static_cast<unsigned>(bit);
						return;
					}
				} catch (const FormatException &) {
					// Every non-canonical mutation must fail at the typed format boundary.
				}
			}
		}
	}
}

TEST(RelocatableEntityPageTest, RejectsAReusedSlotWhoseGenerationIsExhausted) {
	EntityPage empty = EntityPage::create(owner(), EntityPageRecordKind::EPRK_NODE);
	std::vector<std::byte> bytes = empty.serialize();
	const size_t firstGenerationOffset = RSS_ENTITY_PAGE_HEADER_BYTES + empty.header().bitmap_bytes;
	LittleEndianCodec::writeU32(bytes, firstGenerationOffset, std::numeric_limits<uint32_t>::max());
	refreshPageChecksums(bytes);
	EntityPage exhausted = EntityPage::open(owner(), bytes);
	expectFormatError([&] { static_cast<void>(exhausted.insert(1001U, node())); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);

	EntityPage emptyEdge = EntityPage::create(owner(), EntityPageRecordKind::EPRK_EDGE);
	bytes = emptyEdge.serialize();
	const size_t edgeGenerationOffset = RSS_ENTITY_PAGE_HEADER_BYTES + emptyEdge.header().bitmap_bytes;
	LittleEndianCodec::writeU32(bytes, edgeGenerationOffset, std::numeric_limits<uint32_t>::max());
	refreshPageChecksums(bytes);
	EntityPage exhaustedEdge = EntityPage::open(owner(), bytes);
	expectFormatError([&] { static_cast<void>(exhaustedEdge.insert(2001U, edge())); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	const std::array<IdentifiedEdgeEntityRecord, 1U> batch{IdentifiedEdgeEntityRecord{2001U, edge()}};
	expectFormatError([&] { static_cast<void>(exhaustedEdge.insertEdges(batch)); }, FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST(RelocatableEntityPageTest, NodeBatchMatchesScalarBytesAndRejectsInvalidInputAtomically) {
 auto batch = EntityPage::create(owner(), EntityPageRecordKind::EPRK_NODE);
 auto scalar = EntityPage::create(owner(), EntityPageRecordKind::EPRK_NODE);
 const auto first = scalar.insert(700U, node());
 scalar.erase(first);
 const auto batchFirst = batch.insert(700U, node());
 batch.erase(batchFirst);
 std::vector<IdentifiedNodeEntityRecord> records{{701U,node()},{702U,node()},{703U,node()}};
 const auto slots = batch.insertNodes(records);
 for (size_t i = 0; i < records.size(); ++i) {
  EXPECT_EQ(slots[i], scalar.insert(records[i].entity_id, records[i].record));
  EXPECT_EQ(batch.findNode(slots[i], batch.slotGeneration(slots[i])), records[i].record);
 }
 EXPECT_EQ(batch.slotGeneration(slots[0]), 2U);
 EXPECT_EQ(batch.serialize(), scalar.serialize());
 const auto before = batch.serialize();
 const std::array<IdentifiedNodeEntityRecord,2> duplicate{{{710U,node()},{710U,node()}}};
 EXPECT_THROW(static_cast<void>(batch.insertNodes(duplicate)), FormatException);
 EXPECT_EQ(batch.serialize(), before);
 auto invalid = records; invalid[0].entity_id = 710U; invalid[1].entity_id = 0U;
 EXPECT_THROW(static_cast<void>(batch.insertNodes(invalid)), FormatException);
 EXPECT_EQ(batch.serialize(), before);
 invalid[1].entity_id = 711U; invalid[1].record.inline_label_count = 255U;
 EXPECT_THROW(static_cast<void>(batch.insertNodes(invalid)), FormatException);
 EXPECT_EQ(batch.serialize(), before);
 std::vector<IdentifiedNodeEntityRecord> overCapacity(batch.header().slot_capacity+1U, {799U,node()});
 EXPECT_THROW(static_cast<void>(batch.insertNodes(overCapacity)), FormatException);
 EXPECT_EQ(batch.serialize(), before);
 EXPECT_TRUE(batch.insertNodes({}).empty());
 auto edges = EntityPage::create(owner(), EntityPageRecordKind::EPRK_EDGE);
 EXPECT_THROW(static_cast<void>(edges.insertNodes({})), FormatException);
}
