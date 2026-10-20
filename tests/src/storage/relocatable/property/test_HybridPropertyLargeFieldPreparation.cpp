#include <gtest/gtest.h>
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/property/HybridPropertyLargeFieldPreparation.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"
using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyLargeFieldPreparationTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {};
}
TEST_F(HybridPropertyLargeFieldPreparationTest, SingleFieldEncoderPreservesExistingWireBytesAndRefunds) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    const std::vector<PropertyValue> values{PropertyValue{}, PropertyValue(int64_t(-77)), PropertyValue(-0.0),
        PropertyValue(true), PropertyValue(std::string(8192, 'x')),
        PropertyValue(PropertyValue::MapType{{"nested", PropertyValue(int64_t(42))}})};
    for (const auto &value : values) {
        const std::array<PropertyCollectionRootEntry, 1> entries{{{UINT32_MAX, value}}};
        const auto expected = EntityPropertyLargeValuePayloadCodec::encode(entries);
        {
            const auto actual = EntityPropertyLargeValuePayloadCodec::encodeSingleField(UINT32_MAX, value, *memory);
            EXPECT_EQ(std::vector<std::byte>(actual.begin(), actual.end()), expected);
            EXPECT_GE(memory->used(), actual.size());
        }
        EXPECT_EQ(memory->used(), 0U);
    }
    EXPECT_THROW(EntityPropertyLargeValuePayloadCodec::encodeSingleField(0, values.front(), *memory), FormatException);
    MaterializedByteBudget tiny(16);
    EXPECT_ANY_THROW(EntityPropertyLargeValuePayloadCodec::encodeSingleField(1, values.back(), tiny));
    EXPECT_EQ(tiny.used(), 0U);
}
TEST_F(HybridPropertyLargeFieldPreparationTest, ProposalOwnsPayloadWithoutPublishingOrConsumingIdentity) {
    LargeValueStore store(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    auto pin = metadata_->openSnapshot();
    const auto source = pin.superblock();
    const auto size = std::filesystem::file_size(path_);
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    PropertyValue value(std::string(8192, 'x'));
    const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, 1};
    if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED) {
        EXPECT_THROW(HybridPropertyLargeFieldPreparation::prepare(*metadata_, pin, store, owner, 7, value,
            LargeValueId(source.next_large_value_id), memory), FormatException);
        EXPECT_EQ(memory->used(), 0U); return;
    }
    {
        auto proposal = HybridPropertyLargeFieldPreparation::prepare(*metadata_, pin, store, owner, 7, value,
            LargeValueId(source.next_large_value_id), memory);
        EXPECT_FALSE(proposal.reused());
        EXPECT_GT(proposal.newDataPages(), 0U); EXPECT_GT(proposal.newManifestPages(), 0U);
        EXPECT_EQ(proposal.databaseId(), source.database_id);
        EXPECT_EQ(proposal.sourceGeneration(), source.generation);
        const auto decoded = EntityPropertyLargeValuePayloadCodec::decode(proposal.payload());
        ASSERT_EQ(decoded.size(), 1U); EXPECT_EQ(decoded[0].key_token, 7U); EXPECT_EQ(decoded[0].value, value);
        const auto payloadSize = proposal.payload().size();
        value = PropertyValue{};
        EXPECT_EQ(proposal.payload().size(), payloadSize);
        EXPECT_GT(payloadSize, 8192U);
        EXPECT_GT(memory->used(), proposal.payload().size());
    }
    EXPECT_EQ(memory->used(), 0U);
    for (size_t bound : {size_t{0}, RSS_LARGE_VALUE_DEFAULT_MAX_DEDUP_CANDIDATES + size_t{1}}) {
        EXPECT_THROW(HybridPropertyLargeFieldPreparation::prepare(*metadata_, pin, store, owner, 8,
            value, LargeValueId(source.next_large_value_id), memory, {}, LargeValueExtentClass::LVEC_32K, bound), FormatException);
        EXPECT_EQ(memory->used(), 0U);
    }
    EXPECT_EQ(metadata_->openSnapshot().superblock().generation, source.generation);
    EXPECT_EQ(metadata_->openSnapshot().superblock().next_large_value_id, source.next_large_value_id);
    EXPECT_EQ(std::filesystem::file_size(path_), size);
}

TEST_F(HybridPropertyLargeFieldPreparationTest, SharedContentReusesIdentityAndDifferentTokenDoesNot) {
    if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED) return;
    LargeValueStore store(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    const PropertyValue value(std::string(8192, 'x'));
    const std::array<PropertyCollectionRootEntry, 1> entries{{{7, value}}};
    const auto bytes = EntityPropertyLargeValuePayloadCodec::encode(entries);
    LargeValueWriteOptions placement;
    placement.first_data_slot = placement.first_manifest_slot = PhysicalSlotId(40);
    placement.last_data_slot = placement.last_manifest_slot = PhysicalSlotId(80);
    placement.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
    const auto written = store.write(bytes, placement,
        LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1)).reference;
    auto pin = metadata_->openSnapshot();
    const auto source = pin.superblock();
    const auto size = std::filesystem::file_size(path_);
    auto memory = std::make_shared<MaterializedByteBudget>(262144);
    const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_EDGE, 19};
    {
        const auto proposal = HybridPropertyLargeFieldPreparation::prepare(*metadata_, pin, store, owner, 7,
            value, LargeValueId(source.next_large_value_id), memory);
        EXPECT_TRUE(proposal.reused()); EXPECT_EQ(proposal.reference().value_id, written.value_id);
        EXPECT_EQ(proposal.candidatesScanned(), 1U);
        EXPECT_EQ(proposal.newDataPages(), 0U); EXPECT_EQ(proposal.newManifestPages(), 0U);
        const auto different = HybridPropertyLargeFieldPreparation::prepare(*metadata_, pin, store, owner, 8,
            value, LargeValueId(source.next_large_value_id), memory);
        EXPECT_FALSE(different.reused());
    }
    EXPECT_EQ(memory->used(), 0U);
    auto tiny = std::make_shared<MaterializedByteBudget>(128);
    EXPECT_ANY_THROW(HybridPropertyLargeFieldPreparation::prepare(*metadata_, pin, store, owner, 7,
        value, LargeValueId(source.next_large_value_id), tiny));
    EXPECT_EQ(tiny->used(), 0U);
    class Toggle final : public CancellationCheckpoint {
    public:
        mutable size_t calls = 0;
        void check() const override { if (++calls == 3) throw std::runtime_error("late preparation cancellation"); }
    } cancel;
    EXPECT_THROW(HybridPropertyLargeFieldPreparation::prepare(*metadata_, pin, store, owner, 7,
        value, LargeValueId(source.next_large_value_id), memory, {}, LargeValueExtentClass::LVEC_32K,
        RSS_LARGE_VALUE_DEFAULT_MAX_DEDUP_CANDIDATES, &cancel), std::runtime_error);
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_THROW(HybridPropertyLargeFieldPreparation::prepare(*metadata_, pin, store, owner, 8,
        value, LargeValueId{}, memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
    for (size_t bound : {size_t{0}, RSS_LARGE_VALUE_DEFAULT_MAX_DEDUP_CANDIDATES + size_t{1}}) {
        EXPECT_THROW(HybridPropertyLargeFieldPreparation::prepare(*metadata_, pin, store, owner, 8,
            value, LargeValueId(source.next_large_value_id), memory, {}, LargeValueExtentClass::LVEC_32K, bound), FormatException);
        EXPECT_EQ(memory->used(), 0U);
    }
    EXPECT_EQ(metadata_->openSnapshot().superblock().generation, source.generation);
    EXPECT_EQ(metadata_->openSnapshot().superblock().next_large_value_id, source.next_large_value_id);
    EXPECT_EQ(std::filesystem::file_size(path_), size);
}

TEST_F(HybridPropertyLargeFieldPreparationTest, CorruptDedupIndexCannotCompareBeyondShorterInput) {
    LargeValueStore store(io_, *metadata_);
    LargeValueWriteOptions placement;
    placement.first_data_slot = placement.first_manifest_slot = PhysicalSlotId(40);
    placement.last_data_slot = placement.last_manifest_slot = PhysicalSlotId(80);
    placement.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
    const std::vector<std::byte> shorter(37, std::byte{0x51});
    const std::vector<std::byte> longer(43, std::byte{0x51});
    const auto first = store.write(shorter, placement, LogSequenceNumber(2)).reference;
    const auto second = store.write(longer, placement, LogSequenceNumber(3)).reference;
    auto pin = metadata_->openSnapshot();
    BTreeIndex index(RSS_LARGE_VALUE_CONTENT_INDEX_ID, store.contentIndex(), *metadata_);
    auto entries = index.scan(pin, std::nullopt, 3);
    ASSERT_EQ(entries.size(), 2U);
    const auto firstKey = store.contentIndexKey(first), secondKey = store.contentIndexKey(second);
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto &entry) { return entry.key == firstKey; });
    ASSERT_NE(found, entries.end());
    std::copy(secondKey.end() - sizeof(uint64_t), secondKey.end(), found->key.end() - sizeof(uint64_t));
    found->value_or_child = second.value_id.value();
    const auto root = metadata_->findIndexRoot(pin, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
    SharedPagePoolAllocator pools(io_, *metadata_);
    const auto contents = pools.read(pin, root.root_page_id);
    const auto original = BTreePage::open(contents.owner, contents.payload);
    ASSERT_EQ(original.header().kind, BTreeNodeKind::BPN_LEAF);
    auto header = original.header(); header.entry_count = 0;
    auto replacement = BTreePage::create(contents.owner, header);
    for (auto &entry : entries) replacement.insert(std::move(entry));
    const auto location = metadata_->findPageLocation(pin, root.root_page_id);
    const auto pool = metadata_->findCatalog(pin, location.segment_id);
    const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(pin.superblock().data_region_offset,
        pool.physical_slot) + uint64_t(location.page_index) * RSS_PAGE_SIZE + RSS_PAGE_OWNER_HEADER_BYTES;
    const auto payload = replacement.serialize();
    io_->writeAt(offset, payload.data(), payload.size()); io_->sync();
    try {
        static_cast<void>(store.findContentDuplicateWithStats(pin, shorter, first.kind, first.media_type, 1));
        FAIL() << "wrong immutable identity was accepted";
    } catch (const FormatException &error) {
        EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
    }
}
