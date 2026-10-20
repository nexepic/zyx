#include <gtest/gtest.h>
#include "graph/storage/relocatable/property/HybridPropertyDescriptor.hpp"
using namespace graph::storage;
namespace {
HybridPropertyDescriptorPage sample() {
    HybridPropertyDescriptorPage page;
    page.generation = Generation(9); page.committed_lsn = LogSequenceNumber(17);
    page.descriptor.database_id = DatabaseId(0x0102030405060708ULL);
    page.descriptor.semantic_generation = Generation(7);
    for (size_t i = 0; i < page.descriptor.roots.size(); ++i)
        page.descriptor.roots[i] = {PageId(101 + i), Generation(3 + i % 4)};
    return page;
}
auto decode(std::span<const std::byte> bytes) {
    return HybridPropertyDescriptorCodec::decode(bytes, sample().descriptor.database_id, Generation(9),
        Generation(12), LogSequenceNumber(20));
}
void reseal(HybridPropertyDescriptorCodec::Bytes &bytes) {
    LittleEndianCodec::writeU32(bytes, 24, graph::utils::calculateCrc(bytes.data() + 32, bytes.size() - 32));
    LittleEndianCodec::writeU32(bytes, 28, 0);
    LittleEndianCodec::writeU32(bytes, 28, graph::utils::calculateCrc(bytes.data(), 32));
}
TEST(HybridPropertyDescriptorTest, Exact192BytePayloadAndEightIndependentLogicalRootsRoundTrip) {
    const auto page = sample(); const auto bytes = HybridPropertyDescriptorCodec::encode(page);
    EXPECT_EQ(bytes.size(), RSS_PAGE_SIZE); EXPECT_EQ(decode(bytes), page);
    EXPECT_EQ(bytes[0], std::byte{'Z'}); EXPECT_EQ(bytes[1], std::byte{'Y'});
    EXPECT_EQ(bytes[2], std::byte{'X'}); EXPECT_EQ(bytes[3], std::byte{'H'});
    EXPECT_EQ(bytes[32 + 0], std::byte{'Z'}); EXPECT_EQ(bytes[32 + 3], std::byte{'D'});
    EXPECT_EQ(LittleEndianCodec::readU64(bytes, 32 + 8), 7U);
    EXPECT_EQ(LittleEndianCodec::readU64(bytes, 32 + 16), 0x0102030405060708ULL);
    EXPECT_EQ(LittleEndianCodec::readU64(bytes, 32 + 64), 101U);
    EXPECT_EQ(LittleEndianCodec::readU64(bytes, 32 + 176), 108U);
    EXPECT_EQ(LittleEndianCodec::readU64(bytes, 32 + 184), 6U);
    EXPECT_TRUE(std::all_of(bytes.begin() + 224, bytes.end(), [](auto byte) { return byte == std::byte{}; }));
    auto empty = page; empty.descriptor.roots = {};
    EXPECT_EQ(decode(HybridPropertyDescriptorCodec::encode(empty)), empty);
}
TEST(HybridPropertyDescriptorTest, NamespaceIndexIdsAreStableDisjointAndRejectUnknownOrdinals) {
    constexpr std::array<uint64_t, 8> expected{
        UINT64_C(0x8000000000000030), UINT64_C(0x8000000000000031),
        UINT64_C(0x8000000000000032), UINT64_C(0x8000000000000033),
        UINT64_C(0x8000000000000034), UINT64_C(0x8000000000000035),
        UINT64_C(0x8000000000000036), UINT64_C(0x8000000000000037)};
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(hybridPropertyIndexId(static_cast<HybridPropertyRootOrdinal>(i)), expected[i]);
        EXPECT_TRUE(isHybridPropertyIndexId(expected[i]));
    }
    for (const auto id : std::array<uint64_t, 8>{0, 7, UINT64_C(0x8000000000000002),
        UINT64_C(0x8000000000000020), expected.front() - 1, expected.back() + 1,
        UINT64_C(0xA501000000000030), UINT64_MAX}) EXPECT_FALSE(isHybridPropertyIndexId(id));
    EXPECT_THROW(hybridPropertyIndexId(static_cast<HybridPropertyRootOrdinal>(8)), FormatException);
    EXPECT_THROW(hybridPropertyIndexId(static_cast<HybridPropertyRootOrdinal>(255)), FormatException);
}
TEST(HybridPropertyDescriptorTest, ValidChecksumsDoNotAuthorizeWrongDatabaseGenerationOrVisibility) {
    const auto bytes = HybridPropertyDescriptorCodec::encode(sample());
    EXPECT_THROW(HybridPropertyDescriptorCodec::decode(bytes, DatabaseId(2), Generation(9), Generation(12), LogSequenceNumber(20)), FormatException);
    EXPECT_THROW(HybridPropertyDescriptorCodec::decode(bytes, sample().descriptor.database_id, Generation(8), Generation(12), LogSequenceNumber(20)), FormatException);
    EXPECT_THROW(HybridPropertyDescriptorCodec::decode(bytes, sample().descriptor.database_id, Generation(9), Generation(8), LogSequenceNumber(20)), FormatException);
    EXPECT_THROW(HybridPropertyDescriptorCodec::decode(bytes, sample().descriptor.database_id, Generation(9), Generation(12), LogSequenceNumber(16)), FormatException);
    for (size_t offset : {size_t(8), size_t(16), size_t(32 + 8), size_t(32 + 16)}) {
        auto wrong = bytes; LittleEndianCodec::writeU64(wrong, offset, UINT64_MAX); reseal(wrong);
        EXPECT_THROW(decode(wrong), FormatException) << offset;
    }
}
TEST(HybridPropertyDescriptorTest, PartialAndAliasedRootReferencesFailClosed) {
    for (size_t index = 0; index < RSS_HYBRID_PROPERTY_DESCRIPTOR_ROOT_COUNT; ++index) {
        auto page = sample(); page.descriptor.roots[index].generation = Generation{};
        EXPECT_THROW(HybridPropertyDescriptorCodec::encode(page), FormatException);
        page = sample(); page.descriptor.roots[index].page_id = PageId{};
        EXPECT_THROW(HybridPropertyDescriptorCodec::encode(page), FormatException);
    }
    auto page = sample(); page.descriptor.roots[7] = page.descriptor.roots[0];
    EXPECT_THROW(HybridPropertyDescriptorCodec::encode(page), FormatException);
    auto bytes = HybridPropertyDescriptorCodec::encode(sample());
    LittleEndianCodec::writeU64(bytes, 32 + 176, 101); reseal(bytes);
    EXPECT_THROW(decode(bytes), FormatException);
}
TEST(HybridPropertyDescriptorTest, IndependentTreePoolEpochsAreNotSemanticVisibilityBounds) {
    auto page = sample();
    for (size_t index = 0; index < RSS_HYBRID_PROPERTY_DESCRIPTOR_ROOT_COUNT; ++index)
        page.descriptor.roots[index].generation = Generation(99 + index);
    // These epochs exceed both metadata generation 9 and visible generation 12.
    // The frame codec authenticates metadata, not the referenced tree pages.
    EXPECT_EQ(decode(HybridPropertyDescriptorCodec::encode(page)), page);
    auto bytes = HybridPropertyDescriptorCodec::encode(page);
    LittleEndianCodec::writeU64(bytes, 32 + 72, UINT64_MAX); reseal(bytes);
    EXPECT_EQ(decode(bytes).descriptor.roots[0].generation, Generation(UINT64_MAX));
    // Semantic visibility remains a separate, enforced constraint.
    page.descriptor.semantic_generation = Generation(10);
    EXPECT_THROW(HybridPropertyDescriptorCodec::encode(page), FormatException);
}
TEST(HybridPropertyDescriptorTest, ChecksummedReservedBitsVersionsAndMagicAreNotSilentlyReinterpreted) {
    const auto original = HybridPropertyDescriptorCodec::encode(sample());
    for (size_t offset : {size_t(0), size_t(4), size_t(6), size_t(32), size_t(36), size_t(38),
        size_t(56), size_t(95), size_t(224), size_t(4095)}) {
        auto bytes = original; bytes[offset] ^= std::byte{0x40}; reseal(bytes);
        EXPECT_THROW(decode(bytes), FormatException) << offset;
    }
}
TEST(HybridPropertyDescriptorTest, EveryFrameByteIsCoveredAndPhysicalRelocationPreservesSemanticGeneration) {
    const auto original = HybridPropertyDescriptorCodec::encode(sample());
    for (size_t offset = 0; offset < original.size(); ++offset) {
        auto damaged = original; damaged[offset] ^= std::byte{1};
        EXPECT_THROW(decode(damaged), FormatException) << offset;
    }
    auto relocated = sample(); relocated.generation = Generation(10); relocated.committed_lsn = LogSequenceNumber(18);
    const auto bytes = HybridPropertyDescriptorCodec::encode(relocated);
    const auto result = HybridPropertyDescriptorCodec::decode(bytes, relocated.descriptor.database_id,
        Generation(10), Generation(12), LogSequenceNumber(20));
    EXPECT_EQ(result.descriptor, sample().descriptor); EXPECT_EQ(result, relocated);
    EXPECT_THROW(decode(bytes), FormatException); // old physical root cannot accept new frame
    EXPECT_THROW(decode(std::span(original).first(4095)), FormatException);
    std::array<std::byte, 4097> tooWide{};
    EXPECT_THROW(decode(tooWide), FormatException);
}
} // namespace
