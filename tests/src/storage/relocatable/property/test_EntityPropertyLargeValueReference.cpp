/**
 * @file test_EntityPropertyLargeValueReference.cpp
 * @brief Strict wire-codec tests for owner-keyed external Property roots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <string>

#include <gtest/gtest.h>

#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	constexpr size_t LARGE_VALUE_FLAGS_OFFSET = 8U;
	constexpr size_t LARGE_VALUE_PAYLOAD_CRC_OFFSET = 28U;
	constexpr size_t LARGE_VALUE_HEADER_CRC_OFFSET = 32U;
	constexpr size_t LARGE_VALUE_REFERENCE_HEADER_BYTES = 40U;
	constexpr size_t NESTED_REF_PAYLOAD_CRC_OFFSET = 80U;
	constexpr size_t NESTED_REF_HEADER_CRC_OFFSET = 84U;
	constexpr size_t NESTED_REF_HEADER_BYTES = 96U;

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	[[nodiscard]] EntityPropertyLargeValueReference reference() {
		EntityPropertyLargeValueReference result;
		result.owner_entity_id = 71U;
		result.owner_entity_type = graph::Node::typeId;
		result.value.value_id = LargeValueId(91U);
		result.value.generation = Generation(3U);
		result.value.logical_bytes = 8192U;
		for (size_t index = 0U; index < result.value.content_hash.size(); ++index) {
			result.value.content_hash[index] = static_cast<std::byte>(index + 1U);
		}
		result.value.kind = LargeValueKind::LVK_BYTES;
		result.value.codec = LargeValueCodec::LVC_RAW;
		result.value.media_type = "application/x-zyx-property-root";
		return result;
	}

	void recalculateChecksums(std::vector<std::byte> &bytes) {
		const size_t nestedOffset = LARGE_VALUE_REFERENCE_HEADER_BYTES;
		std::span<std::byte> nested = std::span(bytes).subspan(nestedOffset);
		LittleEndianCodec::writeU32(nested, NESTED_REF_PAYLOAD_CRC_OFFSET,
								 graph::utils::calculateCrc(nested.data() + NESTED_REF_HEADER_BYTES,
													  nested.size() - NESTED_REF_HEADER_BYTES));
		LittleEndianCodec::writeU32(nested, NESTED_REF_HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(nested, NESTED_REF_HEADER_CRC_OFFSET,
								 graph::utils::calculateCrc(nested.data(), NESTED_REF_HEADER_BYTES));
		LittleEndianCodec::writeU32(bytes, LARGE_VALUE_PAYLOAD_CRC_OFFSET,
								 graph::utils::calculateCrc(nested.data(), nested.size()));
		LittleEndianCodec::writeU32(bytes, LARGE_VALUE_HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, LARGE_VALUE_HEADER_CRC_OFFSET,
								 graph::utils::calculateCrc(bytes.data(), LARGE_VALUE_REFERENCE_HEADER_BYTES));
	}

} // namespace

TEST(EntityPropertyLargeValueReferenceTest, RoundTripsLogicalIdentityAndRejectsCorruption) {
	const EntityPropertyLargeValueReference expected = reference();
	const std::vector<std::byte> encoded = EntityPropertyLargeValueReferenceCodec::encode(expected);
	EXPECT_TRUE(EntityPropertyLargeValueReferenceCodec::hasMagic(encoded));
	EXPECT_EQ(EntityPropertyLargeValueReferenceCodec::decode(encoded), expected);

	std::vector<std::byte> corrupt = encoded;
	corrupt.back() ^= std::byte{0x1U};
	expectFormatError([&] { static_cast<void>(EntityPropertyLargeValueReferenceCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = encoded;
	corrupt[12U] ^= std::byte{0x1U};
	expectFormatError([&] { static_cast<void>(EntityPropertyLargeValueReferenceCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, LARGE_VALUE_FLAGS_OFFSET, 1U);
	std::array<std::byte, RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES> header{};
	std::copy_n(corrupt.begin(), header.size(), header.begin());
	LittleEndianCodec::writeU32(header, LARGE_VALUE_HEADER_CRC_OFFSET, 0U);
	LittleEndianCodec::writeU32(corrupt, LARGE_VALUE_HEADER_CRC_OFFSET,
							 graph::utils::calculateCrc(header.data(), header.size()));
	expectFormatError([&] { static_cast<void>(EntityPropertyLargeValueReferenceCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	EXPECT_FALSE(EntityPropertyLargeValueReferenceCodec::hasMagic(std::span(encoded).first(1U)));
}

TEST(EntityPropertyLargeValueReferenceTest, RejectsInvalidOwnersAndTruncatedReferences) {
	EntityPropertyLargeValueReference invalid = reference();
	invalid.owner_entity_id = 0U;
	expectFormatError([&] { static_cast<void>(EntityPropertyLargeValueReferenceCodec::encode(invalid)); },
				  FormatError::FERR_INVALID_HEADER);

	invalid = reference();
	invalid.owner_entity_type = 99U;
	expectFormatError([&] { static_cast<void>(EntityPropertyLargeValueReferenceCodec::encode(invalid)); },
				  FormatError::FERR_INVALID_HEADER);

	const std::array<std::byte, 0U> empty{};
	expectFormatError([&] { static_cast<void>(EntityPropertyLargeValueReferenceCodec::decode(empty)); },
				  FormatError::FERR_BUFFER_TOO_SMALL);
	const std::array<std::byte, sizeof(uint32_t)> nonReferenceMagic{};
	EXPECT_FALSE(EntityPropertyLargeValueReferenceCodec::hasMagic(nonReferenceMagic));
}

TEST(EntityPropertyLargeValueReferenceTest, RejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	const std::vector<std::byte> encoded = EntityPropertyLargeValueReferenceCodec::encode(reference());
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto mutation = encoded;
			mutation[byteOffset] ^= static_cast<std::byte>(uint8_t{1U} << bit);
			recalculateChecksums(mutation);
			try {
				const EntityPropertyLargeValueReference decoded =
						EntityPropertyLargeValueReferenceCodec::decode(mutation);
				EXPECT_EQ(EntityPropertyLargeValueReferenceCodec::encode(decoded), mutation)
						<< "checksum-valid mutation at byte " << byteOffset << ", bit "
						<< static_cast<unsigned>(bit);
			} catch (const FormatException &) {
				// Invalid images must stop at the typed Property-root boundary.
			}
		}
	}
}
