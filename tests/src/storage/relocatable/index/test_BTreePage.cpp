/**
 * @file test_BTreePage.cpp
 * @brief Explicit codec and corruption tests for relocatable B+Tree page records.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/index/BTreePage.hpp"

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

	BTreePageHeader leafHeader() {
		BTreePageHeader result;
		result.index_id = 7;
		result.page_id = PageId(11);
		result.generation = Generation(3);
		result.kind = BTreeNodeKind::BPN_LEAF;
		result.left_sibling = PageId(10);
		result.right_sibling = PageId(12);
		result.entry_count = 2;
		return result;
	}

} // namespace

TEST(RelocatableBTreePageCodecTest, RoundTripsLeafAndInternalPageHeaders) {
	const BTreePageHeader leaf = leafHeader();
	EXPECT_EQ(BTreePageHeaderCodec::decode(BTreePageHeaderCodec::encode(leaf)), leaf);

	BTreePageHeader internal = leaf;
	internal.page_id = PageId(20);
	internal.kind = BTreeNodeKind::BPN_INTERNAL;
	internal.level = 1;
	internal.left_sibling = {};
	internal.right_sibling = {};
	internal.leftmost_child = PageId(21);
	internal.entry_count = 1;
	EXPECT_EQ(BTreePageHeaderCodec::decode(BTreePageHeaderCodec::encode(internal)), internal);
}

TEST(RelocatableBTreePageCodecTest, RejectsMalformedPageHeaders) {
	const auto encoded = BTreePageHeaderCodec::encode(leafHeader());
	expectFormatError([&] { static_cast<void>(BTreePageHeaderCodec::decode(std::span(encoded).first(71))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> oversized(encoded.begin(), encoded.end());
	oversized.push_back(std::byte{0});
	expectFormatError([&] { static_cast<void>(BTreePageHeaderCodec::decode(oversized)); },
					  FormatError::FERR_INVALID_HEADER);
	BTreePageHeader invalid = leafHeader();
	invalid.level = 1;
	expectFormatError([&] { static_cast<void>(BTreePageHeaderCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = leafHeader();
	invalid.kind = BTreeNodeKind::BPN_INTERNAL;
	invalid.level = 1;
	expectFormatError([&] { static_cast<void>(BTreePageHeaderCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = leafHeader();
	invalid.leftmost_child = PageId(99);
	expectFormatError([&] { static_cast<void>(BTreePageHeaderCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = leafHeader();
	invalid.kind = BTreeNodeKind::BPN_INTERNAL;
	invalid.leftmost_child = PageId(99);
	expectFormatError([&] { static_cast<void>(BTreePageHeaderCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	auto corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 12, 2);
	expectFormatError([&] { static_cast<void>(BTreePageHeaderCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 68, 1);
	expectFormatError([&] { static_cast<void>(BTreePageHeaderCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 8, 99);
	expectFormatError([&] { static_cast<void>(BTreePageHeaderCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 4, 1);
	expectFormatError([&] { static_cast<void>(BTreePageHeaderCodec::decode(corrupt)); },
					  FormatError::FERR_UNSUPPORTED_VERSION);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 0, 0U);
	expectFormatError([&] { static_cast<void>(BTreePageHeaderCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_MAGIC);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 6, 0U);
	expectFormatError([&] { static_cast<void>(BTreePageHeaderCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = leafHeader();
	invalid.kind = BTreeNodeKind::BPN_INTERNAL;
	invalid.level = 1;
	invalid.leftmost_child = PageId(99);
	invalid.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
	expectFormatError([&] { static_cast<void>(BTreePageHeaderCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableBTreePageCodecTest, RoundTripsVariableKeysAndRejectsMalformedEntries) {
	const BTreePageEntry entry{{std::byte{0x00}, std::byte{0xFF}, std::byte{0x42}}, 123};
	const std::vector<std::byte> encoded = BTreePageEntryCodec::encode(entry);
	EXPECT_EQ(BTreePageEntryCodec::decode(encoded), entry);
	expectFormatError([&] { static_cast<void>(BTreePageEntryCodec::decode(std::span(encoded).first(31))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	auto corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 8, 4);
	expectFormatError([&] { static_cast<void>(BTreePageEntryCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 12, 1);
	expectFormatError([&] { static_cast<void>(BTreePageEntryCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);
	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, 24, 1);
	expectFormatError([&] { static_cast<void>(BTreePageEntryCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);
	EXPECT_EQ(BTreePageEntryCodec::decode(BTreePageEntryCodec::encode({{}, 0})), (BTreePageEntry{{}, 0}));
}

TEST(RelocatableBTreePageCodecTest, StoresOrderedEntriesInPackedPayload) {
	PageOwnerHeader owner;
	owner.page_id = PageId(41);
	owner.owner_id = 17;
	owner.kind = PageOwnerKind::POK_INDEX;
	owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	owner.generation = Generation(2);
	owner.committed_lsn = LogSequenceNumber(9);
	BTreePageHeader header;
	header.index_id = owner.owner_id;
	header.page_id = owner.page_id;
	header.generation = owner.generation;
	BTreePage page = BTreePage::create(owner, header);
	page.insert({{std::byte{'b'}}, 2});
	page.insert({{std::byte{'a'}}, 1});
	ASSERT_EQ(page.header().entry_count, 2U);
	const std::vector<BTreePageEntry> entries = page.entries();
	ASSERT_EQ(entries.size(), 2U);
	EXPECT_EQ(entries[0].key, std::vector<std::byte>({std::byte{'a'}}));
	EXPECT_EQ(entries[1].key, std::vector<std::byte>({std::byte{'b'}}));
	expectFormatError([&] { page.insert({{std::byte{'a'}}, 3}); }, FormatError::FERR_INVALID_STATE);
	page.erase(std::array{std::byte{'a'}});
	EXPECT_EQ(page.header().entry_count, 1U);
	expectFormatError([&] { page.erase(std::array{std::byte{'a'}}); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { page.erase(std::array{std::byte{'z'}}); }, FormatError::FERR_INVALID_STATE);
	BTreePage reopened = BTreePage::open(owner, page.serialize());
	EXPECT_EQ(reopened.entries(), (std::vector<BTreePageEntry>{{{std::byte{'b'}}, 2}}));

	BTreePageHeader internal = header;
	internal.page_id = PageId(42);
	internal.kind = BTreeNodeKind::BPN_INTERNAL;
	internal.level = 1;
	internal.leftmost_child = PageId(43);
	owner.page_id = internal.page_id;
	BTreePage internalPage = BTreePage::create(owner, internal);
	expectFormatError([&] { internalPage.insert({{std::byte{'a'}}, 0}); }, FormatError::FERR_INVALID_HEADER);
	internalPage.insert({{std::byte{'a'}}, PageId(44).value()});
	EXPECT_EQ(internalPage.entries().front().value_or_child, 44U);
}

TEST(RelocatableBTreePageCodecTest, BulkMaterializesOneValidatedPackedPayload) {
	PageOwnerHeader owner;
	owner.page_id = PageId(51);
	owner.owner_id = 17;
	owner.kind = PageOwnerKind::POK_INDEX;
	owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	owner.generation = Generation(2);
	owner.committed_lsn = LogSequenceNumber(9);
	BTreePageHeader header;
	header.index_id = owner.owner_id;
	header.page_id = owner.page_id;
	header.generation = owner.generation;
	const std::vector<BTreePageEntry> entries = {
			{{std::byte{'a'}}, 1U},
			{{std::byte{'b'}}, 2U},
			{{std::byte{'c'}}, 3U},
	};

	BTreePage page = BTreePage::createWithEntries(owner, header, entries);
	EXPECT_EQ(page.header().entry_count, entries.size());
	EXPECT_EQ(page.entries(), entries);
	EXPECT_EQ(BTreePage::open(owner, page.serialize()).entries(), entries);

	header.entry_count = 1U;
	expectFormatError([&] { static_cast<void>(BTreePage::createWithEntries(owner, header, entries)); },
					  FormatError::FERR_INVALID_HEADER);
	header.entry_count = 0U;
	const std::vector<BTreePageEntry> unordered = {
			{{std::byte{'b'}}, 2U},
			{{std::byte{'a'}}, 1U},
	};
	expectFormatError([&] { static_cast<void>(BTreePage::createWithEntries(owner, header, unordered)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableBTreePageCodecTest, FindsKeysAcrossPackedRestartGroups) {
	std::vector<BTreePageEntry> entries;
	entries.reserve(19U);
	for (uint8_t suffix = 0U; suffix < 19U; ++suffix) {
		std::vector<std::byte> key(48U, std::byte{'p'});
		key.push_back(static_cast<std::byte>(suffix));
		entries.push_back({std::move(key), static_cast<uint64_t>(suffix) + 100U});
	}
	const std::vector<std::byte> encoded = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, entries);
	EXPECT_EQ(PackedBTreePageCodec::entryCount(encoded), entries.size());
	for (const size_t index: {0U, 7U, 8U, 15U, 16U, 18U}) {
		const auto found = PackedBTreePageCodec::find(BTreeNodeKind::BPN_LEAF, encoded, entries[index].key);
		ASSERT_TRUE(found.has_value());
		EXPECT_EQ(*found, entries[index]);
	}
	std::vector<std::byte> missing(48U, std::byte{'p'});
	missing.push_back(std::byte{19});
	EXPECT_FALSE(PackedBTreePageCodec::find(BTreeNodeKind::BPN_LEAF, encoded, missing).has_value());
}

TEST(RelocatableBTreePageCodecTest, StoresKeyOnlyLeafEntriesWithoutValueBytes) {
	const std::vector<BTreePageEntry> entries = {
			{{std::byte{'a'}}, 0U},
			{{std::byte{'b'}}, 0U},
			{{std::byte{'c'}}, 0U},
	};
	const std::vector<std::byte> keyOnly =
			PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, entries, BTreeLeafValueLayout::BLVL_EMPTY);
	const std::vector<std::byte> valued = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, entries);
	EXPECT_LT(keyOnly.size(), valued.size());
	EXPECT_EQ(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, keyOnly, BTreeLeafValueLayout::BLVL_EMPTY),
			  entries);
	const auto found = PackedBTreePageCodec::find(BTreeNodeKind::BPN_LEAF, keyOnly, entries[1].key,
												  BTreeLeafValueLayout::BLVL_EMPTY);
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(*found, entries[1]);
	expectFormatError(
			[&] {
				static_cast<void>(PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF,
															   std::vector<BTreePageEntry>{{{std::byte{'a'}}, 1U}},
															   BTreeLeafValueLayout::BLVL_EMPTY));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableBTreePageCodecTest, RejectsCorruptPackedPayloadMetadataAndStreams) {
	const std::vector<BTreePageEntry> entries = {
			{{std::byte{'a'}}, 1U},
			{{std::byte{'b'}}, 2U},
	};
	const std::vector<std::byte> encoded = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, entries);

	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, {})); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	auto corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 0U, 0U);
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_MAGIC);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 4U, 0U);
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_UNSUPPORTED_VERSION);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 6U, 0U);
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 10U, 1U);
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_RESERVED_BITS);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 8U, 0U);
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 16U, 2U);
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 20U, LittleEndianCodec::readU32(corrupt, 20U) + 1U);
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	const uint32_t streamBytes = LittleEndianCodec::readU32(corrupt, 20U);
	LittleEndianCodec::writeU32(corrupt, 32U, streamBytes);
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 32U, 1U);
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	const size_t streamOffset = 32U + sizeof(uint32_t);
	corrupt = encoded;
	corrupt[streamOffset] = std::byte{1U};
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	corrupt.push_back(std::byte{0U});
	LittleEndianCodec::writeU32(corrupt, 20U, LittleEndianCodec::readU32(encoded, 20U) + 1U);
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableBTreePageCodecTest, RejectsInvalidPackedLayoutsAndTruncatedEntryValues) {
	const std::vector<BTreePageEntry> unordered = {
			{{std::byte{'b'}}, 1U},
			{{std::byte{'a'}}, 2U},
	};
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, unordered)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(PackedBTreePageCodec::encode(BTreeNodeKind::BPN_INTERNAL,
															   std::vector<BTreePageEntry>{{{std::byte{'a'}}, 0U}}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(PackedBTreePageCodec::encode(static_cast<BTreeNodeKind>(0xffU), {},
															   BTreeLeafValueLayout::BLVL_U64));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, {},
															   static_cast<BTreeLeafValueLayout>(0xffU)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(PackedBTreePageCodec::encode(BTreeNodeKind::BPN_INTERNAL, {},
															   BTreeLeafValueLayout::BLVL_EMPTY));
			},
			FormatError::FERR_INVALID_HEADER);

	const std::vector<BTreePageEntry> oneEntry = {{{std::byte{'a'}}, 300U}};
	auto truncated = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, oneEntry);
	truncated.pop_back();
	LittleEndianCodec::writeU32(truncated, 20U, LittleEndianCodec::readU32(truncated, 20U) - 1U);
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, truncated)); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
}

TEST(RelocatableBTreePageCodecTest, RejectsPackedEntryStreamsThatDisagreeWithTheirMetadata) {
	std::vector<BTreePageEntry> restartEntries;
	for (uint8_t suffix = 0U; suffix < RSS_BTREE_PACKED_RESTART_INTERVAL + 1U; ++suffix) {
		restartEntries.push_back(
				{{std::byte{'k'}, static_cast<std::byte>(suffix)}, static_cast<uint64_t>(suffix) + 1U});
	}
	auto corrupt = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, restartEntries);
	LittleEndianCodec::writeU32(corrupt, 36U, 1U);
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, restartEntries);
	LittleEndianCodec::writeU32(corrupt, 36U, 0U);
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	const std::vector<BTreePageEntry> entries = {
			{{std::byte{'a'}}, 1U},
			{{std::byte{'b'}}, 2U},
	};
	corrupt = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, entries);
	LittleEndianCodec::writeU32(corrupt, 12U, 3U);
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	corrupt = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, entries);
	constexpr size_t streamOffset = 32U + sizeof(uint32_t);
	corrupt[streamOffset + 6U] = std::byte{'a'};
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, entries);
	corrupt[streamOffset + 4U] = std::byte{2U};
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	const std::vector<BTreePageEntry> internalEntries = {{{std::byte{'a'}}, PageId(1U).value()}};
	const std::vector<std::byte> validInternal =
			PackedBTreePageCodec::encode(BTreeNodeKind::BPN_INTERNAL, internalEntries);
	EXPECT_EQ(PackedBTreePageCodec::find(BTreeNodeKind::BPN_INTERNAL, validInternal, internalEntries.front().key),
			  std::optional<BTreePageEntry>(internalEntries.front()));
	corrupt = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_INTERNAL, internalEntries);
	corrupt.back() = std::byte{0U};
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_INTERNAL, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						PackedBTreePageCodec::find(BTreeNodeKind::BPN_INTERNAL, corrupt, internalEntries.front().key));
			},
			FormatError::FERR_INVALID_HEADER);

	corrupt = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, entries);
	LittleEndianCodec::writeU32(corrupt, 12U, 3U);
	const std::array<std::byte, 1U> missingKey = {std::byte{'z'}};
	expectFormatError(
			[&] { static_cast<void>(PackedBTreePageCodec::find(BTreeNodeKind::BPN_LEAF, corrupt, missingKey)); },
			FormatError::FERR_BUFFER_TOO_SMALL);

	const std::vector<std::byte> empty = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, {});
	EXPECT_TRUE(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, empty).empty());
	EXPECT_FALSE(PackedBTreePageCodec::find(BTreeNodeKind::BPN_LEAF, empty, missingKey).has_value());
	corrupt = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, entries);
	LittleEndianCodec::writeU16(corrupt, 8U, 1U);
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	const std::vector<BTreePageEntry> oneEntry = {{{std::byte{'a'}}, 1U}};
	corrupt = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, oneEntry);
	corrupt[streamOffset + 1U] = std::byte{10U};
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, oneEntry);
	constexpr size_t overflowingVarintBytes = 10U;
	corrupt.resize(streamOffset + overflowingVarintBytes, std::byte{0x80U});
	LittleEndianCodec::writeU32(corrupt, 20U, overflowingVarintBytes);
	for (size_t index = 0U; index < overflowingVarintBytes - 1U; ++index) {
		corrupt[streamOffset + index] = std::byte{0x80U};
	}
	corrupt.back() = std::byte{0xfeU};
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);

	const std::vector<BTreePageEntry> prefixEntries = {
			{{std::byte{'a'}}, 1U},
			{{std::byte{'a'}, std::byte{'b'}}, 2U},
	};
	EXPECT_EQ(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF,
										   PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, prefixEntries)),
			  prefixEntries);
	corrupt = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, prefixEntries);
	constexpr size_t firstEntryBytes = 4U;
	constexpr size_t suffixVarintOffset = streamOffset + firstEntryBytes + 1U;
	constexpr size_t maximumVarintBytes = 10U;
	corrupt.resize(suffixVarintOffset + maximumVarintBytes, std::byte{0U});
	LittleEndianCodec::writeU32(corrupt, 20U, static_cast<uint32_t>(corrupt.size() - streamOffset));
	for (size_t index = 0U; index + 1U < maximumVarintBytes; ++index) {
		corrupt[suffixVarintOffset + index] = std::byte{0xffU};
	}
	corrupt.back() = std::byte{0x01U};
	expectFormatError([&] { static_cast<void>(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF, corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableBTreePageCodecTest, RebindsTypedOwnerWithoutChangingTheLogicalTreePage) {
	PageOwnerHeader sourceOwner;
	sourceOwner.page_id = PageId(41);
	sourceOwner.owner_id = 17;
	sourceOwner.kind = PageOwnerKind::POK_INDEX;
	sourceOwner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	sourceOwner.generation = Generation(2);
	sourceOwner.committed_lsn = LogSequenceNumber(9);
	BTreePageHeader header;
	header.index_id = sourceOwner.owner_id;
	header.page_id = sourceOwner.page_id;
	header.generation = sourceOwner.generation;
	header.left_sibling = PageId(40);
	header.right_sibling = PageId(42);
	BTreePage source = BTreePage::create(sourceOwner, header);
	source.insert({{std::byte{'b'}}, 2});
	source.insert({{std::byte{'a'}}, 1});

	PageOwnerHeader destinationOwner = sourceOwner;
	destinationOwner.generation = Generation(3);
	destinationOwner.committed_lsn = LogSequenceNumber(10);
	const BTreePage relocated = source.rebindOwner(destinationOwner);
	EXPECT_EQ(relocated.header().page_id, sourceOwner.page_id);
	EXPECT_EQ(relocated.header().index_id, sourceOwner.owner_id);
	EXPECT_EQ(relocated.header().generation, destinationOwner.generation);
	EXPECT_EQ(relocated.header().left_sibling, header.left_sibling);
	EXPECT_EQ(relocated.header().right_sibling, header.right_sibling);
	EXPECT_EQ(relocated.entries(), source.entries());
	EXPECT_EQ(relocated.slottedPage().pageGeneration(), destinationOwner.generation);
	EXPECT_EQ(relocated.slottedPage().ownerCommittedLsn(), destinationOwner.committed_lsn);

	PageOwnerHeader invalidOwner = destinationOwner;
	invalidOwner.owner_id = 18;
	expectFormatError([&] { static_cast<void>(source.rebindOwner(invalidOwner)); }, FormatError::FERR_INVALID_STATE);
	invalidOwner = destinationOwner;
	invalidOwner.generation = {};
	expectFormatError([&] { static_cast<void>(source.rebindOwner(invalidOwner)); }, FormatError::FERR_INVALID_STATE);
	invalidOwner = destinationOwner;
	invalidOwner.committed_lsn = {};
	expectFormatError([&] { static_cast<void>(source.rebindOwner(invalidOwner)); }, FormatError::FERR_INVALID_STATE);
}

TEST(RelocatableBTreePageCodecTest, RejectsAReusedPackedPayloadSlotGeneration) {
	PageOwnerHeader owner;
	owner.page_id = PageId(61U);
	owner.owner_id = 17U;
	owner.kind = PageOwnerKind::POK_INDEX;
	owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	owner.generation = Generation(2U);
	owner.committed_lsn = LogSequenceNumber(10U);
	BTreePageHeader header;
	header.index_id = owner.owner_id;
	header.page_id = owner.page_id;
	header.generation = owner.generation;

	SlottedPage malformed = SlottedPage::create(owner);
	static_cast<void>(malformed.insert(BTreePageHeaderCodec::encode(header)));
	const std::vector<std::byte> emptyPayload = PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, {});
	const SlottedSlotReference originalPayload = malformed.insert(emptyPayload);
	ASSERT_EQ(originalPayload.slot_id, 1U);
	ASSERT_EQ(originalPayload.generation, 1U);
	malformed.erase(originalPayload);
	const SlottedSlotReference reusedPayload = malformed.insert(emptyPayload);
	ASSERT_EQ(reusedPayload.slot_id, originalPayload.slot_id);
	ASSERT_EQ(reusedPayload.generation, 2U);

	expectFormatError([&] { static_cast<void>(BTreePage::open(owner, malformed.serialize())); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableBTreePageCodecTest, FailsClosedForPageOwnershipAndSlottedRecordInvariants) {
	PageOwnerHeader owner;
	owner.page_id = PageId(51);
	owner.owner_id = 18;
	owner.kind = PageOwnerKind::POK_INDEX;
	owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	owner.generation = Generation(2);
	owner.committed_lsn = LogSequenceNumber(10);
	BTreePageHeader header;
	header.index_id = owner.owner_id;
	header.page_id = owner.page_id;
	header.generation = owner.generation;
	PageOwnerHeader wrongOwner = owner;
	wrongOwner.owner_id = 19;
	expectFormatError([&] { static_cast<void>(BTreePage::create(wrongOwner, header)); },
					  FormatError::FERR_INVALID_STATE);
	header.entry_count = 1;
	expectFormatError([&] { static_cast<void>(BTreePage::create(owner, header)); }, FormatError::FERR_INVALID_HEADER);
	header.entry_count = 0;

	SlottedPage mismatchedCount = SlottedPage::create(owner);
	header.entry_count = 1;
	static_cast<void>(mismatchedCount.insert(BTreePageHeaderCodec::encode(header)));
	expectFormatError([&] { static_cast<void>(BTreePage::open(owner, mismatchedCount.serialize())); },
					  FormatError::FERR_INVALID_HEADER);
	SlottedPage packedCountMismatch = SlottedPage::create(owner);
	static_cast<void>(packedCountMismatch.insert(BTreePageHeaderCodec::encode(header)));
	static_cast<void>(packedCountMismatch.insert(PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, {})));
	expectFormatError([&] { static_cast<void>(BTreePage::open(owner, packedCountMismatch.serialize())); },
					  FormatError::FERR_INVALID_HEADER);

	SlottedPage duplicateKey = SlottedPage::create(owner);
	header.entry_count = 2;
	static_cast<void>(duplicateKey.insert(BTreePageHeaderCodec::encode(header)));
	static_cast<void>(duplicateKey.insert(BTreePageEntryCodec::encode({{std::byte{'a'}}, 2})));
	static_cast<void>(duplicateKey.insert(BTreePageEntryCodec::encode({{std::byte{'a'}}, 1})));
	expectFormatError([&] { static_cast<void>(BTreePage::open(owner, duplicateKey.serialize())); },
					  FormatError::FERR_INVALID_HEADER);

	SlottedPage legacyEntryPayload = SlottedPage::create(owner);
	header.kind = BTreeNodeKind::BPN_INTERNAL;
	header.level = 1;
	header.leftmost_child = PageId(52);
	header.entry_count = 1;
	static_cast<void>(legacyEntryPayload.insert(BTreePageHeaderCodec::encode(header)));
	static_cast<void>(legacyEntryPayload.insert(BTreePageEntryCodec::encode({{std::byte{'a'}}, 0})));
	// Per-entry slotted records are the retired version-1 page representation,
	// not a valid packed-payload slot in the current page format.
	expectFormatError([&] { static_cast<void>(BTreePage::open(owner, legacyEntryPayload.serialize())); },
					  FormatError::FERR_INVALID_MAGIC);

	BTreePage valid = BTreePage::create(owner, BTreePageHeader{owner.owner_id, owner.page_id, owner.generation});
	BTreePage *samePage = &valid;
	valid = std::move(*samePage);
	EXPECT_EQ(valid.header().page_id, owner.page_id);
	PageOwnerHeader mismatchedOpenOwner = owner;
	mismatchedOpenOwner.owner_id = 20;
	expectFormatError([&] { static_cast<void>(BTreePage::open(mismatchedOpenOwner, valid.serialize())); },
					  FormatError::FERR_INVALID_STATE);
}
