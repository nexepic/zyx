#include <gtest/gtest.h>
#include "graph/storage/relocatable/format/ScanFormatCloneCodec.hpp"
using namespace graph::storage;

#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
namespace {
Superblock original() {
    Superblock value; value.database_id = DatabaseId(7); value.generation = Generation(3);
    value.checkpoint_lsn = LogSequenceNumber(2); value.next_logical_node_id = 800;
    value.metadata_emergency_reserve = {PhysicalSlotId(3), SegmentId(2), Generation(1), LogSequenceNumber(1)};
    return value;
}
void sealLegacy(std::span<std::byte> bytes) {
    LittleEndianCodec::writeU32(bytes, 112, graph::utils::calculateCrc(bytes.data() + 336, bytes.size() - 336));
    LittleEndianCodec::writeU32(bytes, 116, 0);
    LittleEndianCodec::writeU32(bytes, 116, graph::utils::calculateCrc(bytes.data(), 336));
}
auto legacyHeader() {
    const auto modern = SuperblockCodec::encode(original());
    std::array<std::byte, RSS_SUPERBLOCK_BYTES> bytes{};
    std::copy_n(modern.begin(), 336, bytes.begin());
    std::copy_n(modern.begin() + 352, 32, bytes.begin() + 336);
    LittleEndianCodec::writeU16(bytes, 4, 31); LittleEndianCodec::writeU16(bytes, 6, 336);
    sealLegacy(bytes); return bytes;
}
}
TEST(ScanFormatCloneCodecTest, PreservesLegacyIdentityRootsAndReserveButNeverProducesACompleteRoot) {
    const auto source = legacyHeader();
    const auto bytes = ScanFormatCloneCodec::legacySuperblockToBuilding(source);
    const auto decoded = SuperblockCodec::decode(bytes);
    EXPECT_EQ(decoded.database_id, original().database_id);
    EXPECT_EQ(decoded.next_logical_node_id, 800U);
    EXPECT_EQ(decoded.metadata_emergency_reserve, original().metadata_emergency_reserve);
    EXPECT_EQ(decoded.flags, RSS_SUPERBLOCK_FLAG_SCAN_BUILDING);
    EXPECT_FALSE(decoded.hybrid_property_descriptor_root.isValid());
    EXPECT_EQ(source, legacyHeader());
}
TEST(ScanFormatCloneCodecTest, AuthenticatesSourceBeforeChangingVersionsAndRetainsLegacyValidation) {
    const auto source = legacyHeader();
    for (size_t i = 0; i < source.size(); ++i) {
        auto bytes = source; bytes[i] ^= std::byte{1};
        EXPECT_THROW((void)ScanFormatCloneCodec::legacySuperblockToBuilding(bytes), FormatException) << i;
    }
    for (const size_t offset : {size_t{4}, size_t{8}, size_t{12}, size_t{368}}) {
        auto bytes = source; bytes[offset] ^= std::byte{0x20}; sealLegacy(bytes);
        EXPECT_THROW((void)ScanFormatCloneCodec::legacySuperblockToBuilding(bytes), FormatException) << offset;
    }
    auto bytes = source; LittleEndianCodec::writeU64(bytes, 40, 1); sealLegacy(bytes);
    EXPECT_THROW((void)ScanFormatCloneCodec::legacySuperblockToBuilding(bytes), FormatException);
}
TEST(ScanFormatCloneCodecTest, EnvelopeTranslationPreservesOwnershipAndRejectsUnknownSources) {
    SegmentEnvelope envelope; envelope.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
    envelope.state = SegmentState::SGS_SEALED; envelope.segment_id = SegmentId(8);
    envelope.owner_id = 91; envelope.generation = Generation(3); envelope.committed_lsn = LogSequenceNumber(2);
    auto bytes = SegmentEnvelopeCodec::encode(envelope);
    LittleEndianCodec::writeU16(bytes, 4, 5); LittleEndianCodec::writeU32(bytes, 52, 0);
    LittleEndianCodec::writeU32(bytes, 52, graph::utils::calculateCrc(bytes.data(), bytes.size()));
    const auto translated = SegmentEnvelopeCodec::decode(ScanFormatCloneCodec::legacyEnvelopeHeader(bytes));
    EXPECT_EQ(translated.owner_id, envelope.owner_id); EXPECT_EQ(translated.segment_id, envelope.segment_id);
    EXPECT_EQ(translated.generation, envelope.generation); EXPECT_EQ(translated.format_version, 15);
    for (size_t i = 0; i < bytes.size(); ++i) {
        auto corrupt = bytes; corrupt[i] ^= std::byte{1};
        EXPECT_THROW((void)ScanFormatCloneCodec::legacyEnvelopeHeader(corrupt), FormatException);
    }
    LittleEndianCodec::writeU16(bytes, 4, 13); LittleEndianCodec::writeU32(bytes, 52, 0);
    LittleEndianCodec::writeU32(bytes, 52, graph::utils::calculateCrc(bytes.data(), bytes.size()));
    EXPECT_THROW((void)ScanFormatCloneCodec::legacyEnvelopeHeader(bytes), FormatException);
}
#else
TEST(ScanFormatCloneCodecTest, OrdinaryProfilesCannotTranscodeCandidateFiles) {
    EXPECT_THROW((void)ScanFormatCloneCodec::legacySuperblockToBuilding({}), FormatException);
    EXPECT_THROW((void)ScanFormatCloneCodec::legacyEnvelopeHeader({}), FormatException);
}
#endif
