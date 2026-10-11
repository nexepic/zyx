/**
 * @file test_VectorManifestPage.cpp
 * @brief Immutable vector manifest page codec tests for relocatable.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorManifestPage.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"
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

	VectorManifest manifest() {
		VectorManifest result;
		result.index_id = 17;
		result.page_id = PageId(101);
		result.generation = Generation(1);
		result.committed_lsn = LogSequenceNumber(2);
		result.dimensions = 384;
		result.entries = {{PageId(201), VectorStorageKind::VSK_RAW, 100},
						  {PageId(202), VectorStorageKind::VSK_RAW, 50},
						  {PageId(203), VectorStorageKind::VSK_PQ, 150}};
		return result;
	}

	PageOwnerHeader owner(const VectorManifest &manifest) {
		PageOwnerHeader result;
		result.page_id = manifest.page_id;
		result.owner_id = manifest.index_id;
		result.kind = PageOwnerKind::POK_VECTOR_MANIFEST;
		result.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		result.generation = manifest.generation;
		result.committed_lsn = manifest.committed_lsn;
		return result;
	}

	void recalculateChecksums(std::vector<std::byte> &bytes) {
		LittleEndianCodec::writeU32(bytes, 128,
									graph::utils::calculateCrc(bytes.data() + RSS_VECTOR_MANIFEST_HEADER_BYTES,
															   bytes.size() - RSS_VECTOR_MANIFEST_HEADER_BYTES));
		LittleEndianCodec::writeU32(bytes, 132, 0);
		LittleEndianCodec::writeU32(bytes, 132,
									graph::utils::calculateCrc(bytes.data(), RSS_VECTOR_MANIFEST_HEADER_BYTES));
	}

} // namespace

TEST(RelocatableVectorManifestPageTest, EncodesAndReopensImmutableRawAndPqPageReferences) {
	const VectorManifest expected = manifest();
	VectorManifest expectedWithCodebook = expected;
	expectedWithCodebook.metric = VectorDistanceMetric::VDM_COSINE;
	expectedWithCodebook.codebook_id = 18;
	expectedWithCodebook.codebook_generation = Generation(2);
	expectedWithCodebook.entry_point_vector_id = 17;
	expectedWithCodebook.node_mapping_root_page_id = PageId(204);
	expectedWithCodebook.node_mapping_root_generation = Generation(3);
	expectedWithCodebook.vector_node_mapping_index_id =
			RSS_NODE_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	expectedWithCodebook.vector_node_mapping_root_page_id = PageId(207);
	expectedWithCodebook.vector_node_mapping_root_generation = Generation(3);
	expectedWithCodebook.pq_mapping_index_id = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	expectedWithCodebook.pq_mapping_root_page_id = PageId(205);
	expectedWithCodebook.pq_mapping_root_generation = Generation(4);
	expectedWithCodebook.delta_mapping_index_id = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	expectedWithCodebook.delta_mapping_root_page_id = PageId(208);
	expectedWithCodebook.delta_mapping_root_generation = Generation(5);
	expectedWithCodebook.delta_entry_count = 3U;
	expectedWithCodebook.raw_write_tail_page_id = PageId(206);
	const std::vector<std::byte> encoded = VectorManifestPageCodec::encode(expected);
	EXPECT_EQ(encoded.size(), RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
	EXPECT_EQ(LittleEndianCodec::readU32(encoded, 0), RSS_VECTOR_MANIFEST_MAGIC);
	EXPECT_EQ(LittleEndianCodec::readU16(encoded, 4), RSS_VECTOR_MANIFEST_VERSION);
	EXPECT_EQ(VectorManifestPageCodec::decode(encoded), expected);
	EXPECT_EQ(VectorManifestPageCodec::decode(VectorManifestPageCodec::encode(expectedWithCodebook)),
			  expectedWithCodebook);
	VectorManifest expectedWithInnerProduct = expected;
	expectedWithInnerProduct.metric = VectorDistanceMetric::VDM_INNER_PRODUCT;
	EXPECT_EQ(VectorManifestPageCodec::decode(VectorManifestPageCodec::encode(expectedWithInnerProduct)),
			  expectedWithInnerProduct);

	VectorManifestPage page = VectorManifestPage::create(owner(expected), expected);
	EXPECT_EQ(page.manifest(), expected);
	EXPECT_EQ(page.serialize(), encoded);
	EXPECT_EQ(VectorManifestPage::open(owner(expected), page.serialize()).manifest(), expected);
	PageOwnerHeader retiredOwner = owner(expected);
	retiredOwner.flags = RSS_PAGE_OWNER_FLAG_RETIRED;
	EXPECT_EQ(VectorManifestPage::open(retiredOwner, page.serialize()).manifest(), expected);
	expectFormatError([&] { static_cast<void>(VectorManifestPage::create(retiredOwner, expected)); },
					  FormatError::FERR_INVALID_STATE);
	retiredOwner.flags = static_cast<uint16_t>(RSS_PAGE_OWNER_KNOWN_FLAGS | 0x2U);
	expectFormatError([&] { static_cast<void>(VectorManifestPage::open(retiredOwner, page.serialize())); },
					  FormatError::FERR_INVALID_STATE);
}

TEST(RelocatableVectorManifestPageTest, AllowsAnEmptyImmutableIndexAndRejectsInvalidEntries) {
	VectorManifest empty = manifest();
	empty.entries.clear();
	EXPECT_EQ(VectorManifestPageCodec::decode(VectorManifestPageCodec::encode(empty)), empty);

	VectorManifest invalid = manifest();
	invalid.dimensions = 0;
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = manifest();
	invalid.entries[1].page_id = invalid.entries[0].page_id;
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = manifest();
	invalid.entries[0].storage_kind = static_cast<VectorStorageKind>(99);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = manifest();
	invalid.entries[0].record_count = 0;
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = manifest();
	invalid.codebook_id = 19;
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = manifest();
	invalid.node_mapping_root_page_id = PageId(204);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = manifest();
	invalid.node_mapping_root_page_id = PageId(204);
	invalid.node_mapping_root_generation = Generation(3);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = manifest();
	invalid.vector_node_mapping_index_id = RSS_NODE_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	invalid.vector_node_mapping_root_page_id = PageId(204);
	invalid.vector_node_mapping_root_generation = Generation(3);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = manifest();
	invalid.pq_mapping_root_page_id = PageId(205);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = manifest();
	invalid.pq_mapping_index_id = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	invalid.pq_mapping_root_page_id = PageId(205);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = manifest();
	invalid.pq_mapping_index_id = 17;
	invalid.pq_mapping_root_page_id = PageId(205);
	invalid.pq_mapping_root_generation = Generation(4);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = manifest();
	invalid.delta_mapping_root_page_id = PageId(208);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = manifest();
	invalid.delta_mapping_index_id = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	invalid.delta_mapping_root_page_id = PageId(208);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = manifest();
	invalid.delta_mapping_index_id = 17U;
	invalid.delta_mapping_root_page_id = PageId(208);
	invalid.delta_mapping_root_generation = Generation(5);
	invalid.delta_entry_count = 3U;
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = manifest();
	invalid.delta_mapping_index_id = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	invalid.delta_mapping_root_page_id = PageId(208);
	invalid.delta_mapping_root_generation = Generation(5);
	invalid.delta_entry_count = 3U;
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = manifest();
	invalid.delta_entry_count = 1U;
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableVectorManifestPageTest, RetainsChecksummedZeroPaddingAfterAFullEntrySet) {
	VectorManifest full = manifest();
	full.entries.clear();
	full.entries.reserve(RSS_VECTOR_MANIFEST_MAX_ENTRIES);
	for (uint16_t index = 0U; index < RSS_VECTOR_MANIFEST_MAX_ENTRIES; ++index) {
		full.entries.push_back({PageId(1000U + index), VectorStorageKind::VSK_RAW, 1U});
	}
	const std::vector<std::byte> encoded = VectorManifestPageCodec::encode(full);
	EXPECT_EQ(VectorManifestPageCodec::decode(encoded), full);
	EXPECT_TRUE(std::all_of(encoded.end() - RSS_VECTOR_MANIFEST_TRAILING_PADDING_BYTES, encoded.end(),
							[](std::byte value) { return value == std::byte{}; }));
}

TEST(RelocatableVectorManifestPageTest, RejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	const std::vector<std::byte> encoded = VectorManifestPageCodec::encode(manifest());
	ASSERT_EQ(encoded.size(), RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);

	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto mutation = encoded;
			mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
			recalculateChecksums(mutation);
			try {
				const VectorManifest decoded = VectorManifestPageCodec::decode(mutation);
				if (VectorManifestPageCodec::encode(decoded) != mutation) {
					ADD_FAILURE() << "vector manifest mutation was normalized at byte " << byteOffset << ", bit "
								  << static_cast<unsigned>(bit);
					return;
				}
			} catch (const FormatException &) {
				// Every non-canonical manifest image must fail at the typed format boundary.
			}
		}
	}
}

TEST(RelocatableVectorManifestPageTest, RejectsInvalidAdjacencyMappingAndMetricContracts) {
	VectorManifest invalid = manifest();
	invalid.metric = static_cast<VectorDistanceMetric>(99U);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);

	invalid = manifest();
	invalid.adjacency_mapping_index_id = 17U;
	invalid.adjacency_generation = Generation(3U);
	invalid.adjacency_mapping_root_page_id = PageId(204U);
	invalid.adjacency_mapping_root_generation = Generation(3U);
	invalid.entry_point_vector_id = 1U;
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);

	invalid = manifest();
	invalid.adjacency_mapping_index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	invalid.adjacency_generation = Generation(3U);
	invalid.adjacency_mapping_root_page_id = PageId(204U);
	invalid.adjacency_mapping_root_generation = Generation(3U);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableVectorManifestPageTest, FailsClosedForOwnerAndChecksummedDiskCorruption) {
	const VectorManifest expected = manifest();
	const PageOwnerHeader validOwner = owner(expected);
	PageOwnerHeader wrongOwner = validOwner;
	wrongOwner.kind = PageOwnerKind::POK_VECTOR_RAW;
	expectFormatError([&] { static_cast<void>(VectorManifestPage::create(wrongOwner, expected)); },
					  FormatError::FERR_INVALID_STATE);

	std::vector<std::byte> corrupt = VectorManifestPageCodec::encode(expected);
	corrupt.front() ^= std::byte{1};
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = VectorManifestPageCodec::encode(expected);
	corrupt.back() = std::byte{1};
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = VectorManifestPageCodec::encode(expected);
	LittleEndianCodec::writeU16(corrupt, RSS_VECTOR_MANIFEST_HEADER_BYTES + 10U, 0);
	recalculateChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = VectorManifestPageCodec::encode(expected);
	corrupt[RSS_VECTOR_MANIFEST_HEADER_BYTES + expected.entries.size() * RSS_VECTOR_MANIFEST_ENTRY_BYTES] =
			std::byte{1};
	recalculateChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);

	corrupt = VectorManifestPageCodec::encode(expected);
	LittleEndianCodec::writeU32(corrupt, 8, 1);
	recalculateChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::decode(corrupt)); },
				  FormatError::FERR_INVALID_HEADER);

	corrupt = VectorManifestPageCodec::encode(expected);
	LittleEndianCodec::writeU64(corrupt, 168U, 1U);
	recalculateChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::decode(corrupt)); },
				  FormatError::FERR_INVALID_HEADER);

	corrupt = VectorManifestPageCodec::encode(expected);
	LittleEndianCodec::writeU64(corrupt, 192U, 1U);
	recalculateChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::decode(corrupt)); },
				  FormatError::FERR_INVALID_HEADER);

	corrupt = VectorManifestPageCodec::encode(expected);
	LittleEndianCodec::writeU32(corrupt, 220U, 1U);
	recalculateChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::decode(corrupt)); },
				  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::decode(std::span(corrupt).first(1U))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	corrupt.push_back(std::byte{});
	expectFormatError([&] { static_cast<void>(VectorManifestPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
}
