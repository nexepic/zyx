#include <gtest/gtest.h>
#include "graph/storage/relocatable/property/HybridPropertyColdRowBlock.hpp"
#include "graph/storage/relocatable/property/HybridPropertyColdRowBindings.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "graph/utils/Sha256.hpp"
#include <bit>

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
using graph::query::execution::MaterializedByteLimitExceeded;
namespace {
using Block = HybridPropertyColdRowBlock;
constexpr auto NODE = EntityPropertyOwnerKind::EPOK_NODE;
struct Rows {
    std::vector<std::vector<std::byte>> roots;
    std::vector<HybridColdRowInput> inputs;
    explicit Rows(size_t count = 3, EntityPropertyOwnerKind kind = NODE, uint64_t group = 1) {
        roots.reserve(count); inputs.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto suffix = std::to_string(i);
            const auto id = (group - 1) * 64 + i + 1;
            roots.push_back(EntityPropertyCollectionRootCodec::encode({id,
                kind == NODE ? graph::Node::typeId : graph::Edge::typeId,
                {{1, PropertyValue("user-" + std::string(6U - suffix.size(), '0') + suffix)}}}));
            inputs.push_back({uint8_t(i), 1, false, roots.back()});
        }
    }
};
void refreshRawCrc(std::vector<std::byte> &bytes) {
    const auto payload = std::span<const std::byte>(bytes).subspan(Block::HEADER_BYTES);
    const auto crc = graph::utils::calculateCrc(payload.data(), payload.size());
    LittleEndianCodec::writeU32(bytes, 48, crc); LittleEndianCodec::writeU32(bytes, 52, crc);
}
}
TEST(HybridPropertyColdRowBlockTest, RawSparseOwnersRetainNullTextBytesAndExactScalarBitsForBothNamespaces) {
    for (const auto kind : {NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
        auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
        {
            const uint32_t type = kind == NODE ? graph::Node::typeId : graph::Edge::typeId;
            const double nan = std::bit_cast<double>(UINT64_C(0x7ff8000000000021));
            const auto first = EntityPropertyCollectionRootCodec::encode({257, type,
                {{1, PropertyValue(std::string("a\0b", 3))}, {3, PropertyValue()}, {7, PropertyValue(-0.0)}, {9, PropertyValue(nan)}}});
            const auto last = EntityPropertyCollectionRootCodec::encode({320, type,
                {{2, PropertyValue(std::vector<PropertyValue>{PropertyValue("nested"), PropertyValue(int64_t(4))})}}});
            const std::array<HybridColdRowInput, 2> rows{{{0, 4, false, first}, {63, 1, false, last}}};
            const auto bytes = Block::encode(kind, 5, Generation(7), rows, memory, false);
            auto decoded = Block::decode(bytes, kind, 5, Generation(9), memory);
            EXPECT_EQ(decoded.codec, HybridColdRowBlockCodec::HCRBC_RAW);
            EXPECT_EQ(decoded.fields, 5U); EXPECT_EQ(decoded.owners, UINT64_C(1) | (UINT64_C(1) << 63));
            ASSERT_TRUE(decoded.row(0)); ASSERT_TRUE(decoded.row(63)); EXPECT_FALSE(decoded.row(1));
            EXPECT_TRUE(std::ranges::equal(decoded.row(0)->root, first)); EXPECT_TRUE(std::ranges::equal(decoded.row(63)->root, last));
            const auto value = EntityPropertyCollectionRootCodec::decodeSelected(decoded.row(0)->root, 7);
            ASSERT_TRUE(value); EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(value->value.getVariant())), UINT64_C(0x8000000000000000));
            EXPECT_THROW((void)decoded.row(64), FormatException);
        }
        EXPECT_EQ(memory->used(), 0U);
    }
}
TEST(HybridPropertyColdRowBlockTest, ActualLongIdsUseBoundedBlocksAndCompressionCanReduceNativeFrameCount) {
    Rows rows(64); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        // Partition from actual canonical byte sizes, not an assumed ROW
        // header/string width; the account/page limit stays unchanged.
        const size_t limit = ZYX_HAS_LZ4 ? Block::MAX_UNCOMPRESSED_BYTES : Block::MAX_STORED_BYTES;
        size_t split = 0, bodyBytes = 0;
        for (const auto &row : rows.inputs) {
            const auto next = row.root.size() + Block::DIRECTORY_ENTRY_BYTES;
            if (next > limit - bodyBytes) break;
            bodyBytes += next; ++split;
        }
        ASSERT_GT(split, 0U); ASSERT_LT(split, rows.inputs.size());
        const auto first = Block::encode(NODE, 1, Generation(7), std::span(rows.inputs).first(split), memory);
        const auto last = Block::encode(NODE, 1, Generation(7), std::span(rows.inputs).subspan(split), memory);
        auto a = Block::decode(first, NODE, 1, Generation(7), memory);
        auto b = Block::decode(last, NODE, 1, Generation(7), memory);
        for (unsigned i = 0; i < 64; ++i) {
            const auto actual = i < split ? a.row(i) : b.row(i);
            ASSERT_TRUE(actual); EXPECT_TRUE(std::ranges::equal(actual->root, rows.roots[i]));
        }
        const size_t encodedBytes = first.size() + last.size(), blockCount = 2;
        HybridPropertyColdRowBindings bindings; bindings.owners = UINT64_MAX;
        for (unsigned i = 0; i < 64; ++i) bindings.roots[i] = {PageId(41), uint16_t(i < split ? 3 : 4), 1};
        const auto locations = bindings.layout();
        std::vector<std::byte> bindingBytes(locations.bytes); bindings.encode(bindingBytes);
        EXPECT_EQ(HybridPropertyColdRowBindings::decode(bindings.owners, locations.encoding, locations.groups, bindingBytes), bindings);
        // Future-layout geometry only, not a persisted image or whole-pool
        // ratio: three columns, membership/bindings, group and cold block(s).
        constexpr size_t columns = 3 * (64 + 64 * 8);
        const size_t membership = 64 + 3 * 20 + 64 * 8 + locations.bytes;
        const size_t frameBytes = columns + membership + 64 + encodedBytes +
            RSS_SLOTTED_PAGE_HEADER_BYTES + (5 + blockCount) * RSS_SLOTTED_PAGE_SLOT_BYTES;
        RecordProperty("cold_encoded_bytes", std::to_string(encodedBytes));
        RecordProperty("candidate_slotted_payload_bytes", std::to_string(frameBytes));
#if ZYX_HAS_LZ4
        EXPECT_LE(frameBytes, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
#endif
        EXPECT_THROW(Block::encode(NODE, 1, Generation(7), rows.inputs, memory, false), std::length_error);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyColdRowBlockTest, BoundsAndStoredCrcFailBeforeAllocatingDecompressionStorage) {
    Rows rows; auto source = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto valid = Block::encode(NODE, 1, Generation(7), rows.inputs, source, false);
    for (const unsigned defect : {0U, 1U, 2U, 3U, 4U}) {
        std::vector<std::byte> bytes(valid.begin(), valid.end());
        if (defect == 0) LittleEndianCodec::writeU32(bytes, 32, Block::MAX_UNCOMPRESSED_BYTES + 1);
        if (defect == 1) LittleEndianCodec::writeU32(bytes, 36, UINT32_MAX);
        if (defect == 2) bytes[Block::HEADER_BYTES] ^= std::byte{1};
        if (defect == 3) LittleEndianCodec::writeU64(bytes, 56, UINT64_C(1) << 63);
        if (defect == 4) bytes[6] = std::byte{255};
        auto tiny = std::make_shared<MaterializedByteBudget>(1);
        EXPECT_THROW(Block::decode(bytes, NODE, 1, Generation(7), tiny), FormatException) << defect;
        EXPECT_EQ(tiny->used(), 0U);
    }
    auto tiny = std::make_shared<MaterializedByteBudget>(1);
    EXPECT_THROW(Block::decode(valid, NODE, 1, Generation(7), tiny), MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U);
#if !ZYX_HAS_LZ4
    std::vector<std::byte> unsupported(valid.begin(), valid.end());
    unsupported[6] = std::byte{1}; unsupported.pop_back();
    LittleEndianCodec::writeU32(unsupported, 36, uint32_t(unsupported.size() - Block::HEADER_BYTES));
    const auto stored = std::span<const std::byte>(unsupported).subspan(Block::HEADER_BYTES);
    LittleEndianCodec::writeU32(unsupported, 52, graph::utils::calculateCrc(stored.data(), stored.size()));
    try { (void)Block::decode(unsupported, NODE, 1, Generation(7), tiny); FAIL() << "missing LZ4 accepted"; }
    catch (const FormatException &error) { EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION); }
    EXPECT_EQ(tiny->used(), 0U);
#endif
}
TEST(HybridPropertyColdRowBlockTest, ChecksummedMalformedDirectoriesOwnersAndCanonicalRowsNeverEscape) {
    Rows rows(2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto valid = Block::encode(NODE, 1, Generation(7), rows.inputs, memory, false);
    const auto retained = memory->used();
    for (unsigned defect = 0; defect < 9; ++defect) {
        std::vector<std::byte> bytes(valid.begin(), valid.end()); const size_t at = Block::HEADER_BYTES;
        if (defect == 0) LittleEndianCodec::writeU64(bytes, 24, 5); // different owner mask, same count
        if (defect == 1) LittleEndianCodec::writeU32(bytes, 44, 3);
        if (defect == 2) LittleEndianCodec::writeU32(bytes, at, 8);
        if (defect == 3) LittleEndianCodec::writeU32(bytes, at + 4, UINT32_MAX);
        if (defect == 4) LittleEndianCodec::writeU64(bytes, at + 8 + 8, 9); // foreign ROW owner
        if (defect == 5) LittleEndianCodec::writeU32(bytes, at + 8 + 32, 0); // invalid ROW directory
        if (defect == 6) LittleEndianCodec::writeU32(bytes, at + 8 + 16, graph::Edge::typeId);
        if (defect == 7) LittleEndianCodec::writeU64(bytes, 56, 1); // ROW root misdeclared as reference
        if (defect == 8) LittleEndianCodec::writeU32(bytes, at + 4, uint32_t(bytes.size() - at - 1));
        refreshRawCrc(bytes);
        EXPECT_THROW(Block::decode(bytes, NODE, 1, Generation(7), memory), FormatException) << defect;
        EXPECT_EQ(memory->used(), retained);
    }
    EXPECT_THROW(Block::decode(valid, EntityPropertyOwnerKind::EPOK_EDGE, 1, Generation(7), memory), FormatException);
    EXPECT_THROW(Block::decode(valid, NODE, 2, Generation(7), memory), FormatException);
    EXPECT_THROW(Block::decode(valid, NODE, 1, Generation(6), memory), FormatException);
}
TEST(HybridPropertyColdRowBlockTest, EncoderRejectsForeignNoncanonicalAndOversizedSourcesWithoutRetainedAllocations) {
    Rows rows(2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto inputs = rows.inputs; inputs[1].owner_slot = 0;
    EXPECT_THROW(Block::encode(NODE, 1, Generation(7), inputs, memory), FormatException);
    inputs = rows.inputs; inputs[0].field_count = 0;
    EXPECT_THROW(Block::encode(NODE, 1, Generation(7), inputs, memory), FormatException);
    inputs = rows.inputs; inputs[0].owner_slot = 1; inputs.resize(1);
    EXPECT_THROW(Block::encode(NODE, 1, Generation(7), inputs, memory), FormatException);
    EXPECT_THROW(Block::encode(EntityPropertyOwnerKind::EPOK_EDGE, 1, Generation(7), rows.inputs, memory), FormatException);
    EXPECT_THROW(Block::encode(NODE, 1, Generation{}, rows.inputs, memory), FormatException);
    std::vector<std::byte> oversized(Block::MAX_UNCOMPRESSED_BYTES);
    const std::array<HybridColdRowInput, 1> big{{{0, 1, false, oversized}}};
    EXPECT_THROW(Block::encode(NODE, 1, Generation(7), big, memory), std::length_error);
    inputs = rows.inputs; inputs.resize(1); inputs[0].owner_slot = 63;
    EXPECT_THROW(Block::encode(NODE, RSS_HYBRID_MAX_GROUP_ID, Generation(7), inputs, memory), FormatException);
    auto tiny = std::make_shared<MaterializedByteBudget>(1);
    EXPECT_THROW(Block::encode(NODE, 1, Generation(7), rows.inputs, tiny), MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U); EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyColdRowBlockTest, CancellationAfterScratchAllocationRefundsBothEncoderAndDecoder) {
    Rows rows; auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto valid = Block::encode(NODE, 1, Generation(7), rows.inputs, memory, false);
    struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("cold ROW cancelled") {} };
    struct Stop final : CancellationCheckpoint {
        std::shared_ptr<MaterializedByteBudget> memory; size_t retained;
        Stop(std::shared_ptr<MaterializedByteBudget> m, size_t n) : memory(std::move(m)), retained(n) {}
        void check() const override { if (memory->used() > retained) throw Cancelled(); }
    } stop(memory, memory->used());
    EXPECT_THROW(Block::encode(NODE, 1, Generation(7), rows.inputs, memory, true, &stop), Cancelled);
    EXPECT_EQ(memory->used(), stop.retained);
    EXPECT_THROW(Block::decode(valid, NODE, 1, Generation(7), memory, &stop), Cancelled);
    EXPECT_EQ(memory->used(), stop.retained);
}
TEST(HybridPropertyColdRowBlockTest, LargeRootReferencesRemainReferencesAndRequireStorageAuthenticationLater) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        const std::array payload{std::byte{42}};
        LargeValueRef reference{.value_id = LargeValueId(1), .generation = Generation(1), .logical_bytes = payload.size(),
            .content_hash = graph::utils::Sha256::digest(payload), .kind = LargeValueKind::LVK_BYTES,
            .codec = LargeValueCodec::LVC_RAW, .media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE};
        const auto root = EntityPropertyLargeValueReferenceCodec::encode({1, graph::Node::typeId, reference});
        const std::array<HybridColdRowInput, 1> rows{{{0, 1000, true, root}}};
        const auto bytes = Block::encode(NODE, 1, Generation(7), rows, memory);
        auto decoded = Block::decode(bytes, NODE, 1, Generation(7), memory);
        ASSERT_TRUE(decoded.row(0)); EXPECT_TRUE(decoded.row(0)->large_value_reference);
        EXPECT_EQ(decoded.row(0)->field_count, 1000U); EXPECT_TRUE(std::ranges::equal(decoded.row(0)->root, root));
        // No actual LargeValue store is touched and no read capability granted.
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyColdRowBlockTest, EncodedOutputRetainsItsAccountUntilTheLastOwnedBufferDies) {
    Rows rows; std::weak_ptr<MaterializedByteBudget> weak;
    std::optional<graph::utils::OwnedVector<std::byte>> escaped;
    { auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); weak = memory;
      escaped.emplace(Block::encode(NODE, 1, Generation(7), rows.inputs, memory)); }
    EXPECT_FALSE(weak.expired()); escaped.reset(); EXPECT_TRUE(weak.expired());
}

TEST(HybridPropertyColdRowBindingsTest, SharedBlocksUseGroupedMasksAndDistinctBlocksStayWithin1024Bytes) {
    HybridPropertyColdRowBindings grouped; grouped.owners = UINT64_MAX;
    for (unsigned slot = 0; slot < 64; ++slot) grouped.roots[slot] = {PageId(slot < 32 ? 41 : 42), 0, 1};
    const auto shape = grouped.layout();
    EXPECT_EQ(shape.encoding, HybridColdRowLocationEncoding::HCRLE_GROUPED);
    EXPECT_EQ(shape.groups, 2U); EXPECT_EQ(shape.bytes, 48U);
    std::vector<std::byte> bytes(shape.bytes); grouped.encode(bytes);
    EXPECT_EQ(HybridPropertyColdRowBindings::decode(grouped.owners, shape.encoding, shape.groups, bytes), grouped);
    for (unsigned slot = 0; slot < 64; ++slot) grouped.roots[slot] = {PageId(41 + slot), uint16_t(slot), UINT32_MAX};
    const auto dense = grouped.layout();
    EXPECT_EQ(dense.encoding, HybridColdRowLocationEncoding::HCRLE_DENSE);
    EXPECT_EQ(dense.bytes, HybridPropertyColdRowBindings::MAX_BYTES); EXPECT_EQ(dense.groups, 0U);
    bytes.resize(dense.bytes); grouped.encode(bytes);
    EXPECT_EQ(HybridPropertyColdRowBindings::decode(grouped.owners, dense.encoding, dense.groups, bytes), grouped);
}
TEST(HybridPropertyColdRowBindingsTest, TiesChooseDenseAndSparseOwnersKeepExactSlotGenerations) {
    HybridPropertyColdRowBindings bindings; bindings.owners = UINT64_C(1) | (UINT64_C(1) << 2) | (UINT64_C(1) << 63);
    bindings.roots[0] = {PageId(1), 7, 3}; bindings.roots[2] = {PageId(2), 8, 4}; bindings.roots[63] = bindings.roots[0];
    const auto layout = bindings.layout();
    EXPECT_EQ(layout.encoding, HybridColdRowLocationEncoding::HCRLE_DENSE); EXPECT_EQ(layout.bytes, 48U);
    std::vector<std::byte> bytes(layout.bytes); bindings.encode(bytes);
    EXPECT_EQ(HybridPropertyColdRowBindings::decode(bindings.owners, layout.encoding, layout.groups, bytes), bindings);
    HybridPropertyColdRowBindings empty; std::span<std::byte> none;
    EXPECT_EQ(empty.layout().bytes, 0U); empty.encode(none);
    EXPECT_EQ(HybridPropertyColdRowBindings::decode(0, HybridColdRowLocationEncoding::HCRLE_DENSE, 0, none), empty);
}
TEST(HybridPropertyColdRowBindingsTest, UnboundAndHiddenLocationsRefuseBeforeWritingAnyBytes) {
    HybridPropertyColdRowBindings bindings; bindings.owners = 1;
    std::array<std::byte, 16> bytes; bytes.fill(std::byte{0x5a});
    EXPECT_THROW(bindings.encode(bytes), FormatException);
    EXPECT_TRUE(std::ranges::all_of(bytes, [](auto b) { return b == std::byte{0x5a}; }));
    bindings.roots[0] = {PageId(1), 0, 1}; bindings.roots[9] = {PageId(9), 0, 1};
    EXPECT_THROW(bindings.encode(bytes), FormatException);
    bindings.roots[9] = {}; bindings.roots[0].generation = 0;
    EXPECT_THROW(bindings.layout(), FormatException);
}
TEST(HybridPropertyColdRowBindingsTest, GroupedMasksMustBeDisjointCompleteOrderedAndCanonical) {
    HybridPropertyColdRowBindings bindings; bindings.owners = UINT64_MAX;
    for (unsigned slot = 0; slot < 64; ++slot) bindings.roots[slot] = {PageId(slot < 32 ? 41 : 42), 0, 1};
    std::vector<std::byte> valid(bindings.layout().bytes); bindings.encode(valid);
    for (unsigned defect = 0; defect < 5; ++defect) {
        auto bytes = valid;
        if (defect == 0) LittleEndianCodec::writeU64(bytes, 0, 0);
        if (defect == 1) LittleEndianCodec::writeU64(bytes, 24, UINT64_MAX); // overlaps first mask
        if (defect == 2) LittleEndianCodec::writeU64(bytes, 0, 1); // incomplete cover
        if (defect == 3) std::swap_ranges(bytes.begin(), bytes.begin() + 24, bytes.begin() + 24);
        if (defect == 4) LittleEndianCodec::writeU64(bytes, 32, 41); // duplicated block reference
        EXPECT_THROW(HybridPropertyColdRowBindings::decode(bindings.owners,
            HybridColdRowLocationEncoding::HCRLE_GROUPED, 2, bytes), FormatException) << defect;
    }
    HybridPropertyColdRowBindings sparse; sparse.owners = 0x3f;
    for (unsigned i = 0; i < 6; ++i) sparse.roots[i] = {PageId(41), 0, 1};
    std::vector<std::byte> bytes(sparse.layout().bytes); sparse.encode(bytes);
    LittleEndianCodec::writeU64(bytes, 0, sparse.owners | (UINT64_C(1) << 63));
    EXPECT_THROW(HybridPropertyColdRowBindings::decode(sparse.owners,
        HybridColdRowLocationEncoding::HCRLE_GROUPED, 1, bytes), FormatException);
}
TEST(HybridPropertyColdRowBindingsTest, DecoderRejectsBadWidthsReservedBitsInvalidReferencesAndNonminimalForms) {
    HybridPropertyColdRowBindings bindings; bindings.owners = UINT64_MAX;
    for (auto &ref : bindings.roots) ref = {PageId(41), 0, 1};
    std::vector<std::byte> valid(bindings.layout().bytes); bindings.encode(valid);
    for (unsigned defect = 0; defect < 4; ++defect) {
        auto bytes = valid;
        if (defect == 0) bytes[18] = std::byte{1}; // locator reserved bytes
        if (defect == 1) LittleEndianCodec::writeU64(bytes, 8, 0);
        if (defect == 2) LittleEndianCodec::writeU16(bytes, 16, RSS_SLOTTED_PAGE_NO_SLOT);
        if (defect == 3) LittleEndianCodec::writeU32(bytes, 20, 0);
        EXPECT_THROW(HybridPropertyColdRowBindings::decode(bindings.owners,
            HybridColdRowLocationEncoding::HCRLE_GROUPED, 1, bytes), FormatException);
    }
    EXPECT_THROW(HybridPropertyColdRowBindings::decode(bindings.owners,
        HybridColdRowLocationEncoding::HCRLE_GROUPED, 65, valid), FormatException);
    EXPECT_THROW(HybridPropertyColdRowBindings::decode(bindings.owners,
        HybridColdRowLocationEncoding(255), 1, valid), FormatException);
    auto truncated = valid; truncated.pop_back();
    EXPECT_THROW(HybridPropertyColdRowBindings::decode(bindings.owners,
        HybridColdRowLocationEncoding::HCRLE_GROUPED, 1, truncated), FormatException);
    std::vector<std::byte> nonminimal(64 * 16);
    for (unsigned i = 0; i < 64; ++i) std::copy(valid.begin() + 8, valid.end(), nonminimal.begin() + i * 16);
    EXPECT_THROW(HybridPropertyColdRowBindings::decode(bindings.owners,
        HybridColdRowLocationEncoding::HCRLE_DENSE, 0, nonminimal), FormatException);
}

TEST(HybridPropertyColdRowBlockTest, RepackChangesOneOwnerPreservingOtherCanonicalBytesInBothNamespaces) {
    for (const auto kind : {NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
        Rows rows(3, kind); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
        {
            const auto source = Block::encode(kind, 1, Generation(7), rows.inputs, memory);
            const auto oldBytes = std::vector<std::byte>(source.begin(), source.end());
            const auto root = EntityPropertyCollectionRootCodec::encode({2,
                kind == NODE ? graph::Node::typeId : graph::Edge::typeId,
                {{2, PropertyValue()}, {8, PropertyValue(std::string("changed"))}}});
            const std::array<HybridColdRowInput, 1> changes{{{1, 2, false, root}}};
            const auto output = Block::repack(source, kind, 1, Generation(9), Generation(10), changes, memory);
            const auto decoded = Block::decode(output, kind, 1, Generation(10), memory);
            EXPECT_EQ(decoded.fields, 4U); EXPECT_EQ(decoded.owners, 7U);
            EXPECT_EQ(decoded.semantic_generation, Generation(10));
            EXPECT_TRUE(std::ranges::equal(decoded.row(0)->root, rows.roots[0]));
            EXPECT_TRUE(std::ranges::equal(decoded.row(1)->root, root));
            EXPECT_TRUE(std::ranges::equal(decoded.row(2)->root, rows.roots[2]));
            EXPECT_TRUE(std::ranges::equal(source, oldBytes));
        }
        EXPECT_EQ(memory->used(), 0U);
    }
}
TEST(HybridPropertyColdRowBlockTest, RepackDeletesLastRowAndCanInsertMissingOwnerWithoutChangingSource) {
    Rows rows(1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        const auto source = Block::encode(NODE, 1, Generation(7), rows.inputs, memory);
        const std::array<HybridColdRowInput, 1> erase{{{0, 0, false, {}}}};
        EXPECT_TRUE(Block::repack(source, NODE, 1, Generation(7), Generation(8), erase, memory).empty());
        const auto root = EntityPropertyCollectionRootCodec::encode({64, graph::Node::typeId, {{2, PropertyValue()}}});
        const std::array<HybridColdRowInput, 2> changes{{{63, 1, false, root}, {0, 0, false, {}}}};
        const auto output = Block::repack(source, NODE, 1, Generation(7), Generation(8), changes, memory);
        const auto decoded = Block::decode(output, NODE, 1, Generation(8), memory);
        EXPECT_EQ(decoded.owners, UINT64_C(1) << 63); EXPECT_EQ(decoded.fields, 1U);
        EXPECT_TRUE(std::ranges::equal(decoded.row(63)->root, root));
        EXPECT_EQ(Block::decode(source, NODE, 1, Generation(7), memory).owners, 1U);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyColdRowBlockTest, RepackCannotLaunderFutureReferenceByReplacingOrDeletingItsOwner) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const std::array payload{std::byte{42}};
    LargeValueRef reference{.value_id = LargeValueId(1), .generation = Generation(8),
        .logical_bytes = RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES + 16,
        .content_hash = graph::utils::Sha256::digest(payload), .kind = LargeValueKind::LVK_BYTES,
        .codec = LargeValueCodec::LVC_RAW, .media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE};
    {
        const auto root = EntityPropertyLargeValueReferenceCodec::encode({1, graph::Node::typeId, reference});
        const std::array<HybridColdRowInput, 1> original{{{0, 1, true, root}}};
        // Codec construction is not storage/manifest authentication.
        const auto source = Block::encode(NODE, 1, Generation(7), original, memory);
        const std::array<HybridColdRowInput, 1> erase{{{0, 0, false, {}}}};
        const auto used = memory->used();
        EXPECT_THROW(Block::repack(source, NODE, 1, Generation(9), Generation(10), erase, memory), FormatException);
        EXPECT_EQ(memory->used(), used);
        Rows replacement(1);
        EXPECT_THROW(Block::repack(source, NODE, 1, Generation(9), Generation(10), replacement.inputs, memory), FormatException);
        EXPECT_EQ(memory->used(), used);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyColdRowBlockTest, RepackRejectsDuplicateAndInvalidRemovalAndRefundsAfterCancellation) {
    Rows rows(2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        const auto source = Block::encode(NODE, 1, Generation(7), rows.inputs, memory);
        const std::array duplicate{rows.inputs[0], rows.inputs[0]};
        const std::array<HybridColdRowInput, 1> absent{{{63, 0, false, {}}}};
        const auto used = memory->used();
        EXPECT_THROW(Block::repack(source, NODE, 1, Generation(7), Generation(8), duplicate, memory), FormatException);
        EXPECT_THROW(Block::repack(source, NODE, 1, Generation(7), Generation(8), absent, memory), FormatException);
        EXPECT_THROW(Block::repack(source, NODE, 1, Generation(7), Generation(7), rows.inputs, memory), FormatException);
        struct Stop final : CancellationCheckpoint {
            std::shared_ptr<MaterializedByteBudget> memory; size_t before;
            Stop(std::shared_ptr<MaterializedByteBudget> m, size_t n) : memory(std::move(m)), before(n) {}
            void check() const override { if (memory->used() > before) throw std::runtime_error("cancel repack"); }
        } stop(memory, used);
        EXPECT_THROW(Block::repack(source, NODE, 1, Generation(7), Generation(8), rows.inputs, memory, &stop), std::runtime_error);
        EXPECT_EQ(memory->used(), used);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyColdRowBlockTest, RepackOversizeAndQuotaRefuseWithoutPublishingPartialRows) {
    Rows rows(2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        const auto source = Block::encode(NODE, 1, Generation(7), rows.inputs, memory);
        const auto used = memory->used();
        const auto large = EntityPropertyCollectionRootCodec::encode({2, graph::Node::typeId,
            {{1, PropertyValue(std::string(Block::MAX_UNCOMPRESSED_BYTES, 'x'))}}});
        const std::array<HybridColdRowInput, 1> changes{{{1, 1, false, large}}};
        EXPECT_THROW(Block::repack(source, NODE, 1, Generation(7), Generation(8), changes, memory), std::length_error);
        EXPECT_EQ(memory->used(), used);
        auto small = std::make_shared<MaterializedByteBudget>(1);
        EXPECT_THROW(Block::repack(source, NODE, 1, Generation(7), Generation(8), rows.inputs, small), MaterializedByteLimitExceeded);
        EXPECT_EQ(small->used(), 0U);
        EXPECT_EQ(Block::decode(source, NODE, 1, Generation(7), memory).owners, 3U);
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST(HybridPropertyColdRowBlockTest, RepackBlocksSplitsGrowthWithoutChangingCanonicalRowsOrSource) {
    Rows original(3); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        const auto source = Block::encode(NODE, 1, Generation(7), original.inputs, memory);
        std::vector<std::vector<std::byte>> roots; std::vector<HybridColdRowInput> changes;
        roots.reserve(3); changes.reserve(3);
        for (unsigned slot = 0; slot < 3; ++slot) {
            std::string text(2100, 'x'); uint32_t state = 12345 + slot;
            for (auto &c : text) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; c = char(33 + state % 90); }
            roots.push_back(EntityPropertyCollectionRootCodec::encode({slot + 1U, graph::Node::typeId, {{2, PropertyValue(text)}}}));
            changes.push_back({uint8_t(slot), 1, false, roots.back()});
        }
        const auto blocks = Block::repackBlocks(source, NODE, 1, Generation(7), Generation(8), changes, memory);
        ASSERT_EQ(blocks.size(), 3U); uint64_t owners = 0;
        for (const auto &bytes : blocks) {
            const auto block = Block::decode(bytes, NODE, 1, Generation(8), memory);
            EXPECT_EQ(owners & block.owners, 0U); owners |= block.owners;
            EXPECT_LE(block.body.size(), Block::MAX_UNCOMPRESSED_BYTES);
            for (unsigned slot = 0; slot < 3; ++slot)
                if (const auto row = block.row(slot)) EXPECT_TRUE(std::ranges::equal(row->root, roots[slot]));
        }
        EXPECT_EQ(owners, 7U);
        const auto invalid = EntityPropertyCollectionRootCodec::encode({3, graph::Node::typeId, {{2, PropertyValue(std::string(5000, 'x'))}}});
        changes[2].root = invalid; const auto before = memory->used();
        EXPECT_THROW((void)Block::repackBlocks(source, NODE, 1, Generation(7), Generation(8), changes, memory), std::length_error);
        EXPECT_EQ(memory->used(), before);
        EXPECT_EQ(Block::decode(source, NODE, 1, Generation(7), memory).owners, 7U);
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST(HybridPropertyColdRowBlockTest, FreshBlocksRequireCanonicalIdentityAndCannotRemoveMissingRows) {
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    for (const auto kind : {NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
        {
            const auto root = EntityPropertyCollectionRootCodec::encode({2,
                kind == NODE ? graph::Node::typeId : graph::Edge::typeId, {{2, PropertyValue()}}});
            const std::array<HybridColdRowInput, 1> changes{{{1, 1, false, root}}};
            const auto blocks = Block::repackBlocks({}, kind, 1, Generation(7), Generation(8), changes, memory);
            ASSERT_EQ(blocks.size(), 1U); const auto block = Block::decode(blocks.front(), kind, 1, Generation(8), memory);
            EXPECT_EQ(block.owners, 2U); EXPECT_EQ(block.fields, 1U);
            EXPECT_TRUE(std::ranges::equal(block.row(1)->root, root));
            const std::array<HybridColdRowInput, 1> erase{{{1, 0, false, {}}}};
            EXPECT_THROW((void)Block::repackBlocks({}, kind, 1, Generation(7), Generation(8), erase, memory), FormatException);
            EXPECT_THROW((void)Block::repackBlocks({}, kind, 0, Generation(7), Generation(8), changes, memory), FormatException);
        }
        EXPECT_EQ(memory->used(), 0U);
    }
}
