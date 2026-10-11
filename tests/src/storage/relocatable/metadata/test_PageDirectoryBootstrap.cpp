/**
 * @file test_PageDirectoryBootstrap.cpp
 * @brief Typed physical-bootstrap codec tests for Page Directory routing.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/metadata/PageDirectoryBootstrap.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	constexpr size_t PAYLOAD_CRC_OFFSET = 152U;
	constexpr size_t HEADER_CRC_OFFSET = 156U;

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	PhysicalPageRef pageRef(uint64_t slot, uint8_t frame, uint64_t generation) {
		return {PhysicalSlotId(slot), frame, Generation(generation)};
	}

	PageDirectoryBootstrap validBootstrap() {
		PageDirectoryBootstrap bootstrap;
		bootstrap.generation = Generation(7U);
		bootstrap.committed_lsn = LogSequenceNumber(11U);
		bootstrap.data_page_directory_root = pageRef(3U, 1U, 7U);
		bootstrap.metadata_page_directory_root = pageRef(4U, 2U, 7U);
		bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_CATALOG)] =
				MetadataPageId(41U);
		bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_ALLOCATOR)] =
				MetadataPageId(42U);
		return bootstrap;
	}

	void recalculateCrc(PageDirectoryBootstrapCodec::Bytes &bytes) {
		LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET,
								 graph::utils::calculateCrc(bytes.data() + RSS_PAGE_DIRECTORY_BOOTSTRAP_HEADER_BYTES,
													bytes.size() - RSS_PAGE_DIRECTORY_BOOTSTRAP_HEADER_BYTES));
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET,
								 graph::utils::calculateCrc(bytes.data(), RSS_PAGE_DIRECTORY_BOOTSTRAP_HEADER_BYTES));
	}

} // namespace

TEST(RelocatablePageDirectoryBootstrapTest, KeepsDataAndMetadataBranchesPhysicallyTyped) {
	const PageDirectoryBootstrap expected = validBootstrap();
	const auto encoded = PageDirectoryBootstrapCodec::encode(expected);
	EXPECT_EQ(encoded.size(), RSS_PAGE_SIZE);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[0]), 0x5AU);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[1]), 0x59U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[2]), 0x58U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[3]), 0x42U);
	EXPECT_EQ(PageDirectoryBootstrapCodec::decode(encoded), expected);
}

TEST(RelocatablePageDirectoryBootstrapTest, PersistsTypedLogicalMetadataRootsInsideTheBootstrapHeader) {
	const PageDirectoryBootstrap decoded = PageDirectoryBootstrapCodec::decode(PageDirectoryBootstrapCodec::encode(validBootstrap()));
	EXPECT_EQ(decoded.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_CATALOG)],
			  MetadataPageId(41U));
	EXPECT_EQ(decoded.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_ALLOCATOR)],
			  MetadataPageId(42U));
	EXPECT_FALSE(decoded.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_STATE)].isValid());
}

TEST(RelocatablePageDirectoryBootstrapTest, AllowsEmptyBranchesDuringStagedBootstrap) {
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(1U);
	bootstrap.committed_lsn = LogSequenceNumber(1U);
	const PageDirectoryBootstrap decoded = PageDirectoryBootstrapCodec::decode(PageDirectoryBootstrapCodec::encode(bootstrap));
	EXPECT_FALSE(decoded.data_page_directory_root.isValid());
	EXPECT_FALSE(decoded.metadata_page_directory_root.isValid());
}

TEST(RelocatablePageDirectoryBootstrapTest, RejectsInvalidBranchReferencesAndNonCanonicalPayload) {
	PageDirectoryBootstrap bootstrap = validBootstrap();
	bootstrap.metadata_page_directory_root.frame_index = 0U;
	expectFormatError([&] { static_cast<void>(PageDirectoryBootstrapCodec::encode(bootstrap)); },
					  FormatError::FERR_INVALID_HEADER);

	auto encoded = PageDirectoryBootstrapCodec::encode(validBootstrap());
	encoded[RSS_PAGE_DIRECTORY_BOOTSTRAP_HEADER_BYTES] = std::byte{1U};
	recalculateCrc(encoded);
	expectFormatError([&] { static_cast<void>(PageDirectoryBootstrapCodec::decode(encoded)); },
					  FormatError::FERR_RESERVED_BITS);
}

TEST(RelocatablePageDirectoryBootstrapTest, RejectsChecksumValidUnsupportedOrCorruptHeaders) {
	auto encoded = PageDirectoryBootstrapCodec::encode(validBootstrap());
	LittleEndianCodec::writeU16(encoded, 4U, RSS_PAGE_DIRECTORY_BOOTSTRAP_VERSION + 1U);
	recalculateCrc(encoded);
	expectFormatError([&] { static_cast<void>(PageDirectoryBootstrapCodec::decode(encoded)); },
					  FormatError::FERR_UNSUPPORTED_VERSION);

	encoded = PageDirectoryBootstrapCodec::encode(validBootstrap());
	LittleEndianCodec::writeU64(encoded, 40U, 0U);
	LittleEndianCodec::writeU64(encoded, 48U, 1U);
	recalculateCrc(encoded);
	expectFormatError([&] { static_cast<void>(PageDirectoryBootstrapCodec::decode(encoded)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatablePageDirectoryBootstrapTest, RejectsTruncationChecksumDamageAndInvalidInMemoryReferences) {
	const PageDirectoryBootstrapCodec::Bytes encoded = PageDirectoryBootstrapCodec::encode(validBootstrap());
	expectFormatError([&] { static_cast<void>(PageDirectoryBootstrapCodec::decode(std::span(encoded).first(RSS_PAGE_SIZE - 1U))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> oversized(encoded.begin(), encoded.end());
	oversized.push_back(std::byte{});
	expectFormatError([&] { static_cast<void>(PageDirectoryBootstrapCodec::decode(oversized)); },
					  FormatError::FERR_INVALID_HEADER);

	auto corrupt = encoded;
	corrupt[8U] ^= std::byte{1U};
	expectFormatError([&] { static_cast<void>(PageDirectoryBootstrapCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
	corrupt = encoded;
	corrupt[RSS_PAGE_DIRECTORY_BOOTSTRAP_HEADER_BYTES] ^= std::byte{1U};
	expectFormatError([&] { static_cast<void>(PageDirectoryBootstrapCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	PageDirectoryBootstrap invalid = validBootstrap();
	invalid.generation = {};
	expectFormatError([&] { static_cast<void>(PageDirectoryBootstrapCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = validBootstrap();
	invalid.committed_lsn = {};
	expectFormatError([&] { static_cast<void>(PageDirectoryBootstrapCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = validBootstrap();
	invalid.data_page_directory_root = {PhysicalSlotId(3U), 1U,
									  Generation((std::numeric_limits<uint64_t>::max() >> 5U) + 1U)};
	expectFormatError([&] { static_cast<void>(PageDirectoryBootstrapCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = validBootstrap();
	invalid.metadata_page_directory_root = {PhysicalSlotId{}, 1U, Generation(7U)};
	expectFormatError([&] { static_cast<void>(PageDirectoryBootstrapCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatablePageDirectoryBootstrapTest, RejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	const PageDirectoryBootstrapCodec::Bytes encoded = PageDirectoryBootstrapCodec::encode(validBootstrap());
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			PageDirectoryBootstrapCodec::Bytes mutation = encoded;
			mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
			recalculateCrc(mutation);
			try {
				const PageDirectoryBootstrap decoded = PageDirectoryBootstrapCodec::decode(mutation);
				if (PageDirectoryBootstrapCodec::encode(decoded) != mutation) {
					ADD_FAILURE() << "bootstrap mutation was normalized at byte " << byteOffset << ", bit "
							  << static_cast<unsigned>(bit);
					return;
				}
			} catch (const FormatException &) {
				// A checksum-valid mutation is safe only when the typed boundary rejects it.
			}
		}
	}
}
