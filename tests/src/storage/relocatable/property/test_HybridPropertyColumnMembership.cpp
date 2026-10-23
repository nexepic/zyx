#include <gtest/gtest.h>
#include <vector>
#include "graph/storage/relocatable/property/HybridPropertyColumnMembership.hpp"
using namespace graph::storage;
namespace {
using Matrix = HybridPropertyColumnMembership;
constexpr auto NODE = EntityPropertyOwnerKind::EPOK_NODE;
struct Fixture {
    HybridPropertyGroupRecord group;
    std::vector<HybridPropertyColumnRecord> columns;
    Fixture(size_t count, uint64_t presence = UINT64_MAX) {
        group = {NODE, HybridPropertyGroupMode::HPGM_HYBRID, 1, Generation(9), UINT64_MAX,
            uint32_t(count * std::popcount(presence)), uint32_t(count)};
        for (size_t field = 0; field < count; ++field) {
            HybridPropertyColumnRecord column;
            column.identity = {NODE, HybridPropertyScalarKind::HPSK_INT64, 1, uint32_t(field * 7 + 1), Generation(7)};
            column.live = group.live; column.presence = presence;
            // Stored NULL is a membership too, with canonical zero payload.
            column.nulls = presence & 1;
            for (unsigned slot = 1; slot < 64; ++slot)
                if (presence & (UINT64_C(1) << slot)) column.words[slot] = slot + field * 100;
            columns.push_back(column);
        }
    }
    Matrix matrix() const {
        auto result = Matrix::fromColumns(group, columns);
        for (size_t i = 0; i < result.column_count; ++i)
            result.locators[i] = {PageId(100 + i / 8), uint16_t(i % 8), uint32_t(i + 1)};
        return result;
    }
    std::vector<std::byte> bytes() const {
        const auto value = matrix(); std::vector<std::byte> bytes(value.wireSize()); value.encode(bytes); return bytes;
    }
};
TEST(HybridPropertyColumnMembershipTest, RoundTripsBothWordWidthsAndOnlyEnumeratesActualFields) {
    for (const auto count : {1U, 64U, 65U, 85U}) {
        Fixture f(count, (UINT64_C(1) << 48) - 1); const auto bytes = f.bytes();
        const auto matrix = Matrix::decode(bytes, f.group, Generation(10), Generation(11));
        EXPECT_EQ(matrix, f.matrix()); EXPECT_EQ(matrix.memberships(), count * 48);
        EXPECT_EQ(matrix.wordsPerOwner(), count <= 64 ? 1U : 2U);
        for (const uint64_t id : {1U, 48U, 49U, 64U}) {
            std::vector<uint32_t> tokens;
            EXPECT_TRUE(matrix.visitOwner({NODE, id}, 100, [&](uint32_t token) { tokens.push_back(token); return true; }));
            EXPECT_EQ(tokens.size(), id <= 48 ? count : 0U);
            for (size_t i = 0; i < tokens.size(); ++i) EXPECT_EQ(tokens[i], i * 7 + 1);
            EXPECT_EQ(matrix.contains({NODE, id}, uint32_t((count - 1) * 7 + 1)), id <= 48);
            EXPECT_FALSE(matrix.contains({NODE, id}, UINT32_MAX));
        }
    }
    EXPECT_EQ(Fixture(64).matrix().wireSize(), 1856U);
    EXPECT_EQ(Fixture(85).matrix().wireSize(), Matrix::MAX_BYTES - HybridPropertyColdRowBindings::MAX_BYTES);
}
TEST(HybridPropertyColumnMembershipTest, OnlyBoundCanonicalLocatorsCanBeEncodedAndDecoded) {
    Fixture f(2);
    auto unbound = Matrix::fromColumns(f.group, f.columns);
    std::vector<std::byte> scratch(unbound.wireSize(), std::byte{0x5a});
    EXPECT_THROW(unbound.encode(scratch), FormatException);
    EXPECT_TRUE(std::ranges::all_of(scratch, [](std::byte b) { return b == std::byte{0x5a}; }));
    const auto matrix = f.matrix(); const auto bytes = f.bytes();
    const size_t locators = 64U + 4U * matrix.column_count + 64U * matrix.wordsPerOwner() * 8U;
    for (size_t i = 0; i < matrix.column_count; ++i) {
        EXPECT_EQ(matrix.columnLocator(matrix.tokens[i]), matrix.locators[i]);
        EXPECT_EQ(LittleEndianCodec::readU64(bytes, locators + 16U * i), matrix.locators[i].page_id.value());
        EXPECT_EQ(LittleEndianCodec::readU16(bytes, locators + 16U * i + 8U), matrix.locators[i].slot_id);
        EXPECT_EQ(LittleEndianCodec::readU32(bytes, locators + 16U * i + 12U), matrix.locators[i].generation);
    }
    for (unsigned defect = 0; defect < 4; ++defect) {
        auto bad = bytes;
        if (defect == 0) LittleEndianCodec::writeU64(bad, locators, 0);
        if (defect == 1) LittleEndianCodec::writeU16(bad, locators + 8U, RSS_SLOTTED_PAGE_NO_SLOT);
        if (defect == 2) bad[locators + 10U] = std::byte{1};
        if (defect == 3) LittleEndianCodec::writeU32(bad, locators + 12U, 0);
        EXPECT_THROW(Matrix::decode(bad, f.group, Generation(10), Generation(11)), FormatException) << defect;
    }
    auto bad = matrix; bad.locators[bad.column_count] = {PageId(999), 0, 1};
    EXPECT_THROW(bad.encode(scratch), FormatException);
    EXPECT_THROW(matrix.columnLocator(0), FormatException);
    EXPECT_THROW(matrix.columnLocator(UINT32_MAX), FormatException);
}
TEST(HybridPropertyColumnMembershipTest, EnumerationHonorsBoundsCancellationAndOwnerDomain) {
    const auto matrix = Fixture(65).matrix(); size_t calls = 0;
    EXPECT_FALSE(matrix.visitOwner({NODE, 1}, 64, [&](uint32_t) { ++calls; return true; })); EXPECT_EQ(calls, 64U);
    calls = 0; EXPECT_FALSE(matrix.visitOwner({NODE, 1}, 100, [&](uint32_t) { ++calls; return false; })); EXPECT_EQ(calls, 1U);
    EXPECT_THROW(matrix.visitOwner({NODE, 1}, 0, [](uint32_t) { return true; }), FormatException);
    EXPECT_THROW(matrix.contains({EntityPropertyOwnerKind::EPOK_EDGE, 1}, 1), FormatException);
    EXPECT_THROW(matrix.contains({NODE, 65}, 1), FormatException);
    EXPECT_THROW(matrix.contains({NODE, 1}, 0), FormatException);
    EXPECT_THROW(matrix.visitOwner({NODE, 1}, 65, [](uint32_t) -> bool { throw std::runtime_error("cancel"); }), std::runtime_error);
}
TEST(HybridPropertyColumnMembershipTest, RejectsWrongIdentityVersionLengthsAndReservedFields) {
    Fixture f(2); const auto valid = f.bytes();
    for (const size_t offset : {0U, 4U, 6U, 7U, 8U, 16U, 24U, 32U, 34U, 36U, 40U, 64U}) {
        auto bytes = valid; bytes[offset] ^= std::byte{0x80};
        EXPECT_THROW(Matrix::decode(bytes, f.group, Generation(10), Generation(11)), FormatException) << offset;
    }
    auto shortBytes = valid; shortBytes.pop_back();
    EXPECT_THROW(Matrix::decode(shortBytes, f.group, Generation(10), Generation(11)), FormatException);
    auto longBytes = valid; longBytes.push_back(std::byte{});
    EXPECT_THROW(Matrix::decode(longBytes, f.group, Generation(10), Generation(11)), FormatException);
    EXPECT_THROW(Matrix::decode(std::span(valid).first(20), f.group, Generation(10), Generation(11)), FormatException);
    EXPECT_THROW(Matrix::decode(valid, f.group, Generation(8), Generation(11)), FormatException);
    EXPECT_THROW(Matrix::decode(valid, f.group, Generation(10), Generation(9)), FormatException);
    auto wrong = f.group; wrong.owner_kind = EntityPropertyOwnerKind::EPOK_EDGE;
    EXPECT_THROW(Matrix::decode(valid, wrong, Generation(10), Generation(11)), FormatException);
    wrong = f.group; wrong.memberships = 96; // still a valid group, but too few for the record
    EXPECT_THROW(Matrix::decode(valid, wrong, Generation(10), Generation(11)), FormatException);
}
TEST(HybridPropertyColumnMembershipTest, RejectsSemanticallyCorruptMasksEvenWithCorrectDeclaredCounts) {
    Fixture f(2, (UINT64_C(1) << 48) - 1);
    auto bytes = f.bytes(); const size_t maskOffset = 64 + 4 * f.group.columns;
    LittleEndianCodec::writeU64(bytes, maskOffset, UINT64_C(1) << 63 | 3U);
    LittleEndianCodec::writeU32(bytes, 36, 97);
    EXPECT_THROW(Matrix::decode(bytes, f.group, Generation(10), Generation(11)), FormatException);
    bytes = f.bytes(); LittleEndianCodec::writeU64(bytes, maskOffset, 2U); LittleEndianCodec::writeU32(bytes, 36, 95);
    EXPECT_THROW(Matrix::decode(bytes, f.group, Generation(10), Generation(11)), FormatException);
    auto matrix = f.matrix(); matrix.owners[0][1] = 1; EXPECT_THROW(matrix.validate(), FormatException);
    matrix = f.matrix(); matrix.tokens[1] = matrix.tokens[0]; EXPECT_THROW(matrix.validate(), FormatException);
    matrix = f.matrix(); matrix.tokens[2] = 1; EXPECT_THROW(matrix.validate(), FormatException);
}
TEST(HybridPropertyColumnMembershipTest, CommonCompleteOwnersProveEligibilityButDoNotHideNoncanonicalBits) {
    for (const auto count : {64U, 65U, 85U}) {
        auto matrix = Fixture(count).matrix();
        matrix.owners[63][0] &= ~UINT64_C(1);
        ASSERT_NO_THROW(matrix.validate());
        EXPECT_EQ(std::popcount(matrix.presence(matrix.tokens[0])), 63);
        auto corrupt = matrix;
        corrupt.owners[0][1] |= UINT64_C(1) << 63;
        EXPECT_THROW(corrupt.validate(), FormatException);
        corrupt = matrix; corrupt.tokens[1] = corrupt.tokens[0];
        EXPECT_THROW(corrupt.validate(), FormatException);
        corrupt = matrix; corrupt.live &= ~(UINT64_C(1) << 62);
        EXPECT_THROW(corrupt.validate(), FormatException);
    }
}
TEST(HybridPropertyColumnMembershipTest, ComplementaryPartialOwnersStillRequireExactPerColumnEligibility) {
    auto matrix = Fixture(2).matrix();
    for (unsigned slot = 32; slot < 48; ++slot) matrix.owners[slot][0] = 1;
    for (unsigned slot = 48; slot < 64; ++slot) matrix.owners[slot][0] = 2;
    // Only32 owners contain both fields, but each field has48 memberships.
    ASSERT_NO_THROW(matrix.validate());
    EXPECT_EQ(std::popcount(matrix.presence(matrix.tokens[0])), 48);
    EXPECT_EQ(std::popcount(matrix.presence(matrix.tokens[1])), 48);
    matrix.owners[0][0] = 1;
    EXPECT_EQ(std::popcount(matrix.presence(matrix.tokens[1])), 47);
    EXPECT_THROW(matrix.validate(), FormatException);
}
TEST(HybridPropertyColumnMembershipTest, ValidatesFinalGroupDomainAndDeadOwnerMasks) {
    Fixture f(2, (UINT64_C(1) << 48) - 1); auto matrix = f.matrix();
    matrix.live = (UINT64_C(1) << 48) - 1;
    auto group = f.group; group.live = matrix.live;
    matrix.validateInGroup(group, Generation(10), Generation(11));
    matrix.owners[63][0] = 1; EXPECT_THROW(matrix.validate(), FormatException);
    matrix = f.matrix(); matrix.group_id = RSS_HYBRID_MAX_GROUP_ID;
    EXPECT_THROW(matrix.validate(), FormatException); // slot63 would exceed the logical-ID domain
    matrix = f.matrix(); matrix.column_count = 0; EXPECT_THROW(matrix.wireSize(), FormatException);
    matrix = f.matrix(); matrix.column_count = 86; EXPECT_THROW(matrix.wireSize(), FormatException);
}
TEST(HybridPropertyColumnMembershipTest, ConstructionRejectsContradictorySourceColumnsBeforeEncoding) {
    Fixture f(2); f.columns[1].identity.token = f.columns[0].identity.token;
    EXPECT_THROW(f.matrix(), FormatException);
    f = Fixture(2); f.columns[1].identity.group_id = 2; EXPECT_THROW(f.matrix(), FormatException);
    f = Fixture(2); f.columns[1].presence &= ~UINT64_C(1); f.columns[1].words[0] = 0; f.columns[1].nulls = 0;
    const auto matrix = f.matrix(); EXPECT_FALSE(matrix.contains({NODE, 1}, 8)); EXPECT_TRUE(matrix.contains({NODE, 1}, 1));
    auto bytes = f.bytes(); EXPECT_NO_THROW(Matrix::decode(bytes, f.group, Generation(10), Generation(11)));
}
TEST(HybridPropertyColumnMembershipTest, RemovingOrdinalsAcrossWordBoundaryPreservesOtherFieldsAndZeroesUnusedBits) {
    for (const auto count : {1U, 64U, 65U, 85U}) {
        Fixture f(count); auto matrix = f.matrix();
        matrix.eraseColumn(1);
        EXPECT_EQ(matrix.column_count, count - 1);
        EXPECT_EQ(matrix.memberships(), (count - 1) * 64U);
        if (count == 1) { EXPECT_THROW(matrix.wireSize(), FormatException); continue; }
        auto group = f.group; --group.columns; group.memberships -= 64;
        matrix.validateInGroup(group, Generation(10), Generation(11));
        for (size_t field = 1; field < count; ++field) EXPECT_TRUE(matrix.contains({NODE, 64}, uint32_t(field * 7 + 1)));
        EXPECT_FALSE(matrix.contains({NODE, 1}, 1));
        const auto once = matrix; EXPECT_THROW(matrix.eraseColumn(1), FormatException); EXPECT_EQ(matrix, once);
        std::vector<std::byte> bytes(matrix.wireSize()); matrix.encode(bytes);
        EXPECT_EQ(Matrix::decode(bytes, group, Generation(10), Generation(11)), matrix);
    }
}
TEST(HybridPropertyColumnMembershipTest, PresenceReplacementTouchesOnlyItsOrdinalAndRefusesDemotionOrDeadSlots) {
    Fixture f(65); auto matrix = f.matrix(); const auto original = matrix;
    matrix.setPresence(449, (UINT64_C(1) << 48) - 1);
    EXPECT_EQ(matrix.presence(449), (UINT64_C(1) << 48) - 1);
    EXPECT_EQ(matrix.presence(1), UINT64_MAX);
    EXPECT_FALSE(matrix.contains({NODE, 64}, 449)); EXPECT_TRUE(matrix.contains({NODE, 1}, 449));
    const auto valid = matrix;
    EXPECT_THROW(matrix.setPresence(449, (UINT64_C(1) << 47) - 1), FormatException); EXPECT_EQ(matrix, valid);
    EXPECT_THROW(matrix.setPresence(UINT32_MAX, UINT64_MAX), FormatException); EXPECT_EQ(matrix, valid);
    matrix.live &= ~(UINT64_C(1) << 63);
    EXPECT_THROW(matrix.setPresence(1, UINT64_MAX), FormatException);
    EXPECT_EQ(original.presence(449), UINT64_MAX);
}

}

TEST(HybridPropertyColumnMembershipTest, RowRemainderCombinesHotColumnsWithCanonicalColdBlockLocations) {
    Fixture f(2); auto matrix = f.matrix(); matrix.row_remainder = true; matrix.cold_fields = 64;
    matrix.cold_rows.owners = UINT64_MAX;
    for (unsigned i = 0; i < 64; ++i) matrix.cold_rows.roots[i] = {PageId(99), uint16_t(i < 32 ? 0 : 1), 1};
    auto group = f.group; group.mode = HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER;
    group.column_membership = {PageId(100), 4, 1}; group.memberships += 64;
    std::vector<std::byte> bytes(matrix.wireSize()); matrix.encode(bytes);
    EXPECT_EQ(bytes.size(), f.matrix().wireSize() + 48U);
    EXPECT_EQ(HybridPropertyColumnMembership::decode(bytes, group, Generation(10), Generation(11)), matrix);
    EXPECT_EQ(HybridPropertyColumnMembership::decodePayload(bytes, NODE, 1, Generation(11)), matrix);
    auto wrong = group; wrong.mode = HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP;
    EXPECT_THROW(HybridPropertyColumnMembership::decode(bytes, wrong, Generation(10), Generation(11)), FormatException);
    wrong = group; --wrong.memberships;
    EXPECT_THROW(HybridPropertyColumnMembership::decode(bytes, wrong, Generation(10), Generation(11)), FormatException);
    auto corrupt = bytes; corrupt[52] |= std::byte{0x40};
    EXPECT_THROW(HybridPropertyColumnMembership::decode(corrupt, group, Generation(10), Generation(11)), FormatException);
    corrupt = bytes; LittleEndianCodec::writeU32(corrupt, 48, 63);
    EXPECT_THROW(HybridPropertyColumnMembership::decode(corrupt, group, Generation(10), Generation(11)), FormatException);
    auto overflow = matrix; overflow.cold_fields = UINT32_MAX;
    EXPECT_THROW(overflow.encode(bytes), FormatException);
}
TEST(HybridPropertyColumnMembershipTest, ColdOnlyAndPropertylessNativeOwnersRetainTheirGroupAfterLastColumnRemoval) {
    HybridPropertyColumnMembership matrix; matrix.owner_kind = NODE; matrix.group_id = 1;
    matrix.semantic_generation = Generation(9); matrix.live = 3; matrix.row_remainder = true;
    matrix.cold_rows.owners = 3; matrix.cold_fields = 2;
    matrix.cold_rows.roots[0] = matrix.cold_rows.roots[1] = {PageId(99), 0, 1};
    HybridPropertyGroupRecord group{NODE, HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER,
        1, Generation(9), 3, 2, 0, {PageId(100), 1, 1}};
    EXPECT_EQ(matrix.wordsPerOwner(), 0U);
    std::vector<std::byte> bytes(matrix.wireSize()); matrix.encode(bytes);
    EXPECT_EQ(HybridPropertyColumnMembership::decode(bytes, group, Generation(10), Generation(11)), matrix);
    matrix.cold_rows = {}; matrix.cold_fields = 0; group.memberships = 0;
    bytes.resize(matrix.wireSize()); matrix.encode(bytes);
    EXPECT_EQ(bytes.size(), 64U);
    EXPECT_EQ(HybridPropertyColumnMembership::decode(bytes, group, Generation(10), Generation(11)), matrix);
    Fixture f(1); auto removed = f.matrix(); removed.row_remainder = true;
    removed.eraseColumn(removed.tokens[0]);
    EXPECT_EQ(removed.column_count, 0U); EXPECT_EQ(removed.totalMemberships(), 0U);
    EXPECT_EQ(removed.wireSize(), 64U);
}
