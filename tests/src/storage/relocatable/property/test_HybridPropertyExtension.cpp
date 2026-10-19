#include <gtest/gtest.h>
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/property/HybridPropertyExtension.hpp"
#include "../../../../../benchmarks/architecture-design/property-subblock/ReservedGroupConversion.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
using graph::query::execution::MaterializedByteLimitExceeded;
namespace {
const EntityPropertyKey OWNER{EntityPropertyOwnerKind::EPOK_NODE, 65};
constexpr uint32_t TOKEN = 9;
auto encode(const PropertyValue &value, std::pmr::memory_resource &resource) {
    return HybridPropertyExtensionView::encodeInline(OWNER, TOKEN, Generation(5), value,
        RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, resource);
}
auto open(std::span<const std::byte> bytes, const HybridPropertyExtensionView::ReferenceAdmission &admission = {}) {
    return HybridPropertyExtensionView::open(bytes, OWNER, TOKEN, Generation(7), Generation(8), Generation(9), admission);
}
auto largePayload(uint32_t token = TOKEN, size_t fields = 1) {
    std::vector<PropertyCollectionRootEntry> entries;
    for (size_t i = 0; i < fields; ++i) entries.push_back({uint32_t(token + i), PropertyValue(std::string(8192, 'x'))});
    return EntityPropertyLargeValuePayloadCodec::encode(entries);
}
LargeValueRef reference(size_t bytes) {
    LargeValueRef result; result.value_id = LargeValueId(11); result.generation = Generation(3);
    result.logical_bytes = bytes; result.content_hash[0] = std::byte{1};
    result.kind = LargeValueKind::LVK_BYTES; result.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
    return result;
}
auto encodedReference(const LargeValueRef &ref, std::pmr::memory_resource &resource) {
    const auto bytes = LargeValueRefCodec::encode(ref);
    return HybridPropertyExtensionView::encodeReference(OWNER, TOKEN, Generation(5), bytes,
        RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, resource, [](size_t) {});
}
TEST(HybridPropertyExtensionTest, InlineValuesRoundTripCanonicalTypesAndBorrowBytesWithoutWholeRowCopies) {
    std::vector<PropertyValue> values{PropertyValue{}, PropertyValue(true), PropertyValue(INT64_MIN),
        PropertyValue(-0.0), PropertyValue(std::bit_cast<double>(UINT64_C(0x7ff8000000000042))),
        PropertyValue(std::string(200, 'a')), PropertyValue(PropertyValue::Bytes(100, std::byte{0x42})),
        PropertyValue(std::vector<PropertyValue>{PropertyValue{}, PropertyValue(7), PropertyValue("nested")}),
        PropertyValue(PropertyValue::MapType{{"b", PropertyValue(8)}, {"a", PropertyValue("value")}})};
    MaterializedByteBudget memory(65536);
    for (const auto &value : values) {
        auto bytes = encode(value, memory); const auto view = open(bytes);
        EXPECT_EQ(view.owner(), OWNER); EXPECT_EQ(view.token(), TOKEN); EXPECT_EQ(view.semanticGeneration(), Generation(5));
        EXPECT_EQ(view.payload().data(), bytes.data() + 48); EXPECT_FALSE(view.reference());
        EXPECT_EQ(view.mode(), HybridPropertyExtensionMode::HPEM_INLINE);
        size_t calls = 0; auto decoded = view.decodeInline([&](const auto &shape) {
            EXPECT_EQ(shape.encoded_bytes, view.shape().encoded_bytes); ++calls;
        });
        EXPECT_EQ(calls, 1U);
        std::vector<std::byte> before, after;
        CanonicalPropertyValueCodec::append(before, value); CanonicalPropertyValueCodec::append(after, decoded);
        EXPECT_EQ(before, after); EXPECT_EQ(view.payload().size(), before.size());
        EXPECT_GT(memory.used(), 0U);
    }
    EXPECT_EQ(memory.used(), 0U);
}
TEST(HybridPropertyExtensionTest, WrongIdentityVisibilityModeReservedAndCanonicalTrailingDataReject) {
    MaterializedByteBudget memory(65536); const auto original = encode(PropertyValue("value"), memory);
    for (size_t offset : {size_t(0), size_t(4), size_t(6), size_t(7), size_t(8), size_t(16),
        size_t(20), size_t(24), size_t(32), size_t(47), size_t(48)}) {
        auto bytes = original; bytes[offset] ^= std::byte{0x80}; EXPECT_THROW(open(bytes), FormatException) << offset;
    }
    EXPECT_THROW(HybridPropertyExtensionView::open(original, {EntityPropertyOwnerKind::EPOK_EDGE, 65}, TOKEN,
        Generation(7), Generation(8), Generation(9)), FormatException);
    EXPECT_THROW(HybridPropertyExtensionView::open(original, OWNER, TOKEN + 1,
        Generation(7), Generation(8), Generation(9)), FormatException);
    for (auto generations : {std::array<uint64_t,3>{4,8,9}, {7,6,9}, {7,8,7}})
        EXPECT_THROW(HybridPropertyExtensionView::open(original, OWNER, TOKEN,
            Generation(generations[0]), Generation(generations[1]), Generation(generations[2])), FormatException);
    auto trailing = original; trailing.push_back(std::byte{});
    LittleEndianCodec::writeU32(trailing, 20, uint32_t(trailing.size() - 48)); EXPECT_THROW(open(trailing), FormatException);
    for (size_t n = 0; n < original.size(); ++n)
        EXPECT_THROW(open(std::span(original).first(n)), FormatException) << n;
}
TEST(HybridPropertyExtensionTest, NativeUsableRecordLimitAndAccountFailurePrecedeDestinationAllocation) {
    MaterializedByteBudget memory(65536), tiny(1);
    const size_t textBytes = RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES - 48 - CanonicalPropertyValueCodec::encodedSize(PropertyValue(std::string{}));
    { auto exact = encode(PropertyValue(std::string(textBytes, 'x')), memory);
      EXPECT_EQ(exact.size(), RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES); EXPECT_NO_THROW(open(exact)); }
    EXPECT_THROW(encode(PropertyValue(std::string(textBytes + 1, 'x')), memory), FormatException);
    EXPECT_EQ(memory.used(), 0U);
    EXPECT_THROW(encode(PropertyValue(1), tiny), MaterializedByteLimitExceeded); EXPECT_EQ(tiny.used(), 0U);
    EXPECT_THROW(HybridPropertyExtensionView::encodeInline(OWNER, TOKEN, Generation(5), PropertyValue(1),
        RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES + 1, memory), FormatException);
    EXPECT_EQ(memory.used(), 0U);
}
TEST(HybridPropertyExtensionTest, ValueAdmissionRefusalDoesNotChangeBorrowedSourceAndCanRetry) {
    MaterializedByteBudget memory(65536); auto bytes = encode(PropertyValue(std::string(200, 'x')), memory);
    const auto saved = bytes; const auto view = open(bytes);
    EXPECT_THROW(view.decodeInline([](const auto &) { throw std::runtime_error("refused"); }), std::runtime_error);
    EXPECT_EQ(bytes, saved);
    EXPECT_EQ(view.decodeInline([](const auto &) {}), PropertyValue(std::string(200, 'x')));
    EXPECT_THROW(view.decodeInline({}), FormatException);
}
TEST(HybridPropertyExtensionTest, LargeReferenceReusesExistingCodecAndExactlyOneTokenValuePayload) {
    MaterializedByteBudget memory(65536); const auto payload = largePayload(); auto ref = reference(payload.size());
    const auto bytes = encodedReference(ref, memory); size_t referenceCharge = 0;
    const auto view = open(bytes, [&](size_t n) { referenceCharge = n; });
    ASSERT_TRUE(view.reference()); EXPECT_EQ(*view.reference(), ref); EXPECT_GT(referenceCharge, sizeof(LargeValueRef));
    size_t values = 0, slots = 0;
    const auto entry = view.decodeLargePayload(payload, [&](const auto &shape) {
        EXPECT_GE(shape.maximum_heap_request_bytes, 8192U); ++values;
    }, [&](size_t n) { slots = n; });
    EXPECT_EQ(entry.key_token, TOKEN); EXPECT_EQ(entry.value, PropertyValue(std::string(8192, 'x')));
    EXPECT_EQ(values, 1U); EXPECT_GT(slots, 0U);
    EXPECT_THROW(view.decodeInline([](const auto &) {}), FormatException);
    EXPECT_THROW(open(bytes), FormatException);
    EXPECT_THROW(open(bytes, [](size_t) { throw std::runtime_error("reference refused"); }), std::runtime_error);
    EXPECT_NO_THROW(open(bytes, [](size_t) {}));
}
TEST(HybridPropertyExtensionTest, WholeRowsWrongTokensTrailingDataAndWrongLargeMediaNeverBecomeOneField) {
    MaterializedByteBudget memory(65536);
    for (auto payload : {largePayload(TOKEN, 2), largePayload(TOKEN + 1, 1)}) {
        auto bytes = encodedReference(reference(payload.size()), memory); const auto view = open(bytes, [](size_t) {});
        size_t decoded = 0;
        EXPECT_THROW(view.decodeLargePayload(payload, [&](const auto &) { ++decoded; }, [](size_t) {}), FormatException);
        EXPECT_EQ(decoded, 0U);
    }
    auto payload = largePayload(); payload.push_back(std::byte{});
    auto bytes = encodedReference(reference(payload.size()), memory); const auto view = open(bytes, [](size_t) {});
    EXPECT_THROW(view.decodeLargePayload(payload, [](const auto &) {}, [](size_t) {}), FormatException);
    for (unsigned bad = 0; bad < 3; ++bad) {
        auto ref = reference(8192);
        if (bad == 0) ref.media_type = "application/octet-stream";
        if (bad == 1) ref.kind = LargeValueKind::LVK_UTF8_TEXT;
        if (bad == 2) ref.generation = Generation(6);
        EXPECT_THROW(encodedReference(ref, memory), FormatException) << bad;
    }
}
TEST(HybridPropertyExtensionTest, RealDecodeAccountsRefundReferenceDirectoryAndValueRefusalBeforeRetry) {
    using graph::query::execution::MaterializedByteReservation;
    MaterializedByteBudget output(65536), refused(4096), accepted(65536);
    const auto payload = largePayload(); const auto bytes = encodedReference(reference(payload.size()), output);
    const auto before = bytes;
    EXPECT_THROW(([&] {
        MaterializedByteReservation referenceCharge(&refused), directoryCharge(&refused), valueCharge(&refused);
        const auto view = open(bytes, [&](size_t n) { referenceCharge.set(n); });
        static_cast<void>(view.decodeLargePayload(payload,
            [&](const auto &shape) { valueCharge.set(shape.maximum_heap_request_bytes); },
            [&](size_t n) { directoryCharge.set(n); }));
    }()), MaterializedByteLimitExceeded);
    EXPECT_EQ(refused.used(), 0U); EXPECT_EQ(bytes, before);
    {
        MaterializedByteReservation referenceCharge(&accepted), directoryCharge(&accepted), valueCharge(&accepted);
        const auto view = open(bytes, [&](size_t n) { referenceCharge.set(n); });
        const auto entry = view.decodeLargePayload(payload,
            [&](const auto &shape) { valueCharge.set(shape.maximum_heap_request_bytes); },
            [&](size_t n) { directoryCharge.set(n); });
        EXPECT_EQ(entry.value, PropertyValue(std::string(8192, 'x')));
        EXPECT_GT(accepted.used(), 8192U);
    }
    EXPECT_EQ(accepted.used(), 0U);
}
TEST(HybridPropertyExtensionTest, BoundedGroupPlanEncodesOnlyActualExtensionMembersAgainstOutputAccount) {
    using namespace zyx::benchmark::subblock;
    std::vector<PropertyCollectionRoot> rows{{65, graph::Node::typeId,
        {{9, PropertyValue("sparse")}, {10, PropertyValue(std::string(8192, 'x'))}}}};
    MaterializedByteBudget planning(1024 * 1024), output(65536), tiny(1);
    {
        ReservedGroupConversion plan(planning, rows, OwnerKind::HPK_NODE, 2, 5, 3952);
        const auto charge = planning.used();
        { const auto bytes = plan.encodeNativeInlineExtension(65, 9, 3952, output);
          const auto view = open(bytes); EXPECT_EQ(view.decodeInline([](const auto &) {}), PropertyValue("sparse")); }
        EXPECT_EQ(output.used(), 0U);
        EXPECT_THROW(plan.encodeNativeInlineExtension(65, 10, 3952, output), FormatException);
        EXPECT_THROW(plan.encodeNativeInlineExtension(65, 9, 3952, tiny), MaterializedByteLimitExceeded);
        EXPECT_THROW(plan.encodeNativeInlineExtension(65, 11, 3952, output), std::invalid_argument);
        EXPECT_EQ(planning.used(), charge); EXPECT_EQ(output.used(), 0U); EXPECT_EQ(tiny.used(), 0U);
        EXPECT_EQ(rows[0].entries[0].value, PropertyValue("sparse"));
    }
    EXPECT_EQ(planning.used(), 0U);
    rows.clear();
    for (uint64_t owner = 65; owner < 113; ++owner)
        rows.push_back({owner, graph::Node::typeId, {{9, PropertyValue(7)}}});
    {
        ReservedGroupConversion plan(planning, rows, OwnerKind::HPK_NODE, 2, 5, 3952);
        ASSERT_EQ(plan.columns().size(), 1U);
        EXPECT_THROW(plan.encodeNativeInlineExtension(65, 9, 3952, output), std::invalid_argument);
        EXPECT_EQ(output.used(), 0U);
    }
    EXPECT_EQ(planning.used(), 0U);
}
static_assert(!std::is_copy_constructible_v<HybridPropertyExtensionView>);
} // namespace
