#include <gtest/gtest.h>
#include "graph/storage/relocatable/format/Superblock.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDescriptor.hpp"

using namespace graph::storage;
namespace {
Superblock block() {
    Superblock value; value.database_id = DatabaseId(7); value.generation = Generation(3);
    value.checkpoint_lsn = LogSequenceNumber(3); return value;
}
}
TEST(ScanPathFormatTest, VersionsAndBuildingFlagsAreIsolated) {
    auto value = block(); value.flags = RSS_SUPERBLOCK_FLAG_SCAN_BUILDING;
#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
    EXPECT_EQ(RSS_FORMAT_VERSION, 15); EXPECT_EQ(RSS_SUPERBLOCK_VERSION, 41);
    EXPECT_EQ(RSS_SUPERBLOCK_HEADER_BYTES, 352U);
    EXPECT_EQ(SuperblockCodec::decode(SuperblockCodec::encode(value)).flags, value.flags);
#else
    EXPECT_EQ(RSS_FORMAT_VERSION, RSS_HYBRID_PROPERTY_FORMAT_ENABLED ? 13 : 5);
    EXPECT_EQ(RSS_SUPERBLOCK_VERSION, RSS_HYBRID_PROPERTY_FORMAT_ENABLED ? 39 : 31);
    EXPECT_THROW((void)SuperblockCodec::encode(value), FormatException);
#endif
    auto bytes = SuperblockCodec::encode(block());
    LittleEndianCodec::writeU16(bytes, 4, RSS_SCAN_PATH_FORMAT_ENABLED ? 31 : 41);
    LittleEndianCodec::writeU32(bytes, 116, 0);
    LittleEndianCodec::writeU32(bytes, 116, graph::utils::calculateCrc(bytes.data(), RSS_SUPERBLOCK_HEADER_BYTES));
    EXPECT_THROW((void)SuperblockCodec::decode(bytes), FormatException);
    SegmentEnvelope envelope; envelope.format_version = RSS_SCAN_PATH_FORMAT_ENABLED ? 5 : 15;
    EXPECT_THROW((void)SegmentEnvelopeCodec::encode(envelope), FormatException);
}
TEST(ScanPathFormatTest, DescriptorRootAndCountsHaveExplicitVersionedBytes) {
    HybridPropertyDescriptorPage page;
    page.generation = Generation(3); page.committed_lsn = LogSequenceNumber(3);
    page.descriptor.database_id = DatabaseId(7); page.descriptor.semantic_generation = Generation(3);
#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
    EXPECT_EQ(RSS_HYBRID_PROPERTY_DESCRIPTOR_ROOT_COUNT, 9U);
    EXPECT_EQ(RSS_HYBRID_PROPERTY_DESCRIPTOR_BYTES, 208U);
    page.descriptor.roots[8] = {PageId(55), Generation(2)};
    page.descriptor.scan_live_node_count = 65;
    page.descriptor.scan_membership_group_count = 2;
    const auto bytes = HybridPropertyDescriptorCodec::encode(page);
    EXPECT_EQ(LittleEndianCodec::readU16(bytes, 4), 2);
    EXPECT_EQ(LittleEndianCodec::readU64(bytes, 32 + 24), 65U);
    EXPECT_EQ(LittleEndianCodec::readU64(bytes, 32 + 192), 55U);
    EXPECT_EQ(HybridPropertyDescriptorCodec::decode(bytes, DatabaseId(7), Generation(3), Generation(3), LogSequenceNumber(3)), page);
    page.descriptor.scan_membership_group_count = 1;
    EXPECT_THROW((void)HybridPropertyDescriptorCodec::encode(page), FormatException);
    page.descriptor.scan_membership_group_count = 2; page.descriptor.roots[8] = {};
    EXPECT_THROW((void)HybridPropertyDescriptorCodec::encode(page), FormatException);
#else
    EXPECT_EQ(RSS_HYBRID_PROPERTY_DESCRIPTOR_ROOT_COUNT, 8U);
    EXPECT_EQ(RSS_HYBRID_PROPERTY_DESCRIPTOR_BYTES, 192U);
    const auto bytes = HybridPropertyDescriptorCodec::encode(page);
    EXPECT_EQ(LittleEndianCodec::readU16(bytes, 4), 1);
    EXPECT_THROW((void)hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_SCAN), FormatException);
#endif
    EXPECT_TRUE(isHybridPropertyIndexId(UINT64_C(0x8000000000000038)));
}
