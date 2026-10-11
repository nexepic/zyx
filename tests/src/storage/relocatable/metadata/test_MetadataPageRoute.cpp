/**
 * @file test_MetadataPageRoute.cpp
 * @brief Canonical wire tests for typed metadata PageId route leaves.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <cstddef>
#include <functional>
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/metadata/MetadataPageRoute.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	constexpr size_t PAYLOAD_CRC_OFFSET = 40U;
	constexpr size_t HEADER_CRC_OFFSET = 44U;

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	MetadataPageRouteLeafPage validPage() {
		MetadataPageRouteLeafPage page;
		page.page_id = MetadataPageId(77U);
		page.generation = Generation(5U);
		page.committed_lsn = LogSequenceNumber(9U);
		page.routes = {{MetadataPageId(1U), {PhysicalSlotId(3U), 1U, Generation(5U)}},
					   {MetadataPageId(9U), {PhysicalSlotId(4U), 31U, Generation(4U)}}};
		return page;
	}

	void recalculateCrc(MetadataPageRouteLeafCodec::Bytes &bytes) {
		LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET,
								 graph::utils::calculateCrc(bytes.data() + RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES,
													bytes.size() - RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES));
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET,
								 graph::utils::calculateCrc(bytes.data(), RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES));
	}

} // namespace

TEST(RelocatableMetadataPageRouteTest, RoundTripsTypedMetadataRoutes) {
	const MetadataPageRouteLeafPage expected = validPage();
	const auto encoded = MetadataPageRouteLeafCodec::encode(expected);
	EXPECT_EQ(encoded.size(), RSS_PAGE_SIZE);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[0]), 0x5AU);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[1]), 0x59U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[2]), 0x58U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[3]), 0x4DU);
	const MetadataPageRouteLeafPage decoded = MetadataPageRouteLeafCodec::decode(encoded);
	EXPECT_EQ(decoded.page_id, expected.page_id);
	EXPECT_EQ(decoded.routes, expected.routes);
}

TEST(RelocatableMetadataPageRouteTest, RejectsUnorderedIdsAndInvalidPhysicalReferences) {
	MetadataPageRouteLeafPage page = validPage();
	page.routes[1].page_id = MetadataPageId(1U);
	expectFormatError([&] { static_cast<void>(MetadataPageRouteLeafCodec::encode(page)); },
					  FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.routes.front().physical_reference.frame_index = 0U;
	expectFormatError([&] { static_cast<void>(MetadataPageRouteLeafCodec::encode(page)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableMetadataPageRouteTest, RejectsChecksumValidNonCanonicalPayload) {
	auto encoded = MetadataPageRouteLeafCodec::encode(validPage());
	encoded[RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES + 2U * RSS_METADATA_PAGE_ROUTE_ENTRY_BYTES] = std::byte{1U};
	recalculateCrc(encoded);
	expectFormatError([&] { static_cast<void>(MetadataPageRouteLeafCodec::decode(encoded)); },
					  FormatError::FERR_RESERVED_BITS);
}

TEST(RelocatableMetadataPageRouteTest, RejectsTruncationChecksumDamageAndInvalidInMemoryReferences) {
	const MetadataPageRouteLeafCodec::Bytes encoded = MetadataPageRouteLeafCodec::encode(validPage());
	expectFormatError([&] { static_cast<void>(MetadataPageRouteLeafCodec::decode(std::span(encoded).first(RSS_PAGE_SIZE - 1U))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> oversized(encoded.begin(), encoded.end());
	oversized.push_back(std::byte{});
	expectFormatError([&] { static_cast<void>(MetadataPageRouteLeafCodec::decode(oversized)); },
					  FormatError::FERR_INVALID_HEADER);

	auto corrupt = encoded;
	corrupt[8U] ^= std::byte{1U};
	expectFormatError([&] { static_cast<void>(MetadataPageRouteLeafCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
	corrupt = encoded;
	corrupt[RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES] ^= std::byte{1U};
	expectFormatError([&] { static_cast<void>(MetadataPageRouteLeafCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	MetadataPageRouteLeafPage invalid = validPage();
	invalid.page_id = {};
	expectFormatError([&] { static_cast<void>(MetadataPageRouteLeafCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = validPage();
	invalid.generation = {};
	expectFormatError([&] { static_cast<void>(MetadataPageRouteLeafCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = validPage();
	invalid.committed_lsn = {};
	expectFormatError([&] { static_cast<void>(MetadataPageRouteLeafCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = validPage();
	invalid.routes.front().physical_reference.generation =
				Generation((std::numeric_limits<uint64_t>::max() >> 5U) + 1U);
	expectFormatError([&] { static_cast<void>(MetadataPageRouteLeafCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableMetadataPageRouteTest, RejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	const MetadataPageRouteLeafCodec::Bytes encoded = MetadataPageRouteLeafCodec::encode(validPage());
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			MetadataPageRouteLeafCodec::Bytes mutation = encoded;
			mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
			recalculateCrc(mutation);
			try {
				const MetadataPageRouteLeafPage decoded = MetadataPageRouteLeafCodec::decode(mutation);
				if (MetadataPageRouteLeafCodec::encode(decoded) != mutation) {
					ADD_FAILURE() << "metadata route mutation was normalized at byte " << byteOffset << ", bit "
							  << static_cast<unsigned>(bit);
					return;
				}
			} catch (const FormatException &) {
				// A checksum-valid mutation is safe only when the typed boundary rejects it.
			}
		}
	}
}
