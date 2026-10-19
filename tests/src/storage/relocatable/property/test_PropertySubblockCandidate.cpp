#include "../../../../../benchmarks/architecture-design/property-subblock/PropertySubblockCandidate.hpp"
#include <gtest/gtest.h>
#include <algorithm>

namespace {
using namespace zyx::benchmark::subblock;
Column dense(ScalarKind kind = ScalarKind::HPS_INT64) {
    Column column;
    column.identity = {OwnerKind::HPK_NODE, kind, 7, 19, 23};
    column.live = column.presence = UINT64_MAX;
    for (unsigned i = 0; i < 64; ++i) column.words[i] = kind == ScalarKind::HPS_BOOL ? i % 2 : i + 1;
    return column;
}
std::array<std::byte, 576> image(const Column &column) {
    std::array<std::byte, 576> result{};
    column.encode(std::span(result).first(column.wireSize())); return result;
}
CostEvidence completeCost(uint64_t row = 1000, uint64_t candidate = 900) {
    CostEvidence cost; cost.known.fill(true); cost.row_lower[0] = row;
    cost.candidate_upper[0] = candidate; cost.peak_known = true;
    cost.peak_allocated_upper = row + candidate; return cost;
}
ConversionLimits limits() { return {64, 4096, 1048576, 1048576, 100000}; }

TEST(PropertySubblockCandidateTest, ColumnOffsetsAndLittleEndianAreExact) {
    auto column = dense(); const auto bytes = image(column);
    EXPECT_EQ(bytes[0], std::byte{0x48}); EXPECT_EQ(bytes[1], std::byte{0x50});
    EXPECT_EQ(bytes[2], std::byte{0x42}); EXPECT_EQ(bytes[3], std::byte{0x43});
    EXPECT_EQ(read(bytes, 4, 2), 1); EXPECT_EQ(read(bytes, 6, 1), 1);
    EXPECT_EQ(read(bytes, 7, 1), 1); EXPECT_EQ(read(bytes, 8, 8), 7);
    EXPECT_EQ(read(bytes, 16, 4), 19); EXPECT_EQ(read(bytes, 20, 2), 64);
    EXPECT_EQ(read(bytes, 22, 2), 512); EXPECT_EQ(read(bytes, 24, 8), 23);
    EXPECT_EQ(read(bytes, 32, 8), UINT64_MAX); EXPECT_EQ(read(bytes, 40, 8), 0);
    EXPECT_EQ(read(bytes, 48, 8), UINT64_MAX); EXPECT_EQ(read(bytes, 56, 8), 0);
    EXPECT_EQ(read(bytes, 64, 8), 1); EXPECT_EQ(read(bytes, 568, 8), 64);
    auto decoded = Column::decode(bytes, column.identity);
    EXPECT_EQ(decoded.identity, column.identity); EXPECT_EQ(decoded.words, column.words);
}
TEST(PropertySubblockCandidateTest, DoubleBitsPreserveNegativeZeroNaNInfinityAndIntegerDistinction) {
    auto column = dense(ScalarKind::HPS_DOUBLE);
    column.words[0] = UINT64_C(0x8000000000000000);
    column.words[1] = UINT64_C(0x7ff8000000001234);
    column.words[2] = UINT64_C(0x7ff0000000000000);
    column.words[3] = UINT64_C(0xfff0000000000000);
    auto bytes = image(column); auto decoded = Column::decode(bytes, column.identity);
    EXPECT_EQ(decoded.words, column.words);
    auto wrong = column.identity; wrong.scalar_kind = ScalarKind::HPS_INT64;
    EXPECT_THROW(Column::decode(bytes, wrong), std::invalid_argument);
}
TEST(PropertySubblockCandidateTest, BoolIsBitPackedAndNullAndMissingAreDistinct) {
    auto column = dense(ScalarKind::HPS_BOOL);
    column.presence &= ~(uint64_t{1} << 3); column.words[3] = 0;
    column.nulls = uint64_t{1} << 5; column.words[5] = 0;
    auto bytes = image(column); auto wire = std::span(bytes).first(72);
    auto decoded = Column::decode(wire, column.identity);
    EXPECT_EQ(read(wire, 22, 2), 8); EXPECT_EQ(read(wire, 64, 8), UINT64_C(0xaaaaaaaaaaaaaa82));
    EXPECT_EQ(decoded.presence, column.presence); EXPECT_EQ(decoded.nulls, column.nulls);
    EXPECT_EQ(decoded.words, column.words);
    bytes[64] |= std::byte{0x08}; // nonzero payload in missing slot
    EXPECT_THROW(Column::decode(wire, column.identity), std::invalid_argument);
}
TEST(PropertySubblockCandidateTest, OccupancyCountsPresentNullsAndDemandsFortyEight) {
    auto column = dense();
    for (unsigned count : {47U, 48U, 64U}) {
        column.presence = count == 64 ? UINT64_MAX : (uint64_t{1} << count) - 1;
        column.nulls = column.presence;
        column.words.fill(0);
        if (count == 47) EXPECT_THROW(image(column), std::invalid_argument);
        else { auto bytes = image(column); EXPECT_NO_THROW(Column::decode(bytes, column.identity)); }
    }
}
TEST(PropertySubblockCandidateTest, ColumnRejectsEveryTruncationAndTrailingByte) {
    const auto column = dense(); const auto bytes = image(column);
    for (size_t size = 0; size < bytes.size(); ++size)
        EXPECT_THROW(Column::decode(std::span(bytes).first(size), column.identity), std::invalid_argument) << size;
    std::array<std::byte, 577> longer{}; std::copy(bytes.begin(), bytes.end(), longer.begin());
    EXPECT_THROW(Column::decode(longer, column.identity), std::invalid_argument);
}
TEST(PropertySubblockCandidateTest, ColumnRejectsUntrustedHeadersAndValidPayloadWrongIdentity) {
    const auto column = dense(); const auto good = image(column);
    for (const auto &[offset, value] : std::array<std::pair<size_t, uint64_t>, 14>{{
        {0, 0}, {4, 2}, {6, 3}, {7, 4}, {8, 8}, {16, 20}, {20, 63},
        {22, 511}, {24, 24}, {32, 0}, {40, 1}, {48, 0}, {56, 1}, {63, 1}}}) {
        auto bytes = good; write(bytes, offset, offset == 20 || offset == 22 ? 2 : 1, value);
        EXPECT_THROW(Column::decode(bytes, column.identity), std::invalid_argument) << offset;
    }
    auto edge = column.identity; edge.owner_kind = OwnerKind::HPK_EDGE;
    EXPECT_THROW(Column::decode(good, edge), std::invalid_argument);
    auto bad = column; bad.presence &= ~uint64_t{1}; bad.nulls = 1; bad.words[0] = 0;
    EXPECT_THROW(image(bad), std::invalid_argument);
    bad = dense(ScalarKind::HPS_BOOL); bad.words[0] = 2;
    EXPECT_THROW(image(bad), std::invalid_argument);
}
TEST(PropertySubblockCandidateTest, IdArithmeticNeverWrapsAndLastGroupRejectsImpossibleId) {
    EXPECT_THROW(groupFor(0), std::invalid_argument);
    EXPECT_THROW(slotFor(0), std::invalid_argument);
    EXPECT_EQ(groupFor(1), 1); EXPECT_EQ(slotFor(1), 0);
    EXPECT_EQ(groupFor(64), 1); EXPECT_EQ(slotFor(64), 63);
    EXPECT_EQ(groupFor(65), 2); EXPECT_EQ(slotFor(65), 0);
    EXPECT_EQ(slotFor(UINT64_MAX), 62);
    auto column = dense(); column.identity.group = groupFor(UINT64_MAX);
    EXPECT_THROW(image(column), std::invalid_argument);
    column.presence = column.live = UINT64_MAX >> 1; column.words[63] = 0;
    EXPECT_NO_THROW(image(column));
    ++column.identity.group;
    EXPECT_THROW(image(column), std::invalid_argument);
}
TEST(PropertySubblockCandidateTest, GroupWireOffsetsAndOwnerGenerationChecks) {
    GroupHeader group{OwnerKind::HPK_EDGE, GroupMode::HPG_HYBRID, 9, 12, UINT64_MAX, 4096, 64};
    auto bytes = group.encode();
    EXPECT_EQ(read(bytes, 7, 1), 1); EXPECT_EQ(read(bytes, 8, 8), 9);
    EXPECT_EQ(read(bytes, 16, 8), 12); EXPECT_EQ(read(bytes, 24, 8), UINT64_MAX);
    EXPECT_EQ(read(bytes, 32, 4), 4096); EXPECT_EQ(read(bytes, 36, 4), 64);
    EXPECT_EQ(GroupHeader::decode(bytes, group.kind, 9, 12).memberships, 4096);
    EXPECT_THROW(GroupHeader::decode(bytes, OwnerKind::HPK_NODE, 9, 12), std::invalid_argument);
    EXPECT_THROW(GroupHeader::decode(bytes, group.kind, 8, 12), std::invalid_argument);
    EXPECT_THROW(GroupHeader::decode(bytes, group.kind, 9, 11), std::invalid_argument);
    for (size_t i = 40; i < 64; ++i) {
        auto bad = bytes; bad[i] = std::byte{1};
        EXPECT_THROW(GroupHeader::decode(bad, group.kind, 9, 12), std::invalid_argument);
    }
}
TEST(PropertySubblockCandidateTest, GroupRejectsInvalidCountsModesEmptyAndSparseColumns) {
    GroupHeader group; group.live = UINT64_MAX;
    EXPECT_NO_THROW(group.encode());
    group.memberships = 4097;
    EXPECT_THROW(group.encode(), std::invalid_argument);
    group.memberships = 47; group.columns = 1;
    EXPECT_THROW(group.encode(), std::invalid_argument);
    group.memberships = 48; EXPECT_NO_THROW(group.encode());
    group.live = (uint64_t{1} << 47) - 1;
    EXPECT_THROW(group.encode(), std::invalid_argument);
    group.columns = 0; group.live = 0;
    EXPECT_THROW(group.encode(), std::invalid_argument);
    group.live = 1; group.mode = GroupMode(0);
    EXPECT_THROW(group.encode(), std::invalid_argument);
}
TEST(PropertySubblockCandidateTest, DescriptorHasEightIndependentRootsAndExactOffsets) {
    Descriptor descriptor; descriptor.database = 0x1122334455667788; descriptor.generation = 20;
    for (size_t i = 0; i < 8; ++i) descriptor.roots[i] = {100 + i, 10 + i};
    auto bytes = descriptor.encode(20);
    EXPECT_EQ(read(bytes, 8, 8), 20); EXPECT_EQ(read(bytes, 16, 8), descriptor.database);
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(read(bytes, 64 + i * 16, 8), 100 + i);
        EXPECT_EQ(read(bytes, 72 + i * 16, 8), 10 + i);
    }
    EXPECT_EQ(Descriptor::decode(bytes, descriptor.database, 20, 20).roots, descriptor.roots);
    EXPECT_THROW(Descriptor::decode(bytes, 1, 20, 20), std::invalid_argument);
    EXPECT_THROW(Descriptor::decode(bytes, descriptor.database, 19, 20), std::invalid_argument);
    EXPECT_THROW(Descriptor::decode(bytes, descriptor.database, 20, 19), std::invalid_argument);
}
TEST(PropertySubblockCandidateTest, DescriptorEmptyRootsPartialRootsFutureRootsAndReservedBytes) {
    Descriptor descriptor; auto bytes = descriptor.encode(1);
    EXPECT_NO_THROW(Descriptor::decode(bytes, 1, 1, 1));
    for (size_t i = 0; i < 8; ++i) {
        for (RootReference root : {RootReference{1, 0}, RootReference{0, 1}, RootReference{1, 2}}) {
            descriptor.roots[i] = root;
    EXPECT_THROW(descriptor.encode(1), std::invalid_argument);
        }
        descriptor.roots[i] = {};
    }
    for (size_t i = 6; i < 64; ++i) if (i < 8 || i >= 24) {
        auto bad = bytes; bad[i] = std::byte{1};
        EXPECT_THROW(Descriptor::decode(bad, 1, 1, 1), std::invalid_argument) << i;
    }
}
TEST(PropertySubblockCandidateTest, MembershipPreservesUniqueAuthorityAndRejectsWrongOwnerToken) {
    for (Source source : {Source::HPS_COLUMN, Source::HPS_EXTENSION}) {
        Membership member{UINT64_MAX, UINT32_MAX, source}; auto bytes = member.encode();
        EXPECT_EQ(read(bytes, 0, 8), UINT64_MAX); EXPECT_EQ(read(bytes, 8, 4), UINT32_MAX);
        EXPECT_EQ(Membership::decode(bytes, member.owner, member.token).source, source);
        EXPECT_THROW(Membership::decode(bytes, 1, member.token), std::invalid_argument);
        EXPECT_THROW(Membership::decode(bytes, member.owner, 1), std::invalid_argument);
        for (size_t i = 13; i < 16; ++i) {
            auto bad = bytes; bad[i] = std::byte{1};
            EXPECT_THROW(Membership::decode(bad, member.owner, member.token), std::invalid_argument);
        }
    }
    for (Membership bad : {Membership{0, 1}, Membership{1, 0}, Membership{1, 1, Source(0)}, Membership{1, 1, Source(3)}})
        EXPECT_THROW(bad.encode(), std::invalid_argument);
}
TEST(PropertySubblockCandidateTest, LocatorChecksPageSlotGenerationAndZeroReservedBytes) {
    Locator locator{UINT64_MAX, UINT16_MAX, UINT32_MAX}; auto bytes = locator.encode();
    EXPECT_EQ(read(bytes, 0, 8), locator.page); EXPECT_EQ(read(bytes, 8, 2), locator.slot);
    EXPECT_EQ(read(bytes, 10, 2), 0); EXPECT_EQ(read(bytes, 12, 4), locator.generation);
    EXPECT_EQ(Locator::decode(bytes), locator);
    for (Locator wrong : {Locator{1, locator.slot, locator.generation}, Locator{locator.page, 0, locator.generation}, Locator{locator.page, locator.slot, 1}})
        EXPECT_THROW(locator.authenticate(wrong), std::invalid_argument);
    EXPECT_NO_THROW(locator.authenticate(locator));
    for (size_t offset : {10, 11}) {
        auto bad = bytes; bad[offset] = std::byte{1};
    EXPECT_THROW(Locator::decode(bad), std::invalid_argument);
    }
    EXPECT_THROW((Locator{0, 0, 1}.encode()), std::invalid_argument);
    EXPECT_THROW((Locator{1, 0, 0}.encode()), std::invalid_argument);
}
TEST(PropertySubblockCandidateTest, EveryFixedRecordRejectsTruncationAndUnknownVersion) {
    GroupHeader group; group.live = 1;
    Descriptor descriptor; Membership membership; Locator locator;
    const auto g = group.encode(); const auto d = descriptor.encode(1);
    const auto m = membership.encode(); const auto l = locator.encode();
    for (size_t n = 0; n < g.size(); ++n) EXPECT_THROW(GroupHeader::decode(std::span(g).first(n), group.kind, 1, 1), std::invalid_argument);
    for (size_t n = 0; n < d.size(); ++n) EXPECT_THROW(Descriptor::decode(std::span(d).first(n), 1, 1, 1), std::invalid_argument);
    for (size_t n = 0; n < 16; ++n) {
        EXPECT_THROW(Membership::decode(std::span(m).first(n), 1, 1), std::invalid_argument);
        EXPECT_THROW(Locator::decode(std::span(l).first(n)), std::invalid_argument);
    }
    auto badG = g; badG[4] = std::byte{2};
    EXPECT_THROW(GroupHeader::decode(badG, group.kind, 1, 1), std::invalid_argument);
    auto badD = d; badD[4] = std::byte{2};
    EXPECT_THROW(Descriptor::decode(badD, 1, 1, 1), std::invalid_argument);
}
TEST(PropertySubblockCandidateTest, DirectoryEntriesHaveExactSizesIdentityAndLocatorAuthentication) {
    ColumnDirectoryEntry column{42, 99, {123, 7, 8}};
    GroupDirectoryEntry group{42, {456, 9, 10}};
    auto c = column.encode(); auto g = group.encode();
    EXPECT_EQ(c.size(), 28U); EXPECT_EQ(g.size(), 24U);
    EXPECT_EQ(read(c, 0, 8), 42U); EXPECT_EQ(read(c, 8, 4), 99U);
    EXPECT_EQ(read(c, 12, 8), 123U); EXPECT_EQ(read(c, 20, 2), 7U);
    EXPECT_EQ(read(c, 22, 2), 0U); EXPECT_EQ(read(c, 24, 4), 8U);
    EXPECT_EQ(read(g, 8, 8), 456U); EXPECT_EQ(read(g, 16, 2), 9U);
    EXPECT_EQ(read(g, 18, 2), 0U); EXPECT_EQ(read(g, 20, 4), 10U);
    EXPECT_EQ(ColumnDirectoryEntry::decode(c, 42, 99).locator, column.locator);
    EXPECT_EQ(GroupDirectoryEntry::decode(g, 42).locator, group.locator);
    EXPECT_THROW(ColumnDirectoryEntry::decode(c, 43, 99), std::invalid_argument);
    EXPECT_THROW(ColumnDirectoryEntry::decode(c, 42, 100), std::invalid_argument);
    EXPECT_THROW(GroupDirectoryEntry::decode(g, 43), std::invalid_argument);
    for (size_t n = 0; n < c.size(); ++n)
        EXPECT_THROW(ColumnDirectoryEntry::decode(std::span(c).first(n), 42, 99), std::invalid_argument);
    for (size_t n = 0; n < g.size(); ++n)
        EXPECT_THROW(GroupDirectoryEntry::decode(std::span(g).first(n), 42), std::invalid_argument);
    c[22] = std::byte{1}; EXPECT_THROW(ColumnDirectoryEntry::decode(c, 42, 99), std::invalid_argument);
    g[18] = std::byte{1}; EXPECT_THROW(GroupDirectoryEntry::decode(g, 42), std::invalid_argument);
}
TEST(PropertySubblockCandidateTest, DeterministicMutationsEitherRejectOrRoundTripCanonically) {
    uint64_t random = UINT64_C(0xbadfacedeadbeef);
    auto next = [&] { random ^= random << 13; random ^= random >> 7; random ^= random << 17; return random; };
    // This is bounded malformed-input verification, not a latency measurement.
    // A scalar payload mutation may be structurally valid: outer native page
    // authentication is mandatory and deliberately outside this payload codec.
    size_t accepted = 0, rejected = 0;
    for (size_t iteration = 0; iteration < 10000; ++iteration) {
        auto column = dense(iteration % 2 ? ScalarKind::HPS_BOOL : ScalarKind::HPS_DOUBLE);
        auto bytes = image(column); auto wire = std::span(bytes).first(column.wireSize());
        for (size_t n = 0, count = 1 + next() % 4; n < count; ++n)
            wire[next() % wire.size()] ^= std::byte(1U << (next() % 8));
        try {
            const auto decoded = Column::decode(wire, column.identity);
            const auto canonical = image(decoded);
            EXPECT_TRUE(std::equal(wire.begin(), wire.end(), canonical.begin())); ++accepted;
        } catch (const std::invalid_argument &) { ++rejected; }
    }
    EXPECT_GT(accepted, 0U); EXPECT_GT(rejected, 0U);
    RecordProperty("mutations", 10000); RecordProperty("accepted_canonical", int(accepted));
    RecordProperty("rejected", int(rejected));
}
TEST(PropertySubblockCandidateTest, ConversionRequiresEveryNativeCostAndPeakEvidence) {
    auto cost = completeCost(); auto bound = limits();
    EXPECT_EQ(conversionGate(cost, bound), GateResult::HPG_ADMIT);
    for (size_t i = 0; i < COST_PARTS; ++i) {
        cost.known[i] = false; EXPECT_EQ(conversionGate(cost, bound), GateResult::HPG_UNKNOWN_COST); cost.known[i] = true;
    }
    cost.peak_known = false; EXPECT_EQ(conversionGate(cost, bound), GateResult::HPG_UNKNOWN_COST);
}
TEST(PropertySubblockCandidateTest, ConversionTenPercentGateRoundsConservativelyWithoutOverflow) {
    auto bound = limits(); bound.file_cap = UINT64_MAX;
    EXPECT_EQ(conversionGate(completeCost(1000, 900), bound), GateResult::HPG_ADMIT);
    EXPECT_EQ(conversionGate(completeCost(1000, 901), bound), GateResult::HPG_INSUFFICIENT_SAVING);
    EXPECT_EQ(conversionGate(completeCost(1001, 900), bound), GateResult::HPG_ADMIT);
    EXPECT_EQ(conversionGate(completeCost(1001, 901), bound), GateResult::HPG_INSUFFICIENT_SAVING);
    auto huge = completeCost(); huge.row_lower[0] = UINT64_MAX; huge.candidate_upper[0] = (UINT64_MAX / 10) * 9;
    huge.peak_allocated_upper = UINT64_MAX;
    EXPECT_EQ(conversionGate(huge, bound), GateResult::HPG_ADMIT);
    ++huge.row_lower[1]; EXPECT_EQ(conversionGate(huge, bound), GateResult::HPG_OVERFLOW);
    huge = completeCost(); huge.candidate_upper[1] = UINT64_MAX;
    EXPECT_EQ(conversionGate(huge, bound), GateResult::HPG_OVERFLOW);
}
TEST(PropertySubblockCandidateTest, ConversionEnforcesWorkAccountFileAndConsistentPeakBounds) {
    auto cost = completeCost(); auto bound = limits();
    for (auto wrong : {ConversionLimits{0,4096,1048576,1048576,100000},
        ConversionLimits{65,4096,1048576,1048576,100000}, ConversionLimits{64,4097,1048576,1048576,100000},
        ConversionLimits{64,4096,1048577,1048577,100000}})
        EXPECT_EQ(conversionGate(cost, wrong), GateResult::HPG_WORK_LIMIT);
    --bound.account_remaining; EXPECT_EQ(conversionGate(cost, bound), GateResult::HPG_RESOURCE_LIMIT);
    bound = limits(); bound.file_cap = 1899; EXPECT_EQ(conversionGate(cost, bound), GateResult::HPG_RESOURCE_LIMIT);
    bound.file_cap = 1900; EXPECT_EQ(conversionGate(cost, bound), GateResult::HPG_ADMIT);
    cost.peak_allocated_upper = 899; EXPECT_EQ(conversionGate(cost, bound), GateResult::HPG_INVALID_COST);
    cost = completeCost(); cost.peak_allocated_upper = 999;
    EXPECT_EQ(conversionGate(cost, bound), GateResult::HPG_INVALID_COST);
    cost = completeCost(); cost.row_lower[0] = 0; EXPECT_EQ(conversionGate(cost, bound), GateResult::HPG_INVALID_COST);
}
TEST(PropertySubblockCandidateTest, ReservedPoolCostCanRejectAttractivePayloadLowerBound) {
    // 1716 < ROW 3456 is merely a payload comparison. Two reserved native
    // pools for candidate versus one ROW pool lose even before extra metadata.
    auto cost = completeCost(131072, 262144); cost.peak_allocated_upper = 393216;
    auto bound = limits(); bound.file_cap = 1048576;
    EXPECT_EQ(conversionGate(cost, bound), GateResult::HPG_INSUFFICIENT_SAVING);
    // Explicit zero for unchanged categories still requires known=true.
    cost.known[size_t(CostPart::HPC_CATALOG_ALLOCATOR)] = false;
    EXPECT_EQ(conversionGate(cost, bound), GateResult::HPG_UNKNOWN_COST);
}
} // namespace
