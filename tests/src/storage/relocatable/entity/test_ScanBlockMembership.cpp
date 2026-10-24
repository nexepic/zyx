#include <gtest/gtest.h>
#include "graph/storage/relocatable/entity/ScanBlockMembership.hpp"

using namespace graph::storage;
namespace {
ScanBlockMembership membership() {
    ScanBlockMembership value;
    value.database_id = DatabaseId(7); value.semantic_generation = Generation(3);
    value.group_id = 2; value.live = 0b1111; value.label_count = 2;
    value.labels[0] = {2, 0b0011}; value.labels[1] = {5, 0b1010};
    return value;
}
}
TEST(ScanBlockMembershipTest, RoundTripAndOverlappingLabelsPreserveLiveIds) {
    const auto source = membership();
    const auto decoded = ScanBlockMembership::decode(source.encode(), DatabaseId(7), Generation(4), 2);
    EXPECT_EQ(decoded.select({}), 0b1111U);
    const std::array<uint64_t, 2> both{2, 5};
    EXPECT_EQ(decoded.select(both), 0b0010U);
    const std::array<uint64_t, 1> unknown{8};
    EXPECT_EQ(decoded.select(unknown), 0U);
    const std::array<uint64_t, 2> duplicate{2, 2}, reversed{5, 2};
    EXPECT_THROW((void)decoded.select(duplicate), FormatException);
    EXPECT_THROW((void)decoded.select(reversed), FormatException);
}
TEST(ScanBlockMembershipTest, RejectsCorruptionTruncationAndWrongSnapshotIdentity) {
    const auto bytes = membership().encode();
    for (size_t offset = 0; offset < bytes.size(); ++offset) {
        auto corrupt = bytes; corrupt[offset] ^= std::byte{1};
        EXPECT_THROW((void)ScanBlockMembership::decode(corrupt, DatabaseId(7), Generation(3), 2), FormatException) << offset;
    }
    for (size_t size = 0; size < bytes.size(); ++size)
        EXPECT_THROW((void)ScanBlockMembership::decode(std::span(bytes).first(size), DatabaseId(7), Generation(3), 2), FormatException);
    EXPECT_THROW((void)ScanBlockMembership::decode(bytes, DatabaseId(8), Generation(3), 2), FormatException);
    EXPECT_THROW((void)ScanBlockMembership::decode(bytes, DatabaseId(7), Generation(2), 2), FormatException);
    EXPECT_THROW((void)ScanBlockMembership::decode(bytes, DatabaseId(7), Generation(3), 1), FormatException);
}
TEST(ScanBlockMembershipTest, RejectsInvalidAuthorityAndRetainsBoundaryIds) {
    auto value = membership(); value.labels[1].members |= 16;
    EXPECT_THROW((void)value.encode(), FormatException);
    value = membership(); value.labels[1].token = 2;
    EXPECT_THROW((void)value.encode(), FormatException);
    value = membership(); value.label_count = 65;
    EXPECT_THROW((void)value.encode(), FormatException);
    value = membership(); value.group_id = 1U + (UINT64_MAX - 1U) / 64U;
    value.live |= UINT64_C(1) << 62U;
    EXPECT_NO_THROW((void)value.encode());
    value.live |= UINT64_C(1) << 63U;
    EXPECT_THROW((void)value.encode(), FormatException);
    value = membership(); value.label_count = 0;
    EXPECT_EQ(ScanBlockMembership::decode(value.encode(), DatabaseId(7), Generation(3), 2).select({}), value.live);
}

TEST(ScanBlockMembershipTest, RejectsNoncanonicalHeadersEvenWithRecomputedChecksums) {
    const auto fixCrc = [](std::vector<std::byte> &bytes) {
        LittleEndianCodec::writeU32(bytes, 48, graph::utils::calculateCrc(bytes.data() + 64, bytes.size() - 64));
        LittleEndianCodec::writeU32(bytes, 52, 0);
        LittleEndianCodec::writeU32(bytes, 52, graph::utils::calculateCrc(bytes.data(), 64));
    };
    for (size_t offset : {size_t{4}, size_t{6}, size_t{42}, size_t{44}, size_t{56}}) {
        auto bytes = membership().encode(); bytes[offset] ^= std::byte{1}; fixCrc(bytes);
        EXPECT_THROW((void)ScanBlockMembership::decode(bytes, DatabaseId(7), Generation(3), 2), FormatException);
    }
    auto bytes = membership().encode();
    LittleEndianCodec::writeU64(bytes, 80, 2); fixCrc(bytes);
    EXPECT_THROW((void)ScanBlockMembership::decode(bytes, DatabaseId(7), Generation(3), 2), FormatException);
    auto value = membership(); value.label_count = 64;
    for (size_t i = 0; i < 64; ++i) value.labels[i] = {i + 1, value.live};
    EXPECT_EQ(value.encode().size(), ScanBlockMembership::MAX_BYTES);
    const auto decoded = ScanBlockMembership::decode(value.encode(), DatabaseId(7), Generation(3), 2);
    const std::array<uint64_t, 2> limits{1, 64};
    EXPECT_EQ(decoded.select(limits), value.live);
}
