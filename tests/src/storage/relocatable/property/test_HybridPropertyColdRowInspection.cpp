#include <gtest/gtest.h>
#include <bit>
#include "graph/core/PropertyDecodeAllocationScope.hpp"
#include "graph/storage/relocatable/property/HybridPropertyColdRowInspection.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
using graph::query::execution::MaterializedByteLimitExceeded;
namespace {
constexpr auto NODE = EntityPropertyOwnerKind::EPOK_NODE;
using Inspection = HybridPropertyColdRowInspection;
PropertyValue decode(std::span<const std::byte> bytes) {
    size_t at = 0; auto value = CanonicalPropertyValueCodec::decodeNext(bytes, at);
    EXPECT_EQ(at, bytes.size()); return value;
}
struct Stop final : CancellationCheckpoint { void check() const override { throw std::runtime_error("cold read cancelled"); } };
}
class HybridPropertyColdRowInspectionTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {
protected:
    std::unique_ptr<LargeValueStore> values_;
    std::vector<std::byte> reference_;
    LargeValueRef value_;
    void large(std::vector<PropertyCollectionRootEntry> entries, std::string media = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE) {
        values_ = std::make_unique<LargeValueStore>(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        const auto payload = EntityPropertyLargeValuePayloadCodec::encode(entries);
        write(payload, std::move(media));
    }
    void write(std::span<const std::byte> payload, std::string media = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE) {
        if (!values_) values_ = std::make_unique<LargeValueStore>(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueWriteOptions options;
        options.first_data_slot = options.first_manifest_slot = PhysicalSlotId(40);
        options.last_data_slot = options.last_manifest_slot = PhysicalSlotId(80); options.media_type = std::move(media);
        value_ = values_->write(payload, options,
            LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1)).reference;
        reference_ = EntityPropertyLargeValueReferenceCodec::encode({1, graph::Node::typeId, value_});
    }
};
TEST_F(HybridPropertyColdRowInspectionTest, InlineSparseProjectionPreservesNullMissingTextAndExactBitsWithoutOwningValues) {
    auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    for (const auto kind : {NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
        const uint32_t type = kind == NODE ? graph::Node::typeId : graph::Edge::typeId;
        const auto root = EntityPropertyCollectionRootCodec::encode({49, type,
            {{2, PropertyValue(std::string("a\0b", 3))}, {4, PropertyValue()}, {7, PropertyValue(-0.0)},
             {9, PropertyValue(std::bit_cast<double>(UINT64_C(0x7ff8000000000021)))}}});
        {
            const std::array<uint32_t, 1> hot{1};
            Inspection source(pin, {48, 4, false, root}, {kind, 49}, pin.superblock().generation, hot, memory);
            EXPECT_EQ(source.count(), 4U); EXPECT_EQ(source.token(0), 2U); EXPECT_EQ(source.token(3), 9U);
            EXPECT_THROW(source.token(4), FormatException); EXPECT_FALSE(source.reference());
            const std::array<uint32_t, 5> selected{2, 3, 4, 7, 9}; size_t calls = 0, admitted = 0;
            const SelectedPropertyReadAdmission admission = [&](size_t bytes) { admitted += bytes; };
            source.visitSelected(selected, [&](size_t ordinal, auto bytes) {
                ++calls;
                if (ordinal == 1) { EXPECT_FALSE(bytes); return; }
                ASSERT_TRUE(bytes); const auto value = decode(*bytes);
                if (ordinal == 0) EXPECT_EQ(value, PropertyValue(std::string("a\0b", 3)));
                if (ordinal == 2) EXPECT_EQ(value, PropertyValue());
                if (ordinal >= 3) EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(value.getVariant())),
                    ordinal == 3 ? UINT64_C(0x8000000000000000) : UINT64_C(0x7ff8000000000021));
            }, &admission);
            EXPECT_EQ(calls, selected.size()); EXPECT_GT(admitted, 0U);
        }
        EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyColdRowInspectionTest, InlineIdentityCountHotOverlapQuotaAndCancelledSourcesRefuseBeforeOutput) {
    auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto root = EntityPropertyCollectionRootCodec::encode({1, graph::Node::typeId, {{2, PropertyValue("id")}}});
    const HybridColdRowInput row{0, 1, false, root};
    const std::array<uint32_t, 1> hot{2};
    EXPECT_THROW(Inspection(pin, row, {NODE, 1}, pin.superblock().generation, hot, memory), FormatException);
    EXPECT_THROW(Inspection(pin, row, {NODE, 65}, pin.superblock().generation, {}, memory), FormatException);
    EXPECT_THROW(Inspection(pin, {0, 2, false, root}, {NODE, 1}, pin.superblock().generation, {}, memory), FormatException);
    EXPECT_THROW(Inspection(pin, row, {EntityPropertyOwnerKind::EPOK_EDGE, 1}, pin.superblock().generation, {}, memory), FormatException);
    auto tiny = std::make_shared<MaterializedByteBudget>(1);
    EXPECT_THROW(Inspection(pin, row, {NODE, 1}, pin.superblock().generation, {}, tiny), MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U); EXPECT_EQ(memory->used(), 0U);
    Stop stop;
    EXPECT_THROW(Inspection(pin, row, {NODE, 1}, pin.superblock().generation, {}, memory, nullptr, &stop), std::runtime_error);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowInspectionTest, LargeWholeRootAuthenticatesRealContentAndProjectsOnlySelectedEntryRanges) {
    const auto text = std::string(32769, 'x');
    large({{2, PropertyValue(text)}, {4, PropertyValue()}, {9, PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(7))})}});
    auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        Inspection source(pin, {0, 3, true, reference_}, {NODE, 1}, pin.superblock().generation, {}, memory, values_.get());
        EXPECT_EQ(source.count(), 3U); EXPECT_EQ(source.token(1), 4U); ASSERT_TRUE(source.reference());
        EXPECT_EQ(source.reference()->value, value_);
        const std::array<uint32_t, 4> tokens{2, 3, 4, 9}; size_t calls = 0;
        source.visitSelected(tokens, [&](size_t ordinal, auto bytes) {
            ++calls;
            if (ordinal == 1) { EXPECT_FALSE(bytes); return; }
            ASSERT_TRUE(bytes);
            const auto value = decode(*bytes);
            if (ordinal == 0) EXPECT_EQ(value, PropertyValue(text));
            if (ordinal == 2) EXPECT_EQ(value, PropertyValue());
            if (ordinal == 3) EXPECT_EQ(value, PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(7))}));
        });
        EXPECT_EQ(calls, 4U); EXPECT_GT(memory->used(), 0U);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowInspectionTest, LargeReferenceOwnerMediaGenerationHashCountAndGlobalHotTokensAreChecked) {
    large({{2, PropertyValue("user-000000")}, {4, PropertyValue()}});
    auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const HybridColdRowInput row{0, 2, true, reference_};
    const std::array<uint32_t, 1> hot{2};
    EXPECT_THROW(Inspection(pin, row, {NODE, 1}, pin.superblock().generation, hot, memory, values_.get()), FormatException);
    EXPECT_THROW(Inspection(pin, row, {NODE, 1}, pin.superblock().generation, {}, memory), FormatException);
    EXPECT_THROW(Inspection(pin, {0, 1, true, reference_}, {NODE, 1}, pin.superblock().generation, {}, memory, values_.get()), FormatException);
    EXPECT_THROW(Inspection(pin, row, {NODE, 65}, pin.superblock().generation, {}, memory, values_.get()), FormatException);
    EXPECT_THROW(Inspection(pin, row, {NODE, 1}, Generation(value_.generation.value() - 1), {}, memory, values_.get()), FormatException);
    auto wrong = value_; wrong.content_hash[0] ^= std::byte{1};
    auto encoded = EntityPropertyLargeValueReferenceCodec::encode({1, graph::Node::typeId, wrong});
    EXPECT_THROW(Inspection(pin, {0, 2, true, encoded}, {NODE, 1}, pin.superblock().generation, {}, memory, values_.get()), FormatException);
    wrong = value_; wrong.media_type = "other";
    encoded = EntityPropertyLargeValueReferenceCodec::encode({1, graph::Node::typeId, wrong});
    EXPECT_THROW(Inspection(pin, {0, 2, true, encoded}, {NODE, 1}, pin.superblock().generation, {}, memory, values_.get()), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowInspectionTest, NoncanonicalLargeHeaderDirectoryAndEntryTokenFailBeforeSelectedExposure) {
    auto bytes = EntityPropertyLargeValuePayloadCodec::encode(std::vector<PropertyCollectionRootEntry>{{2, PropertyValue("id")}});
    // Persist an authentic immutable value with an internally invalid payload.
    // Content hashing alone must never authorize its Property directory.
    LittleEndianCodec::writeU32(bytes, 0, 0);
    write(bytes); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    EXPECT_THROW(Inspection(pin, {0, 1, true, reference_}, {NODE, 1}, pin.superblock().generation, {}, memory, values_.get()), FormatException);
    EXPECT_EQ(memory->used(), 0U); pin.release();
    bytes = EntityPropertyLargeValuePayloadCodec::encode(std::vector<PropertyCollectionRootEntry>{{2, PropertyValue("id")}});
    LittleEndianCodec::writeU32(bytes, RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES + 8U, 3);
    write(bytes); pin = metadata_->openSnapshot();
    {
        Inspection source(pin, {0, 1, true, reference_}, {NODE, 1}, pin.superblock().generation, {}, memory, values_.get());
        const std::array<uint32_t, 1> tokens{2}; size_t calls = 0;
        EXPECT_THROW(source.visitSelected(tokens, [&](size_t, auto) { ++calls; }), FormatException);
        EXPECT_EQ(calls, 0U);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowInspectionTest, ConsumerFailureDoesNotReplayAndReleasedPinCannotExposeBorrowedBytes) {
    auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto root = EntityPropertyCollectionRootCodec::encode({1, graph::Node::typeId,
        {{2, PropertyValue("id")}, {4, PropertyValue(int64_t(4))}}});
    {
        Inspection source(pin, {0, 2, false, root}, {NODE, 1}, pin.superblock().generation, {}, memory);
        const std::array<uint32_t, 2> tokens{2, 4}; size_t calls = 0;
        EXPECT_THROW(source.visitSelected(tokens, [&](size_t, auto) { ++calls; throw std::runtime_error("consumer"); }), std::runtime_error);
        EXPECT_EQ(calls, 1U);
        const std::array<uint32_t, 2> invalid{4, 2};
        EXPECT_THROW(source.visitSelected(invalid, [&](size_t, auto) { ++calls; }), FormatException); EXPECT_EQ(calls, 1U);
        pin.release();
        EXPECT_THROW(source.visitSelected(tokens, [&](size_t, auto) { ++calls; }), FormatException); EXPECT_EQ(calls, 1U);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowInspectionTest, LargeQuotaAndCancellationRefundAllTemporaryManifestAndRangeOwnership) {
    large({{2, PropertyValue(std::string(70000, 'x'))}});
    auto pin = metadata_->openSnapshot(); auto tiny = std::make_shared<MaterializedByteBudget>(64);
    EXPECT_THROW(Inspection(pin, {0, 1, true, reference_}, {NODE, 1}, pin.superblock().generation, {}, tiny, values_.get()), MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    struct AfterAllocation final : CancellationCheckpoint {
        std::shared_ptr<MaterializedByteBudget> memory;
        explicit AfterAllocation(std::shared_ptr<MaterializedByteBudget> m) : memory(std::move(m)) {}
        void check() const override { if (memory->used()) throw std::runtime_error("cancel after allocation"); }
    } stop(memory);
    EXPECT_THROW(Inspection(pin, {0, 1, true, reference_}, {NODE, 1}, pin.superblock().generation, {}, memory, values_.get(), &stop), std::runtime_error);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyColdRowInspectionTest, SelectedSourceNeverMaterializesNestedPropertyValues) {
    large({{2, PropertyValue(std::vector<PropertyValue>{PropertyValue(std::string(7000, 'x')), PropertyValue(int64_t(4))})}});
    auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
            throw std::logic_error("encoded cold source cannot allocate decoded Property values");
        });
        Inspection source(pin, {0, 1, true, reference_}, {NODE, 1}, pin.superblock().generation, {}, memory, values_.get());
        const std::array<uint32_t, 1> tokens{2}; size_t calls = 0;
        source.visitSelected(tokens, [&](size_t, auto bytes) { ASSERT_TRUE(bytes); EXPECT_GT(bytes->size(), 7000U); ++calls; });
        EXPECT_EQ(calls, 1U);
    }
    EXPECT_EQ(memory->used(), 0U);
}
