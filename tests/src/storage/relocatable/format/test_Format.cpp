/**
 * @file test_Format.cpp
 * @brief Golden-byte and validation tests for the isolated relocatable format codec.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <gtest/gtest.h>
#include <span>

#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	SegmentEnvelope allocatedEnvelope() {
		SegmentEnvelope envelope;
		envelope.header_bytes = 128;
		envelope.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
		envelope.state = SegmentState::SGS_SEALED;
		envelope.segment_id = SegmentId(0x0102030405060708ULL);
		envelope.owner_id = 0x1112131415161718ULL;
		envelope.generation = Generation(3);
		envelope.committed_lsn = LogSequenceNumber(9);
		envelope.payload_bytes = 32;
		envelope.payload_crc = 0xAABBCCDDU;
		return envelope;
	}

	void recalculateHeaderCrc(std::array<std::byte, RSS_ENVELOPE_BYTES> &bytes) {
		LittleEndianCodec::writeU32(bytes, 52, 0);
		const uint32_t crc = graph::utils::calculateCrc(bytes.data(), bytes.size());
		LittleEndianCodec::writeU32(bytes, 52, crc);
	}

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

} // namespace

TEST(RelocatableFormatTest, StrongIdentifiersAreExplicitAndComparable) {
	const SegmentId segment(7);
	const PageId page(7);
	const ExtentId extent(8);
	const Generation generation(9);
	const LogSequenceNumber lsn(10);

	EXPECT_TRUE(segment.isValid());
	EXPECT_FALSE(SegmentId().isValid());
	EXPECT_EQ(segment.value(), 7u);
	EXPECT_EQ(page.value(), 7u);
	EXPECT_LT(segment, SegmentId(8));
	EXPECT_LT(extent, ExtentId(9));
	EXPECT_LT(generation, Generation(10));
	EXPECT_LT(lsn, LogSequenceNumber(11));
}

TEST(RelocatableFormatTest, LittleEndianPrimitivesUsePortableCanonicalBytes) {
	std::array<std::byte, 14> bytes{};
	LittleEndianCodec::writeU16(bytes, 0, 0x1234U);
	LittleEndianCodec::writeU32(bytes, 2, 0x89ABCDEFU);
	LittleEndianCodec::writeU64(bytes, 6, 0x0123456789ABCDEFULL);

	const std::array<uint8_t, 14> expected = {0x34, 0x12, 0xEF, 0xCD, 0xAB, 0x89, 0xEF,
											  0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01};
	for (size_t index = 0; index < expected.size(); ++index) {
		EXPECT_EQ(std::to_integer<uint8_t>(bytes[index]), expected[index]);
	}

	EXPECT_EQ(LittleEndianCodec::readU16(bytes, 0), 0x1234U);
	EXPECT_EQ(LittleEndianCodec::readU32(bytes, 2), 0x89ABCDEFU);
	EXPECT_EQ(LittleEndianCodec::readU64(bytes, 6), 0x0123456789ABCDEFULL);
}

TEST(RelocatableFormatTest, LittleEndianPrimitivesRejectTruncatedBuffers) {
	std::array<std::byte, 7> writable{};
	const std::array<std::byte, 7> readable{};

	expectFormatError([&] { LittleEndianCodec::writeU16(writable, 7, 1); }, FormatError::FERR_BUFFER_TOO_SMALL);
	expectFormatError([&] { LittleEndianCodec::writeU16(writable, 8, 1); }, FormatError::FERR_BUFFER_TOO_SMALL);
	expectFormatError([&] { LittleEndianCodec::writeU32(writable, 5, 1); }, FormatError::FERR_BUFFER_TOO_SMALL);
	expectFormatError([&] { LittleEndianCodec::writeU64(writable, 0, 1); }, FormatError::FERR_BUFFER_TOO_SMALL);
	expectFormatError([&] { static_cast<void>(LittleEndianCodec::readU16(readable, 7)); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	expectFormatError([&] { static_cast<void>(LittleEndianCodec::readU32(readable, 5)); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	expectFormatError([&] { static_cast<void>(LittleEndianCodec::readU64(readable, 0)); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
}

TEST(RelocatableFormatTest, EnvelopeCodecMatchesGoldenLittleEndianBytes) {
	const auto encoded = SegmentEnvelopeCodec::encode(allocatedEnvelope());
	const std::array<uint8_t, RSS_ENVELOPE_BYTES> expected = {
			0x5A, 0x59, 0x58, 0x47, 0x04, 0x00, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
			0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x20, 0x00, 0x00, 0x00, 0x2C, 0xCD, 0x71, 0x9E, 0xDD, 0xCC, 0xBB, 0xAA, 0x00, 0x00, 0x00, 0x00};
	for (size_t index = 0; index < expected.size(); ++index) {
		EXPECT_EQ(std::to_integer<uint8_t>(encoded[index]), expected[index]);
	}

	const SegmentEnvelope decoded = SegmentEnvelopeCodec::decode(encoded);
	EXPECT_EQ(decoded.magic, RSS_SEGMENT_MAGIC);
	EXPECT_EQ(decoded.format_version, RSS_FORMAT_VERSION);
	EXPECT_EQ(decoded.header_bytes, 128u);
	EXPECT_EQ(decoded.kind, SegmentKind::SGK_ENTITY_PAGE_POOL);
	EXPECT_EQ(decoded.state, SegmentState::SGS_SEALED);
	EXPECT_EQ(decoded.segment_id.value(), 0x0102030405060708ULL);
	EXPECT_EQ(decoded.owner_id, 0x1112131415161718ULL);
	EXPECT_EQ(decoded.generation.value(), 3u);
	EXPECT_EQ(decoded.committed_lsn.value(), 9u);
	EXPECT_EQ(decoded.payload_bytes, 32u);
	EXPECT_EQ(decoded.payload_crc, 0xAABBCCDDU);
}

TEST(RelocatableFormatTest, EnvelopeCodecAcceptsCanonicalFreeSegment) {
	const auto encoded = SegmentEnvelopeCodec::encode(SegmentEnvelope{});
	const SegmentEnvelope decoded = SegmentEnvelopeCodec::decode(encoded);
	EXPECT_EQ(decoded.kind, SegmentKind::SGK_NONE);
	EXPECT_EQ(decoded.state, SegmentState::SGS_FREE);
	EXPECT_FALSE(decoded.segment_id.isValid());
	EXPECT_EQ(decoded.payload_bytes, 0u);
}

TEST(RelocatableFormatTest, EnvelopeCodecRejectsTruncatedAndCorruptInput) {
	const auto encoded = SegmentEnvelopeCodec::encode(allocatedEnvelope());
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::decode(std::span(encoded).first(63))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);

	auto corrupt = encoded;
	corrupt[0] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST(RelocatableFormatTest, EnvelopeCodecRejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	const auto encoded = SegmentEnvelopeCodec::encode(allocatedEnvelope());
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto mutation = encoded;
			mutation[byteOffset] ^= static_cast<std::byte>(uint8_t{1U} << bit);
			recalculateHeaderCrc(mutation);
			try {
				const SegmentEnvelope decoded = SegmentEnvelopeCodec::decode(mutation);
				EXPECT_EQ(SegmentEnvelopeCodec::encode(decoded), mutation)
						<< "checksum-valid mutation at byte " << byteOffset << ", bit "
						<< static_cast<unsigned>(bit);
			} catch (const FormatException &) {
				// All rejected mutations must stop at the typed format boundary.
			}
		}
	}
}

TEST(RelocatableFormatTest, EnvelopeCodecRejectsMalformedFieldsWithDeterministicErrors) {
	auto encoded = SegmentEnvelopeCodec::encode(allocatedEnvelope());

	LittleEndianCodec::writeU32(encoded, 0, 0);
	recalculateHeaderCrc(encoded);
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::decode(encoded)); },
					  FormatError::FERR_INVALID_MAGIC);

	encoded = SegmentEnvelopeCodec::encode(allocatedEnvelope());
	LittleEndianCodec::writeU16(encoded, 4, RSS_FORMAT_VERSION + 1);
	recalculateHeaderCrc(encoded);
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::decode(encoded)); },
					  FormatError::FERR_UNSUPPORTED_VERSION);

	encoded = SegmentEnvelopeCodec::encode(allocatedEnvelope());
	LittleEndianCodec::writeU16(encoded, 8, 99);
	recalculateHeaderCrc(encoded);
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::decode(encoded)); },
					  FormatError::FERR_INVALID_ENUM);

	encoded = SegmentEnvelopeCodec::encode(allocatedEnvelope());
	LittleEndianCodec::writeU16(encoded, 10, 99);
	recalculateHeaderCrc(encoded);
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::decode(encoded)); },
					  FormatError::FERR_INVALID_ENUM);

	encoded = SegmentEnvelopeCodec::encode(allocatedEnvelope());
	LittleEndianCodec::writeU32(encoded, 12, 1);
	recalculateHeaderCrc(encoded);
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::decode(encoded)); },
					  FormatError::FERR_RESERVED_BITS);

	encoded = SegmentEnvelopeCodec::encode(allocatedEnvelope());
	LittleEndianCodec::writeU32(encoded, 60, 1);
	recalculateHeaderCrc(encoded);
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::decode(encoded)); },
					  FormatError::FERR_RESERVED_BITS);
}

TEST(RelocatableFormatTest, EnvelopeCodecRejectsImpossibleHeaderAndLifecycleCombinations) {
	SegmentEnvelope envelope = allocatedEnvelope();
	envelope.header_bytes = RSS_ENVELOPE_BYTES - 8;
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::encode(envelope)); },
					  FormatError::FERR_INVALID_HEADER);

	envelope = allocatedEnvelope();
	envelope.header_bytes = std::numeric_limits<uint16_t>::max();
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::encode(envelope)); },
					  FormatError::FERR_INVALID_HEADER);

	envelope = allocatedEnvelope();
	envelope.header_bytes = 130;
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::encode(envelope)); },
					  FormatError::FERR_INVALID_HEADER);

	envelope = allocatedEnvelope();
	envelope.payload_bytes = RSS_SEGMENT_SIZE;
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::encode(envelope)); },
					  FormatError::FERR_INVALID_HEADER);

	envelope = SegmentEnvelope{};
	envelope.owner_id = 1;
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::encode(envelope)); },
					  FormatError::FERR_INVALID_HEADER);

	envelope = SegmentEnvelope{};
	envelope.kind = SegmentKind::SGK_METADATA_PAGE_POOL;
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::encode(envelope)); },
					  FormatError::FERR_INVALID_HEADER);

	envelope = SegmentEnvelope{};
	envelope.payload_bytes = 1;
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::encode(envelope)); },
					  FormatError::FERR_INVALID_HEADER);

	envelope = allocatedEnvelope();
	envelope.kind = SegmentKind::SGK_NONE;
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::encode(envelope)); },
					  FormatError::FERR_INVALID_HEADER);

	envelope = allocatedEnvelope();
	envelope.segment_id = SegmentId{};
	expectFormatError([&] { static_cast<void>(SegmentEnvelopeCodec::encode(envelope)); },
					  FormatError::FERR_INVALID_HEADER);
}
