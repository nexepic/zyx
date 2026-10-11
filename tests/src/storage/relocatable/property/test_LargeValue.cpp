/**
 * @file test_LargeValue.cpp
 * @brief Boundary and corruption tests for native generic LargeValue codecs.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/property/LargeValue.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	constexpr size_t MANIFEST_PAYLOAD_CRC_OFFSET = 96U;
	constexpr size_t MANIFEST_HEADER_CRC_OFFSET = 100U;
	constexpr size_t REF_PAYLOAD_CRC_OFFSET = 80U;
	constexpr size_t REF_HEADER_CRC_OFFSET = 84U;
	constexpr size_t DATA_PAYLOAD_CRC_OFFSET = 60U;
	constexpr size_t DATA_HEADER_CRC_OFFSET = 64U;

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	uint32_t checksum(std::span<const std::byte> bytes) {
		const std::byte empty{};
		return graph::utils::calculateCrc(bytes.empty() ? &empty : bytes.data(), bytes.size());
	}

	std::array<std::byte, RSS_LARGE_VALUE_CONTENT_HASH_BYTES> contentHash() {
		std::array<std::byte, RSS_LARGE_VALUE_CONTENT_HASH_BYTES> hash{};
		for (size_t index = 0U; index < hash.size(); ++index) {
			hash[index] = static_cast<std::byte>(index + 1U);
		}
		return hash;
	}

	LargeValueManifest manifest() {
		LargeValueManifest result;
		result.value_id = LargeValueId(71U);
		result.page_id = PageId(101U);
		result.generation = Generation(4U);
		result.committed_lsn = LogSequenceNumber(9U);
		result.logical_bytes = RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 25U;
		result.content_hash = contentHash();
		result.kind = LargeValueKind::LVK_UTF8_TEXT;
		result.codec = LargeValueCodec::LVC_MIXED;
		result.media_type = "text/x-c++src";
		result.chunks = {
				{0U, LargeValueCodec::LVC_RAW, 0U, RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES,
				 RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 3U, 0x01020304U, 0x11121314U, PageId(201U), 2U},
				{1U, LargeValueCodec::LVC_LZ4, RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES, 25U, 17U, 0x21222324U,
				 0x31323334U, PageId(203U), 1U},
		};
		return result;
	}

	LargeValueManifest internalManifest() {
		LargeValueManifest result = manifest();
		result.node_kind = LargeValueManifestNodeKind::LVMNK_INTERNAL;
		result.node_level = 1U;
		result.chunks.clear();
		result.node_logical_bytes = result.logical_bytes;
		result.children = {
				{PageId(301U), 0U, 1U, 0U, RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES},
				{PageId(302U), 1U, 1U, RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES, 25U},
		};
		return result;
	}

	PageOwnerHeader manifestOwner(const LargeValueManifest &manifest) {
		return {manifest.page_id,
				manifest.value_id.value(),
				PageOwnerKind::POK_LARGE_VALUE_MANIFEST,
				0U,
				RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				manifest.generation,
				manifest.committed_lsn};
	}

	LargeValueDataPage firstDataPage() {
		LargeValueDataPage result;
		result.value_id = LargeValueId(71U);
		result.page_id = PageId(201U);
		result.generation = Generation(4U);
		result.chunk_ordinal = 0U;
		result.page_ordinal = 0U;
		result.page_count = 2U;
		result.stored_chunk_bytes = RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 3U;
		result.stored_offset = 0U;
		result.payload.assign(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES, std::byte{0xA5});
		return result;
	}

	LargeValueDataPage secondDataPage() {
		LargeValueDataPage result = firstDataPage();
		result.page_id = PageId(202U);
		result.page_ordinal = 1U;
		result.stored_offset = RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES;
		result.payload.assign(3U, std::byte{0x5A});
		return result;
	}

	PageOwnerHeader dataOwner(const LargeValueDataPage &page) {
		return {page.page_id,
				page.value_id.value(),
				PageOwnerKind::POK_LARGE_VALUE_DATA,
				0U,
				RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				page.generation,
				LogSequenceNumber(9U)};
	}

	void recalculateManifestChecksums(std::vector<std::byte> &bytes) {
		LittleEndianCodec::writeU32(bytes, MANIFEST_PAYLOAD_CRC_OFFSET,
									checksum(std::span(bytes).subspan(RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES)));
		LittleEndianCodec::writeU32(bytes, MANIFEST_HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, MANIFEST_HEADER_CRC_OFFSET,
									checksum(std::span(bytes).first(RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES)));
	}

	void recalculateReferenceChecksums(std::vector<std::byte> &bytes) {
		LittleEndianCodec::writeU32(bytes, REF_PAYLOAD_CRC_OFFSET,
									checksum(std::span(bytes).subspan(RSS_LARGE_VALUE_REF_HEADER_BYTES)));
		LittleEndianCodec::writeU32(bytes, REF_HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, REF_HEADER_CRC_OFFSET,
									checksum(std::span(bytes).first(RSS_LARGE_VALUE_REF_HEADER_BYTES)));
	}

	void recalculateDataChecksums(std::vector<std::byte> &bytes) {
		const size_t payloadBytes = std::min(
				static_cast<size_t>(LittleEndianCodec::readU32(bytes, 56U)),
				bytes.size() - RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES);
		LittleEndianCodec::writeU32(
				bytes, DATA_PAYLOAD_CRC_OFFSET,
				checksum(std::span(bytes).subspan(RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES, payloadBytes)));
		LittleEndianCodec::writeU32(bytes, DATA_HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, DATA_HEADER_CRC_OFFSET,
									checksum(std::span(bytes).first(RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES)));
	}

} // namespace

TEST(RelocatableLargeValueRefTest, RoundTripsLogicalIdentityAndRejectsChecksumOrMetadataCorruption) {
	const LargeValueRef expected = manifest().reference();
	const std::vector<std::byte> encoded = LargeValueRefCodec::encode(expected);
	EXPECT_EQ(LargeValueRefCodec::decode(encoded), expected);
	LargeValueRef missingMediaType = expected;
	missingMediaType.media_type.clear();
	expectFormatError([&] { static_cast<void>(LargeValueRefCodec::encode(missingMediaType)); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<std::byte> corrupt = encoded;
	corrupt.front() ^= std::byte{1};
	expectFormatError([&] { static_cast<void>(LargeValueRefCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = encoded;
	corrupt.back() ^= std::byte{1};
	expectFormatError([&] { static_cast<void>(LargeValueRefCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 12U, 99U);
	recalculateReferenceChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueRefCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	corrupt[90U] = std::byte{1};
	recalculateReferenceChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueRefCodec::decode(corrupt)); }, FormatError::FERR_RESERVED_BITS);

	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 16U, 0U);
	corrupt.resize(RSS_LARGE_VALUE_REF_HEADER_BYTES);
	recalculateReferenceChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueRefCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableLargeValueRefTest, RejectsNonCanonicalIdentityAndWellChecksummedWireCorruption) {
	const LargeValueRef expected = manifest().reference();
	const std::vector<std::byte> encoded = LargeValueRefCodec::encode(expected);

	expectFormatError([&] { static_cast<void>(LargeValueRefCodec::decode(std::span(encoded).first(1U))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);

	const auto expectInvalidReference = [&](const std::function<void(LargeValueRef &)> &mutate) {
		LargeValueRef invalid = expected;
		mutate(invalid);
		expectFormatError([&] { static_cast<void>(LargeValueRefCodec::encode(invalid)); },
						  FormatError::FERR_INVALID_HEADER);
	};
	expectInvalidReference([](LargeValueRef &reference) { reference.value_id = LargeValueId{}; });
	expectInvalidReference([](LargeValueRef &reference) { reference.generation = Generation{}; });
	expectInvalidReference([](LargeValueRef &reference) { reference.content_hash.fill(std::byte{}); });
	expectInvalidReference([](LargeValueRef &reference) { reference.kind = static_cast<LargeValueKind>(0xFFFFU); });
	expectInvalidReference([](LargeValueRef &reference) { reference.codec = static_cast<LargeValueCodec>(0xFFFFU); });
	expectInvalidReference([](LargeValueRef &reference) {
		reference.media_type.assign(RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES + 1U, 'x');
	});
	expectInvalidReference([](LargeValueRef &reference) { reference.media_type = "text/\x01plain"; });
	expectInvalidReference([](LargeValueRef &reference) { reference.media_type = "text/\x7Fplain"; });

	LargeValueRef bytesReference = expected;
	bytesReference.kind = LargeValueKind::LVK_BYTES;
	bytesReference.codec = LargeValueCodec::LVC_ZSTD;
	bytesReference.media_type = "application/octet-stream";
	EXPECT_EQ(LargeValueRefCodec::decode(LargeValueRefCodec::encode(bytesReference)), bytesReference);

	std::vector<std::byte> corrupt = encoded;
	corrupt[RSS_LARGE_VALUE_REF_HEADER_BYTES] = std::byte{0x01};
	recalculateReferenceChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueRefCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	corrupt.push_back(std::byte{});
	recalculateReferenceChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueRefCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableLargeValueRefTest, RejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	const std::vector<std::byte> encoded = LargeValueRefCodec::encode(manifest().reference());
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto mutation = encoded;
			mutation[byteOffset] ^= static_cast<std::byte>(uint8_t{1U} << bit);
			recalculateReferenceChecksums(mutation);
			try {
				const LargeValueRef decoded = LargeValueRefCodec::decode(mutation);
				EXPECT_EQ(LargeValueRefCodec::encode(decoded), mutation)
						<< "checksum-valid mutation at byte " << byteOffset << ", bit "
						<< static_cast<unsigned>(bit);
			} catch (const FormatException &) {
				// Rejected references never reach property or LargeValue readers.
			}
		}
	}
}

TEST(RelocatableLargeValueManifestPageTest, RoundTripsMixedChunksAndBindsTypedOwner) {
	const LargeValueManifest expected = manifest();
	const PageOwnerHeader owner = manifestOwner(expected);
	const std::vector<std::byte> encoded = LargeValueManifestPageCodec::encode(expected);
	EXPECT_EQ(encoded.size(), RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
	EXPECT_EQ(LargeValueManifestPageCodec::decode(encoded), expected);
	size_t admittedBytes = 0U;
	const auto decoded = LargeValueManifestPage::decodeManifest(owner, encoded,
			[&](size_t bytes) { admittedBytes = bytes; });
	EXPECT_EQ(decoded, expected);
	EXPECT_EQ(admittedBytes, decoded.media_type.capacity() +
			decoded.chunks.capacity() * sizeof(LargeValueChunk));
	EXPECT_THROW(LargeValueManifestPage::decodeManifest(owner, encoded,
			[](size_t) { throw std::length_error("manifest budget"); }), std::length_error);
	EXPECT_EQ(LargeValueManifestPage::decodeManifest(owner, encoded), expected);

	const LargeValueManifestPage page = LargeValueManifestPage::create(owner, expected);
	EXPECT_EQ(page.manifest(), expected);
	EXPECT_EQ(page.serialize(), encoded);
	EXPECT_EQ(LargeValueManifestPage::open(owner, page.serialize()).manifest(), expected);
	PageOwnerHeader retiredOwner = owner;
	retiredOwner.flags = RSS_PAGE_OWNER_FLAG_RETIRED;
	EXPECT_EQ(LargeValueManifestPage::open(retiredOwner, page.serialize()).manifest(), expected);
	expectFormatError([&] { static_cast<void>(LargeValueManifestPage::create(retiredOwner, expected)); },
					  FormatError::FERR_INVALID_STATE);
	LargeValueManifest missingMediaType = expected;
	missingMediaType.media_type.clear();
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::encode(missingMediaType)); },
					  FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> missingMediaTypeBytes = encoded;
	LittleEndianCodec::writeU16(missingMediaTypeBytes, 16U, 0U);
	std::fill_n(missingMediaTypeBytes.begin() + 128U, expected.media_type.size(), std::byte{});
	recalculateManifestChecksums(missingMediaTypeBytes);
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::decode(missingMediaTypeBytes)); },
					  FormatError::FERR_INVALID_HEADER);

	PageOwnerHeader wrongOwner = owner;
	wrongOwner.owner_id += 1U;
	expectFormatError([&] { static_cast<void>(LargeValueManifestPage::decodeManifest(wrongOwner, encoded)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(LargeValueManifestPage::open(wrongOwner, page.serialize())); },
					  FormatError::FERR_INVALID_STATE);
}

TEST(RelocatableLargeValueManifestPageTest, RoundTripsInternalAndLeafNodesWithOrderedRanges) {
	const LargeValueManifest leaf = manifest();
	LargeValueManifest leafNode = leaf;
	leafNode.page_id = PageId(301U);
	leafNode.node_kind = LargeValueManifestNodeKind::LVMNK_LEAF;
	leafNode.node_level = 0U;
	leafNode.first_chunk_ordinal = 0U;
	leafNode.first_uncompressed_offset = 0U;
	leafNode.node_logical_bytes = leaf.logical_bytes;
	const std::vector<std::byte> leafBytes = LargeValueManifestPageCodec::encode(leafNode);
	EXPECT_EQ(LargeValueManifestPageCodec::decode(leafBytes), leafNode);

	LargeValueManifest root = leaf;
	root.node_kind = LargeValueManifestNodeKind::LVMNK_ROOT;
	root.node_level = 1U;
	root.chunks.clear();
	root.children = {
			{leafNode.page_id, 0U, 2U, 0U, leafNode.logical_bytes},
			{PageId(302U), 2U, 1U, leafNode.logical_bytes, 17U},
	};
	root.logical_bytes = leafNode.logical_bytes + 17U;
	root.page_id = PageId(101U);
	const std::vector<std::byte> rootBytes = LargeValueManifestPageCodec::encode(root);
	EXPECT_EQ(LargeValueManifestPageCodec::decode(rootBytes), root);

	std::vector<std::byte> corrupt = rootBytes;
	LittleEndianCodec::writeU32(corrupt, RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES + 8U, 99U);
	recalculateManifestChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableLargeValueManifestPageTest, FailsClosedForMalformedChunksAndUnusedBytes) {
	const std::vector<std::byte> encoded = LargeValueManifestPageCodec::encode(manifest());
	std::vector<std::byte> corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES + 4U, 99U);
	recalculateManifestChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	corrupt.back() = std::byte{1};
	recalculateManifestChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);

	corrupt = encoded;
	corrupt[128U] = std::byte{0};
	LittleEndianCodec::writeU16(corrupt, 16U, 0U);
	recalculateManifestChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);

	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::decode(std::span(encoded).first(1U))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
}

TEST(RelocatableLargeValueManifestPageTest, RejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	const std::vector<std::byte> encoded = LargeValueManifestPageCodec::encode(manifest());
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto mutation = encoded;
			mutation[byteOffset] ^= static_cast<std::byte>(uint8_t{1U} << bit);
			recalculateManifestChecksums(mutation);
			try {
				const LargeValueManifest decoded = LargeValueManifestPageCodec::decode(mutation);
				EXPECT_EQ(LargeValueManifestPageCodec::encode(decoded), mutation)
						<< "checksum-valid mutation at byte " << byteOffset << ", bit "
						<< static_cast<unsigned>(bit);
			} catch (const FormatException &) {
				// All rejected mutations must stop at the typed format boundary.
			}
		}
	}
}

TEST(RelocatableLargeValueManifestPageTest, RejectsChecksummedHeaderAndLogicalShapeCorruption) {
	const LargeValueManifest expected = manifest();
	const std::vector<std::byte> encoded = LargeValueManifestPageCodec::encode(expected);

	std::vector<std::byte> corrupt = encoded;
	corrupt.push_back(std::byte{});
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 18U, 0U);
	recalculateManifestChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	corrupt[RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES] ^= std::byte{1};
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	const auto expectInvalidManifest = [&](const std::function<void(LargeValueManifest &)> &mutate) {
		LargeValueManifest invalid = expected;
		mutate(invalid);
		expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::encode(invalid)); },
						  FormatError::FERR_INVALID_HEADER);
	};
	expectInvalidManifest([](LargeValueManifest &value) { value.value_id = LargeValueId{}; });
	expectInvalidManifest([](LargeValueManifest &value) { value.page_id = PageId{}; });
	expectInvalidManifest([](LargeValueManifest &value) { value.generation = Generation{}; });
	expectInvalidManifest([](LargeValueManifest &value) { value.committed_lsn = LogSequenceNumber{}; });
	expectInvalidManifest([](LargeValueManifest &value) { value.kind = static_cast<LargeValueKind>(0xFFFFU); });
	expectInvalidManifest([](LargeValueManifest &value) { value.codec = static_cast<LargeValueCodec>(0xFFFFU); });
	expectInvalidManifest([](LargeValueManifest &value) { value.media_type = "text/\x01plain"; });
	expectInvalidManifest([](LargeValueManifest &value) { value.chunks.front().ordinal = 1U; });
	expectInvalidManifest([](LargeValueManifest &value) {
		value.chunks.resize(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS + 1U, value.chunks.front());
	});
	expectInvalidManifest([](LargeValueManifest &value) {
		value.children.resize(RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN + 1U,
							  {PageId(301U), 0U, 1U, 0U, value.logical_bytes});
	});
	expectInvalidManifest([](LargeValueManifest &value) { value.chunks.front().stored_bytes = 0U; });
	expectInvalidManifest([](LargeValueManifest &value) { value.chunks.front().uncompressed_bytes = 0U; });
	expectInvalidManifest([](LargeValueManifest &value) { value.chunks.front().first_data_page_id = PageId{}; });
	expectInvalidManifest([](LargeValueManifest &value) { value.chunks.front().data_page_count = 1U; });
	expectInvalidManifest([](LargeValueManifest &value) { value.codec = LargeValueCodec::LVC_RAW; });
	expectInvalidManifest([](LargeValueManifest &value) { value.node_level = 1U; });
	expectInvalidManifest([](LargeValueManifest &value) {
		value.chunks.clear();
		value.children = {{PageId(301U), 0U, 2U, 0U, value.logical_bytes}};
	});
	expectInvalidManifest([](LargeValueManifest &value) {
		value.node_kind = LargeValueManifestNodeKind::LVMNK_INTERNAL;
		value.node_level = 1U;
		value.chunks.clear();
	});
	expectInvalidManifest([](LargeValueManifest &value) {
		value.node_kind = LargeValueManifestNodeKind::LVMNK_INTERNAL;
		value.node_level = 0U;
		value.chunks.clear();
		value.children = {{PageId(301U), 0U, 2U, 0U, value.logical_bytes}};
	});
	expectInvalidManifest([](LargeValueManifest &value) {
		value.node_kind = LargeValueManifestNodeKind::LVMNK_INTERNAL;
		value.node_level = 1U;
		value.children = {{PageId(301U), 0U, 2U, 0U, value.logical_bytes}};
	});
	expectInvalidManifest([](LargeValueManifest &value) {
		value.node_kind = LargeValueManifestNodeKind::LVMNK_LEAF;
		value.node_level = 1U;
	});
	expectInvalidManifest([](LargeValueManifest &value) {
		value.node_kind = LargeValueManifestNodeKind::LVMNK_LEAF;
		value.node_level = 0U;
		value.children = {{PageId(301U), 0U, 2U, 0U, value.logical_bytes}};
	});
	expectInvalidManifest([](LargeValueManifest &value) { value.logical_bytes = 0U; });
	expectInvalidManifest([](LargeValueManifest &value) { value.first_uncompressed_offset = 1U; });
	expectInvalidManifest([](LargeValueManifest &value) { value.first_chunk_ordinal = 1U; });
	expectInvalidManifest([](LargeValueManifest &value) { value.node_logical_bytes = 1U; });
	expectInvalidManifest(
			[](LargeValueManifest &value) { value.node_kind = static_cast<LargeValueManifestNodeKind>(0xFFFFU); });

	const auto expectInvalidInternal = [&](const std::function<void(LargeValueManifest &)> &mutate,
										   FormatError error = FormatError::FERR_INVALID_HEADER) {
		LargeValueManifest invalid = internalManifest();
		mutate(invalid);
		expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::encode(invalid)); }, error);
	};
	const LargeValueManifest internal = internalManifest();
	EXPECT_EQ(LargeValueManifestPageCodec::decode(LargeValueManifestPageCodec::encode(internal)), internal);
	expectInvalidInternal([](LargeValueManifest &value) { value.node_logical_bytes = 0U; });
	expectInvalidInternal(
			[](LargeValueManifest &value) { value.first_uncompressed_offset = std::numeric_limits<uint64_t>::max(); });
	expectInvalidInternal([](LargeValueManifest &value) { value.children.front().page_id = PageId{}; });
	expectInvalidInternal([](LargeValueManifest &value) { value.children.front().first_chunk_ordinal = 1U; });
	expectInvalidInternal([](LargeValueManifest &value) { value.children.front().chunk_count = 0U; });
	expectInvalidInternal([](LargeValueManifest &value) { value.children.front().uncompressed_offset = 1U; });
	expectInvalidInternal([](LargeValueManifest &value) { value.children.front().logical_bytes = 0U; });
	expectInvalidInternal(
			[](LargeValueManifest &value) { value.children.front().logical_bytes = value.node_logical_bytes + 1U; });
	expectInvalidInternal([](LargeValueManifest &value) {
		value.children.front().first_chunk_ordinal = std::numeric_limits<uint32_t>::max();
	});
	expectInvalidInternal([](LargeValueManifest &value) { value.node_logical_bytes += 1U; });
	expectInvalidInternal(
			[](LargeValueManifest &value) {
				value.first_chunk_ordinal = std::numeric_limits<uint32_t>::max() - 1U;
				value.children.front().first_chunk_ordinal = value.first_chunk_ordinal;
				value.children.front().chunk_count = 2U;
			},
			FormatError::FERR_ARITHMETIC_OVERFLOW);

	LargeValueManifest ordinalOverflow = manifest();
	ordinalOverflow.node_kind = LargeValueManifestNodeKind::LVMNK_LEAF;
	ordinalOverflow.page_id = PageId(303U);
	ordinalOverflow.node_level = 0U;
	ordinalOverflow.first_chunk_ordinal = std::numeric_limits<uint32_t>::max();
	ordinalOverflow.first_uncompressed_offset = 0U;
	ordinalOverflow.node_logical_bytes = ordinalOverflow.logical_bytes;
	ordinalOverflow.chunks.front().ordinal = std::numeric_limits<uint32_t>::max();
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::encode(ordinalOverflow)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);

	LargeValueManifest pageIdOverflow = manifest();
	pageIdOverflow.chunks.front().first_data_page_id = PageId(std::numeric_limits<uint64_t>::max());
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::encode(pageIdOverflow)); },
					  FormatError::FERR_INVALID_HEADER);

	LargeValueManifest nonContiguousLeaf = manifest();
	nonContiguousLeaf.node_kind = LargeValueManifestNodeKind::LVMNK_LEAF;
	nonContiguousLeaf.page_id = PageId(304U);
	nonContiguousLeaf.node_level = 0U;
	nonContiguousLeaf.node_logical_bytes = nonContiguousLeaf.logical_bytes + 1U;
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::encode(nonContiguousLeaf)); },
					  FormatError::FERR_INVALID_HEADER);

	LargeValueManifest rootWithoutChildren = manifest();
	rootWithoutChildren.node_level = 1U;
	rootWithoutChildren.chunks.clear();
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::encode(rootWithoutChildren)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	corrupt[128U] = std::byte{0x01};
	recalculateManifestChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	corrupt.front() ^= std::byte{1};
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 8U, 0xFFFFU);
	recalculateManifestChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	const PageOwnerHeader owner = manifestOwner(expected);
	const LargeValueManifestPage page = LargeValueManifestPage::create(owner, expected);
	EXPECT_EQ(page.rebindOwner(owner).manifest(), expected);
}

TEST(RelocatableLargeValueDataPageTest, PreservesCanonicalChunkFragmentsAndOwnerIdentity) {
	for (const LargeValueDataPage &expected: {firstDataPage(), secondDataPage()}) {
		const PageOwnerHeader owner = dataOwner(expected);
		const std::vector<std::byte> encoded = LargeValueDataPageCodec::encode(expected);
		EXPECT_EQ(encoded.size(), RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
		EXPECT_EQ(LargeValueDataPageCodec::decode(encoded), expected);
		const LargeValueDataFrame frame = LargeValueDataFrame::create(owner, expected);
		EXPECT_EQ(frame.page(), expected);
		EXPECT_EQ(LargeValueDataFrame::open(owner, frame.serialize()).page(), expected);
	}
}

TEST(RelocatableLargeValueDataPageTest, RejectsChecksummedAndCanonicalLayoutCorruption) {
	const std::vector<std::byte> encoded = LargeValueDataPageCodec::encode(firstDataPage());
	std::vector<std::byte> corrupt = encoded;
	corrupt[RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES] ^= std::byte{1};
	expectFormatError([&] { static_cast<void>(LargeValueDataPageCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 52U, 1U);
	recalculateDataChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueDataPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = LargeValueDataPageCodec::encode(secondDataPage());
	corrupt.back() = std::byte{1};
	recalculateDataChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueDataPageCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);
}

TEST(RelocatableLargeValueDataPageTest, RejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	const std::vector<std::byte> encoded = LargeValueDataPageCodec::encode(firstDataPage());
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto mutation = encoded;
			mutation[byteOffset] ^= static_cast<std::byte>(uint8_t{1U} << bit);
			recalculateDataChecksums(mutation);
			try {
				const LargeValueDataPage decoded = LargeValueDataPageCodec::decode(mutation);
				EXPECT_EQ(LargeValueDataPageCodec::encode(decoded), mutation)
						<< "checksum-valid mutation at byte " << byteOffset << ", bit "
						<< static_cast<unsigned>(bit);
			} catch (const FormatException &) {
				// All rejected mutations must stop at the typed format boundary.
			}
		}
	}
}

TEST(RelocatableLargeValueDataPageTest, RejectsEveryInvalidFrameIdentityAndChecksummedHeaderShape) {
	const LargeValueDataPage expected = firstDataPage();
	const std::vector<std::byte> encoded = LargeValueDataPageCodec::encode(expected);

	const auto expectInvalidPage = [&](const std::function<void(LargeValueDataPage &)> &mutate,
									   FormatError error = FormatError::FERR_INVALID_HEADER) {
		LargeValueDataPage invalid = expected;
		mutate(invalid);
		expectFormatError([&] { static_cast<void>(LargeValueDataPageCodec::encode(invalid)); }, error);
	};
	expectInvalidPage([](LargeValueDataPage &page) { page.value_id = LargeValueId{}; });
	expectInvalidPage([](LargeValueDataPage &page) { page.page_id = PageId{}; });
	expectInvalidPage([](LargeValueDataPage &page) { page.generation = Generation{}; });
	expectInvalidPage([](LargeValueDataPage &page) { page.page_count = 0U; });
	expectInvalidPage([](LargeValueDataPage &page) { page.page_ordinal = page.page_count; });
	expectInvalidPage([](LargeValueDataPage &page) { page.stored_chunk_bytes = 0U; });
	expectInvalidPage([](LargeValueDataPage &page) { page.payload.clear(); });
	expectInvalidPage([](LargeValueDataPage &page) {
		page.payload.resize(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 1U, std::byte{});
	});
	expectInvalidPage([](LargeValueDataPage &page) { page.stored_offset = page.stored_chunk_bytes; });
	expectInvalidPage([](LargeValueDataPage &page) { page.page_count = 1U; });
	expectInvalidPage([](LargeValueDataPage &page) { page.stored_chunk_bytes = std::numeric_limits<uint32_t>::max(); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);

	expectFormatError([&] { static_cast<void>(LargeValueDataPageCodec::decode(std::span(encoded).first(1U))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> corrupt = encoded;
	corrupt.push_back(std::byte{});
	expectFormatError([&] { static_cast<void>(LargeValueDataPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 8U, 1U);
	recalculateDataChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueDataPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 56U, RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES - 1U);
	corrupt[RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES + RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES - 1U] = std::byte{};
	recalculateDataChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueDataPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 56U, RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 1U);
	recalculateDataChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(LargeValueDataPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	corrupt.front() ^= std::byte{1};
	expectFormatError([&] { static_cast<void>(LargeValueDataPageCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	const PageOwnerHeader owner = dataOwner(expected);
	const LargeValueDataFrame frame = LargeValueDataFrame::create(owner, expected);
	PageOwnerHeader wrongOwner = owner;
	wrongOwner.kind = PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
	expectFormatError([&] { static_cast<void>(LargeValueDataFrame::open(wrongOwner, frame.serialize())); },
					  FormatError::FERR_INVALID_STATE);
	wrongOwner = owner;
	wrongOwner.flags = RSS_PAGE_OWNER_FLAG_RETIRED;
	expectFormatError([&] { static_cast<void>(frame.rebindOwner(wrongOwner)); }, FormatError::FERR_INVALID_STATE);
}

TEST(RelocatableLargeValueManifestPageTest, AdmitsDecodedManifestArraysBeforeAllocationAndAllowsRetry) {
	for (bool internal: {false, true}) {
		SCOPED_TRACE(internal);
		auto source = manifest();
		if (internal) {
			source.node_level = 1U;
			source.children = {{PageId(301U), 0U, static_cast<uint32_t>(source.chunks.size()), 0U, source.logical_bytes}};
			source.chunks.clear();
		}
		const auto bytes = LargeValueManifestPageCodec::encode(source);
		size_t calls = 0U, requested = 0U;
		const auto reject = [&](size_t count) { ++calls; requested = count; throw std::length_error("manifest arrays rejected"); };
		EXPECT_THROW(static_cast<void>(LargeValueManifestPageCodec::decode(bytes, reject)), std::length_error);
		EXPECT_EQ(calls, 1U);
		EXPECT_EQ(requested, source.media_type.size() + source.chunks.size() * sizeof(LargeValueChunk)
			+ source.children.size() * sizeof(LargeValueManifestChild));
		for (size_t retry = 0U; retry < 2U; ++retry) {
			const auto decoded = LargeValueManifestPageCodec::decode(bytes, [&](size_t count) { requested = count; });
			EXPECT_EQ(decoded, source);
			EXPECT_EQ(requested, decoded.media_type.capacity() + decoded.chunks.capacity() * sizeof(LargeValueChunk)
				+ decoded.children.capacity() * sizeof(LargeValueManifestChild));
		}
		auto corrupt = bytes; corrupt[0] ^= std::byte{1}; calls = 0U;
		expectFormatError([&] { static_cast<void>(LargeValueManifestPageCodec::decode(corrupt, reject)); }, FormatError::FERR_CHECKSUM_MISMATCH);
		EXPECT_EQ(calls, 0U);
	}
}

TEST(RelocatableLargeValueDataPageTest, AdmitsDecodedPayloadAfterIntegrityChecksAndAllowsRetry) {
	const auto expected = firstDataPage();
	const auto encoded = LargeValueDataPageCodec::encode(expected);
	size_t calls = 0U, requested = 0U;
	EXPECT_THROW(static_cast<void>(LargeValueDataPageCodec::decode(encoded, [&](size_t bytes) {
		++calls; requested = bytes; throw std::length_error("data payload budget");
	})), std::length_error);
	EXPECT_EQ(calls, 1U);
	EXPECT_EQ(requested, expected.payload.size());
	for (int retry = 0; retry < 2; ++retry) {
		const auto decoded = LargeValueDataPageCodec::decode(encoded, [&](size_t bytes) { requested = bytes; });
		EXPECT_EQ(decoded, expected);
		EXPECT_EQ(requested, decoded.payload.capacity());
	}
	auto corrupt = encoded;
	corrupt[0] ^= std::byte{1U};
	calls = 0U;
	expectFormatError([&] {
		static_cast<void>(LargeValueDataPageCodec::decode(corrupt, [&](size_t) { ++calls; }));
	}, FormatError::FERR_CHECKSUM_MISMATCH);
	EXPECT_EQ(calls, 0U);
}
