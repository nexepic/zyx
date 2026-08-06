/**
 * @file test_VectorCodebook.cpp
 * @brief Immutable relocatable vector codebook codec and extent-store tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookExtent.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
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

	std::vector<std::byte> payload() { return std::vector<std::byte>(24U, std::byte{0x5A}); }

	VectorCodebookHeader header() {
		VectorCodebookHeader result;
		result.codebook_id = 17;
		result.generation = Generation(1);
		result.element_type = VectorCodebookElementType::VCET_FLOAT32;
		result.dimensions = 2;
		result.subspace_count = 1;
		result.centroid_count = 3;
		result.element_bytes = 4;
		result.payload_bytes = 24;
		const std::vector<std::byte> bytes = payload();
		result.payload_crc = graph::utils::calculateCrc(bytes.data(), bytes.size());
		return result;
	}

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	VectorCodebookWriteRequest request() {
		VectorCodebookWriteRequest result;
		result.codebook_id = 17;
		result.manifest_segment_id = SegmentId(71);
		result.generation = Generation(1);
		result.element_type = VectorCodebookElementType::VCET_FLOAT32;
		result.dimensions = 2;
		result.subspace_count = 1;
		result.centroid_count = 3;
		return result;
	}

	void refreshHeaderChecksum(VectorCodebookHeaderCodec::Bytes &bytes) {
		LittleEndianCodec::writeU32(bytes, 32, 0);
		LittleEndianCodec::writeU32(bytes, 32, graph::utils::calculateCrc(bytes.data(), bytes.size()));
	}

	std::vector<std::byte> manifestBlock() {
		VectorCodebookManifest manifest;
		manifest.header = header();
		manifest.inline_payload = payload();
		return VectorCodebookManifestCodec::encode(manifest);
	}

	VectorCodebookExtentHeaderCodec::Bytes extentHeader(std::span<const std::byte> block) {
		VectorCodebookExtentHeader result;
		result.kind = VectorCodebookExtentKind::VCEK_MANIFEST;
		result.codebook_id = 17U;
		result.codebook_generation = Generation(1U);
		result.extent_id = VectorCodebookExtentIdCodec::manifestExtentId(17U);
		result.ordinal = RSS_VECTOR_CODEBOOK_MANIFEST_ORDINAL;
		result.block_bytes = static_cast<uint32_t>(block.size());
		result.block_crc = graph::utils::calculateCrc(block.data(), block.size());
		return VectorCodebookExtentHeaderCodec::encode(result);
	}

	void refreshExtentHeaderChecksum(VectorCodebookExtentHeaderCodec::Bytes &bytes) {
		constexpr size_t HEADER_CRC_OFFSET = 48U;
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, graph::utils::calculateCrc(bytes.data(), bytes.size()));
	}

	VectorCodebookExtentHeader dataExtentHeader(std::span<const std::byte> block, SegmentId segmentId) {
		VectorCodebookExtentHeader result;
		result.kind = VectorCodebookExtentKind::VCEK_DATA;
		result.codebook_id = 17U;
		result.codebook_generation = Generation(1U);
		result.extent_id = VectorCodebookExtentIdCodec::dataExtentId(segmentId);
		result.ordinal = 0U;
		result.block_bytes = static_cast<uint32_t>(block.size());
		result.block_crc = graph::utils::calculateCrc(block.data(), block.size());
		return result;
	}

	VectorCodebookManifest multiExtentManifest() {
		const uint32_t centroidCount = RSS_VECTOR_CODEBOOK_INLINE_PAYLOAD_BYTES / sizeof(float) + 1U;
		const uint32_t payloadBytes = centroidCount * sizeof(float);
		std::vector<std::byte> bytes(payloadBytes, std::byte{0xA5});
		VectorCodebookManifest result;
		result.header.codebook_id = 17U;
		result.header.generation = Generation(1U);
		result.header.element_type = VectorCodebookElementType::VCET_FLOAT32;
		result.header.dimensions = 1U;
		result.header.subspace_count = 1U;
		result.header.centroid_count = centroidCount;
		result.header.element_bytes = sizeof(float);
		result.header.payload_bytes = payloadBytes;
		result.header.payload_crc = graph::utils::calculateCrc(bytes.data(), bytes.size());
		result.entries.push_back(
				{VectorCodebookExtentIdCodec::dataExtentId(SegmentId(72U)), payloadBytes, result.header.payload_crc});
		return result;
	}

	PhysicalSegmentAllocationRequest rawRequest(const VectorCodebookExtentHeaderCodec::Bytes &encoded,
												std::span<const std::byte> bytes) {
		PhysicalSegmentAllocationRequest result;
		result.segment_id = SegmentId(71);
		result.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
		result.owner_id = 17;
		result.generation = Generation(1);
		result.header_bytes = RSS_ENVELOPE_BYTES + encoded.size();
		result.header_extension.assign(encoded.begin(), encoded.end());
		result.payload.assign(bytes.begin(), bytes.end());
		return result;
	}

	ExtentLocation extent() {
		return {VectorCodebookExtentIdCodec::manifestExtentId(17U), SegmentId(71), RSS_PAGE_POOL_FIRST_DATA_FRAME,
				RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1)};
	}

	void overwriteCodebookExtent(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
								 SegmentId segmentId, std::span<const std::byte> header,
								 std::span<const std::byte> payload) {
		const MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const CatalogEntry entry = metadata.findCatalog(snapshot, segmentId);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			entry.physical_slot);
		std::array<std::byte, RSS_ENVELOPE_BYTES> encodedEnvelope{};
		ASSERT_EQ(io->readAt(offset, encodedEnvelope.data(), encodedEnvelope.size()), encodedEnvelope.size());
		SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(encodedEnvelope);
		envelope.header_bytes = static_cast<uint16_t>(RSS_ENVELOPE_BYTES + header.size());
		envelope.payload_bytes = static_cast<uint32_t>(payload.size());
		const std::byte empty{};
		envelope.payload_crc = graph::utils::calculateCrc(payload.empty() ? &empty : payload.data(), payload.size());
		const auto replacementEnvelope = SegmentEnvelopeCodec::encode(envelope);
		io->writeAt(offset, replacementEnvelope.data(), replacementEnvelope.size());
		io->writeAt(offset + RSS_ENVELOPE_BYTES, header.data(), header.size());
		if (!payload.empty()) {
			io->writeAt(offset + envelope.header_bytes, payload.data(), payload.size());
		}
	}

	struct MultiExtentCodebookFixture {
		VectorCodebookWriteRequest request;
		std::vector<std::byte> payload;
	};

	MultiExtentCodebookFixture multiExtentCodebook(uint64_t codebookId = 17U,
												   SegmentId manifestSegment = SegmentId(71U),
												   SegmentId dataSegment = SegmentId(72U)) {
		MultiExtentCodebookFixture result;
		result.payload.assign(static_cast<size_t>(RSS_VECTOR_CODEBOOK_INLINE_PAYLOAD_BYTES) + sizeof(float),
							  std::byte{0xA5});
		result.request.codebook_id = codebookId;
		result.request.manifest_segment_id = manifestSegment;
		result.request.generation = Generation(1U);
		result.request.element_type = VectorCodebookElementType::VCET_FLOAT32;
		result.request.dimensions = 1U;
		result.request.subspace_count = 1U;
		result.request.centroid_count = static_cast<uint32_t>(result.payload.size() / sizeof(float));
		result.request.data_extents.push_back({VectorCodebookExtentIdCodec::dataExtentId(dataSegment), dataSegment});
		return result;
	}

} // namespace

TEST(RelocatableVectorCodebookTest, EncodesChecksummedTypedHeaderAndPayload) {
	const VectorCodebookHeader expected = header();
	const VectorCodebookHeaderCodec::Bytes encoded = VectorCodebookHeaderCodec::encode(expected);
	EXPECT_EQ(LittleEndianCodec::readU32(encoded, 0), RSS_VECTOR_CODEBOOK_MAGIC);
	EXPECT_EQ(LittleEndianCodec::readU16(encoded, 4), RSS_VECTOR_CODEBOOK_VERSION);
	EXPECT_EQ(VectorCodebookHeaderCodec::decode(encoded), expected);
	VectorCodebookHeaderCodec::validatePayload(expected, payload());

	VectorCodebookHeader corrupt = expected;
	corrupt.payload_crc ^= 1U;
	expectFormatError([&] { VectorCodebookHeaderCodec::validatePayload(corrupt, payload()); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
	corrupt = expected;
	corrupt.element_type = static_cast<VectorCodebookElementType>(99);
	expectFormatError([&] { static_cast<void>(VectorCodebookHeaderCodec::encode(corrupt)); },
					  FormatError::FERR_INVALID_ENUM);
	VectorCodebookHeaderCodec::Bytes corruptBytes = encoded;
	corruptBytes[10] = std::byte{1};
	expectFormatError([&] { static_cast<void>(VectorCodebookHeaderCodec::decode(corruptBytes)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST(RelocatableVectorCodebookTest, RejectsMalformedHeadersAndPayloadShapesBeforeUse) {
	const VectorCodebookHeader expected = header();
	const VectorCodebookHeaderCodec::Bytes encoded = VectorCodebookHeaderCodec::encode(expected);
	expectFormatError(
			[&] {
				static_cast<void>(VectorCodebookHeaderCodec::decode(std::span(encoded).first(encoded.size() - 1U)));
			},
			FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> oversized(encoded.begin(), encoded.end());
	oversized.push_back(std::byte{});
	expectFormatError([&] { static_cast<void>(VectorCodebookHeaderCodec::decode(oversized)); },
					  FormatError::FERR_INVALID_HEADER);

	VectorCodebookHeaderCodec::Bytes corrupted = encoded;
	LittleEndianCodec::writeU16(corrupted, 10, 1);
	refreshHeaderChecksum(corrupted);
	expectFormatError([&] { static_cast<void>(VectorCodebookHeaderCodec::decode(corrupted)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupted = encoded;
	LittleEndianCodec::writeU16(corrupted, 8, 99);
	refreshHeaderChecksum(corrupted);
	expectFormatError([&] { static_cast<void>(VectorCodebookHeaderCodec::decode(corrupted)); },
					  FormatError::FERR_INVALID_ENUM);
	corrupted = encoded;
	LittleEndianCodec::writeU64(corrupted, 40, 0);
	refreshHeaderChecksum(corrupted);
	expectFormatError([&] { static_cast<void>(VectorCodebookHeaderCodec::decode(corrupted)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupted = encoded;
	LittleEndianCodec::writeU32(corrupted, 36, 0);
	refreshHeaderChecksum(corrupted);
	expectFormatError([&] { static_cast<void>(VectorCodebookHeaderCodec::decode(corrupted)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupted = encoded;
	LittleEndianCodec::writeU32(corrupted, 36, expected.dimensions + 1U);
	refreshHeaderChecksum(corrupted);
	expectFormatError([&] { static_cast<void>(VectorCodebookHeaderCodec::decode(corrupted)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupted = encoded;
	LittleEndianCodec::writeU32(corrupted, 12, 3U);
	LittleEndianCodec::writeU32(corrupted, 24, 36U);
	LittleEndianCodec::writeU32(corrupted, 36, 2U);
	refreshHeaderChecksum(corrupted);
	expectFormatError([&] { static_cast<void>(VectorCodebookHeaderCodec::decode(corrupted)); },
					  FormatError::FERR_INVALID_HEADER);

	VectorCodebookHeader overflow = expected;
	overflow.dimensions = std::numeric_limits<uint32_t>::max();
	overflow.centroid_count = std::numeric_limits<uint32_t>::max();
	overflow.payload_bytes = 0;
	expectFormatError([&] { static_cast<void>(VectorCodebookHeaderCodec::encode(overflow)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { VectorCodebookHeaderCodec::validatePayload(expected, {}); },
					  FormatError::FERR_INVALID_HEADER);
	VectorCodebookHeader invalid = expected;
	invalid.subspace_count = 0;
	expectFormatError([&] { static_cast<void>(VectorCodebookHeaderCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = expected;
	invalid.subspace_count = invalid.dimensions + 1U;
	expectFormatError([&] { static_cast<void>(VectorCodebookHeaderCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = expected;
	invalid.dimensions = 3;
	invalid.subspace_count = 2;
	invalid.payload_bytes = 36;
	expectFormatError([&] { static_cast<void>(VectorCodebookHeaderCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableVectorCodebookTest, ValidatesTypedExtentHeadersAndPayloadsBeforePhysicalUse) {
	const std::vector<std::byte> block = payload();
	const VectorCodebookExtentHeader manifestHeader = VectorCodebookExtentHeaderCodec::decode(extentHeader(block));
	EXPECT_EQ(manifestHeader.kind, VectorCodebookExtentKind::VCEK_MANIFEST);
	EXPECT_EQ(manifestHeader.extent_id, VectorCodebookExtentIdCodec::manifestExtentId(17U));
	VectorCodebookExtentHeaderCodec::validateBlock(manifestHeader, block);

	const VectorCodebookExtentHeader expectedData = dataExtentHeader(block, SegmentId(72U));
	EXPECT_EQ(VectorCodebookExtentHeaderCodec::decode(VectorCodebookExtentHeaderCodec::encode(expectedData)),
			  expectedData);

	const auto encoded = extentHeader(block);
	expectFormatError(
			[&] {
				static_cast<void>(
						VectorCodebookExtentHeaderCodec::decode(std::span(encoded).first(encoded.size() - 1U)));
			},
			FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> oversized(encoded.begin(), encoded.end());
	oversized.push_back(std::byte{});
	expectFormatError([&] { static_cast<void>(VectorCodebookExtentHeaderCodec::decode(oversized)); },
					  FormatError::FERR_INVALID_HEADER);

	auto corruptChecksum = encoded;
	corruptChecksum[20U] ^= std::byte{1};
	expectFormatError([&] { static_cast<void>(VectorCodebookExtentHeaderCodec::decode(corruptChecksum)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
	auto corruptFixedField = encoded;
	LittleEndianCodec::writeU16(corruptFixedField, 10U, 1U);
	refreshExtentHeaderChecksum(corruptFixedField);
	expectFormatError([&] { static_cast<void>(VectorCodebookExtentHeaderCodec::decode(corruptFixedField)); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<std::byte> shortBlock = block;
	shortBlock.pop_back();
	expectFormatError([&] { VectorCodebookExtentHeaderCodec::validateBlock(manifestHeader, shortBlock); },
					  FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> corruptBlock = block;
	corruptBlock.front() ^= std::byte{1};
	expectFormatError([&] { VectorCodebookExtentHeaderCodec::validateBlock(manifestHeader, corruptBlock); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	VectorCodebookExtentHeader invalidData = expectedData;
	invalidData.extent_id = VectorCodebookExtentIdCodec::manifestExtentId(invalidData.codebook_id);
	expectFormatError([&] { static_cast<void>(VectorCodebookExtentHeaderCodec::encode(invalidData)); },
					  FormatError::FERR_INVALID_HEADER);
	invalidData = expectedData;
	invalidData.kind = static_cast<VectorCodebookExtentKind>(99U);
	expectFormatError([&] { static_cast<void>(VectorCodebookExtentHeaderCodec::encode(invalidData)); },
					  FormatError::FERR_INVALID_HEADER);
	VectorCodebookExtentHeader invalidManifest = manifestHeader;
	invalidManifest.ordinal = 0U;
	expectFormatError([&] { static_cast<void>(VectorCodebookExtentHeaderCodec::encode(invalidManifest)); },
					  FormatError::FERR_INVALID_HEADER);
	invalidData = expectedData;
	invalidData.ordinal = RSS_VECTOR_CODEBOOK_MANIFEST_ORDINAL;
	expectFormatError([&] { static_cast<void>(VectorCodebookExtentHeaderCodec::encode(invalidData)); },
					  FormatError::FERR_INVALID_HEADER);
	invalidData = expectedData;
	invalidData.block_bytes = 0U;
	invalidData.block_crc = 0U;
	expectFormatError([&] { static_cast<void>(VectorCodebookExtentHeaderCodec::encode(invalidData)); },
					  FormatError::FERR_INVALID_HEADER);

	const std::array<std::byte, 1U> emptyBlock{};
	VectorCodebookExtentHeader emptyManifest = manifestHeader;
	emptyManifest.block_bytes = 0U;
	emptyManifest.block_crc = graph::utils::calculateCrc(emptyBlock.data(), 0U);
	VectorCodebookExtentHeaderCodec::validateBlock(emptyManifest, {});
}

TEST(RelocatableVectorCodebookTest, RejectsMalformedInlineAndMultiExtentManifestLayouts) {
	VectorCodebookManifest inlineManifest;
	inlineManifest.header = header();
	inlineManifest.inline_payload = payload();
	const std::vector<std::byte> encodedInline = VectorCodebookManifestCodec::encode(inlineManifest);
	EXPECT_EQ(VectorCodebookManifestCodec::decode(encodedInline), inlineManifest);
	EXPECT_EQ(VectorCodebookManifestCodec::requiredDataExtentCount(0U), 0U);
	EXPECT_EQ(VectorCodebookManifestCodec::requiredDataExtentCount(RSS_VECTOR_CODEBOOK_INLINE_PAYLOAD_BYTES), 0U);
	EXPECT_EQ(VectorCodebookManifestCodec::requiredDataExtentCount(RSS_VECTOR_CODEBOOK_INLINE_PAYLOAD_BYTES + 1U), 1U);

	VectorCodebookManifest invalidInline = inlineManifest;
	invalidInline.inline_payload.clear();
	expectFormatError([&] { static_cast<void>(VectorCodebookManifestCodec::encode(invalidInline)); },
					  FormatError::FERR_INVALID_HEADER);

	const VectorCodebookManifest multi = multiExtentManifest();
	const std::vector<std::byte> encodedMulti = VectorCodebookManifestCodec::encode(multi);
	EXPECT_EQ(VectorCodebookManifestCodec::decode(encodedMulti), multi);
	VectorCodebookManifest wrongCount = multi;
	wrongCount.entries.push_back({VectorCodebookExtentIdCodec::dataExtentId(SegmentId(73U)), 1U, 0U});
	expectFormatError([&] { static_cast<void>(VectorCodebookManifestCodec::encode(wrongCount)); },
					  FormatError::FERR_INVALID_HEADER);
	VectorCodebookManifest invalidEntry = multi;
	invalidEntry.entries.front().extent_id = {};
	expectFormatError([&] { static_cast<void>(VectorCodebookManifestCodec::encode(invalidEntry)); },
					  FormatError::FERR_INVALID_HEADER);
	invalidEntry = multi;
	invalidEntry.entries.front().extent_id = VectorCodebookExtentIdCodec::manifestExtentId(17U);
	expectFormatError([&] { static_cast<void>(VectorCodebookManifestCodec::encode(invalidEntry)); },
					  FormatError::FERR_INVALID_HEADER);
	VectorCodebookManifest oversizedInline = multi;
	oversizedInline.entries.clear();
	oversizedInline.inline_payload.assign(oversizedInline.header.payload_bytes, std::byte{0xA5});
	expectFormatError([&] { static_cast<void>(VectorCodebookManifestCodec::encode(oversizedInline)); },
					  FormatError::FERR_INVALID_HEADER);
	VectorCodebookManifest mixedLayout = multi;
	mixedLayout.inline_payload = {std::byte{0xA5}};
	expectFormatError([&] { static_cast<void>(VectorCodebookManifestCodec::encode(mixedLayout)); },
					  FormatError::FERR_INVALID_HEADER);
	VectorCodebookManifest smallMulti = inlineManifest;
	smallMulti.inline_payload.clear();
	smallMulti.entries = {{VectorCodebookExtentIdCodec::dataExtentId(SegmentId(73U)),
						   static_cast<uint32_t>(payload().size()), inlineManifest.header.payload_crc}};
	expectFormatError([&] { static_cast<void>(VectorCodebookManifestCodec::encode(smallMulti)); },
					  FormatError::FERR_INVALID_HEADER);
	VectorCodebookManifest zeroPayload = inlineManifest;
	zeroPayload.header.payload_bytes = 0U;
	zeroPayload.inline_payload.clear();
	expectFormatError([&] { static_cast<void>(VectorCodebookManifestCodec::encode(zeroPayload)); },
					  FormatError::FERR_INVALID_HEADER);
	VectorCodebookManifest zeroCodebook = inlineManifest;
	zeroCodebook.header.codebook_id = 0U;
	expectFormatError([&] { static_cast<void>(VectorCodebookManifestCodec::encode(zeroCodebook)); },
					  FormatError::FERR_INVALID_HEADER);

	expectFormatError(
			[&] {
				static_cast<void>(VectorCodebookManifestCodec::decode(
						std::span(encodedInline).first(RSS_VECTOR_CODEBOOK_MANIFEST_PREFIX_BYTES - 1U)));
			},
			FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> inconsistentCount = encodedInline;
	LittleEndianCodec::writeU32(inconsistentCount, RSS_VECTOR_CODEBOOK_HEADER_BYTES, 1U);
	expectFormatError([&] { static_cast<void>(VectorCodebookManifestCodec::decode(inconsistentCount)); },
					  FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> shortInline = encodedInline;
	shortInline.pop_back();
	expectFormatError([&] { static_cast<void>(VectorCodebookManifestCodec::decode(shortInline)); },
					  FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> trailingMulti = encodedMulti;
	trailingMulti.push_back(std::byte{});
	expectFormatError([&] { static_cast<void>(VectorCodebookManifestCodec::decode(trailingMulti)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableVectorCodebookTest, RejectsManifestEntryTablesThatCannotFitOneImmutableExtent) {
	VectorCodebookManifest oversized;
	oversized.header.codebook_id = 17U;
	oversized.header.generation = Generation(1U);
	oversized.header.element_type = VectorCodebookElementType::VCET_UINT8;
	oversized.header.dimensions = 1U;
	oversized.header.subspace_count = 1U;
	oversized.header.centroid_count = std::numeric_limits<uint32_t>::max();
	oversized.header.element_bytes = 1U;
	oversized.header.payload_bytes = std::numeric_limits<uint32_t>::max();
	const size_t extentCount = VectorCodebookManifestCodec::requiredDataExtentCount(oversized.header.payload_bytes);
	ASSERT_GT(extentCount * RSS_VECTOR_CODEBOOK_MANIFEST_ENTRY_BYTES + RSS_VECTOR_CODEBOOK_MANIFEST_PREFIX_BYTES,
			  RSS_VECTOR_CODEBOOK_MAX_BLOCK_BYTES);
	oversized.entries.reserve(extentCount);
	uint64_t remaining = oversized.header.payload_bytes;
	for (size_t ordinal = 0U; ordinal < extentCount; ++ordinal) {
		const uint32_t blockBytes =
				static_cast<uint32_t>(std::min<uint64_t>(remaining, RSS_VECTOR_CODEBOOK_MAX_BLOCK_BYTES));
		oversized.entries.push_back(
				{VectorCodebookExtentIdCodec::dataExtentId(SegmentId(100U + ordinal)), blockBytes, 0U});
		remaining -= blockBytes;
	}
	EXPECT_EQ(remaining, 0U);
	expectFormatError([&] { static_cast<void>(VectorCodebookManifestCodec::encode(oversized)); },
					  FormatError::FERR_INVALID_HEADER);
}

class RelocatableVectorCodebookStoreTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::filesystem::path walPath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<std::fstream> walFile;
	std::shared_ptr<StorageIO> io;
	std::shared_ptr<StorageIO> walIo;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_vector_codebook_" + to_string(uuid));
		walPath = path.string() + ".foreign";
		file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		walFile = std::make_shared<std::fstream>(walPath,
												 std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		ASSERT_TRUE(walFile->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		walIo = std::make_shared<StorageIO>(walFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		walIo.reset();
		if (file && file->is_open()) {
			file->close();
		}
		if (walFile && walFile->is_open()) {
			walFile->close();
		}
		std::error_code error;
		std::filesystem::remove(path, error);
		std::filesystem::remove(walPath, error);
	}
};

TEST_F(RelocatableVectorCodebookStoreTest, MaterializesReopensAndRejectsDuplicateImmutableCodebooks) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	const std::vector<std::byte> expectedPayload = payload();
	const VectorCodebook expected{header(), expectedPayload};
	const Superblock published = store.writeImmutable(request(), expectedPayload, PhysicalSlotId(40),
													  PhysicalSlotId(40), LogSequenceNumber(2));
	EXPECT_EQ(published.generation, Generation(2));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(store.read(snapshot, 17), expected);
	snapshot.release();
	expectFormatError(
			[&] {
				static_cast<void>(store.writeImmutable(request(), expectedPayload, PhysicalSlotId(40),
													   PhysicalSlotId(40), LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_STATE);
	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator reopenedSegments(io, *reopened);
	VectorCodebookStore reopenedStore(reopenedSegments, *reopened);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, 17), expected);
}

TEST_F(RelocatableVectorCodebookStoreTest, PreparesCodebookExtentsBeforeOneCombinedCatalogAndExtentPublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	const MultiExtentCodebookFixture expected = multiExtentCodebook();
	const VectorCodebookPreparedWrite candidate =
			VectorCodebookStore::prepareImmutable(expected.request, expected.payload);

	ASSERT_EQ(candidate.codebookId(), expected.request.codebook_id);
	ASSERT_EQ(candidate.generation(), expected.request.generation);
	ASSERT_EQ(candidate.extents().size(), expected.request.data_extents.size() + 1U);
	ASSERT_TRUE(std::ranges::is_sorted(candidate.extents(), {}, &VectorCodebookPreparedExtent::segment_id));
	ASSERT_TRUE(std::ranges::is_sorted(candidate.extentLocations(), {}, &ExtentLocation::extent_id));
	const std::vector<CatalogSlotReservationRequest> reservations = candidate.catalogReservations();
	ASSERT_EQ(reservations.size(), candidate.extents().size());

	const std::optional<CatalogSlotReservations> published = metadata->reserveFirstFreeAndUpsertCatalogAndLocations(
			reservations, {}, candidate.extentLocations(), PhysicalSlotId(40U), PhysicalSlotId(41U),
			LogSequenceNumber(2U), [&store, &candidate](std::span<const CatalogEntry> entries) {
				store.writePreparedImmutable(candidate, entries);
			});
	ASSERT_TRUE(published.has_value());
	EXPECT_EQ(published->superblock.generation, Generation(2U));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const VectorCodebook actual = store.read(snapshot, expected.request.codebook_id);
	EXPECT_EQ(actual.header, candidate.manifest().header);
	EXPECT_EQ(actual.payload, expected.payload);
}

TEST_F(RelocatableVectorCodebookStoreTest, RejectsPreparedCodebookEntriesThatDisagreeWithTheCandidate) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	const VectorCodebookPreparedWrite candidate = VectorCodebookStore::prepareImmutable(request(), payload());
	std::vector<CatalogEntry> entries(candidate.extents().size());
	for (size_t index = 0U; index < entries.size(); ++index) {
		entries[index] = {candidate.extents()[index].segment_id,
						  PhysicalSlotId(static_cast<uint64_t>(40U + index)),
						  SegmentKind::SGK_CODEBOOK_EXTENT,
						  candidate.codebookId(),
						  candidate.generation(),
						  SegmentState::SGS_SEALED,
						  LogSequenceNumber(2U)};
	}
	entries.front().segment_id = SegmentId(99U);
	expectFormatError([&] { store.writePreparedImmutable(candidate, entries); }, FormatError::FERR_INVALID_STATE);

	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findExtentLocationOptional(
								 snapshot, VectorCodebookExtentIdCodec::manifestExtentId(candidate.codebookId()))
						 .has_value());
}

TEST(RelocatableVectorCodebookExtentIdCodecTest, UsesDisjointManifestAndDataNamespaces) {
	constexpr uint64_t codebookId = 17U;
	const SegmentId dataSegment(codebookId);
	const ExtentId manifest = VectorCodebookExtentIdCodec::manifestExtentId(codebookId);
	const ExtentId data = VectorCodebookExtentIdCodec::dataExtentId(dataSegment);

	EXPECT_TRUE(manifest.isValid());
	EXPECT_TRUE(data.isValid());
	EXPECT_NE(manifest, data);
	EXPECT_TRUE(VectorCodebookExtentIdCodec::isManifestExtentForCodebook(manifest, codebookId));
	EXPECT_FALSE(VectorCodebookExtentIdCodec::isDataExtentId(manifest));
	EXPECT_TRUE(VectorCodebookExtentIdCodec::isDataExtentForSegment(data, dataSegment));
}

TEST(RelocatableVectorCodebookExtentIdCodecTest, RejectsZeroAndHighBitPayloads) {
	const SegmentId maximumSegment(RSS_VECTOR_CODEBOOK_EXTENT_PAYLOAD_MASK);
	const SegmentId highBitSegment(RSS_VECTOR_CODEBOOK_DATA_EXTENT_TAG);

	EXPECT_FALSE(VectorCodebookExtentIdCodec::canEncodeCodebookId(0U));
	EXPECT_FALSE(VectorCodebookExtentIdCodec::manifestExtentId(0U).isValid());
	EXPECT_TRUE(VectorCodebookExtentIdCodec::canEncodeCodebookId(RSS_VECTOR_CODEBOOK_EXTENT_PAYLOAD_MASK));
	EXPECT_TRUE(VectorCodebookExtentIdCodec::canEncodeDataSegment(maximumSegment));
	EXPECT_FALSE(VectorCodebookExtentIdCodec::canEncodeDataSegment(highBitSegment));
	EXPECT_FALSE(VectorCodebookExtentIdCodec::dataExtentId(highBitSegment).isValid());
	EXPECT_FALSE(VectorCodebookExtentIdCodec::isDataExtentId(ExtentId(RSS_VECTOR_CODEBOOK_DATA_EXTENT_TAG)));
}

TEST_F(RelocatableVectorCodebookStoreTest, PersistsAProductionShapeAcrossMultipleImmutableExtents) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	VectorCodebookWriteRequest large = request();
	large.dimensions = 768U;
	large.subspace_count = 96U;
	large.centroid_count = 256U;
	const size_t payloadBytes = static_cast<size_t>(large.dimensions) * large.centroid_count * sizeof(float);
	std::vector<std::byte> bytes(payloadBytes);
	for (size_t index = 0U; index < bytes.size(); ++index) {
		bytes[index] = std::byte{static_cast<uint8_t>(index % 251U)};
	}
	const size_t extentCount = VectorCodebookStore::requiredDataExtentCount(bytes.size());
	ASSERT_GT(extentCount, 1U);
	for (size_t ordinal = 0U; ordinal < extentCount; ++ordinal) {
		const SegmentId segmentId(72U + ordinal);
		large.data_extents.push_back({VectorCodebookExtentIdCodec::dataExtentId(segmentId), segmentId});
	}

	static_cast<void>(store.writeImmutable(large, bytes, PhysicalSlotId(40U),
										   PhysicalSlotId(static_cast<uint32_t>(40U + extentCount)),
										   LogSequenceNumber(2U)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const VectorCodebookManifest immutableManifest = store.manifest(snapshot, large.codebook_id);
	EXPECT_FALSE(immutableManifest.isInline());
	EXPECT_EQ(immutableManifest.entries.size(), extentCount);
	EXPECT_EQ(store.read(snapshot, large.codebook_id).payload, bytes);
	for (const VectorCodebookDataExtent &extent: large.data_extents) {
		const ExtentLocation location = metadata->findExtentLocation(snapshot, extent.extent_id);
		EXPECT_EQ(location.segment_id, extent.segment_id);
		EXPECT_EQ(metadata->findCatalog(snapshot, extent.segment_id).owner_id, large.codebook_id);
	}
	const VectorCodebookDataExtent relocatedExtent = large.data_extents.front();
	const std::optional<PhysicalSegmentRelocation> relocation =
			store.relocateExtent(large.codebook_id, relocatedExtent.extent_id, PhysicalSlotId(48U), PhysicalSlotId(63U),
								 LogSequenceNumber(3U));
	ASSERT_TRUE(relocation.has_value());
	EXPECT_EQ(store.read(snapshot, large.codebook_id).payload, bytes);
	EXPECT_THROW(static_cast<void>(segments.releaseRelocatedSource(
						 relocatedExtent.segment_id, relocation->destination.generation, LogSequenceNumber(4U))),
				 FormatException);
	snapshot.release();
	static_cast<void>(segments.releaseRelocatedSource(relocatedExtent.segment_id, relocation->destination.generation,
													  LogSequenceNumber(4U)));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findExtentLocation(current, relocatedExtent.extent_id).generation, Generation(2U));
	EXPECT_EQ(store.read(current, large.codebook_id).payload, bytes);
	current.release();
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator reopenedSegments(io, *reopened);
	VectorCodebookStore reopenedStore(reopenedSegments, *reopened);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, large.codebook_id).payload, bytes);
}

TEST_F(RelocatableVectorCodebookStoreTest, RelocatesPhysicalExtentWithoutChangingLogicalCodebookGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	const VectorCodebook expected{header(), payload()};
	static_cast<void>(store.writeImmutable(request(), expected.payload, PhysicalSlotId(40), PhysicalSlotId(40),
										   LogSequenceNumber(2)));

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	const std::optional<PhysicalSegmentRelocation> relocation = store.relocateExtent(
			expected.header.codebook_id, VectorCodebookExtentIdCodec::manifestExtentId(expected.header.codebook_id),
			PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3));
	ASSERT_TRUE(relocation.has_value());
	EXPECT_EQ(relocation->source.physical_slot, PhysicalSlotId(40));
	EXPECT_EQ(relocation->destination.physical_slot, PhysicalSlotId(41));
	EXPECT_EQ(relocation->destination.generation, Generation(2));
	EXPECT_EQ(store.read(oldSnapshot, expected.header.codebook_id), expected);
	EXPECT_THROW(static_cast<void>(segments.releaseRelocatedSource(SegmentId(71), Generation(2), LogSequenceNumber(4))),
				 FormatException);

	oldSnapshot.release();
	static_cast<void>(segments.releaseRelocatedSource(SegmentId(71), Generation(2), LogSequenceNumber(4)));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findExtentLocation(current,
										   VectorCodebookExtentIdCodec::manifestExtentId(expected.header.codebook_id))
					  .generation,
			  Generation(2));
	EXPECT_EQ(store.read(current, expected.header.codebook_id), expected);
	EXPECT_EQ(metadata->findCatalog(current, SegmentId(71)).physical_slot, PhysicalSlotId(41));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(current, PhysicalSlotId(40)));
	current.release();
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator reopenedSegments(io, *reopened);
	VectorCodebookStore reopenedStore(reopenedSegments, *reopened);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, expected.header.codebook_id), expected);
}

TEST_F(RelocatableVectorCodebookStoreTest, RecoversTypedExtentRelocationAtWalPublicationBoundaries) {
	const VectorCodebook expected{header(), payload()};
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	static_cast<void>(store.writeImmutable(request(), expected.payload, PhysicalSlotId(40), PhysicalSlotId(40),
										   LogSequenceNumber(2)));

	MetadataPublishOptions afterRecord;
	afterRecord.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(store.relocateExtent(
						 expected.header.codebook_id,
						 VectorCodebookExtentIdCodec::manifestExtentId(expected.header.codebook_id), PhysicalSlotId(41),
						 PhysicalSlotId(41), LogSequenceNumber(3), afterRecord)),
				 BootstrapPublishInterrupted);
	metadata.reset();
	auto beforeHeaderRecovery = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator beforeHeaderSegments(io, *beforeHeaderRecovery);
	VectorCodebookStore beforeHeaderStore(beforeHeaderSegments, *beforeHeaderRecovery);
	MetadataSnapshotPin beforeHeader = beforeHeaderRecovery->openSnapshot();
	EXPECT_EQ(beforeHeaderStore.read(beforeHeader, expected.header.codebook_id), expected);
	EXPECT_EQ(beforeHeaderRecovery
					  ->findExtentLocation(beforeHeader,
										   VectorCodebookExtentIdCodec::manifestExtentId(expected.header.codebook_id))
					  .generation,
			  Generation(1));
	beforeHeader.release();

	MetadataPublishOptions afterHeader;
	afterHeader.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(beforeHeaderStore.relocateExtent(
						 expected.header.codebook_id,
						 VectorCodebookExtentIdCodec::manifestExtentId(expected.header.codebook_id), PhysicalSlotId(41),
						 PhysicalSlotId(41), LogSequenceNumber(3), afterHeader)),
				 BootstrapPublishInterrupted);
	beforeHeaderRecovery.reset();
	auto afterHeaderRecovery = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator afterHeaderSegments(io, *afterHeaderRecovery);
	VectorCodebookStore afterHeaderStore(afterHeaderSegments, *afterHeaderRecovery);
	MetadataSnapshotPin afterHeaderSnapshot = afterHeaderRecovery->openSnapshot();
	EXPECT_EQ(afterHeaderStore.read(afterHeaderSnapshot, expected.header.codebook_id), expected);
	EXPECT_EQ(afterHeaderRecovery
					  ->findExtentLocation(afterHeaderSnapshot,
										   VectorCodebookExtentIdCodec::manifestExtentId(expected.header.codebook_id))
					  .generation,
			  Generation(2));
	EXPECT_EQ(afterHeaderRecovery->findCatalog(afterHeaderSnapshot, SegmentId(71)).physical_slot, PhysicalSlotId(41));
}

TEST_F(RelocatableVectorCodebookStoreTest, RecordDurableBeforeWalHeaderLeavesNoCodebookAfterRecovery) {
	const std::vector<std::byte> expectedPayload = payload();
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(store.writeImmutable(request(), expectedPayload, PhysicalSlotId(40),
														PhysicalSlotId(40), LogSequenceNumber(2), interrupted)),
				 BootstrapPublishInterrupted);
	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin invisible = reopened->openSnapshot();
	expectFormatError(
			[&] {
				static_cast<void>(
						reopened->findExtentLocation(invisible, VectorCodebookExtentIdCodec::manifestExtentId(17U)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorCodebookStoreTest, WalHeaderDurablePublicationReplaysCompleteImmutableCodebook) {
	const std::vector<std::byte> expectedPayload = payload();
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(store.writeImmutable(request(), expectedPayload, PhysicalSlotId(40),
														PhysicalSlotId(40), LogSequenceNumber(2), interrupted)),
				 BootstrapPublishInterrupted);
	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator reopenedSegments(io, *reopened);
	VectorCodebookStore reopenedStore(reopenedSegments, *reopened);
	MetadataSnapshotPin visible = reopened->openSnapshot();
	const VectorCodebook expected{header(), expectedPayload};
	EXPECT_EQ(reopenedStore.read(visible, 17), expected);
}

TEST_F(RelocatableVectorCodebookStoreTest, ValidatesRequestsAndSupportsUint8Codebooks) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	VectorCodebookWriteRequest invalid = request();
	invalid.codebook_id = 0;
	expectFormatError(
			[&] {
				static_cast<void>(store.writeImmutable(invalid, payload(), PhysicalSlotId(40), PhysicalSlotId(40),
													   LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	invalid = request();
	invalid.element_type = static_cast<VectorCodebookElementType>(99);
	expectFormatError(
			[&] {
				static_cast<void>(store.writeImmutable(invalid, payload(), PhysicalSlotId(40), PhysicalSlotId(40),
													   LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_ENUM);
	invalid = request();
	invalid.manifest_segment_id = {};
	expectFormatError(
			[&] {
				static_cast<void>(store.writeImmutable(invalid, payload(), PhysicalSlotId(40), PhysicalSlotId(40),
													   LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						store.writeImmutable(request(), payload(), PhysicalSlotId(40), PhysicalSlotId(40), {}));
			},
			FormatError::FERR_INVALID_HEADER);
	MetadataSnapshotPin noCodebook = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.read(noCodebook, 0)); }, FormatError::FERR_INVALID_HEADER);
	noCodebook.release();

	VectorCodebookWriteRequest quantized = request();
	quantized.codebook_id = 18;
	quantized.manifest_segment_id = SegmentId(72);
	quantized.element_type = VectorCodebookElementType::VCET_UINT8;
	const std::vector<std::byte> quantizedPayload = {std::byte{1}, std::byte{2}, std::byte{3},
													 std::byte{4}, std::byte{5}, std::byte{6}};
	static_cast<void>(store.writeImmutable(quantized, quantizedPayload, PhysicalSlotId(40), PhysicalSlotId(40),
										   LogSequenceNumber(2)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const VectorCodebook read = store.read(snapshot, quantized.codebook_id);
	EXPECT_EQ(read.header.element_type, VectorCodebookElementType::VCET_UINT8);
	EXPECT_EQ(read.header.element_bytes, 1U);
	EXPECT_EQ(read.payload, quantizedPayload);
	snapshot.release();
	VectorCodebookWriteRequest exhausted = request();
	exhausted.codebook_id = 19;
	exhausted.manifest_segment_id = SegmentId(73);
	expectFormatError(
			[&] {
				static_cast<void>(store.writeImmutable(exhausted, payload(), PhysicalSlotId(40), PhysicalSlotId(40),
													   LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(store.relocateExtent(0, ExtentId(18), PhysicalSlotId(41), PhysicalSlotId(41),
													   LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						store.relocateExtent(18, {}, PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.relocateExtent(18, ExtentId(18), PhysicalSlotId(41), PhysicalSlotId(41), {}));
			},
			FormatError::FERR_INVALID_HEADER);
	EXPECT_FALSE(store.relocateExtent(18, VectorCodebookExtentIdCodec::manifestExtentId(18U), PhysicalSlotId(40),
									  PhysicalSlotId(40), LogSequenceNumber(3))
						 .has_value());
}

TEST_F(RelocatableVectorCodebookStoreTest, RejectsCatalogBoundHeadersAndPayloadsThatDisagreeWithTheExtent) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	std::vector<std::byte> block = manifestBlock();
	LittleEndianCodec::writeU64(block, 40U, 18U);
	LittleEndianCodec::writeU32(block, 32U, 0U);
	LittleEndianCodec::writeU32(block, 32U, graph::utils::calculateCrc(block.data(), RSS_VECTOR_CODEBOOK_HEADER_BYTES));
	PhysicalSegmentAllocationRequest allocation = rawRequest(extentHeader(block), block);
	const ExtentLocation location = extent();
	ASSERT_TRUE(segments.allocateExtentSegments(std::span(&allocation, 1), std::span(&location, 1), PhysicalSlotId(40),
												PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.read(snapshot, 17)); }, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableVectorCodebookStoreTest, RejectsPayloadChecksumMismatchesInsideValidExtentImages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	std::vector<std::byte> block = manifestBlock();
	LittleEndianCodec::writeU32(block, 28U, LittleEndianCodec::readU32(block, 28U) ^ 1U);
	LittleEndianCodec::writeU32(block, 32U, 0U);
	LittleEndianCodec::writeU32(block, 32U, graph::utils::calculateCrc(block.data(), RSS_VECTOR_CODEBOOK_HEADER_BYTES));
	PhysicalSegmentAllocationRequest allocation = rawRequest(extentHeader(block), block);
	const ExtentLocation location = extent();
	ASSERT_TRUE(segments.allocateExtentSegments(std::span(&allocation, 1), std::span(&location, 1), PhysicalSlotId(40),
												PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.read(snapshot, 17)); }, FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST_F(RelocatableVectorCodebookStoreTest, RejectsPartialExtentDirectoryEntriesAndWrongSubtypeHeaderLengths) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	static_cast<void>(
			store.writeImmutable(request(), payload(), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)));
	ExtentLocation partial = extent();
	partial.page_count = 1;
	static_cast<void>(metadata->upsertLocations({}, std::span(&partial, 1), LogSequenceNumber(3)));
	MetadataSnapshotPin partialSnapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.read(partialSnapshot, 17)); }, FormatError::FERR_INVALID_STATE);
	partialSnapshot.release();

	metadata.reset();
	file->close();
	walFile->close();
	file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	walFile =
			std::make_shared<std::fstream>(walPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(file->is_open());
	ASSERT_TRUE(walFile->is_open());
	io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	walIo = std::make_shared<StorageIO>(walFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto fresh = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator freshSegments(io, *fresh);
	VectorCodebookStore freshStore(freshSegments, *fresh);
	const std::array<std::byte, 8> shortHeader{};
	PhysicalSegmentAllocationRequest malformed;
	malformed.segment_id = SegmentId(71);
	malformed.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
	malformed.owner_id = 17;
	malformed.generation = Generation(1);
	malformed.header_bytes = RSS_ENVELOPE_BYTES + shortHeader.size();
	malformed.header_extension.assign(shortHeader.begin(), shortHeader.end());
	const std::vector<std::byte> bytes = payload();
	malformed.payload.assign(bytes.begin(), bytes.end());
	const ExtentLocation location = extent();
	ASSERT_TRUE(freshSegments
						.allocateExtentSegments(std::span(&malformed, 1), std::span(&location, 1), PhysicalSlotId(40),
												PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin malformedSnapshot = fresh->openSnapshot();
	expectFormatError([&] { static_cast<void>(freshStore.read(malformedSnapshot, 17)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorCodebookStoreTest, ReusesCachedMultiExtentPayloadAcrossReadsAndPhysicalRelocation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookWriteRequest large = request();
	large.dimensions = 768U;
	large.subspace_count = 96U;
	large.centroid_count = 256U;
	const size_t payloadBytes = static_cast<size_t>(large.dimensions) * large.centroid_count * sizeof(float);
	std::vector<std::byte> bytes(payloadBytes);
	for (size_t index = 0U; index < bytes.size(); ++index) {
		bytes[index] = std::byte{static_cast<uint8_t>(index % 251U)};
	}
	const size_t extentCount = VectorCodebookStore::requiredDataExtentCount(bytes.size());
	for (size_t ordinal = 0U; ordinal < extentCount; ++ordinal) {
		const SegmentId segmentId(72U + ordinal);
		large.data_extents.push_back({VectorCodebookExtentIdCodec::dataExtentId(segmentId), segmentId});
	}
	VectorCodebookStore store(segments, *metadata, bytes.size() * 2U, 4U);
	static_cast<void>(store.writeImmutable(large, bytes, PhysicalSlotId(40U),
										   PhysicalSlotId(static_cast<uint32_t>(40U + extentCount)),
										   LogSequenceNumber(2U)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	io->resetStats();
	const std::shared_ptr<const VectorCodebook> first =
			store.readSharedBounded(snapshot, large.codebook_id, bytes.size(), extentCount);
	const StorageIOStats firstRead = io->stats();
	io->resetStats();
	const std::shared_ptr<const VectorCodebook> second =
			store.readSharedBounded(snapshot, large.codebook_id, bytes.size(), extentCount);
	const StorageIOStats cachedRead = io->stats();
	EXPECT_EQ(first, second);
	EXPECT_EQ(first->payload, bytes);
	EXPECT_GT(cachedRead.read_bytes, 0U);
	EXPECT_LT(cachedRead.read_bytes, firstRead.read_bytes);
	snapshot.release();

	const VectorCodebookDataExtent relocated = large.data_extents.front();
	ASSERT_TRUE(store.relocateExtent(large.codebook_id, relocated.extent_id, PhysicalSlotId(48U), PhysicalSlotId(63U),
									 LogSequenceNumber(3U))
						.has_value());
	MetadataSnapshotPin relocatedSnapshot = metadata->openSnapshot();
	const std::shared_ptr<const VectorCodebook> afterRelocation =
			store.readSharedBounded(relocatedSnapshot, large.codebook_id, bytes.size(), extentCount);
	EXPECT_EQ(afterRelocation, first);
	EXPECT_EQ(afterRelocation->header.generation, Generation(1U));
}

TEST_F(RelocatableVectorCodebookStoreTest, ValidatesCurrentRootManifestBeforeReturningACachedPayload) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata, 1024U, 4U);
	static_cast<void>(store.writeImmutable(request(), payload(), PhysicalSlotId(40U), PhysicalSlotId(40U),
										   LogSequenceNumber(2U)));
	MetadataSnapshotPin initial = metadata->openSnapshot();
	const std::shared_ptr<const VectorCodebook> cached = store.readSharedBounded(initial, 17U, 1024U, 0U);
	ASSERT_NE(cached, nullptr);
	initial.release();

	ExtentLocation malformed = extent();
	malformed.page_count = 1U;
	static_cast<void>(metadata->upsertLocations({}, std::span(&malformed, 1U), LogSequenceNumber(3U)));
	MetadataSnapshotPin current = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.readSharedBounded(current, 17U, 1024U, 0U)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorCodebookStoreTest, SeparatesLogicalGenerationsAndEvictsByEntryAndByteBounds) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore generationCache(segments, *metadata, 1024U, 4U);
	static_cast<void>(generationCache.writeImmutable(request(), payload(), PhysicalSlotId(40U), PhysicalSlotId(40U),
													 LogSequenceNumber(2U)));
	MetadataSnapshotPin firstSnapshot = metadata->openSnapshot();
	const std::shared_ptr<const VectorCodebook> firstGeneration =
			generationCache.readSharedBounded(firstSnapshot, 17U, 1024U, 0U);
	firstSnapshot.release();

	std::vector<std::byte> replacementPayload(24U, std::byte{0x2A});
	VectorCodebookHeader replacementHeader = header();
	replacementHeader.generation = Generation(2U);
	replacementHeader.payload_crc = graph::utils::calculateCrc(replacementPayload.data(), replacementPayload.size());
	VectorCodebookManifest replacementManifest;
	replacementManifest.header = replacementHeader;
	replacementManifest.inline_payload = replacementPayload;
	const std::vector<std::byte> replacementBlock = VectorCodebookManifestCodec::encode(replacementManifest);
	VectorCodebookExtentHeader replacementExtentHeader;
	replacementExtentHeader.kind = VectorCodebookExtentKind::VCEK_MANIFEST;
	replacementExtentHeader.codebook_id = 17U;
	replacementExtentHeader.codebook_generation = Generation(2U);
	replacementExtentHeader.extent_id = VectorCodebookExtentIdCodec::manifestExtentId(17U);
	replacementExtentHeader.ordinal = RSS_VECTOR_CODEBOOK_MANIFEST_ORDINAL;
	replacementExtentHeader.block_bytes = static_cast<uint32_t>(replacementBlock.size());
	replacementExtentHeader.block_crc = graph::utils::calculateCrc(replacementBlock.data(), replacementBlock.size());
	const VectorCodebookExtentHeaderCodec::Bytes replacementExtentBytes =
			VectorCodebookExtentHeaderCodec::encode(replacementExtentHeader);
	PhysicalSegmentAllocationRequest replacement;
	replacement.segment_id = SegmentId(72U);
	replacement.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
	replacement.owner_id = 17U;
	replacement.generation = Generation(2U);
	replacement.header_bytes = RSS_ENVELOPE_BYTES + replacementExtentBytes.size();
	replacement.header_extension.assign(replacementExtentBytes.begin(), replacementExtentBytes.end());
	replacement.payload.assign(replacementBlock.begin(), replacementBlock.end());
	ASSERT_TRUE(segments.allocate(replacement, PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(3U))
						.has_value());
	ExtentLocation replacementLocation = {VectorCodebookExtentIdCodec::manifestExtentId(17U), SegmentId(72U),
										  RSS_PAGE_POOL_FIRST_DATA_FRAME, RSS_PAGE_POOL_DATA_FRAME_COUNT,
										  Generation(2U)};
	static_cast<void>(metadata->upsertLocations({}, std::span(&replacementLocation, 1U), LogSequenceNumber(4U)));
	MetadataSnapshotPin secondSnapshot = metadata->openSnapshot();
	const std::shared_ptr<const VectorCodebook> secondGeneration =
			generationCache.readSharedBounded(secondSnapshot, 17U, 1024U, 0U);
	EXPECT_NE(secondGeneration, firstGeneration);
	EXPECT_EQ(secondGeneration->header.generation, Generation(2U));
	EXPECT_EQ(secondGeneration->payload, replacementPayload);
	secondSnapshot.release();

	metadata.reset();
	file->close();
	walFile->close();
	file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	walFile =
			std::make_shared<std::fstream>(walPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(file->is_open());
	ASSERT_TRUE(walFile->is_open());
	io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	walIo = std::make_shared<StorageIO>(walFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator boundedSegments(io, *metadata);
	auto publishInline = [&](uint64_t codebookId, uint64_t segmentId, uint32_t slot, uint64_t lsn) {
		VectorCodebookWriteRequest codebookRequest = request();
		codebookRequest.codebook_id = codebookId;
		codebookRequest.manifest_segment_id = SegmentId(segmentId);
		VectorCodebookStore writer(boundedSegments, *metadata);
		static_cast<void>(writer.writeImmutable(codebookRequest, payload(), PhysicalSlotId(slot), PhysicalSlotId(slot),
												LogSequenceNumber(lsn)));
	};
	publishInline(17U, 71U, 40U, 2U);
	publishInline(18U, 72U, 41U, 3U);
	publishInline(19U, 73U, 42U, 4U);
	MetadataSnapshotPin boundedSnapshot = metadata->openSnapshot();
	VectorCodebookStore entryBounded(boundedSegments, *metadata, 1024U, 1U);
	const std::shared_ptr<const VectorCodebook> entryFirst =
			entryBounded.readSharedBounded(boundedSnapshot, 17U, 1024U, 0U);
	static_cast<void>(entryBounded.readSharedBounded(boundedSnapshot, 18U, 1024U, 0U));
	EXPECT_NE(entryBounded.readSharedBounded(boundedSnapshot, 17U, 1024U, 0U), entryFirst);

	VectorCodebookStore byteBounded(boundedSegments, *metadata, payload().size(), 3U);
	const std::shared_ptr<const VectorCodebook> byteFirst =
			byteBounded.readSharedBounded(boundedSnapshot, 17U, 1024U, 0U);
	static_cast<void>(byteBounded.readSharedBounded(boundedSnapshot, 18U, 1024U, 0U));
	EXPECT_NE(byteBounded.readSharedBounded(boundedSnapshot, 17U, 1024U, 0U), byteFirst);

	VectorCodebookStore disabled(boundedSegments, *metadata);
	const std::shared_ptr<const VectorCodebook> disabledFirst =
			disabled.readSharedBounded(boundedSnapshot, 19U, 1024U, 0U);
	const std::shared_ptr<const VectorCodebook> disabledSecond =
			disabled.readSharedBounded(boundedSnapshot, 19U, 1024U, 0U);
	EXPECT_NE(disabledFirst, disabledSecond);
	EXPECT_THROW(static_cast<void>(VectorCodebookStore(boundedSegments, *metadata, 1U, 0U)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(VectorCodebookStore(boundedSegments, *metadata, 0U, 1U)), std::invalid_argument);
}

TEST_F(RelocatableVectorCodebookStoreTest, EnforcesReadAndValidationBoundsBeforeMaterializingCodebookPayload) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	static_cast<void>(store.writeImmutable(request(), payload(), PhysicalSlotId(40U), PhysicalSlotId(40U),
										   LogSequenceNumber(2U)));
	const MultiExtentCodebookFixture multi = multiExtentCodebook(18U, SegmentId(72U), SegmentId(73U));
	static_cast<void>(store.writeImmutable(multi.request, multi.payload, PhysicalSlotId(41U), PhysicalSlotId(42U),
										   LogSequenceNumber(3U)));

	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.readBounded(snapshot, 17U, payload().size() - 1U, 0U)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { store.validate(snapshot, 17U, payload().size() - 1U, 0U); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store.readBounded(snapshot, 18U, multi.payload.size(), 0U)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { store.validate(snapshot, 18U, multi.payload.size(), 0U); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_NO_THROW(store.validate(snapshot, 17U, payload().size(), 0U));
	EXPECT_NO_THROW(store.validate(snapshot, 18U, multi.payload.size(), 1U));
}

TEST_F(RelocatableVectorCodebookStoreTest, LeavesPayloadsLargerThanTheCacheBudgetUncached) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore writer(segments, *metadata);
	static_cast<void>(writer.writeImmutable(request(), payload(), PhysicalSlotId(40U), PhysicalSlotId(40U),
											LogSequenceNumber(2U)));

	VectorCodebookStore bounded(segments, *metadata, payload().size() - 1U, 1U);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::shared_ptr<const VectorCodebook> first = bounded.readSharedBounded(snapshot, 17U, 1024U, 0U);
	const std::shared_ptr<const VectorCodebook> second = bounded.readSharedBounded(snapshot, 17U, 1024U, 0U);
	EXPECT_NE(first, second);
}

TEST_F(RelocatableVectorCodebookStoreTest, CoalescesConcurrentLoadsAfterTheInitialCacheLookup) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	const size_t payloadBytes = 4U * 1024U * 1024U;
	VectorCodebookWriteRequest large = request();
	large.dimensions = 1U;
	large.subspace_count = 1U;
	large.centroid_count = static_cast<uint32_t>(payloadBytes / sizeof(float));
	const size_t extentCount = VectorCodebookStore::requiredDataExtentCount(payloadBytes);
	for (size_t ordinal = 0U; ordinal < extentCount; ++ordinal) {
		const SegmentId segmentId(72U + ordinal);
		large.data_extents.push_back({VectorCodebookExtentIdCodec::dataExtentId(segmentId), segmentId});
	}
	std::vector<std::byte> bytes(payloadBytes, std::byte{0x7A});
	VectorCodebookStore writer(segments, *metadata);
	static_cast<void>(writer.writeImmutable(large, bytes, PhysicalSlotId(40U),
											PhysicalSlotId(static_cast<uint32_t>(40U + extentCount)),
											LogSequenceNumber(2U)));

	VectorCodebookStore cache(segments, *metadata, payloadBytes * 2U, 2U);
	std::shared_ptr<const VectorCodebook> first;
	std::shared_ptr<const VectorCodebook> second;
	std::exception_ptr failure;
	std::mutex failureMutex;
	std::atomic<bool> firstStarted{false};
	auto load = [&](std::shared_ptr<const VectorCodebook> &result, bool publishStart) {
		try {
			const MetadataSnapshotPin snapshot = metadata->openSnapshot();
			if (publishStart) {
				firstStarted.store(true, std::memory_order_release);
			}
			result = cache.readSharedBounded(snapshot, large.codebook_id, bytes.size(), extentCount);
		} catch (...) {
			std::lock_guard lock(failureMutex);
			if (!failure) {
				failure = std::current_exception();
			}
		}
	};

	io->resetStats();
	std::thread firstReader(load, std::ref(first), true);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	const uint64_t requiredReadBytes = static_cast<uint64_t>(RSS_VECTOR_CODEBOOK_MAX_BLOCK_BYTES) * 2U;
	while ((!firstStarted.load(std::memory_order_acquire) || io->stats().read_bytes < requiredReadBytes) &&
		   std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
	}
	const bool firstWasLoading = firstStarted.load(std::memory_order_acquire) &&
								 io->stats().read_bytes >= requiredReadBytes && io->stats().read_bytes < bytes.size();
	std::thread secondReader(load, std::ref(second), false);
	firstReader.join();
	secondReader.join();
	{
		std::lock_guard lock(failureMutex);
		if (failure) {
			std::rethrow_exception(failure);
		}
	}
	ASSERT_TRUE(firstWasLoading);
	ASSERT_NE(first, nullptr);
	EXPECT_EQ(first, second);
}

TEST_F(RelocatableVectorCodebookStoreTest, RejectsDuplicateMultiExtentRequestsBeforeAnyPublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);

	VectorCodebookWriteRequest invalid = request();
	invalid.dimensions = 1U;
	invalid.subspace_count = 1U;
	invalid.centroid_count =
			static_cast<uint32_t>((static_cast<size_t>(RSS_VECTOR_CODEBOOK_MAX_BLOCK_BYTES) * 2U) / sizeof(float));
	invalid.data_extents = {{VectorCodebookExtentIdCodec::dataExtentId(SegmentId(72U)), SegmentId(72U)},
							{VectorCodebookExtentIdCodec::dataExtentId(SegmentId(72U)), SegmentId(72U)}};
	const std::vector<std::byte> oversized(static_cast<size_t>(RSS_VECTOR_CODEBOOK_MAX_BLOCK_BYTES) * 2U,
										   std::byte{0x5A});
	expectFormatError(
			[&] {
				static_cast<void>(store.writeImmutable(invalid, oversized, PhysicalSlotId(40U), PhysicalSlotId(42U),
													   LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findExtentLocationOptional(snapshot, VectorCodebookExtentIdCodec::manifestExtentId(17U))
						 .has_value());
}

TEST_F(RelocatableVectorCodebookStoreTest, RejectsMismatchedDataExtentHeadersForReadAndRelocation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	const MultiExtentCodebookFixture fixture = multiExtentCodebook();
	static_cast<void>(store.writeImmutable(fixture.request, fixture.payload, PhysicalSlotId(40U), PhysicalSlotId(41U),
										   LogSequenceNumber(2U)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const PhysicalSegmentContents contents = segments.read(snapshot, fixture.request.data_extents.front().segment_id);
	VectorCodebookExtentHeader corrupt = VectorCodebookExtentHeaderCodec::decode(contents.header_extension);
	corrupt.ordinal = 1U;
	const VectorCodebookExtentHeaderCodec::Bytes corruptHeader = VectorCodebookExtentHeaderCodec::encode(corrupt);
	snapshot.release();
	overwriteCodebookExtent(io, *metadata, fixture.request.data_extents.front().segment_id, corruptHeader,
							contents.payload);

	snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.read(snapshot, fixture.request.codebook_id)); },
					  FormatError::FERR_INVALID_STATE);
	snapshot.release();
	expectFormatError(
			[&] {
				static_cast<void>(store.relocateExtent(
						fixture.request.codebook_id, fixture.request.data_extents.front().extent_id,
						PhysicalSlotId(42U), PhysicalSlotId(42U), LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorCodebookStoreTest, RejectsRelocationOfAValidExtentOutsideTheImmutableManifest) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	const MultiExtentCodebookFixture fixture = multiExtentCodebook();
	static_cast<void>(store.writeImmutable(fixture.request, fixture.payload, PhysicalSlotId(40U), PhysicalSlotId(41U),
										   LogSequenceNumber(2U)));

	const SegmentId rogueSegment(73U);
	const ExtentId rogueExtent = VectorCodebookExtentIdCodec::dataExtentId(rogueSegment);
	VectorCodebookExtentHeader rogueHeader;
	rogueHeader.kind = VectorCodebookExtentKind::VCEK_DATA;
	rogueHeader.codebook_id = fixture.request.codebook_id;
	rogueHeader.codebook_generation = fixture.request.generation;
	rogueHeader.extent_id = rogueExtent;
	rogueHeader.ordinal = 0U;
	rogueHeader.block_bytes = static_cast<uint32_t>(fixture.payload.size());
	rogueHeader.block_crc = graph::utils::calculateCrc(fixture.payload.data(), fixture.payload.size());
	const VectorCodebookExtentHeaderCodec::Bytes encodedHeader = VectorCodebookExtentHeaderCodec::encode(rogueHeader);
	PhysicalSegmentAllocationRequest rogue;
	rogue.segment_id = rogueSegment;
	rogue.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
	rogue.owner_id = fixture.request.codebook_id;
	rogue.generation = fixture.request.generation;
	rogue.header_bytes = static_cast<uint16_t>(RSS_ENVELOPE_BYTES + encodedHeader.size());
	rogue.header_extension.assign(encodedHeader.begin(), encodedHeader.end());
	rogue.payload.assign(fixture.payload.begin(), fixture.payload.end());
	const ExtentLocation rogueLocation = {rogueExtent, rogueSegment, RSS_PAGE_POOL_FIRST_DATA_FRAME,
										  RSS_PAGE_POOL_DATA_FRAME_COUNT, fixture.request.generation};
	ASSERT_TRUE(segments.allocateExtentSegments(std::span(&rogue, 1U), std::span(&rogueLocation, 1U),
												PhysicalSlotId(42U), PhysicalSlotId(42U), LogSequenceNumber(3U))
						.has_value());

	expectFormatError(
			[&] {
				static_cast<void>(store.relocateExtent(fixture.request.codebook_id, rogueExtent, PhysicalSlotId(43U),
													   PhysicalSlotId(43U), LogSequenceNumber(4U)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorCodebookStoreTest, RejectsExtentHeaderIdentityThatDisagreesWithTheCatalogOwner) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	const MultiExtentCodebookFixture fixture = multiExtentCodebook();
	static_cast<void>(store.writeImmutable(fixture.request, fixture.payload, PhysicalSlotId(40U), PhysicalSlotId(41U),
										   LogSequenceNumber(2U)));

	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const PhysicalSegmentContents contents = segments.read(snapshot, fixture.request.data_extents.front().segment_id);
	VectorCodebookExtentHeader corrupt = VectorCodebookExtentHeaderCodec::decode(contents.header_extension);
	corrupt.codebook_id = fixture.request.codebook_id + 1U;
	const VectorCodebookExtentHeaderCodec::Bytes corruptHeader = VectorCodebookExtentHeaderCodec::encode(corrupt);
	overwriteCodebookExtent(io, *metadata, fixture.request.data_extents.front().segment_id, corruptHeader,
							contents.payload);
	expectFormatError([&] { static_cast<void>(store.read(snapshot, fixture.request.codebook_id)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableVectorCodebookStoreTest, DetectsLogicalPayloadChecksumMismatchAfterVerifyingDataExtents) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	const MultiExtentCodebookFixture fixture = multiExtentCodebook();
	static_cast<void>(store.writeImmutable(fixture.request, fixture.payload, PhysicalSlotId(40U), PhysicalSlotId(41U),
										   LogSequenceNumber(2U)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	VectorCodebookManifest manifest = store.manifest(snapshot, fixture.request.codebook_id);
	EXPECT_NO_THROW(store.validate(snapshot, fixture.request.codebook_id, fixture.payload.size(), 1U));
	const PhysicalSegmentContents data = segments.read(snapshot, fixture.request.data_extents.front().segment_id);
	const PhysicalSegmentContents root = segments.read(snapshot, fixture.request.manifest_segment_id);
	VectorCodebookExtentHeader dataHeader = VectorCodebookExtentHeaderCodec::decode(data.header_extension);
	std::vector<std::byte> corruptedPayload = data.payload;
	corruptedPayload.front() ^= std::byte{0x01};
	dataHeader.block_crc = graph::utils::calculateCrc(corruptedPayload.data(), corruptedPayload.size());
	manifest.entries.front().block_crc = dataHeader.block_crc;
	const std::vector<std::byte> corruptedManifest = VectorCodebookManifestCodec::encode(manifest);
	VectorCodebookExtentHeader rootHeader = VectorCodebookExtentHeaderCodec::decode(root.header_extension);
	rootHeader.block_bytes = static_cast<uint32_t>(corruptedManifest.size());
	rootHeader.block_crc = graph::utils::calculateCrc(corruptedManifest.data(), corruptedManifest.size());
	const VectorCodebookExtentHeaderCodec::Bytes encodedData = VectorCodebookExtentHeaderCodec::encode(dataHeader);
	const VectorCodebookExtentHeaderCodec::Bytes encodedRoot = VectorCodebookExtentHeaderCodec::encode(rootHeader);
	snapshot.release();

	overwriteCodebookExtent(io, *metadata, fixture.request.data_extents.front().segment_id, encodedData,
							corruptedPayload);
	overwriteCodebookExtent(io, *metadata, fixture.request.manifest_segment_id, encodedRoot, corruptedManifest);
	snapshot = metadata->openSnapshot();
	expectFormatError([&] { store.validate(snapshot, fixture.request.codebook_id, fixture.payload.size(), 1U); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST_F(RelocatableVectorCodebookStoreTest, RejectsManifestGenerationAndDataExtentCrcMismatches) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata);
	const MultiExtentCodebookFixture fixture = multiExtentCodebook();
	static_cast<void>(store.writeImmutable(fixture.request, fixture.payload, PhysicalSlotId(40U), PhysicalSlotId(41U),
										   LogSequenceNumber(2U)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const PhysicalSegmentContents root = segments.read(snapshot, fixture.request.manifest_segment_id);
	VectorCodebookManifest generationMismatch = VectorCodebookManifestCodec::decode(root.payload);
	generationMismatch.header.generation = Generation(2U);
	const std::vector<std::byte> generationPayload = VectorCodebookManifestCodec::encode(generationMismatch);
	VectorCodebookExtentHeader rootHeader = VectorCodebookExtentHeaderCodec::decode(root.header_extension);
	rootHeader.block_bytes = static_cast<uint32_t>(generationPayload.size());
	rootHeader.block_crc = graph::utils::calculateCrc(generationPayload.data(), generationPayload.size());
	const VectorCodebookExtentHeaderCodec::Bytes generationHeader = VectorCodebookExtentHeaderCodec::encode(rootHeader);
	snapshot.release();
	overwriteCodebookExtent(io, *metadata, fixture.request.manifest_segment_id, generationHeader, generationPayload);

	snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.read(snapshot, fixture.request.codebook_id)); },
					  FormatError::FERR_INVALID_HEADER);
	snapshot.release();

	metadata.reset();
	file->close();
	walFile->close();
	file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	walFile =
			std::make_shared<std::fstream>(walPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(file->is_open());
	ASSERT_TRUE(walFile->is_open());
	io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	walIo = std::make_shared<StorageIO>(walFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator freshSegments(io, *metadata);
	VectorCodebookStore freshStore(freshSegments, *metadata);
	static_cast<void>(freshStore.writeImmutable(fixture.request, fixture.payload, PhysicalSlotId(40U),
												PhysicalSlotId(41U), LogSequenceNumber(2U)));

	snapshot = metadata->openSnapshot();
	const PhysicalSegmentContents freshRoot = freshSegments.read(snapshot, fixture.request.manifest_segment_id);
	VectorCodebookManifest crcMismatch = VectorCodebookManifestCodec::decode(freshRoot.payload);
	crcMismatch.entries.front().block_crc ^= UINT32_C(1);
	const std::vector<std::byte> crcPayload = VectorCodebookManifestCodec::encode(crcMismatch);
	VectorCodebookExtentHeader freshRootHeader = VectorCodebookExtentHeaderCodec::decode(freshRoot.header_extension);
	freshRootHeader.block_bytes = static_cast<uint32_t>(crcPayload.size());
	freshRootHeader.block_crc = graph::utils::calculateCrc(crcPayload.data(), crcPayload.size());
	const VectorCodebookExtentHeaderCodec::Bytes crcHeader = VectorCodebookExtentHeaderCodec::encode(freshRootHeader);
	snapshot.release();
	overwriteCodebookExtent(io, *metadata, fixture.request.manifest_segment_id, crcHeader, crcPayload);

	snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(freshStore.read(snapshot, fixture.request.codebook_id)); },
					  FormatError::FERR_INVALID_STATE);
	snapshot.release();
	expectFormatError(
			[&] {
				static_cast<void>(freshStore.relocateExtent(
						fixture.request.codebook_id, fixture.request.data_extents.front().extent_id,
						PhysicalSlotId(42U), PhysicalSlotId(42U), LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorCodebookStoreTest, RejectsCachedCodebookWhoseCurrentManifestHeaderWasCorrupted) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore store(segments, *metadata, 1024U, 4U);
	static_cast<void>(store.writeImmutable(request(), payload(), PhysicalSlotId(40U), PhysicalSlotId(40U),
										   LogSequenceNumber(2U)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	ASSERT_NE(store.readSharedBounded(snapshot, 17U, 1024U, 0U), nullptr);
	const PhysicalSegmentContents root = segments.read(snapshot, SegmentId(71U));
	VectorCodebookManifest corrupt = VectorCodebookManifestCodec::decode(root.payload);
	std::vector<std::byte> replacement = corrupt.inline_payload;
	replacement.front() ^= std::byte{0x01};
	corrupt.inline_payload = replacement;
	corrupt.header.payload_crc = graph::utils::calculateCrc(replacement.data(), replacement.size());
	const std::vector<std::byte> corruptPayload = VectorCodebookManifestCodec::encode(corrupt);
	VectorCodebookExtentHeader rootHeader = VectorCodebookExtentHeaderCodec::decode(root.header_extension);
	rootHeader.block_bytes = static_cast<uint32_t>(corruptPayload.size());
	rootHeader.block_crc = graph::utils::calculateCrc(corruptPayload.data(), corruptPayload.size());
	const VectorCodebookExtentHeaderCodec::Bytes corruptHeader = VectorCodebookExtentHeaderCodec::encode(rootHeader);
	snapshot.release();

	overwriteCodebookExtent(io, *metadata, SegmentId(71U), corruptHeader, corruptPayload);
	snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.readSharedBounded(snapshot, 17U, 1024U, 0U)); },
					  FormatError::FERR_INVALID_STATE);
}
