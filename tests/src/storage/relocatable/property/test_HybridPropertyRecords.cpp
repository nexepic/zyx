#include <gtest/gtest.h>
#include "graph/storage/relocatable/property/HybridPropertyRecords.hpp"

using namespace graph::storage;
namespace {
HybridPropertyGroupRecord group() {
    return {EntityPropertyOwnerKind::EPOK_EDGE, HybridPropertyGroupMode::HPGM_HYBRID,
        3, Generation(9), UINT64_MAX, 64, 1};
}
HybridPropertyColumnRecord column(HybridPropertyScalarKind kind = HybridPropertyScalarKind::HPSK_INT64) {
    HybridPropertyColumnRecord result;
    result.identity = {EntityPropertyOwnerKind::EPOK_EDGE, kind, 3, 17, Generation(7)};
    result.live = UINT64_MAX; result.presence = (UINT64_C(1) << 48) - 1;
    result.nulls = UINT64_C(1) << 4;
    for (unsigned i = 0; i < 48; ++i) if (i != 4)
        result.words[i] = kind == HybridPropertyScalarKind::HPSK_BOOL ? i % 2 : UINT64_MAX - i;
    return result;
}
auto encoded(const HybridPropertyColumnRecord &record) {
    std::vector<std::byte> bytes(record.wireSize()); record.encode(bytes); return bytes;
}
auto decode(std::span<const std::byte> bytes, const HybridPropertyColumnRecord &record,
    const HybridPropertyGroupRecord &header = group()) {
    return HybridPropertyColumnRecord::decode(bytes, record.identity, header, Generation(11), Generation(13));
}
TEST(HybridPropertyRecordsTest, CanonicalPrimitiveLayoutsPreserveRawBitsAndNullVersusMissing) {
    for (auto scalar : {HybridPropertyScalarKind::HPSK_INT64, HybridPropertyScalarKind::HPSK_DOUBLE,
        HybridPropertyScalarKind::HPSK_BOOL}) {
        auto record = column(scalar);
        if (scalar == HybridPropertyScalarKind::HPSK_DOUBLE) {
            record.words[0] = UINT64_C(0x8000000000000000); // negative zero
            record.words[1] = UINT64_C(0x7ff8000000000042); // NaN payload
        }
        const auto bytes = encoded(record);
        EXPECT_EQ(decode(bytes, record), record);
        EXPECT_EQ(bytes[0], std::byte{'Z'}); EXPECT_EQ(bytes[3], std::byte{'C'});
        EXPECT_EQ(bytes.size(), scalar == HybridPropertyScalarKind::HPSK_BOOL ? 72U : 576U);
        EXPECT_EQ(LittleEndianCodec::readU64(bytes, 8), 3U);
        EXPECT_EQ(LittleEndianCodec::readU32(bytes, 16), 17U);
        EXPECT_EQ(LittleEndianCodec::readU16(bytes, 20), 64U);
        EXPECT_EQ(LittleEndianCodec::readU64(bytes, 24), 7U);
        EXPECT_EQ(LittleEndianCodec::readU64(bytes, 40), UINT64_C(1) << 4);
        EXPECT_EQ(decode(bytes, record).words[4], 0U);
        EXPECT_EQ(decode(bytes, record).words[48], 0U);
    }
}
TEST(HybridPropertyRecordsTest, NativeIdDomainRejectsZeroOverflowAndFinalGroupPhantomSlot) {
    for (auto kind : {EntityPropertyOwnerKind::EPOK_NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
        EXPECT_EQ(hybridPropertyGroupFor({kind, 1}), 1U);
        EXPECT_EQ(hybridPropertySlotFor({kind, 64}), 63U);
        EXPECT_EQ(hybridPropertyGroupFor({kind, 65}), 2U);
        EXPECT_EQ(hybridPropertyGroupFor({kind, RSS_HYBRID_MAX_ENTITY_ID}), RSS_HYBRID_MAX_GROUP_ID);
        EXPECT_EQ(hybridPropertySlotFor({kind, RSS_HYBRID_MAX_ENTITY_ID}), 62U);
        for (uint64_t id : {UINT64_C(0), UINT64_C(1) << 63, UINT64_MAX}) {
            EXPECT_THROW(hybridPropertyGroupFor({kind, id}), FormatException);
            EXPECT_THROW(hybridPropertySlotFor({kind, id}), FormatException);
        }
    }
    EXPECT_THROW(hybridPropertyGroupFor({EntityPropertyOwnerKind(0), 1}), FormatException);
    auto header = group(); header.group_id = RSS_HYBRID_MAX_GROUP_ID;
    EXPECT_THROW(header.encode(), FormatException);
    header.live >>= 1; EXPECT_NO_THROW(header.encode());
    header.group_id++; EXPECT_THROW(header.encode(), FormatException);
    auto record = column(); record.identity.group_id = RSS_HYBRID_MAX_GROUP_ID;
    EXPECT_THROW(encoded(record), FormatException);
    record.live >>= 1; EXPECT_NO_THROW(encoded(record));
}
TEST(HybridPropertyRecordsTest, GroupCountUsesWireRangeWhileConversionKeepsItsSeparateWorkBound) {
    auto header = group();
    for (uint32_t count : {4096U, 4097U, UINT32_MAX}) {
        header.memberships = count; header.columns = count / 48;
        const auto bytes = header.encode();
        EXPECT_EQ(HybridPropertyGroupRecord::decode(bytes, header.owner_kind, 3, Generation(11), Generation(13)), header);
        header.columns++;
        EXPECT_THROW(header.encode(), FormatException);
    }
    header = group(); header.live = 0; EXPECT_THROW(header.encode(), FormatException);
    header = group(); header.live = (UINT64_C(1) << 47) - 1; EXPECT_THROW(header.encode(), FormatException);
    header = group(); header.memberships = 47; EXPECT_THROW(header.encode(), FormatException);
    header = group(); header.mode = HybridPropertyGroupMode(2); EXPECT_THROW(header.encode(), FormatException);
}
TEST(HybridPropertyRecordsTest, RetainedColumnsAllowBirthAndDeathOnlyOnAbsentCells) {
    const auto record = column(); const auto bytes = encoded(record);
    auto header = group(); header.live &= ~(UINT64_C(1) << 63);
    EXPECT_EQ(decode(bytes, record, header), record);
    header.live = record.presence; EXPECT_EQ(decode(bytes, record, header), record);
    header.live &= ~UINT64_C(1); EXPECT_THROW(decode(bytes, record, header), FormatException);
    auto older = record; older.live = record.presence;
    EXPECT_EQ(decode(encoded(older), older, group()), older);
    older.presence &= ~UINT64_C(1); older.words[0] = 0;
    EXPECT_THROW(encoded(older), FormatException); // 47 requires field demotion
}
TEST(HybridPropertyRecordsTest, CorrectBytesDoNotAuthorizeWrongGroupTokenKindTypeOrGeneration) {
    const auto record = column(); const auto bytes = encoded(record);
    for (unsigned field = 0; field < 5; ++field) {
        auto other = record;
        if (field == 0) other.identity.owner_kind = EntityPropertyOwnerKind::EPOK_NODE;
        if (field == 1) other.identity.scalar_kind = HybridPropertyScalarKind::HPSK_DOUBLE;
        if (field == 2) other.identity.group_id++;
        if (field == 3) other.identity.token++;
        if (field == 4) other.identity.semantic_generation = Generation(8);
        EXPECT_THROW(decode(bytes, other), FormatException) << field;
    }
    for (unsigned field = 0; field < 6; ++field) {
        auto header = group();
        if (field == 0) header.semantic_generation = Generation(6);
        if (field == 1) header.semantic_generation = Generation(12);
        if (field == 2) header.owner_kind = EntityPropertyOwnerKind::EPOK_NODE;
        if (field == 3) header.group_id++;
        if (field == 4) header.columns = 0;
        if (field == 5) header.memberships = 47;
        EXPECT_THROW(decode(bytes, record, header), FormatException) << field;
    }
    EXPECT_THROW(HybridPropertyColumnRecord::decode(bytes, record.identity, group(), Generation(11), Generation(10)), FormatException);
    EXPECT_THROW(HybridPropertyColumnRecord::decode(bytes, record.identity, group(), Generation{}, Generation(13)), FormatException);
    const auto groupBytes = group().encode();
    EXPECT_THROW(HybridPropertyGroupRecord::decode(groupBytes, EntityPropertyOwnerKind::EPOK_NODE, 3, Generation(11), Generation(13)), FormatException);
    EXPECT_THROW(HybridPropertyGroupRecord::decode(groupBytes, group().owner_kind, 4, Generation(11), Generation(13)), FormatException);
    EXPECT_THROW(HybridPropertyGroupRecord::decode(groupBytes, group().owner_kind, 3, Generation(8), Generation(13)), FormatException);
}
TEST(HybridPropertyRecordsTest, MalformedHeadersMasksCountsAndNoncanonicalPayloadFailClosed) {
    const auto record = column(); const auto original = encoded(record);
    for (size_t offset : {size_t(0), size_t(4), size_t(6), size_t(7), size_t(20), size_t(22),
        size_t(56), size_t(63), size_t(64 + 4 * 8), size_t(64 + 63 * 8)}) {
        auto bytes = original; bytes[offset] ^= std::byte{0x80};
        EXPECT_THROW(decode(bytes, record), FormatException) << offset;
    }
    for (size_t offset : {size_t(32), size_t(40), size_t(48)}) {
        auto bytes = original; LittleEndianCodec::writeU64(bytes, offset, offset == 40 ? UINT64_MAX : 0);
        EXPECT_THROW(decode(bytes, record), FormatException) << offset;
    }
    auto boolRecord = column(HybridPropertyScalarKind::HPSK_BOOL);
    auto boolBytes = encoded(boolRecord); boolBytes[64] |= std::byte{0x10};
    EXPECT_THROW(decode(boolBytes, boolRecord), FormatException); // NULL bit has payload
    boolRecord.words[0] = 2; EXPECT_THROW(encoded(boolRecord), FormatException);
    auto groupBytes = group().encode();
    for (size_t offset : {size_t(0), size_t(4), size_t(6), size_t(7), size_t(40), size_t(63)}) {
        auto bytes = groupBytes; bytes[offset] ^= std::byte{0x80};
        EXPECT_THROW(HybridPropertyGroupRecord::decode(bytes, group().owner_kind, 3, Generation(11), Generation(13)), FormatException);
    }
}
TEST(HybridPropertyRecordsTest, EveryTruncatedWidthAndOversizedRecordRejectWithoutReadingOutsideInput) {
    for (auto kind : {HybridPropertyScalarKind::HPSK_INT64, HybridPropertyScalarKind::HPSK_BOOL}) {
        const auto record = column(kind); auto bytes = encoded(record);
        for (size_t n = 0; n < bytes.size(); ++n)
            EXPECT_THROW(decode(std::span(bytes).first(n), record), FormatException) << n;
        bytes.push_back(std::byte{}); EXPECT_THROW(decode(bytes, record), FormatException);
    }
    const auto bytes = group().encode();
    for (size_t n = 0; n < bytes.size(); ++n)
        EXPECT_THROW(HybridPropertyGroupRecord::decode(std::span(bytes).first(n), group().owner_kind, 3,
            Generation(11), Generation(13)), FormatException) << n;
    std::array<std::byte, 65> oversized{}; std::copy(bytes.begin(), bytes.end(), oversized.begin());
    EXPECT_THROW(HybridPropertyGroupRecord::decode(oversized, group().owner_kind, 3, Generation(11), Generation(13)), FormatException);
}
} // namespace

TEST(HybridPropertyRecordsTest, ShortTextWordsAreCanonicalByteValuesAcrossNullAndMissingMasks) {
 auto record=column();record.identity.scalar_kind=HybridPropertyScalarKind::HPSK_SHORT_TEXT7;
 const std::array<std::string,5> values{"",std::string("a\0b",3),"1234567",std::string("\xe4\xb8\xad",3),std::string("\xff",1)};
 for(unsigned i=0;i<64;++i) {
  const auto bit=UINT64_C(1)<<i;
  record.words[i]=(record.presence&~record.nulls&bit)?*HybridPropertyShortText::encode(values[i%values.size()]):0;
 }
 const auto wire=encoded(record);const auto restored=decode(wire,record);
 EXPECT_EQ(restored,record);EXPECT_EQ(wire.size(),576U);
 for(unsigned i=0;i<48;++i)if(i!=4)EXPECT_EQ(HybridPropertyShortText::decode(restored.words[i]),values[i%values.size()]);
 EXPECT_FALSE(HybridPropertyShortText::encode("12345678"));
 for(const uint64_t invalid : {UINT64_C(8)<<56U,(UINT64_C(1)<<56U)|UINT64_C(0x0100),UINT64_C(1)}) {
  auto bad=record;bad.words[0]=invalid;EXPECT_THROW(bad.validate(),FormatException);
 }
 auto badNull=record;badNull.words[4]=*HybridPropertyShortText::encode("x");EXPECT_THROW(badNull.validate(),FormatException);
 auto badMissing=record;badMissing.words[63]=*HybridPropertyShortText::encode("x");EXPECT_THROW(badMissing.validate(),FormatException);
}
