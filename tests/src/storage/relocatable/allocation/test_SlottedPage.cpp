/**
 * @file test_SlottedPage.cpp
 * @brief Tests for relocatable slotted-page durability and stable slot references.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/allocation/SlottedPage.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	constexpr size_t PAGE_CRC_OFFSET = 52;
	constexpr size_t PAGE_RESERVED0_OFFSET = 30;
	constexpr size_t PAGE_ID_OFFSET = 8;
	constexpr size_t PAGE_GENERATION_OFFSET = 16;
	constexpr size_t PAGE_SLOT_COUNT_OFFSET = 24;
	constexpr size_t PAGE_LIVE_SLOT_COUNT_OFFSET = 26;
	constexpr size_t PAGE_FREE_SLOT_HINT_OFFSET = 28;
	constexpr size_t PAGE_FREE_START_OFFSET = 32;
	constexpr size_t PAGE_FREE_END_OFFSET = 34;
	constexpr size_t PAGE_LIVE_BYTES_OFFSET = 40;
	constexpr size_t PAGE_RECLAIMABLE_BYTES_OFFSET = 44;
	constexpr size_t SLOT_CAPACITY_OFFSET = 2;
	constexpr size_t SLOT_VALUE_BYTES_OFFSET = 4;
	constexpr size_t SLOT_FLAGS_OFFSET = 6;
	constexpr size_t SLOT_GENERATION_OFFSET = 8;
	constexpr size_t SLOT_RESERVED_OFFSET = 12;

	std::vector<std::byte> bytes(std::string_view value) {
		std::vector<std::byte> result;
		result.reserve(value.size());
		for (const char character: value) {
			result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
		}
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

	void reseal(std::vector<std::byte> &page) {
		LittleEndianCodec::writeU32(page, PAGE_CRC_OFFSET, 0);
		LittleEndianCodec::writeU32(page, PAGE_CRC_OFFSET, graph::utils::calculateCrc(page.data(), page.size()));
	}

	void expectValue(SlottedPage &page, const SlottedSlotReference &reference, std::string_view expected) {
		EXPECT_EQ(page.read(reference), bytes(expected));
	}

} // namespace

TEST(RelocatableSlottedPageTest, CreatesRoundTripsAndReportsEmptyStats) {
	SlottedPage page = SlottedPage::create(PageId(17), Generation(3));
	EXPECT_EQ(page.pageId(), PageId(17));
	EXPECT_EQ(page.pageGeneration(), Generation(3));
	EXPECT_EQ(page.stats().slot_count, 0U);
	EXPECT_EQ(page.stats().live_slot_count, 0U);
	EXPECT_EQ(page.stats().live_bytes, 0U);
	EXPECT_EQ(page.stats().reclaimable_bytes, 0U);
	EXPECT_EQ(page.stats().contiguous_free_bytes, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES);

	SlottedPage reopened = SlottedPage::open(page.serialize());
	EXPECT_EQ(reopened.pageId(), PageId(17));
	EXPECT_EQ(reopened.pageGeneration(), Generation(3));
	EXPECT_EQ(reopened.stats().contiguous_free_bytes, page.stats().contiguous_free_bytes);
	expectFormatError([&] { static_cast<void>(SlottedPage::create(PageId{}, Generation(1))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(SlottedPage::create(PageId(1), Generation{})); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableSlottedPageTest, DefinesThePublicSlotReferenceAndOwnerKindDomains) {
	EXPECT_FALSE(SlottedSlotReference{}.isValid());
	EXPECT_FALSE((SlottedSlotReference{PageId(1U), RSS_SLOTTED_PAGE_NO_SLOT, 1U}.isValid()));
	EXPECT_FALSE((SlottedSlotReference{PageId(1U), 0U, 0U}.isValid()));
	EXPECT_TRUE((SlottedSlotReference{PageId(1U), 0U, 1U}.isValid()));

	for (const PageOwnerKind kind: {PageOwnerKind::POK_PROPERTY, PageOwnerKind::POK_INDEX, PageOwnerKind::POK_STATE,
								   PageOwnerKind::POK_VECTOR_ADJACENCY, PageOwnerKind::POK_ENTITY_LABEL}) {
		EXPECT_TRUE(SlottedPage::supportsOwnerKind(kind));
	}
	EXPECT_FALSE(SlottedPage::supportsOwnerKind(PageOwnerKind::POK_NONE));
}

TEST(RelocatableSlottedPageTest, BindsVariablePageBytesToTheirSharedPoolOwner) {
	PageOwnerHeader owner;
	owner.page_id = PageId(18);
	owner.owner_id = 43;
	owner.kind = PageOwnerKind::POK_PROPERTY;
	owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	owner.generation = Generation(3);
	owner.committed_lsn = LogSequenceNumber(7);
	SlottedPage page = SlottedPage::create(owner);
	const SlottedSlotReference reference = page.insert(bytes("bound"));
	SlottedPage reopened = SlottedPage::open(owner, page.serialize());
	expectValue(reopened, reference, "bound");

	PageOwnerHeader wrongPage = owner;
	wrongPage.page_id = PageId(19);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(wrongPage, page.serialize())); },
					  FormatError::FERR_INVALID_STATE);
	PageOwnerHeader wrongGeneration = owner;
	wrongGeneration.generation = Generation(4);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(wrongGeneration, page.serialize())); },
					  FormatError::FERR_INVALID_STATE);
	PageOwnerHeader indexOwner = owner;
	indexOwner.kind = PageOwnerKind::POK_INDEX;
	EXPECT_EQ(SlottedPage::create(indexOwner).pageId(), indexOwner.page_id);
	PageOwnerHeader adjacencyOwner = owner;
	adjacencyOwner.kind = PageOwnerKind::POK_VECTOR_ADJACENCY;
	EXPECT_EQ(SlottedPage::create(adjacencyOwner).pageId(), adjacencyOwner.page_id);
	PageOwnerHeader labelOwner = owner;
	labelOwner.kind = PageOwnerKind::POK_ENTITY_LABEL;
	EXPECT_EQ(SlottedPage::create(labelOwner).pageId(), labelOwner.page_id);
	PageOwnerHeader rawVector = owner;
	rawVector.kind = PageOwnerKind::POK_VECTOR_RAW;
	expectFormatError([&] { static_cast<void>(SlottedPage::create(rawVector)); }, FormatError::FERR_INVALID_HEADER);
	PageOwnerHeader shortPayload = owner;
	shortPayload.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - 1U;
	expectFormatError([&] { static_cast<void>(SlottedPage::create(shortPayload)); }, FormatError::FERR_INVALID_HEADER);
	PageOwnerHeader invalidLsn = owner;
	invalidLsn.committed_lsn = LogSequenceNumber{};
	expectFormatError([&] { static_cast<void>(SlottedPage::create(invalidLsn)); }, FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableSlottedPageTest, RejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	SlottedPage page = SlottedPage::create(PageId(37), Generation(5));
	const SlottedSlotReference first = page.insert(bytes("first-value"));
	static_cast<void>(page.insert(bytes("second-value-with-more-bytes")));
	page.erase(first);
	const std::vector<std::byte> encoded = page.serialize();
	ASSERT_EQ(encoded.size(), RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);

	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto mutation = encoded;
			mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
			reseal(mutation);
			try {
				const SlottedPage decoded = SlottedPage::open(mutation);
				if (decoded.serialize() != mutation) {
					ADD_FAILURE() << "slotted-page mutation was normalized at byte " << byteOffset << ", bit "
							  << static_cast<unsigned>(bit);
					return;
				}
			} catch (const FormatException &) {
				// A checksum-valid mutation is safe only when the typed boundary rejects it.
			}
		}
	}
}

TEST(RelocatableSlottedPageTest, RebindsAnActiveOwnerWithoutChangingStableSlotReferences) {
	PageOwnerHeader source;
	source.page_id = PageId(29);
	source.owner_id = 43;
	source.kind = PageOwnerKind::POK_PROPERTY;
	source.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	source.generation = Generation(3);
	source.committed_lsn = LogSequenceNumber(7);
	SlottedPage page = SlottedPage::create(source);
	const SlottedSlotReference reference = page.insert(bytes("relocated"));

	PageOwnerHeader destination = source;
	destination.generation = Generation(4);
	destination.committed_lsn = LogSequenceNumber(8);
	page.rebindOwner(destination);
	EXPECT_EQ(page.pageId(), source.page_id);
	EXPECT_EQ(page.pageGeneration(), destination.generation);
	EXPECT_EQ(page.ownerCommittedLsn(), destination.committed_lsn);
	EXPECT_EQ(page.read(reference), bytes("relocated"));
	SlottedPage reopened = SlottedPage::open(destination, page.serialize());
	EXPECT_EQ(reopened.read(reference), bytes("relocated"));

	PageOwnerHeader retired = destination;
	retired.flags = RSS_PAGE_OWNER_FLAG_RETIRED;
	expectFormatError([&] { page.rebindOwner(retired); }, FormatError::FERR_INVALID_STATE);
	PageOwnerHeader differentPage = destination;
	differentPage.page_id = PageId(30);
	expectFormatError([&] { page.rebindOwner(differentPage); }, FormatError::FERR_INVALID_STATE);
	PageOwnerHeader differentOwner = destination;
	differentOwner.owner_id = 44;
	expectFormatError([&] { page.rebindOwner(differentOwner); }, FormatError::FERR_INVALID_STATE);
	PageOwnerHeader differentKind = destination;
	differentKind.kind = PageOwnerKind::POK_INDEX;
	expectFormatError([&] { page.rebindOwner(differentKind); }, FormatError::FERR_INVALID_STATE);
}

TEST(RelocatableSlottedPageTest, ExposesBoundOwnerAndOnlyCurrentLiveSlotReferences) {
	PageOwnerHeader owner;
	owner.page_id = PageId(34);
	owner.owner_id = 143;
	owner.kind = PageOwnerKind::POK_ENTITY_LABEL;
	owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	owner.generation = Generation(11);
	owner.committed_lsn = LogSequenceNumber(21);
	SlottedPage page = SlottedPage::create(owner);

	EXPECT_EQ(page.ownerId(), owner.owner_id);
	EXPECT_EQ(page.ownerKind(), owner.kind);
	EXPECT_EQ(page.ownerCommittedLsn(), owner.committed_lsn);
	EXPECT_TRUE(page.liveSlots().empty());

	const SlottedSlotReference first = page.insert(bytes("first"));
	const SlottedSlotReference second = page.insert(bytes("second"));
	page.erase(first);
	EXPECT_EQ(page.liveSlots(), std::vector<SlottedSlotReference>{second});

	SlottedPage reopened = SlottedPage::open(owner, page.serialize());
	EXPECT_EQ(reopened.ownerId(), owner.owner_id);
	EXPECT_EQ(reopened.ownerKind(), owner.kind);
	EXPECT_EQ(reopened.ownerCommittedLsn(), owner.committed_lsn);
	EXPECT_EQ(reopened.liveSlots(), std::vector<SlottedSlotReference>{second});

	SlottedPage *const self = &page;
	page = std::move(*self);
	EXPECT_EQ(page.liveSlots(), std::vector<SlottedSlotReference>{second});
}

TEST(RelocatableSlottedPageTest, InsertsReadsAndUsesAllOverwritePlacementStrategies) {
	SlottedPage page = SlottedPage::create(PageId(21), Generation(4));
	const SlottedSlotReference first = page.insert(bytes("first"));
	const SlottedSlotReference tail = page.insert(bytes("tail"));
	EXPECT_EQ(first.slot_id, 0U);
	EXPECT_EQ(first.generation, 1U);
	EXPECT_EQ(tail.slot_id, 1U);

	// The last payload is adjacent to the contiguous free region, so growth
	// moves only that payload and preserves its stable slot reference.
	page.overwrite(tail, bytes("tail-grows-in-place"));
	page.overwrite(tail, bytes("short"));
	page.overwrite(tail, {});
	// The first payload is not adjacent to free space, so this replacement is
	// appended elsewhere and its former allocation becomes reclaimable.
	page.overwrite(first, bytes("first-moves-to-a-new-page-local-allocation"));
	expectValue(page, first, "first-moves-to-a-new-page-local-allocation");
	EXPECT_TRUE(page.read(tail).empty());
	EXPECT_EQ(page.stats().live_slot_count, 2U);
	EXPECT_GT(page.stats().reclaimable_bytes, 0U);

	SlottedPage reopened = SlottedPage::open(page.serialize());
	expectValue(reopened, first, "first-moves-to-a-new-page-local-allocation");
	EXPECT_TRUE(reopened.read(tail).empty());
}

TEST(RelocatableSlottedPageTest, DeletesReusesSlotAndRejectsStaleReferences) {
	SlottedPage page = SlottedPage::create(PageId(22), Generation(4));
	const SlottedSlotReference first = page.insert(bytes("dead"));
	const SlottedSlotReference second = page.insert(bytes("live"));
	page.erase(first);
	EXPECT_EQ(page.stats().live_slot_count, 1U);
	EXPECT_GT(page.stats().reclaimable_bytes, 0U);
	expectFormatError([&] { static_cast<void>(page.read(first)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { page.erase(first); }, FormatError::FERR_INVALID_STATE);

	const SlottedSlotReference reused = page.insert(bytes("replacement"));
	EXPECT_EQ(reused.slot_id, first.slot_id);
	EXPECT_EQ(reused.generation, first.generation + 1U);
	expectValue(page, reused, "replacement");
	expectValue(page, second, "live");
	expectFormatError([&] { page.overwrite(first, bytes("must-not-write")); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(page.read({PageId(999), reused.slot_id, reused.generation})); },
					  FormatError::FERR_INVALID_STATE);
}

TEST(RelocatableSlottedPageTest, CompactsFragmentationWithoutChangingLiveReferences) {
	SlottedPage page = SlottedPage::create(PageId(23), Generation(5));
	const std::vector<std::byte> firstValue(1000, std::byte{0x11});
	const std::vector<std::byte> secondValue(1000, std::byte{0x22});
	const std::vector<std::byte> thirdValue(1000, std::byte{0x33});
	const SlottedSlotReference first = page.insert(firstValue);
	const SlottedSlotReference second = page.insert(secondValue);
	const SlottedSlotReference third = page.insert(thirdValue);
	page.erase(second);
	EXPECT_LT(page.stats().contiguous_free_bytes, 1500U);
	EXPECT_GE(page.stats().contiguous_free_bytes + page.stats().reclaimable_bytes, 1500U);

	const std::vector<std::byte> replacement(1500, std::byte{0x44});
	const SlottedSlotReference reused = page.insert(replacement);
	EXPECT_EQ(reused.slot_id, second.slot_id);
	EXPECT_EQ(page.read(first), firstValue);
	EXPECT_EQ(page.read(third), thirdValue);
	EXPECT_EQ(page.read(reused), replacement);
	EXPECT_EQ(page.stats().reclaimable_bytes, 0U);

	page.compact();
	EXPECT_EQ(page.stats().reclaimable_bytes, 0U);
	EXPECT_EQ(page.read(first), firstValue);
	EXPECT_EQ(page.read(third), thirdValue);
	EXPECT_EQ(page.read(reused), replacement);
}

TEST(RelocatableSlottedPageTest, CompactsForLargeReplacementAndRejectsFullPageWrites) {
	SlottedPage page = SlottedPage::create(PageId(24), Generation(5));
	const SlottedSlotReference first = page.insert(std::vector<std::byte>(1200, std::byte{0x11}));
	const SlottedSlotReference middle = page.insert(std::vector<std::byte>(1200, std::byte{0x22}));
	const SlottedSlotReference tail = page.insert(std::vector<std::byte>(600, std::byte{0x33}));
	page.erase(middle);
	const std::vector<std::byte> larger(1800, std::byte{0x55});
	page.overwrite(first, larger);
	EXPECT_EQ(page.read(first), larger);
	EXPECT_EQ(page.read(tail), std::vector<std::byte>(600, std::byte{0x33}));
	expectFormatError([&] { static_cast<void>(page.read(middle)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(page.insert(std::vector<std::byte>(4000, std::byte{0x77}))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST(RelocatableSlottedPageTest, RejectsTruncationChecksumAndValidatedCorruption) {
	SlottedPage page = SlottedPage::create(PageId(25), Generation(6));
	const SlottedSlotReference reference = page.insert(bytes("payload"));
	const std::vector<std::byte> clean = page.serialize();
	std::vector<std::byte> truncated(clean.begin(), clean.end() - 1U);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(truncated)); }, FormatError::FERR_BUFFER_TOO_SMALL);

	std::vector<std::byte> checksumCorrupt = clean;
	checksumCorrupt.back() ^= std::byte{0x80};
	expectFormatError([&] { static_cast<void>(SlottedPage::open(checksumCorrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	std::vector<std::byte> reservedCorrupt = clean;
	LittleEndianCodec::writeU16(reservedCorrupt, PAGE_RESERVED0_OFFSET, 1);
	reseal(reservedCorrupt);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(reservedCorrupt)); }, FormatError::FERR_RESERVED_BITS);

	std::vector<std::byte> rangeCorrupt = clean;
	const size_t slotOffset = RSS_SLOTTED_PAGE_HEADER_BYTES + reference.slot_id * RSS_SLOTTED_PAGE_SLOT_BYTES;
	LittleEndianCodec::writeU16(rangeCorrupt, slotOffset + SLOT_CAPACITY_OFFSET, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
	reseal(rangeCorrupt);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(rangeCorrupt)); }, FormatError::FERR_INVALID_HEADER);

	std::vector<std::byte> flagsCorrupt = clean;
	LittleEndianCodec::writeU16(flagsCorrupt, slotOffset + SLOT_FLAGS_OFFSET, 2U);
	reseal(flagsCorrupt);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(flagsCorrupt)); }, FormatError::FERR_RESERVED_BITS);

	std::vector<std::byte> freeEndCorrupt = clean;
	LittleEndianCodec::writeU16(freeEndCorrupt, PAGE_FREE_END_OFFSET, RSS_SLOTTED_PAGE_HEADER_BYTES - 1U);
	reseal(freeEndCorrupt);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(freeEndCorrupt)); }, FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableSlottedPageTest, RetiresGenerationExhaustedSlotAndAllocatesANewDirectorySlot) {
	SlottedPage page = SlottedPage::create(PageId(26), Generation(6));
	const SlottedSlotReference reference = page.insert(bytes("generation"));
	page.erase(reference);
	std::vector<std::byte> encoded = page.serialize();
	const size_t slotOffset = RSS_SLOTTED_PAGE_HEADER_BYTES + reference.slot_id * RSS_SLOTTED_PAGE_SLOT_BYTES;
	LittleEndianCodec::writeU32(encoded, slotOffset + SLOT_GENERATION_OFFSET, UINT32_MAX);
	reseal(encoded);
	SlottedPage exhausted = SlottedPage::open(encoded);

	// The exhausted slot remains permanently dead. A different directory slot
	// starts at generation one, so no stale reference can become live again.
	const SlottedSlotReference replacement = exhausted.insert(bytes("new-slot"));
	EXPECT_NE(replacement.slot_id, reference.slot_id);
	EXPECT_EQ(replacement.generation, 1U);
	EXPECT_EQ(exhausted.stats().slot_count, 2U);
	EXPECT_EQ(exhausted.stats().reusable_free_slot_count, 0U);
	expectFormatError([&] { static_cast<void>(exhausted.read(reference)); }, FormatError::FERR_INVALID_STATE);
	expectValue(exhausted, replacement, "new-slot");

	SlottedPage reopened = SlottedPage::open(exhausted.serialize());
	expectFormatError([&] { static_cast<void>(reopened.read(reference)); }, FormatError::FERR_INVALID_STATE);
	expectValue(reopened, replacement, "new-slot");
}

TEST(RelocatableSlottedPageTest, RejectsEveryHeaderAndSlotIntegrityViolationBeforeExposingValues) {
	SlottedPage empty = SlottedPage::create(PageId(27), Generation(7));
	const std::vector<std::byte> emptyBytes = empty.serialize();
	const auto expectInvalidHeader = [](std::vector<std::byte> encoded) {
		reseal(encoded);
		expectFormatError([&] { static_cast<void>(SlottedPage::open(encoded)); }, FormatError::FERR_INVALID_HEADER);
	};

	std::vector<std::byte> invalidPageId = emptyBytes;
	LittleEndianCodec::writeU64(invalidPageId, PAGE_ID_OFFSET, 0);
	expectInvalidHeader(std::move(invalidPageId));
	std::vector<std::byte> invalidGeneration = emptyBytes;
	LittleEndianCodec::writeU64(invalidGeneration, PAGE_GENERATION_OFFSET, 0);
	expectInvalidHeader(std::move(invalidGeneration));
	std::vector<std::byte> excessiveSlots = emptyBytes;
	LittleEndianCodec::writeU16(excessiveSlots, PAGE_SLOT_COUNT_OFFSET, 249);
	expectInvalidHeader(std::move(excessiveSlots));
	std::vector<std::byte> excessiveLiveSlots = emptyBytes;
	LittleEndianCodec::writeU16(excessiveLiveSlots, PAGE_LIVE_SLOT_COUNT_OFFSET, 1);
	expectInvalidHeader(std::move(excessiveLiveSlots));
	std::vector<std::byte> badFreeStart = emptyBytes;
	LittleEndianCodec::writeU16(badFreeStart, PAGE_FREE_START_OFFSET, RSS_SLOTTED_PAGE_HEADER_BYTES + 1U);
	expectInvalidHeader(std::move(badFreeStart));
	std::vector<std::byte> oversizedFreeEnd = emptyBytes;
	LittleEndianCodec::writeU16(oversizedFreeEnd, PAGE_FREE_END_OFFSET, UINT16_MAX);
	expectInvalidHeader(std::move(oversizedFreeEnd));

	SlottedPage populated = SlottedPage::create(PageId(28), Generation(7));
	const SlottedSlotReference first = populated.insert(bytes("first"));
	const SlottedSlotReference second = populated.insert(bytes("second"));
	const std::vector<std::byte> populatedBytes = populated.serialize();
	const size_t firstOffset = RSS_SLOTTED_PAGE_HEADER_BYTES + first.slot_id * RSS_SLOTTED_PAGE_SLOT_BYTES;
	const size_t secondOffset = RSS_SLOTTED_PAGE_HEADER_BYTES + second.slot_id * RSS_SLOTTED_PAGE_SLOT_BYTES;

	std::vector<std::byte> zeroSlotGeneration = populatedBytes;
	LittleEndianCodec::writeU32(zeroSlotGeneration, firstOffset + SLOT_GENERATION_OFFSET, 0);
	reseal(zeroSlotGeneration);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(zeroSlotGeneration)); },
					  FormatError::FERR_RESERVED_BITS);
	std::vector<std::byte> reservedSlot = populatedBytes;
	LittleEndianCodec::writeU32(reservedSlot, firstOffset + SLOT_RESERVED_OFFSET, 1);
	reseal(reservedSlot);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(reservedSlot)); }, FormatError::FERR_RESERVED_BITS);
	std::vector<std::byte> oversizedValue = populatedBytes;
	LittleEndianCodec::writeU16(oversizedValue, firstOffset + SLOT_VALUE_BYTES_OFFSET, 6);
	reseal(oversizedValue);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(oversizedValue)); }, FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> beforeFreeEnd = populatedBytes;
	const uint16_t freeEnd = LittleEndianCodec::readU16(beforeFreeEnd, PAGE_FREE_END_OFFSET);
	LittleEndianCodec::writeU16(beforeFreeEnd, firstOffset, static_cast<uint16_t>(freeEnd - 1U));
	reseal(beforeFreeEnd);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(beforeFreeEnd)); }, FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> overlapping = populatedBytes;
	LittleEndianCodec::writeU16(overlapping, secondOffset, LittleEndianCodec::readU16(overlapping, firstOffset));
	LittleEndianCodec::writeU16(overlapping, secondOffset + SLOT_CAPACITY_OFFSET, 5);
	LittleEndianCodec::writeU16(overlapping, secondOffset + SLOT_VALUE_BYTES_OFFSET, 5);
	reseal(overlapping);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(overlapping)); }, FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> wrongLiveCount = populatedBytes;
	LittleEndianCodec::writeU16(wrongLiveCount, PAGE_LIVE_SLOT_COUNT_OFFSET, 1);
	expectInvalidHeader(std::move(wrongLiveCount));
	std::vector<std::byte> wrongLiveBytes = populatedBytes;
	LittleEndianCodec::writeU32(wrongLiveBytes, PAGE_LIVE_BYTES_OFFSET, 0);
	expectInvalidHeader(std::move(wrongLiveBytes));
	std::vector<std::byte> wrongFreeHint = populatedBytes;
	LittleEndianCodec::writeU16(wrongFreeHint, PAGE_FREE_SLOT_HINT_OFFSET, 0);
	expectInvalidHeader(std::move(wrongFreeHint));

	populated.erase(first);
	SlottedPage reopenedWithFreeSlot = SlottedPage::open(populated.serialize());
	EXPECT_EQ(reopenedWithFreeSlot.read(second), bytes("second"));
	std::vector<std::byte> retainedFreePayload = populated.serialize();
	LittleEndianCodec::writeU16(retainedFreePayload, firstOffset, 1);
	reseal(retainedFreePayload);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(retainedFreePayload)); },
					  FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> retainedFreeCapacity = populated.serialize();
	LittleEndianCodec::writeU16(retainedFreeCapacity, firstOffset + SLOT_CAPACITY_OFFSET, 1);
	reseal(retainedFreeCapacity);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(retainedFreeCapacity)); },
					  FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> retainedFreeLength = populated.serialize();
	LittleEndianCodec::writeU16(retainedFreeLength, firstOffset + SLOT_VALUE_BYTES_OFFSET, 1);
	reseal(retainedFreeLength);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(retainedFreeLength)); },
					  FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> wrongReclaimable = populatedBytes;
	LittleEndianCodec::writeU32(wrongReclaimable, PAGE_RECLAIMABLE_BYTES_OFFSET, 1);
	expectInvalidHeader(std::move(wrongReclaimable));
}

TEST(RelocatableSlottedPageTest, RejectsOversizedValuesInvalidReferencesAndSupportsMoveOperations) {
	SlottedPage page = SlottedPage::create(PageId(29), Generation(8));
	const SlottedSlotReference empty = page.insert({});
	page.compact();
	EXPECT_TRUE(page.read(empty).empty());
	expectFormatError([&] { static_cast<void>(page.insert(std::vector<std::byte>(UINT16_MAX + 1ULL))); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(page.read({page.pageId(), RSS_SLOTTED_PAGE_NO_SLOT, 1})); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(page.read({page.pageId(), 3, 1})); }, FormatError::FERR_INVALID_STATE);

	SlottedPage moved(std::move(page));
	EXPECT_TRUE(moved.read(empty).empty());
	SlottedPage assigned = SlottedPage::create(PageId(30), Generation(8));
	assigned = std::move(moved);
	EXPECT_TRUE(assigned.read(empty).empty());

	std::vector<std::byte> oversized(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 1U);
	expectFormatError([&] { static_cast<void>(SlottedPage::open(oversized)); }, FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableSlottedPageTest, OpensZeroLengthPayloadsAndMixedLiveFreeSlots) {
	SlottedPage zeroFirst = SlottedPage::create(PageId(31), Generation(9));
	const SlottedSlotReference empty = zeroFirst.insert({});
	const SlottedSlotReference value = zeroFirst.insert(bytes("value"));
	SlottedPage reopenedZeroFirst = SlottedPage::open(zeroFirst.serialize());
	EXPECT_TRUE(reopenedZeroFirst.read(empty).empty());
	EXPECT_EQ(reopenedZeroFirst.read(value), bytes("value"));

	SlottedPage zeroSecond = SlottedPage::create(PageId(32), Generation(9));
	const SlottedSlotReference preceding = zeroSecond.insert(bytes("value"));
	const SlottedSlotReference trailingEmpty = zeroSecond.insert({});
	SlottedPage reopenedZeroSecond = SlottedPage::open(zeroSecond.serialize());
	EXPECT_EQ(reopenedZeroSecond.read(preceding), bytes("value"));
	EXPECT_TRUE(reopenedZeroSecond.read(trailingEmpty).empty());
}

TEST(RelocatableSlottedPageTest, MatchesReferenceModelAcrossMixedOperationsAndReopen) {
	struct ModelRecord {
		SlottedSlotReference reference;
		std::vector<std::byte> value;
	};

	SlottedPage page = SlottedPage::create(PageId(33), Generation(10));
	std::vector<ModelRecord> model;
	std::mt19937 generator(0x5A17U);
	for (uint32_t iteration = 0; iteration < 500U; ++iteration) {
		const uint32_t operation = generator() % 100U;
		if (model.empty() || operation < 45U) {
			const size_t length = generator() % 192U;
			std::vector<std::byte> value(length);
			for (std::byte &byte: value) {
				byte = static_cast<std::byte>(generator() & 0xFFU);
			}
			const std::vector<std::byte> before = page.serialize();
			try {
				model.push_back({page.insert(value), std::move(value)});
			} catch (const FormatException &error) {
				ASSERT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
				EXPECT_EQ(page.serialize(), before);
			}
		} else {
			const size_t index = generator() % model.size();
			if (operation < 78U) {
				const size_t length = generator() % 256U;
				std::vector<std::byte> value(length);
				for (std::byte &byte: value) {
					byte = static_cast<std::byte>(generator() & 0xFFU);
				}
				const std::vector<std::byte> before = page.serialize();
				try {
					page.overwrite(model[index].reference, value);
					model[index].value = std::move(value);
				} catch (const FormatException &error) {
					ASSERT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
					EXPECT_EQ(page.serialize(), before);
				}
			} else {
				page.erase(model[index].reference);
				model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
			}
		}
		if (iteration % 19U == 0U) {
			page = SlottedPage::open(page.serialize());
		}
		for (const ModelRecord &record: model) {
			EXPECT_EQ(page.read(record.reference), record.value);
		}
	}
}
