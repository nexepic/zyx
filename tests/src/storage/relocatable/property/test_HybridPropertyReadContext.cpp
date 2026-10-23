#include <gtest/gtest.h>
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/property/HybridPropertyReadContext.hpp"
#include "graph/storage/relocatable/property/HybridPropertyColumnDelta.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectoryIntegrity.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyColumnWritePreparation.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyRowPreparation.hpp"
#include "graph/storage/relocatable/property/HybridPropertyValueReader.hpp"
#include "graph/storage/relocatable/property/HybridPropertyLargeFieldPreparation.hpp"
#include "graph/storage/relocatable/property/HybridPropertyPageImage.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;
#include "storage/relocatable/property/HybridPropertyReadContextTestAccess.hpp"
class HybridPropertyReadContextTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {
protected:
    HybridPropertyDescriptor descriptor_;
    std::unique_ptr<LargeValueStore> large_values_;
    bool extra_large_field_ = false;
    HybridPropertyScalarKind scalar_kind_ = HybridPropertyScalarKind::HPSK_INT64;
    uint64_t scalar_word_ = 100;
    uint32_t group_memberships_ = 49;
    unsigned live_owner_count_ = 48, extra_columns_ = 0, extra_column_missing_owner_ = 0, column_missing_owner_ = 0;
    bool orphan_owner_member_ = false, orphan_column_entry_ = false;
    unsigned orphan_physical_record_ = 0;
    bool empty_physical_page_ = false, stale_free_hint_ = false;
    bool shared_large_owner_ = false, wrong_large_hash_ = false;
    void seed(bool competingRow = false, size_t textLength = 5, bool largeField = false, bool extraRow = false) {
        NativeGraphWritePlannerOptions options;
        options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
        options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
        auto transaction = planner.beginTransaction(); transaction.reserveNodeIds(extraRow ? 65 : live_owner_count_);
        for (uint64_t i = 1; i <= live_owner_count_; ++i) transaction.createNodeWithReservedId(i, {});
        if (extraRow) {
            transaction.createNodeWithReservedId(65, {});
            transaction.replaceNodeProperty({65, graph::Node::typeId, {{2, graph::PropertyValue("row-field")}}});
        }
        if (competingRow) transaction.replaceNodeProperty({1, graph::Node::typeId, {{1, graph::PropertyValue(int64_t(1))}}});
        ASSERT_TRUE(transaction.commit().committed());
        std::optional<LargeValueRef> largeReference;
        if (largeField) {
            large_values_ = std::make_unique<LargeValueStore>(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            std::vector<PropertyCollectionRootEntry> values{{2, graph::PropertyValue(std::string(textLength, 'x'))}};
            if (extra_large_field_) values.push_back({3, graph::PropertyValue("forbidden whole-row carrier")});
            const auto payload = EntityPropertyLargeValuePayloadCodec::encode(values);
            LargeValueWriteOptions placement;
            placement.first_data_slot = placement.first_manifest_slot = PhysicalSlotId(40);
            placement.last_data_slot = placement.last_manifest_slot = PhysicalSlotId(80);
            placement.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
            largeReference = large_values_->write(payload, placement,
                LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1)).reference;
            if (wrong_large_hash_) largeReference->content_hash[0] ^= std::byte{1};
        }
        auto current = metadata_->openSnapshot().superblock();
        const auto lsn = LogSequenceNumber(current.checkpoint_lsn.value() + 1);
        // Large source values can consume the old fixture's fixed PageIds.
        // Choose unused logical IDs while retaining the historical small case.
        const auto firstPage = (std::max)(UINT64_C(41), current.next_logical_page_id);
        PageOwnerHeader owner{PageId(firstPage), 902, PageOwnerKind::POK_PROPERTY, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(99), lsn};
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
        auto image = HybridPropertyPageImage::create(owner.page_id, Generation(99), metadata_->openSnapshot().superblock().generation, memory);
        HybridPropertyGroupRecord group{EntityPropertyOwnerKind::EPOK_NODE, HybridPropertyGroupMode::HPGM_HYBRID,
            1, Generation(1), (live_owner_count_ == 64 ? UINT64_MAX : (UINT64_C(1) << live_owner_count_) - 1), group_memberships_, 1};
        if (largeReference) group.semantic_generation = metadata_->openSnapshot().superblock().generation;
        group.memberships -= unsigned(bool(column_missing_owner_));
        group.columns += extra_columns_;
        group.memberships += extra_columns_ * (live_owner_count_ - unsigned(bool(extra_column_missing_owner_)));
        const auto groupSlot = image.appendGroup(group);
        HybridPropertyColumnRecord column;
        column.identity = {group.owner_kind, scalar_kind_, 1, 1, Generation(1)};
        column.live = column.presence = group.live; column.nulls = UINT64_C(1) << 2;
        for (unsigned i = 0; i < live_owner_count_; ++i) column.words[i] = i == 2 ? 0 :
            (scalar_kind_ == HybridPropertyScalarKind::HPSK_INT64 ? i + 100 : scalar_word_);
        if (column_missing_owner_) {
            const auto slot = column_missing_owner_ - 1U;
            const auto bit = UINT64_C(1) << slot;
            column.presence &= ~bit; column.nulls &= ~bit; column.words[slot] = 0;
        }
        const auto columnSlot = image.appendColumn(column, group, group.semantic_generation);
        graph::utils::OwnedHeapReservation referenceCredit(memory);
        const auto admitReference = [&](size_t bytes) { referenceCredit.set(bytes); };
        std::pmr::vector<std::byte> extension(memory.get());
        if (largeReference) {
            const auto encoded = LargeValueRefCodec::encode(*largeReference);
            extension = HybridPropertyExtensionView::encodeReference({group.owner_kind, 1}, 2, group.semantic_generation,
                encoded, 3952, *memory, std::ref(admitReference));
        } else extension = HybridPropertyExtensionView::encodeInline({group.owner_kind, 1}, 2, group.semantic_generation,
            graph::PropertyValue(textLength == 5 ? std::string("field") : std::string(textLength, 'x')), 3952, *memory);
        const auto extensionSlot = image.appendExtension(extension, {group.owner_kind, 1}, 2, group,
            group.semantic_generation, std::ref(admitReference));
        std::vector<SlottedSlotReference> extraColumns;
        for (unsigned i = 0; i < extra_columns_; ++i) {
            auto extra = column; extra.identity.token = i + 3;
            if (extra_column_missing_owner_) {
                const auto slot = extra_column_missing_owner_ - 1U;
                const auto bit = UINT64_C(1) << slot;
                extra.presence &= ~bit; extra.nulls &= ~bit; extra.words[slot] = 0;
            }
            extraColumns.push_back(image.appendColumn(extra, group, group.semantic_generation));
        }
        std::optional<SlottedSlotReference> sharedSlot;
        if (shared_large_owner_) {
            ASSERT_TRUE(largeReference);
            const auto encoded = LargeValueRefCodec::encode(*largeReference);
            const auto other = HybridPropertyExtensionView::encodeReference({group.owner_kind, 2}, 2,
                group.semantic_generation, encoded, 3952, *memory, std::ref(admitReference));
            sharedSlot = image.appendExtension(other, {group.owner_kind, 2}, 2, group,
                group.semantic_generation, std::ref(admitReference));
        }
        if (orphan_physical_record_ == 1) static_cast<void>(image.appendGroup(group));
        if (orphan_physical_record_ == 2) static_cast<void>(image.appendColumn(column, group, group.semantic_generation));
        if (orphan_physical_record_ == 3) static_cast<void>(image.appendExtension(extension,
            {group.owner_kind, 1}, 2, group, group.semantic_generation, std::ref(admitReference)));
        if (empty_physical_page_) { image.erase(groupSlot); image.erase(columnSlot); image.erase(extensionSlot); }
        // Publish the exact slotted payload and its real free-space hint under
        // the ordinary allocation envelope, then install only the laboratory
        // native outer owner. No fabricated allocator/Page Directory hints.
        SharedPagePoolAllocationRequest request;
        request.segment_id = SegmentId(31); request.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
        request.pool_owner_id = 902; request.generation = Generation(99); request.page_id = owner.page_id;
        request.page_owner_id = 902; request.page_owner_kind = PageOwnerKind::POK_PROPERTY;
        {
            const auto encoded = image.serialize();
            request.payload.assign(encoded.begin(), encoded.end());
        }
        request.page_free_bytes = image.stats().contiguous_free_bytes + (stale_free_hint_ ? 1U : 0U);
        SharedPagePoolAllocator pools(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        const auto allocation = pools.allocateNewPoolPage(std::move(request), PhysicalSlotId(40), PhysicalSlotId(80), lsn);
        ASSERT_TRUE(allocation);
        owner.kind = PageOwnerKind::POK_HYBRID_PROPERTY;
        auto bound = std::move(image).bindPrivateOwner(owner, allocation->pool, lsn, std::ref(admitReference));
        const auto payload = bound.serializeOwned(memory);
        std::array<std::byte, RSS_PAGE_SIZE> frame{};
        const auto encodedOwner = PageOwnerHeaderCodec::encode(owner);
        std::copy(encodedOwner.begin(), encodedOwner.end(), frame.begin());
        std::copy(payload.begin(), payload.end(), frame.begin() + RSS_PAGE_OWNER_HEADER_BYTES);
        const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(current.data_region_offset,
            allocation->pool.physical_slot) + uint64_t(allocation->location.page_index) * RSS_PAGE_SIZE;
        io_->writeAt(BufferPoolPageKey{offset, 99}, frame.data(), frame.size());
        std::array<std::vector<BTreePageEntry>, 4> entries;
        const auto add = [&]<class Map>(size_t role, const typename Map::Key &key, const typename Map::Value &value) {
            const auto bytes = Map::encode(key, value);
            entries[role].push_back({std::vector<std::byte>(bytes.begin(), bytes.end()), 0});
        };
        using Codec = HybridPropertyDirectoryCodec;
        for (uint64_t id = 1; id <= live_owner_count_; ++id)
            if (id != column_missing_owner_)
                add.template operator()<HybridPropertyOwnerMap>(0, Codec::ownerKey({group.owner_kind, id}, 1),
                    Codec::sourceValue(HybridPropertySource::HPS_COLUMN));
        for (unsigned i = 0; i < extra_columns_; ++i) {
            for (uint64_t id = 1; id <= live_owner_count_; ++id)
                if (id != extra_column_missing_owner_)
                    add.template operator()<HybridPropertyOwnerMap>(0, Codec::ownerKey({group.owner_kind, id}, i + 3),
                        Codec::sourceValue(HybridPropertySource::HPS_COLUMN));
            add.template operator()<HybridPropertyFieldDirectoryMap>(1, Codec::columnKey(1, i + 3),
                Codec::locatorValue(extraColumns[i]));
        }
        add.template operator()<HybridPropertyOwnerMap>(0, Codec::ownerKey({group.owner_kind, 1}, 2),
            Codec::sourceValue(HybridPropertySource::HPS_EXTENSION));
        if (orphan_owner_member_ || shared_large_owner_)
            add.template operator()<HybridPropertyOwnerMap>(0, Codec::ownerKey({group.owner_kind, 2}, 2),
                Codec::sourceValue(HybridPropertySource::HPS_EXTENSION));
        add.template operator()<HybridPropertyFieldDirectoryMap>(1, Codec::columnKey(1, 1), Codec::locatorValue(columnSlot));
        if (orphan_column_entry_)
            add.template operator()<HybridPropertyFieldDirectoryMap>(1, Codec::columnKey(2, 1), Codec::locatorValue(columnSlot));
        add.template operator()<HybridPropertyFieldDirectoryMap>(2, Codec::ownerKey({group.owner_kind, 1}, 2), Codec::locatorValue(extensionSlot));
        if (sharedSlot) add.template operator()<HybridPropertyFieldDirectoryMap>(2,
            Codec::ownerKey({group.owner_kind, 2}, 2), Codec::locatorValue(*sharedSlot));
        add.template operator()<HybridPropertyGroupDirectoryMap>(3, Codec::groupKey(1), Codec::locatorValue(groupSlot));
        std::array<IndexRootLocation, 4> roots;
        for (size_t role = 0; role < 4; ++role) {
            const auto id = hybridPropertyIndexId(static_cast<HybridPropertyRootOrdinal>(role));
            const auto next = LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1);
            PageOwnerHeader treeOwner{PageId(firstPage + 60 + role), id, PageOwnerKind::POK_INDEX, 0,
                RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(99), next};
            BTreePageHeader header; header.index_id = id; header.page_id = treeOwner.page_id;
            header.generation = Generation(99); header.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
            auto tree = BTreePage::create(treeOwner, header);
            for (const auto &entry : entries[role]) tree.insert(entry);
            SlottedPagePoolAllocationRequest placement;
            placement.segment_id = SegmentId(71 + role); placement.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
            placement.pool_owner_id = id; placement.generation = Generation(99);
            placement.page_id = treeOwner.page_id; placement.page_owner_id = id; placement.page_owner_kind = PageOwnerKind::POK_INDEX;
            ASSERT_TRUE(btree_pages_->allocate(placement, tree, PhysicalSlotId(40), PhysicalSlotId(80), next));
            roots[role] = {id, treeOwner.page_id, Generation(99)};
            descriptor_.roots[role] = {treeOwner.page_id, Generation(99)};
        }
        static_cast<void>(btree_pages_->publishRoots(roots, {},
            LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1)));
        const auto final = metadata_->openSnapshot().superblock();
        descriptor_.database_id = final.database_id; descriptor_.semantic_generation = final.generation;
    }
};
TEST_F(HybridPropertyReadContextTest, AbsentDurableDescriptorPreservesRowAuthorityAndChecksPinLifetime) {
    auto pin = metadata_->openSnapshot();
    HybridPropertyReadContext reader(io_, *slotted_pages_, *metadata_, pin);
    size_t callbacks = 0;
    EXPECT_EQ(reader.visitField({EntityPropertyOwnerKind::EPOK_NODE, 1}, 1,
        [&](const auto &) { ++callbacks; }, [&](const auto &) { ++callbacks; }), HybridPropertyReadContext::FieldRoute::HFR_ROW);
    EXPECT_FALSE(reader.visitOwner({EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, [](auto, auto) { return true; }).routed);
    EXPECT_EQ(callbacks, 0U); pin.release();
    EXPECT_THROW(reader.visitOwner({EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, [](auto, auto) { return true; }), FormatException);
}
TEST_F(HybridPropertyReadContextTest, ReplacingTheCallerPinVariableDoesNotRebindAnExistingContext) {
    auto pin = metadata_->openSnapshot();
    HybridPropertyReadContext reader(io_, *slotted_pages_, *metadata_, pin);
    const auto generation = pin.superblock().generation;
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    auto transaction = planner.beginTransaction(); transaction.reserveNodeIds(1);
    transaction.createNodeWithReservedId(1, {}); ASSERT_TRUE(transaction.commit().committed());
    pin = metadata_->openSnapshot(); ASSERT_NE(pin.superblock().generation, generation);
    EXPECT_THROW(reader.visitOwner({EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, [](auto, auto) { return true; }), FormatException);
    HybridPropertyReadContext fresh(io_, *slotted_pages_, *metadata_, pin);
    EXPECT_FALSE(fresh.visitOwner({EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, [](auto, auto) { return true; }).routed);
}
TEST_F(HybridPropertyReadContextTest, ExistingPinCanAttachInsideTheMetadataWriterWithoutOpeningAnotherSnapshot) {
    const auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    CatalogSlotReservationRequest request;
    request.segment_id = SegmentId(222); request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
    request.owner_id = 666; request.segment_generation = Generation(1);
    request.state = SegmentState::SGS_SEALED; request.require_absent = true;
    struct ReaderUnderWriterComplete {};
    bool visited = false;
    EXPECT_THROW(static_cast<void>(metadata_->reserveFirstFreeAndUpsertCatalog(std::span(&request, 1),
        PhysicalSlotId(40), PhysicalSlotId(80), LogSequenceNumber(before.checkpoint_lsn.value() + 1),
        [&](std::span<const CatalogEntry>) {
            HybridPropertyReadContext reader(io_, *slotted_pages_, *metadata_, pin);
            EXPECT_FALSE(reader.visitOwner({EntityPropertyOwnerKind::EPOK_NODE, 1}, 1,
                [](auto, auto) { return true; }).routed);
            visited = true;
            throw ReaderUnderWriterComplete{};
        })), ReaderUnderWriterComplete);
    EXPECT_TRUE(visited); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
}
TEST_F(HybridPropertyReadContextTest, ColumnDeltaRequiresTheExperimentalFormatAndARealColumnSource) {
    const auto pin = metadata_->openSnapshot();
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    const auto next = Generation(pin.superblock().generation.value() + 1);
    if constexpr (RSS_HYBRID_PROPERTY_FORMAT_ENABLED) {
        EXPECT_FALSE(HybridPropertyColumnDelta::planExistingColumn(context, pin,
            {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, next, memory, nullptr));
    } else EXPECT_THROW(HybridPropertyColumnDelta::planExistingColumn(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, next, memory, nullptr), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyReadContextTest, CandidateRoutesActualMembershipsColumnsAndSingleFieldExtensions) {
    seed(); const auto pin = metadata_->openSnapshot();
    auto reader = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    std::vector<uint32_t> tokens;
    auto result = reader.visitOwner({EntityPropertyOwnerKind::EPOK_NODE, 1}, 2,
        [&](uint32_t token, auto) { tokens.push_back(token); return true; });
    EXPECT_TRUE(result.routed); EXPECT_TRUE(result.owner_live); EXPECT_TRUE(result.complete);
    EXPECT_EQ(result.fields, 2U); EXPECT_EQ(tokens, (std::vector<uint32_t>{1, 2}));
    const auto scalar = [&](uint64_t id, uint32_t token) {
        return reader.visitField({EntityPropertyOwnerKind::EPOK_NODE, id}, token,
            [&](const auto &cell) { EXPECT_TRUE(cell.present); EXPECT_EQ(cell.is_null, id == 3); EXPECT_EQ(cell.word, id == 3 ? 0 : id + 99); },
            [&](const auto &value) { EXPECT_EQ(value.decodeInline([](const auto &) {}), graph::PropertyValue("field")); });
    };
    EXPECT_EQ(scalar(1, 1), HybridPropertyReadContext::FieldRoute::HFR_FOUND);
    EXPECT_EQ(scalar(3, 1), HybridPropertyReadContext::FieldRoute::HFR_FOUND);
    EXPECT_EQ(scalar(1, 2), HybridPropertyReadContext::FieldRoute::HFR_FOUND);
    EXPECT_EQ(scalar(2, 2), HybridPropertyReadContext::FieldRoute::HFR_MISSING);
    EXPECT_EQ(scalar(49, 1), HybridPropertyReadContext::FieldRoute::HFR_MISSING);
    EXPECT_EQ(scalar(65, 1), HybridPropertyReadContext::FieldRoute::HFR_ROW);
    EXPECT_EQ(reader.visitField({EntityPropertyOwnerKind::EPOK_EDGE, 1}, 1, [](const auto &) {}, [](const auto &) {}),
        HybridPropertyReadContext::FieldRoute::HFR_ROW);
    EXPECT_FALSE(reader.visitOwner({EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, [](auto, auto) { return true; }).complete);
    // The normal constructor must not trust this laboratory's orphan trees.
    HybridPropertyReadContext normal(io_, *slotted_pages_, *metadata_, pin);
    EXPECT_FALSE(normal.visitOwner({EntityPropertyOwnerKind::EPOK_NODE, 1}, 2, [](auto, auto) { return true; }).routed);
}
TEST_F(HybridPropertyReadContextTest, OwningCompatibilityDecodeRetainsCallerOutputCreditAndRefundsTemporaries) {
    seed(false, 128); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    graph::utils::OwnedAllocationScope owned(memory);
    graph::utils::OwnedHeapReservation output(memory);
    graph::property_decode::AllocationScope decode(&output, [](void *credit, size_t bytes) {
        auto &value = *static_cast<graph::utils::OwnedHeapReservation *>(credit);
        value.set(value.bytes() + bytes);
    });
    {
        graph::query::execution::MaterializedByteReservation hold(memory.get()); hold.set(memory->available());
        EXPECT_THROW(HybridPropertyValueReader::read(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 2),
            graph::query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(memory->used(), 0U);
    auto result = HybridPropertyValueReader::read(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 2);
    ASSERT_TRUE(result.value); EXPECT_EQ(result.value->value, graph::PropertyValue(std::string(128, 'x')));
    EXPECT_GT(output.bytes(), 0U); EXPECT_EQ(memory->used(), output.bytes());
    result.value.reset(); output.set(0); EXPECT_EQ(memory->used(), 0U);
    auto scalar = HybridPropertyValueReader::read(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1);
    ASSERT_TRUE(scalar.value); EXPECT_EQ(scalar.value->value, graph::PropertyValue(int64_t(100)));
    auto missing = HybridPropertyValueReader::read(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 2}, 2);
    EXPECT_FALSE(missing.value);
    auto anotherPin = metadata_->openSnapshot();
    EXPECT_THROW(HybridPropertyValueReader::read(context, anotherPin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1), FormatException);
}
TEST_F(HybridPropertyReadContextTest, IndependentLargeFieldAuthenticatesContentAndRetainsOnlyOutputCredit) {
    seed(false, 8192, true); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    // Selected scalars never require the unrelated immutable value store.
    auto scalar = HybridPropertyValueReader::read(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1);
    ASSERT_TRUE(scalar.value); EXPECT_EQ(scalar.value->value, graph::PropertyValue(int64_t(100)));
    try {
        static_cast<void>(HybridPropertyValueReader::read(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 2));
        FAIL() << "large field requires its value store";
    } catch (const FormatException &error) { EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE); }
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    graph::utils::OwnedAllocationScope owned(memory);
    graph::utils::OwnedHeapReservation output(memory);
    graph::property_decode::AllocationScope decode(&output, [](void *credit, size_t bytes) {
        auto &value = *static_cast<graph::utils::OwnedHeapReservation *>(credit); value.set(value.bytes() + bytes);
    });
    auto result = HybridPropertyValueReader::read(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 2, large_values_.get());
    ASSERT_TRUE(result.value); EXPECT_EQ(result.value->value, graph::PropertyValue(std::string(8192, 'x')));
    EXPECT_GT(output.bytes(), 0U); EXPECT_EQ(memory->used(), output.bytes());
    result.value.reset(); output.set(0); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyReadContextTest, WholeRowLargeCarrierWithValidManifestAndHashStillFailsSingleFieldDecode) {
    extra_large_field_ = true; seed(false, 8192, true); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    EXPECT_THROW(HybridPropertyValueReader::read(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 2, large_values_.get()), FormatException);
}
TEST_F(HybridPropertyReadContextTest, MembershipCountsTokensAndPresenceDoNotDecodePropertyValues) {
    seed(false, 128); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    graph::utils::OwnedAllocationScope owned(memory);
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
        throw std::logic_error("metadata projection must not decode a PropertyValue");
    });
    EXPECT_EQ(context.count({EntityPropertyOwnerKind::EPOK_NODE, 1}), 2U);
    EXPECT_EQ(context.count({EntityPropertyOwnerKind::EPOK_NODE, 2}), 1U);
    EXPECT_EQ(context.count({EntityPropertyOwnerKind::EPOK_NODE, 49}), 0U);
    EXPECT_FALSE(context.count({EntityPropertyOwnerKind::EPOK_NODE, 65}));
    graph::utils::OwnedHeapReservation output(memory), transient(memory);
    const auto admitOutput = [&](size_t bytes) { output.set(bytes); };
    const auto admitTransient = [&](size_t bytes) { transient.set(bytes); };
    auto tokens = context.tokens({EntityPropertyOwnerKind::EPOK_NODE, 1}, std::ref(admitOutput), std::ref(admitTransient));
    ASSERT_TRUE(tokens); EXPECT_EQ(*tokens, (std::vector<uint32_t>{1, 2}));
    EXPECT_EQ(memory->used(), output.bytes()); EXPECT_EQ(transient.bytes(), 0U);
    tokens.reset(); output.set(0); EXPECT_EQ(memory->used(), 0U);
    EXPECT_THROW(context.tokens({EntityPropertyOwnerKind::EPOK_NODE, 1},
        [](size_t bytes) { if (bytes) throw std::length_error("deny token output"); }, std::ref(admitTransient)), std::length_error);
    auto retry = context.tokens({EntityPropertyOwnerKind::EPOK_NODE, 1}, std::ref(admitOutput), std::ref(admitTransient));
    ASSERT_TRUE(retry); EXPECT_EQ(retry->size(), 2U); retry.reset(); output.set(0);
    EXPECT_EQ(HybridPropertyValueReader::exists(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 3}, 1), true);
    EXPECT_EQ(HybridPropertyValueReader::exists(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 3}, 1, true), false);
    EXPECT_EQ(HybridPropertyValueReader::exists(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 2, true), true);
    EXPECT_EQ(HybridPropertyValueReader::exists(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 2}, 2), false);
    EXPECT_FALSE(HybridPropertyValueReader::exists(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 65}, 1));
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyReadContextTest, LargeNonNullProjectionAuthenticatesWithoutMaterializingTheValueTree) {
    seed(false, 8192, true); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    graph::utils::OwnedAllocationScope owned(memory);
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
        throw std::logic_error("non-NULL projection must not decode a PropertyValue");
    });
    EXPECT_EQ(HybridPropertyValueReader::exists(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 2), true);
    EXPECT_EQ(HybridPropertyValueReader::exists(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 2, true, large_values_.get()), true);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyReadContextTest, MissingSourceAndCompetingRowFailClosed) {
    seed(true); const auto pin = metadata_->openSnapshot();
    auto reader = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    EXPECT_THROW(reader.visitField({EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, [](const auto &) {}, [](const auto &) {}), FormatException);
    auto incomplete = descriptor_; incomplete.roots[1] = {};
    auto missing = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, incomplete);
    EXPECT_THROW(missing.visitField({EntityPropertyOwnerKind::EPOK_NODE, 2}, 1, [](const auto &) {}, [](const auto &) {}), FormatException);
    EXPECT_THROW(missing.count({EntityPropertyOwnerKind::EPOK_NODE, 2}), FormatException);
}
#endif

#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyReadContextTest, FullCollectionAdmitsAllEntriesBeforeOutputAndRetainsOnlyReturnedStorage) {
    seed(false, 128); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    graph::utils::OwnedAllocationScope owned(memory);
    graph::utils::OwnedHeapReservation output(memory), slots(memory);
    graph::property_decode::AllocationScope decode(&output, [](void *credit, size_t bytes) {
        auto &value = *static_cast<graph::utils::OwnedHeapReservation *>(credit);
        value.set(value.bytes() + bytes);
    });
    const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 1};
    size_t admissions = 0, wire = 0;
    SelectedPropertyReadAdmission reject = [&](size_t bytes) {
        EXPECT_EQ(output.bytes(), 0U); EXPECT_EQ(slots.bytes(), 0U);
        ++admissions; wire += bytes;
        if (admissions == 2) throw std::length_error("reject second encoded entry");
    };
    SelectedPropertyReadAdmission admitSlots = [&](size_t bytes) { slots.set(bytes); };
    EXPECT_THROW(HybridPropertyValueReader::readCollection(context, pin, key, graph::Node::typeId,
        nullptr, &reject, &admitSlots), std::length_error);
    EXPECT_EQ(admissions, 2U); EXPECT_EQ(memory->used(), 0U);
    size_t accepted = 0;
    SelectedPropertyReadAdmission accept = [&](size_t bytes) {
        EXPECT_EQ(output.bytes(), 0U); EXPECT_EQ(slots.bytes(), 0U); accepted += bytes;
    };
    auto result = HybridPropertyValueReader::readCollection(context, pin, key, graph::Node::typeId,
        nullptr, &accept, &admitSlots);
    ASSERT_TRUE(result.routed); ASSERT_TRUE(result.value);
    EXPECT_EQ(accepted, wire); EXPECT_EQ(result.value->owner_entity_id, 1U);
    EXPECT_EQ(result.value->owner_entity_type, graph::Node::typeId);
    ASSERT_EQ(result.value->entries.size(), 2U);
    EXPECT_EQ(result.value->entries[0], (PropertyCollectionRootEntry{1, graph::PropertyValue(int64_t(100))}));
    EXPECT_EQ(result.value->entries[1], (PropertyCollectionRootEntry{2, graph::PropertyValue(std::string(128, 'x'))}));
    EXPECT_EQ(memory->used(), output.bytes() + slots.bytes());
    result.value.reset(); output.set(0); slots.set(0); EXPECT_EQ(memory->used(), 0U);
    auto empty = HybridPropertyValueReader::readCollection(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 49}, graph::Node::typeId);
    EXPECT_TRUE(empty.routed); EXPECT_FALSE(empty.value);
    auto row = HybridPropertyValueReader::readCollection(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 65}, graph::Node::typeId);
    EXPECT_FALSE(row.routed); EXPECT_FALSE(row.value);
}

TEST_F(HybridPropertyReadContextTest, FullCollectionLargeAdmissionDoesNotAllocateDecodedValues) {
    seed(false, 8192, true); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
        throw std::logic_error("encoded admission must precede decoded values");
    });
    size_t entries = 0;
    SelectedPropertyReadAdmission reject = [&](size_t) {
        if (++entries == 2) throw std::length_error("deny large entry");
    };
    EXPECT_THROW(HybridPropertyValueReader::readCollection(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 1}, graph::Node::typeId, large_values_.get(), &reject), std::length_error);
    EXPECT_EQ(entries, 2U);
}


TEST_F(HybridPropertyReadContextTest, SelectedBatchPreservesRowPositionsCountsNullsAndSinkOrdinals) {
    seed(false, 5, false, true); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    const std::array<EntityPropertyKey, 4> owners{{{EntityPropertyOwnerKind::EPOK_NODE, 1},
        {EntityPropertyOwnerKind::EPOK_NODE, 3}, {EntityPropertyOwnerKind::EPOK_NODE, 49},
        {EntityPropertyOwnerKind::EPOK_NODE, 65}}};
    const std::array<uint32_t, 3> tokens{1, 2, 3};
    std::array<std::optional<PropertyCollectionRootEntry>, 12> values{};
    values[10] = PropertyCollectionRootEntry{2, graph::PropertyValue("untouched-row-output")};
    std::array<std::optional<size_t>, 4> counts{777, 777, 777, 777};
    EXPECT_FALSE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, values, counts));
    EXPECT_EQ(counts, (std::array<std::optional<size_t>, 4>{2, 1, 0, 777}));
    ASSERT_TRUE(values[0]); EXPECT_EQ(values[0]->value, graph::PropertyValue(int64_t(100)));
    ASSERT_TRUE(values[1]); EXPECT_EQ(values[1]->value, graph::PropertyValue("field"));
    EXPECT_FALSE(values[2]); ASSERT_TRUE(values[3]); EXPECT_EQ(values[3]->value, graph::PropertyValue());
    EXPECT_FALSE(values[4]); EXPECT_FALSE(values[6]);
    ASSERT_TRUE(values[10]); EXPECT_EQ(values[10]->value, graph::PropertyValue("untouched-row-output"));
    auto row = properties_->readEntityPropertyOptional(pin, owners[3], 2);
    ASSERT_TRUE(row); EXPECT_EQ(row->value, graph::PropertyValue("row-field"));
    EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin,
        std::span(owners).first(3), tokens, std::span(values).first(9), std::span(counts).first(3)));
    std::vector<std::pair<size_t, size_t>> emitted;
    std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> sink =
        [&](size_t owner, size_t token, std::optional<PropertyCollectionRootEntry> value) {
            emitted.emplace_back(owner, token);
            EXPECT_EQ(value, values[owner * tokens.size() + token]);
        };
    EXPECT_FALSE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr, nullptr, &sink));
    EXPECT_EQ(emitted, (std::vector<std::pair<size_t, size_t>>{{0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}}));
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
        throw std::logic_error("count-only batch cannot decode values");
    });
    EXPECT_FALSE(HybridPropertyValueReader::readSelected(context, pin, owners, {}, {}, counts));
    EXPECT_EQ(counts[0], 2U); EXPECT_EQ(counts[3], 777U);
    const std::array<EntityPropertyKey, 2> reversed{owners[1], owners[0]};
    const auto before = counts;
    EXPECT_THROW(HybridPropertyValueReader::readSelected(context, pin, reversed, {}, {}, std::span(counts).first(2)), FormatException);
    EXPECT_EQ(counts, before);
}

TEST_F(HybridPropertyReadContextTest, NativeScalarBatchBypassesOwningValuesAndRefundsOnRefusalAndRetry) {
    seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    graph::utils::OwnedAllocationScope owned(memory);
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
        throw std::logic_error("native scalar must not materialize PropertyValue");
    });
    const std::array<EntityPropertyKey, 2> owners{{{EntityPropertyOwnerKind::EPOK_NODE, 1},
        {EntityPropertyOwnerKind::EPOK_NODE, 3}}};
    const std::array<uint32_t, 1> tokens{1};
    std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
        [](size_t, size_t, auto) { throw std::logic_error("native column used owning fallback"); };
    size_t emitted = 0, encoded = 0;
    PropertyScalarReadSink scalar = [&](size_t row, size_t column, graph::PropertyType type, uint64_t bits) {
        EXPECT_EQ(row, emitted++); EXPECT_EQ(column, 0U);
        EXPECT_EQ(type, row == 0 ? graph::PropertyType::INTEGER : graph::PropertyType::NULL_TYPE);
        EXPECT_EQ(bits, row == 0 ? 100U : 0U);
    };
    SelectedPropertyReadAdmission admission = [&](size_t bytes) { encoded += bytes; };
    {
        graph::query::execution::MaterializedByteReservation hold(memory.get()); hold.set(memory->available());
        EXPECT_THROW(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {},
            nullptr, &admission, &owning, &scalar), graph::query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(emitted, 0U); EXPECT_EQ(memory->used(), 0U);
    EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr,
        &admission, &owning, &scalar));
    EXPECT_EQ(emitted, 2U); EXPECT_EQ(encoded, 28U); EXPECT_EQ(memory->used(), 0U);
    PropertyScalarReadSink cancel = [](size_t, size_t, graph::PropertyType, uint64_t) {
        throw std::runtime_error("consumer cancellation");
    };
    EXPECT_THROW(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr,
        nullptr, &owning, &cancel), std::runtime_error);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, RawBoolColumnPreservesAuthenticatedBitsWithoutOwningValues) {
    scalar_kind_ = HybridPropertyScalarKind::HPSK_BOOL; scalar_word_ = 1;
    seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
        throw std::logic_error("raw column cannot allocate decoded PropertyValue");
    });
    const std::array<EntityPropertyKey, 2> owners{{{EntityPropertyOwnerKind::EPOK_NODE, 1},
        {EntityPropertyOwnerKind::EPOK_NODE, 3}}};
    const std::array<uint32_t, 1> tokens{1};
    std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
        [](size_t, size_t, auto) { throw std::logic_error("raw column used owning decoder"); };
    size_t emitted = 0, bytes = 0;
    PropertyScalarReadSink raw = [&](size_t row, size_t column, graph::PropertyType type, uint64_t bits) {
        EXPECT_EQ(row, emitted++); EXPECT_EQ(column, 0U);
        EXPECT_EQ(type, row == 0 ? graph::PropertyType::BOOLEAN : graph::PropertyType::NULL_TYPE);
        EXPECT_EQ(bits, row == 0 ? 1 : 0U);
    };
    SelectedPropertyReadAdmission admit = [&](size_t count) { bytes += count; };
    EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr,
        &admit, &owning, &raw));
    EXPECT_EQ(emitted, 2U); EXPECT_EQ(bytes, 21U);
}

TEST_F(HybridPropertyReadContextTest, RawDoubleNaNColumnPreservesAuthenticatedBitsWithoutOwningValues) {
    scalar_kind_ = HybridPropertyScalarKind::HPSK_DOUBLE; scalar_word_ = UINT64_C(0x7ff8000000000042);
    seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
        throw std::logic_error("raw column cannot allocate decoded PropertyValue");
    });
    const std::array<EntityPropertyKey, 2> owners{{{EntityPropertyOwnerKind::EPOK_NODE, 1},
        {EntityPropertyOwnerKind::EPOK_NODE, 3}}};
    const std::array<uint32_t, 1> tokens{1};
    std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
        [](size_t, size_t, auto) { throw std::logic_error("raw column used owning decoder"); };
    size_t emitted = 0, bytes = 0;
    PropertyScalarReadSink raw = [&](size_t row, size_t column, graph::PropertyType type, uint64_t bits) {
        EXPECT_EQ(row, emitted++); EXPECT_EQ(column, 0U);
        EXPECT_EQ(type, row == 0 ? graph::PropertyType::DOUBLE : graph::PropertyType::NULL_TYPE);
        EXPECT_EQ(bits, row == 0 ? UINT64_C(0x7ff8000000000042) : 0U);
    };
    SelectedPropertyReadAdmission admit = [&](size_t count) { bytes += count; };
    EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr,
        &admit, &owning, &raw));
    EXPECT_EQ(emitted, 2U); EXPECT_EQ(bytes, 28U);
}

TEST_F(HybridPropertyReadContextTest, RawDoubleNegativeZeroColumnPreservesAuthenticatedBitsWithoutOwningValues) {
    scalar_kind_ = HybridPropertyScalarKind::HPSK_DOUBLE; scalar_word_ = UINT64_C(0x8000000000000000);
    seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
        throw std::logic_error("raw column cannot allocate decoded PropertyValue");
    });
    const std::array<EntityPropertyKey, 2> owners{{{EntityPropertyOwnerKind::EPOK_NODE, 1},
        {EntityPropertyOwnerKind::EPOK_NODE, 3}}};
    const std::array<uint32_t, 1> tokens{1};
    std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
        [](size_t, size_t, auto) { throw std::logic_error("raw column used owning decoder"); };
    size_t emitted = 0, bytes = 0;
    PropertyScalarReadSink raw = [&](size_t row, size_t column, graph::PropertyType type, uint64_t bits) {
        EXPECT_EQ(row, emitted++); EXPECT_EQ(column, 0U);
        EXPECT_EQ(type, row == 0 ? graph::PropertyType::DOUBLE : graph::PropertyType::NULL_TYPE);
        EXPECT_EQ(bits, row == 0 ? UINT64_C(0x8000000000000000) : 0U);
    };
    SelectedPropertyReadAdmission admit = [&](size_t count) { bytes += count; };
    EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr,
        &admit, &owning, &raw));
    EXPECT_EQ(emitted, 2U); EXPECT_EQ(bytes, 28U);
}

TEST_F(HybridPropertyReadContextTest, SelectedOnlyBatchDoesNotEnumerateUnrequestedFieldMemberships) {
    seed(); const auto pin = metadata_->openSnapshot();
    // The requested column and its owner membership remain valid. An absent
    // unrelated extension directory must be found by counts/integrity, without
    // forcing selected-only reads to traverse that unrelated membership.
    descriptor_.roots[2] = {};
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    const std::array<EntityPropertyKey, 1> owners{{{EntityPropertyOwnerKind::EPOK_NODE, 1}}};
    const std::array<uint32_t, 1> tokens{1};
    size_t emitted = 0;
    std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
        [](size_t, size_t, auto) { throw std::logic_error("selected scalar used owning decoder"); };
    PropertyScalarReadSink raw = [&](size_t row, size_t column, graph::PropertyType type, uint64_t bits) {
        ++emitted; EXPECT_EQ(row, 0U); EXPECT_EQ(column, 0U);
        EXPECT_EQ(type, graph::PropertyType::INTEGER); EXPECT_EQ(bits, 100U);
    };
    EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr,
        nullptr, &owning, &raw));
    EXPECT_EQ(emitted, 1U);
    std::array<std::optional<size_t>, 1> counts{};
    EXPECT_THROW(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, counts, nullptr,
        nullptr, &owning, &raw), FormatException);
    EXPECT_EQ(emitted, 1U); EXPECT_FALSE(counts[0]);
}

TEST_F(HybridPropertyReadContextTest, ColumnDeltaUsesActualNativeSourceAndRetainsOnlyItsBoundedPlan) {
    seed(); const auto pin = metadata_->openSnapshot(); const auto bytes = io_->fileSize();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, 1};
    const auto next = Generation(pin.superblock().generation.value() + 1);
    const graph::PropertyValue unchanged(int64_t(100)), changed(int64_t(200)), null;
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
        throw std::logic_error("native column plan must not decode owning values");
    });
    {
        graph::query::execution::MaterializedByteReservation hold(memory.get()); hold.set(memory->available());
        EXPECT_THROW(HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1, next, memory, &changed),
            graph::query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(memory->used(), 0U);
    auto plan = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1, next, memory, &changed);
    ASSERT_TRUE(plan); EXPECT_EQ(plan->action(), HybridPropertyColumnDelta::Action::HCD_REPLACE_COLUMN);
    EXPECT_EQ(plan->sourceColumn().words[0], 100U); EXPECT_EQ(plan->replacementColumn().words[0], 200U);
    EXPECT_EQ(plan->replacementColumn().identity.semantic_generation, next);
    EXPECT_EQ(plan->sourceGeneration(), pin.superblock().generation); EXPECT_EQ(plan->databaseId(), pin.superblock().database_id);
    EXPECT_EQ(plan->effects().memberships, 0); EXPECT_EQ(plan->effects().columns, 0);
    EXPECT_GT(memory->used(), 0U); EXPECT_LT(memory->used(), 8192U);
    EXPECT_THROW(plan->demotedCells(), FormatException);
    plan.reset(); EXPECT_EQ(memory->used(), 0U);
    auto noop = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1, next, memory, &unchanged,
        false, 1); // No encoding is needed for a true no-op.
    ASSERT_TRUE(noop); EXPECT_EQ(noop->action(), HybridPropertyColumnDelta::Action::HCD_NO_CHANGE);
    EXPECT_EQ(noop->effects().columns, 0); noop.reset(); EXPECT_EQ(memory->used(), 0U);
    auto storedNull = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1, next, memory, &null);
    ASSERT_TRUE(storedNull); EXPECT_EQ(storedNull->replacementColumn().nulls & 1U, 1U);
    EXPECT_EQ(storedNull->replacementColumn().words[0], 0U); storedNull.reset();
    EXPECT_THROW(HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1, next, memory, &changed,
        false, 128), FormatException);
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_FALSE(HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 2, next, memory, &changed));
    EXPECT_THROW(HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1,
        pin.superblock().generation, memory, &changed), FormatException);
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(io_->fileSize(), bytes);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), pin.superblock());
}

TEST_F(HybridPropertyReadContextTest, ColumnDeltaDemotesOnlyTheChangedFieldAndReportsGroupEffects) {
    seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, 1};
    const auto next = Generation(pin.superblock().generation.value() + 1);
    auto erased = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1, next, memory, nullptr);
    ASSERT_TRUE(erased); EXPECT_EQ(erased->action(), HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD);
    EXPECT_EQ(erased->effects().memberships, -1); EXPECT_EQ(erased->effects().columns, -1);
    EXPECT_EQ(erased->effects().clear_live, 0U);
    auto cells = erased->demotedCells(); ASSERT_EQ(cells.size(), 64U);
    EXPECT_EQ(cells[0].kind, HybridPropertyColumnDelta::CellKind::HCC_MISSING);
    EXPECT_EQ(cells[1].kind, HybridPropertyColumnDelta::CellKind::HCC_INT64); EXPECT_EQ(cells[1].bits, 101U);
    EXPECT_EQ(cells[2].kind, HybridPropertyColumnDelta::CellKind::HCC_NULL);
    EXPECT_EQ(cells[48].kind, HybridPropertyColumnDelta::CellKind::HCC_MISSING);
    EXPECT_THROW(erased->replacementColumn(), FormatException);
    erased.reset(); EXPECT_EQ(memory->used(), 0U);
    auto deletedOwner = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1, next, memory, nullptr, true);
    ASSERT_TRUE(deletedOwner); EXPECT_EQ(deletedOwner->effects().clear_live, 1U); deletedOwner.reset();
    const graph::PropertyValue extension(std::string(128, 'x'));
    auto demoted = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1, next, memory, &extension);
    ASSERT_TRUE(demoted); EXPECT_EQ(demoted->action(), HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD);
    EXPECT_EQ(demoted->effects().memberships, 0); EXPECT_EQ(demoted->demotedCells()[0].extension, &extension);
    EXPECT_EQ(demoted->demotedCells()[0].kind, HybridPropertyColumnDelta::CellKind::HCC_EXTENSION);
    EXPECT_EQ(demoted->sourceGroup().memberships, 49U);
    EXPECT_EQ(demoted->sourceColumn().words[0], 100U);
    demoted.reset(); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, ColumnDeltaKeepsRowDoubleNoopRulesAndExactReplacementBits) {
    scalar_kind_ = HybridPropertyScalarKind::HPSK_DOUBLE; scalar_word_ = 0;
    seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, 1};
    const auto next = Generation(pin.superblock().generation.value() + 1);
    const graph::PropertyValue negativeZero(std::bit_cast<double>(UINT64_C(0x8000000000000000)));
    auto noop = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1, next, memory, &negativeZero);
    ASSERT_TRUE(noop); EXPECT_EQ(noop->action(), HybridPropertyColumnDelta::Action::HCD_NO_CHANGE);
    EXPECT_EQ(noop->sourceColumn().words[0], 0U); noop.reset();
    const graph::PropertyValue nan(std::bit_cast<double>(UINT64_C(0x7ff8000000000042)));
    auto change = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1, next, memory, &nan);
    ASSERT_TRUE(change); EXPECT_EQ(change->action(), HybridPropertyColumnDelta::Action::HCD_REPLACE_COLUMN);
    EXPECT_EQ(change->replacementColumn().words[0], UINT64_C(0x7ff8000000000042));
    change.reset(); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, EqualNanColumnBitsStillRequireAChange) {
    scalar_kind_ = HybridPropertyScalarKind::HPSK_DOUBLE; scalar_word_ = UINT64_C(0x7ff8000000000042);
    seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    const graph::PropertyValue nan(std::bit_cast<double>(scalar_word_));
    auto change = HybridPropertyColumnDelta::planExistingColumn(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, Generation(pin.superblock().generation.value() + 1), memory, &nan);
    ASSERT_TRUE(change); EXPECT_EQ(change->action(), HybridPropertyColumnDelta::Action::HCD_REPLACE_COLUMN);
    EXPECT_EQ(change->replacementColumn().words[0], scalar_word_);
}

TEST_F(HybridPropertyReadContextTest, CompatibleColumnWritesCoalesceOwnersAndGroupIntoOneActualPrivateFrame) {
    seed(false, 128); const auto pin = metadata_->openSnapshot(); const auto bytes = io_->fileSize();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    const auto next = Generation(pin.superblock().generation.value() + 1);
    const graph::PropertyValue first(int64_t(200)), second(int64_t(300)), original(int64_t(100));
    auto one = HybridPropertyColumnDelta::planExistingColumn(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, next, memory, &first);
    auto two = HybridPropertyColumnDelta::planExistingColumn(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 2}, 1, next, memory, &second);
    ASSERT_TRUE(one); ASSERT_TRUE(two);
    ASSERT_EQ(one->columnReference(), two->columnReference());
    ASSERT_EQ(one->groupReference().page_id, one->columnReference().page_id);
    const auto baseline = memory->used();
    const std::array<const HybridPropertyColumnDelta *, 2> changes{&*one, &*two};
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
        throw std::logic_error("native column COW must not decode unrelated owning values");
    });
    {
        HybridPropertyColumnWritePreparation prepared(pin, fields, changes, memory);
        ASSERT_EQ(prepared.pages().size(), 1U); const auto &image = prepared.pages()[0];
        EXPECT_EQ(image.pageId(), one->columnReference().page_id);
        const auto group = image.readGroup(one->groupReference(), EntityPropertyOwnerKind::EPOK_NODE, 1, next);
        auto expectedGroup = one->sourceGroup(); expectedGroup.semantic_generation = next;
        EXPECT_EQ(group, expectedGroup);
        auto identity = one->sourceColumn().identity; identity.semantic_generation = next;
        const auto column = image.readColumn(one->columnReference(), identity, group, next);
        EXPECT_EQ(column.words[0], 200U); EXPECT_EQ(column.words[1], 300U);
        EXPECT_EQ(column.nulls, one->sourceColumn().nulls); EXPECT_EQ(column.presence, one->sourceColumn().presence);
        EXPECT_EQ(column.words[3], 103U);
        const auto old = fields.cloneForMutation(pin, image.pageId(), next, memory);
        // The independently stored extension is byte-identical in this COW
        // image; changing two scalar owners must not rewrite its value body.
        const SlottedSlotReference extension{image.pageId(), 2, 1};
        EXPECT_EQ(image.readRecord(extension), old.readRecord(extension));
    }
    EXPECT_EQ(memory->used(), baseline);
    const std::array<const HybridPropertyColumnDelta *, 2> duplicate{&*one, &*one};
    EXPECT_THROW((HybridPropertyColumnWritePreparation(pin, fields, duplicate, memory)), FormatException);
    EXPECT_EQ(memory->used(), baseline);
    auto noop = HybridPropertyColumnDelta::planExistingColumn(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, next, memory, &original);
    ASSERT_TRUE(noop);
    const std::array<const HybridPropertyColumnDelta *, 1> noChanges{&*noop};
    {
        HybridPropertyColumnWritePreparation prepared(pin, fields, noChanges, memory);
        EXPECT_TRUE(prepared.pages().empty());
    }
    size_t observed = 0;
    static_cast<void>(context.visitField({EntityPropertyOwnerKind::EPOK_NODE, 1}, 1,
        [&](const auto &cell) { ++observed; EXPECT_EQ(cell.word, 100U); }, [](const auto &) {}));
    EXPECT_EQ(observed, 1U); EXPECT_EQ(io_->fileSize(), bytes);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), pin.superblock());
    noop.reset(); one.reset(); two.reset(); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, NativeColumnCowQuotaAndLateCancellationRefundImagesForRetry) {
    seed(); const auto pin = metadata_->openSnapshot(); const auto bytes = io_->fileSize();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    const auto next = Generation(pin.superblock().generation.value() + 1);
    const graph::PropertyValue replacement(int64_t(200));
    auto plan = HybridPropertyColumnDelta::planExistingColumn(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, next, memory, &replacement);
    ASSERT_TRUE(plan); const auto baseline = memory->used();
    const std::array<const HybridPropertyColumnDelta *, 1> changes{&*plan};
    {
        graph::query::execution::MaterializedByteReservation hold(memory.get()); hold.set(memory->available());
        EXPECT_THROW((HybridPropertyColumnWritePreparation(pin, fields, changes, memory)),
            graph::query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(memory->used(), baseline);
    class CancelAfterImage final : public CancellationCheckpoint {
    public:
        graph::query::execution::MaterializedByteBudget &memory;
        size_t before;
        mutable bool triggered = false;
        CancelAfterImage(graph::query::execution::MaterializedByteBudget &account, size_t bytes)
            : memory(account), before(bytes) {}
        void check() const override {
            if (memory.used() > before + RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
                triggered = true; throw std::runtime_error("cancel after private source-frame copy");
            }
        }
    } cancellation(*memory, baseline);
    EXPECT_THROW((HybridPropertyColumnWritePreparation(pin, fields, changes, memory, &cancellation)), std::runtime_error);
    EXPECT_TRUE(cancellation.triggered); EXPECT_EQ(memory->used(), baseline);
    {
        HybridPropertyColumnWritePreparation retry(pin, fields, changes, memory);
        EXPECT_EQ(retry.pages().size(), 1U);
    }
    EXPECT_EQ(memory->used(), baseline);
    EXPECT_THROW(fields.cloneForMutation(pin, plan->columnReference().page_id,
        Generation(next.value() + 1), memory), FormatException);
    auto demotion = HybridPropertyColumnDelta::planExistingColumn(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, next, memory, nullptr);
    ASSERT_TRUE(demotion); const std::array<const HybridPropertyColumnDelta *, 1> unsupported{&*demotion};
    const auto held = memory->used();
    EXPECT_THROW((HybridPropertyColumnWritePreparation(pin, fields, unsupported, memory)), FormatException);
    EXPECT_EQ(memory->used(), held); EXPECT_EQ(io_->fileSize(), bytes);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), pin.superblock());
    demotion.reset(); plan.reset(); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, DirectoryIntegrityStreamsActualOwnershipWithoutDecodingValues) {
    seed(false, 128); const auto pin = metadata_->openSnapshot(); const auto size = io_->fileSize();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
        throw std::logic_error("integrity cannot materialize PropertyValue");
    });
    {
        graph::query::execution::MaterializedByteReservation hold(memory.get()); hold.set(memory->available());
        EXPECT_THROW(HybridPropertyDirectoryIntegrity::verify(context, pin, memory),
            graph::query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(memory->used(), 0U);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.memberships, 49U); EXPECT_EQ(report.columns, 1U);
    EXPECT_EQ(report.extensions, 1U); EXPECT_EQ(report.large_values, 0U); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, PageId(41), memory),
        report.groups + report.columns + report.extensions);
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, PageId(101), memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(io_->fileSize(), size); EXPECT_EQ(metadata_->openSnapshot().superblock(), pin.superblock());
}

TEST_F(HybridPropertyReadContextTest, DirectoryIntegrityRejectsValidlyEncodedWrongGroupCounts) {
    group_memberships_ = 50; seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verify(context, pin, memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, DirectoryIntegrityRejectsSelectorWithoutBodyEvenWhenHeaderCountMatches) {
    group_memberships_ = 50; orphan_owner_member_ = true; seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    EXPECT_EQ(context.count({EntityPropertyOwnerKind::EPOK_NODE, 2}), 2U);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verify(context, pin, memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, DirectoryIntegrityRejectsColumnDirectoryOutsideRegisteredGroups) {
    orphan_column_entry_ = true; seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verify(context, pin, memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, DirectoryIntegrityAuthenticatesSingleLargeBodyWithoutOwningValueDecode) {
    seed(false, 8192, true); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
        throw std::logic_error("large integrity cannot decode PropertyValue");
    });
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verify(context, pin, memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory, large_values_.get());
    EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.memberships, 49U); EXPECT_EQ(report.large_values, 1U);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, DirectoryIntegrityRejectsAuthenticatedWholeRowLargeCarrier) {
    extra_large_field_ = true; seed(false, 8192, true); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verify(context, pin, memory, large_values_.get()), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, DirectoryIntegrityVerifiesLargeFieldBeyondItsWorkspaceWithoutOwningBody) {
    seed(false, 512 * 1024, true); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(128 * 1024);
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
        throw std::logic_error("large integrity cannot decode PropertyValue");
    });
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory, large_values_.get());
    EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.large_values, 1U); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, LargeAdmissionAndNonNullReadsUseBoundedBuffersAndRefundBeforeRetry) {
    seed(false, 512 * 1024, true); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto tiny = std::make_shared<graph::query::execution::MaterializedByteBudget>(1);
    {
        graph::utils::OwnedAllocationScope owned(tiny);
        EXPECT_THROW(static_cast<void>(HybridPropertyValueReader::exists(context, pin,
            {EntityPropertyOwnerKind::EPOK_NODE, 1}, 2, true, large_values_.get())),
            graph::query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(tiny->used(), 0U);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(128 * 1024);
    {
        graph::utils::OwnedAllocationScope owned(memory);
        graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
            throw std::logic_error("inspection must not decode the large field");
        });
        EXPECT_EQ(HybridPropertyValueReader::exists(context, pin,
            {EntityPropertyOwnerKind::EPOK_NODE, 1}, 2, true, large_values_.get()), true);
        size_t encoded = 0;
        const SelectedPropertyReadAdmission admission = [&](size_t bytes) { encoded += bytes; };
        const auto field = HybridPropertyValueReader::read(context, pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 2,
            large_values_.get(), &admission, HybridPropertyValueReader::ReadMode::HRM_ADMISSION_ONLY);
        EXPECT_EQ(field.route, HybridPropertyReadContext::FieldRoute::HFR_FOUND); EXPECT_FALSE(field.value);
        EXPECT_EQ(encoded, sizeof(uint32_t) + CanonicalPropertyValueCodec::HEADER_BYTES + sizeof(uint32_t) + 512 * 1024);
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, DirectoryIntegrityCancellationAndMissingSourceNeverReturnSuccess) {
    seed(); const auto pin = metadata_->openSnapshot();
    class Toggle final : public CancellationCheckpoint {
    public:
        bool cancelled = false;
        void check() const override { if (cancelled) throw std::runtime_error("cancel native integrity"); }
    } cancellation;
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_, &cancellation);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    cancellation.cancelled = true;
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verify(context, pin, memory), std::runtime_error);
    EXPECT_EQ(memory->used(), 0U);
    cancellation.cancelled = false;
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, pin, memory).memberships, 49U);
    descriptor_.roots[2] = {};
    auto missing = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verify(missing, pin, memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, PhysicalOwnershipRejectsUnroutedDuplicateGroupSlot) {
    orphan_physical_record_ = 1; seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    // The logical directories are internally consistent. The additional valid
    // physical slot is invisible to that forward walk and needs reverse lookup.
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, pin, memory).memberships, 49U);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, PageId(41), memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, PhysicalOwnershipRejectsUnroutedDuplicateColumnSlot) {
    orphan_physical_record_ = 2; seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    // The logical directories are internally consistent. The additional valid
    // physical slot is invisible to that forward walk and needs reverse lookup.
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, pin, memory).memberships, 49U);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, PageId(41), memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, PhysicalOwnershipRejectsUnroutedDuplicateExtensionSlot) {
    orphan_physical_record_ = 3; seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    // The logical directories are internally consistent. The additional valid
    // physical slot is invisible to that forward walk and needs reverse lookup.
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, pin, memory).memberships, 49U);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, PageId(41), memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, PhysicalOwnershipRejectsStaleFreeHintWithoutHidingItAsLogicalCorruption) {
    stale_free_hint_ = true; seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, pin, memory).memberships, 49U);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, PageId(41), memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, EmptyNativePageMayRemainReusableWithoutADescriptor) {
    empty_physical_page_ = true; seed(); const auto pin = metadata_->openSnapshot();
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, PageId(41), memory), 0U);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, LiveNativePageCannotClaimOwnershipWithoutADescriptor) {
    seed(); const auto pin = metadata_->openSnapshot();
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, PageId(41), memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyReadContextTest, SharedLargeReferenceWalkAuthenticatesMetadataWithinBudgetWithoutReadingBodies) {
    shared_large_owner_ = true; group_memberships_ = 50;
    seed(false, 65536, true); const auto pin = metadata_->openSnapshot(); const auto bytes = io_->fileSize();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    graph::utils::OwnedAllocationScope owned(memory);
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
        throw std::logic_error("reference marking must not materialize PropertyValue");
    });
    std::array<LargeValueId, 2> seen{}; size_t count = 0;
    const auto observe = [&](const LargeValueRef &value) {
        ASSERT_LT(count, seen.size()); seen[count++] = value.value_id;
        EXPECT_GT(value.logical_bytes, 65536U);
        EXPECT_EQ(value.media_type, RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE);
    };
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, PageId(41), memory,
        large_values_.get(), std::ref(observe)), 4U);
    EXPECT_EQ(count, 2U); EXPECT_TRUE(seen[0].isValid()); EXPECT_EQ(seen[0], seen[1]);
    EXPECT_EQ(memory->used(), 0U);
    // A requested full body needs more than this account; metadata marking
    // above nevertheless succeeds for both owners of the same immutable value.
    EXPECT_THROW(HybridPropertyValueReader::read(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 1}, 2, large_values_.get()),
        graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(memory->used(), 0U);
    const auto stop = [](const LargeValueRef &) { throw std::runtime_error("discard partial marking"); };
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, PageId(41), memory,
        large_values_.get(), std::ref(stop)), std::runtime_error);
    EXPECT_EQ(memory->used(), 0U); count = 0;
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, PageId(41), memory,
        large_values_.get(), std::ref(observe)), 4U);
    EXPECT_EQ(count, 2U); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(io_->fileSize(), bytes); EXPECT_EQ(metadata_->openSnapshot().superblock(), pin.superblock());
}

TEST_F(HybridPropertyReadContextTest, NativeReferenceWalkRejectsWrongManifestIdentityBeforeEmission) {
    wrong_large_hash_ = true; seed(false, 8192, true); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    size_t emitted = 0;
    const auto observe = [&](const LargeValueRef &) { ++emitted; };
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, PageId(41), memory,
        large_values_.get(), std::ref(observe)), FormatException);
    EXPECT_EQ(emitted, 0U); EXPECT_EQ(memory->used(), 0U);
}
namespace {
class DemotionNoPhysicalAllocator final : public MetadataPageAllocator {
public:
    MetadataPageId allocateMetadataPageId() override { throw std::logic_error("unexpected metadata allocation"); }
    MetadataPageAllocation allocatePage(Generation, LogSequenceNumber, std::span<const PhysicalSlotId>) override {
        throw std::logic_error("unexpected physical allocation");
    }
    void abandonPage(PhysicalPageRef) noexcept override {}
};
}
TEST_F(HybridPropertyReadContextTest, FieldDemotionPreparesEveryFinalDirectoryAndPreservesUnrelatedBytes) {
    seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    const auto next = Generation(pin.superblock().generation.value() + 1);
    const auto size = io_->fileSize(), epoch = io_->dataMutationEpoch();
    for (bool remove : {false, true}) {
        const graph::PropertyValue replacement(std::string("changed type"));
        auto change = HybridPropertyColumnDelta::planExistingColumn(context, pin,
            {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, next, memory, remove ? nullptr : &replacement);
        ASSERT_TRUE(change); const auto baseline = memory->used();
        DemotionNoPhysicalAllocator allocator;
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
        {
            HybridPropertyDemotionPreparation prepared(batch, pin, fields, *change, memory);
            EXPECT_EQ(prepared.group().columns, 0U);
            EXPECT_EQ(prepared.group().memberships, change->sourceGroup().memberships - unsigned(remove));
            EXPECT_EQ(prepared.group().live, change->sourceGroup().live);
            EXPECT_EQ(prepared.erasedOwners().size(), unsigned(remove));
            EXPECT_EQ(prepared.owners().size(), 48U - unsigned(remove));
            EXPECT_EQ(prepared.extensions().size(), prepared.owners().size());
            EXPECT_EQ(prepared.erasedColumn(), HybridPropertyDirectoryCodec::columnKey(1, 1));
            ASSERT_GE(prepared.pages().size(), 2U);
            const auto &sourceImage = prepared.pages()[0];
            const SlottedSlotReference unrelated{sourceImage.pageId(), 2, 1};
            auto oldImage = fields.cloneForMutation(pin, sourceImage.pageId(), next, memory);
            EXPECT_EQ(sourceImage.readRecord(unrelated), oldImage.readRecord(unrelated));
            EXPECT_THROW(sourceImage.readRecord(change->columnReference()), FormatException);
            EXPECT_EQ(oldImage.readColumn(change->columnReference(), change->sourceColumn().identity,
                change->sourceGroup(), next), change->sourceColumn());
            for (const auto &entry : prepared.extensions()) {
                const auto key = HybridPropertyDirectoryCodec::decodeOwnerKey(entry.key, EntityPropertyOwnerKind::EPOK_NODE);
                const auto locator = HybridPropertyDirectoryCodec::decodeLocator(entry.value);
                const auto image = std::find_if(prepared.pages().begin(), prepared.pages().end(),
                    [&](const auto &page) { return page.pageId() == locator.page_id; });
                ASSERT_NE(image, prepared.pages().end());
                const auto bytes = image->readRecord(locator);
                const auto extension = HybridPropertyExtensionView::open(bytes, key.owner, key.token, next, next, next);
                const auto value = extension.decodeInline([](const auto &) {});
                if (key.owner.entity_id == 1) EXPECT_EQ(value, replacement);
                else if (key.owner.entity_id == 3) EXPECT_EQ(value, graph::PropertyValue());
                else EXPECT_EQ(value, graph::PropertyValue(int64_t(key.owner.entity_id + 99)));
            }
        }
        EXPECT_EQ(memory->used(), baseline);
        EXPECT_EQ(batch.allocatedPageCount(), 0U); EXPECT_EQ(batch.createdPoolCount(), 0U);
        EXPECT_EQ(batch.current(), pin.superblock());
        EXPECT_EQ(io_->fileSize(), size); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyReadContextTest, FieldDemotionRefusesPartialOwnerDeletionAndRefundsQuotaAndCancellation) {
    seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    const auto next = Generation(pin.superblock().generation.value() + 1);
    auto deleted = HybridPropertyColumnDelta::planExistingColumn(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, next, memory, nullptr, true);
    auto change = HybridPropertyColumnDelta::planExistingColumn(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, next, memory, nullptr);
    ASSERT_TRUE(deleted); ASSERT_TRUE(change); const auto baseline = memory->used();
    const auto size = io_->fileSize(), epoch = io_->dataMutationEpoch();
    DemotionNoPhysicalAllocator allocator;
    {
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
        EXPECT_THROW((HybridPropertyDemotionPreparation(batch, pin, fields, *deleted, memory)), FormatException);
        EXPECT_EQ(batch.candidate().next_logical_page_id, pin.superblock().next_logical_page_id);
    }
    {
        auto small = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024);
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), small);
        EXPECT_THROW((HybridPropertyDemotionPreparation(batch, pin, fields, *change, small)),
            graph::query::execution::MaterializedByteLimitExceeded);
        EXPECT_EQ(small->used(), 0U);
    }
    struct CancelAfterCopy final : CancellationCheckpoint {
        graph::query::execution::MaterializedByteBudget &memory;
        size_t before;
        mutable bool triggered = false;
        CancelAfterCopy(graph::query::execution::MaterializedByteBudget &m, size_t n) : memory(m), before(n) {}
        void check() const override {
            if (memory.used() > before + RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
                triggered = true; throw std::runtime_error("cancel after demotion frame copy");
            }
        }
    } cancellation(*memory, baseline);
    {
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
        EXPECT_THROW((HybridPropertyDemotionPreparation(batch, pin, fields, *change, memory, &cancellation)), std::runtime_error);
        EXPECT_TRUE(cancellation.triggered);
    }
    EXPECT_EQ(memory->used(), baseline);
    struct CancelAfterPrivateId final : CancellationCheckpoint {
        const MetadataWriteBatch &batch;
        uint64_t source_next;
        mutable bool triggered = false;
        explicit CancelAfterPrivateId(const MetadataWriteBatch &b)
            : batch(b), source_next(b.current().next_logical_page_id) {}
        void check() const override {
            if (batch.candidate().next_logical_page_id > source_next) {
                triggered = true; throw std::runtime_error("cancel after private demotion ID allocation");
            }
        }
    };
    {
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
        CancelAfterPrivateId late(batch);
        EXPECT_THROW((HybridPropertyDemotionPreparation(batch, pin, fields, *change, memory, &late)), std::runtime_error);
        EXPECT_TRUE(late.triggered);
        EXPECT_GT(batch.candidate().next_logical_page_id, pin.superblock().next_logical_page_id);
        EXPECT_EQ(batch.current(), pin.superblock());
    }
    EXPECT_EQ(memory->used(), baseline);
    {
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
        HybridPropertyDemotionPreparation retry(batch, pin, fields, *change, memory);
        EXPECT_EQ(retry.extensions().size(), 47U);
    }
    EXPECT_EQ(memory->used(), baseline); EXPECT_EQ(io_->fileSize(), size); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), pin.superblock());
}
TEST_F(HybridPropertyReadContextTest, FieldDemotionRequiresTheSameBatchLargeFieldResolverForOversizedReplacement) {
    seed(false, 8192, true); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    const auto next = Generation(pin.superblock().generation.value() + 1);
    const graph::PropertyValue replacement(std::string(8192, 'z'));
    auto change = HybridPropertyColumnDelta::planExistingColumn(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, next, memory, &replacement);
    ASSERT_TRUE(change); const auto baseline = memory->used();
    const auto size = io_->fileSize(), epoch = io_->dataMutationEpoch();
    DemotionNoPhysicalAllocator allocator;
    {
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
        EXPECT_THROW((HybridPropertyDemotionPreparation(batch, pin, fields, *change, memory)), FormatException);
    }
    EXPECT_EQ(memory->used(), baseline);
    static_cast<void>(large_values_->contentIndex());
    {
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
        std::optional<HybridPropertyLargeFieldPreparation> proposal;
        size_t resolved = 0;
        const auto resolve = [&](EntityPropertyKey owner, uint32_t token, const graph::PropertyValue &value) {
            ++resolved;
            proposal = HybridPropertyLargeFieldPreparation::prepare(*metadata_, pin, *large_values_, owner, token,
                value, LargeValueId(batch.candidate().next_large_value_id), memory);
            if (!proposal->reused()) EXPECT_EQ(batch.allocateLargeValueId(), proposal->reference().value_id);
            const auto encoded = LargeValueRefCodec::encode(proposal->reference());
            return HybridPropertyExtensionView::encodeReference(owner, token, next, encoded,
                RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, *memory, [](size_t) {});
        };
        HybridPropertyDemotionPreparation prepared(batch, pin, fields, *change, memory, nullptr, resolve);
        EXPECT_EQ(resolved, 1U); ASSERT_TRUE(proposal); EXPECT_FALSE(proposal->reused());
        ASSERT_EQ(prepared.extensions().size(), 48U);
        const auto locator = HybridPropertyDirectoryCodec::decodeLocator(prepared.extensions()[0].value);
        const auto image = std::find_if(prepared.pages().begin(), prepared.pages().end(),
            [&](const auto &page) { return page.pageId() == locator.page_id; });
        ASSERT_NE(image, prepared.pages().end());
        const auto bytes = image->readRecord(locator);
        graph::utils::OwnedHeapReservation referenceCredit(memory);
        const auto reference = HybridPropertyExtensionView::open(bytes,
            {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, next, next, next,
            [&](size_t bytes) { referenceCredit.set(bytes); });
        ASSERT_TRUE(reference.reference()); EXPECT_EQ(*reference.reference(), proposal->reference());
        const SlottedSlotReference unrelated{prepared.pages()[0].pageId(), 2, 1};
        auto oldImage = fields.cloneForMutation(pin, unrelated.page_id, next, memory);
        EXPECT_EQ(prepared.pages()[0].readRecord(unrelated), oldImage.readRecord(unrelated));
    }
    EXPECT_EQ(memory->used(), baseline); EXPECT_EQ(io_->fileSize(), size); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), pin.superblock());
}
TEST_F(HybridPropertyReadContextTest, MergedColumnRemovalsApplyTheThresholdToFinalPresenceAndCoordinateAllDirectoryDeletes) {
    live_owner_count_ = 49; group_memberships_ = 50; seed();
    const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    const auto next = Generation(pin.superblock().generation.value() + 1);
    auto one = HybridPropertyColumnDelta::planExistingColumn(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, next, memory, nullptr);
    auto two = HybridPropertyColumnDelta::planExistingColumn(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 2}, 1, next, memory, nullptr);
    ASSERT_TRUE(one); ASSERT_TRUE(two);
    ASSERT_EQ(one->action(), HybridPropertyColumnDelta::Action::HCD_REPLACE_COLUMN);
    ASSERT_EQ(two->action(), HybridPropertyColumnDelta::Action::HCD_REPLACE_COLUMN);
    const std::array<const HybridPropertyColumnDelta *, 2> changes{&*one, &*two};
    const auto baseline = memory->used(); const auto size = io_->fileSize(), epoch = io_->dataMutationEpoch();
    {
        auto merged = HybridPropertyColumnDelta::mergeExistingColumnChanges(changes, memory);
        EXPECT_EQ(merged.action(), HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD);
        EXPECT_EQ(merged.changedSlots(), 3U); EXPECT_EQ(merged.requestedSlots(), 3U);
        EXPECT_EQ(merged.effects().memberships, -2); EXPECT_EQ(merged.effects().columns, -1);
        EXPECT_EQ(merged.demotedCells()[0].kind, HybridPropertyColumnDelta::CellKind::HCC_MISSING);
        EXPECT_EQ(merged.demotedCells()[1].kind, HybridPropertyColumnDelta::CellKind::HCC_MISSING);
        EXPECT_EQ(merged.demotedCells()[2].kind, HybridPropertyColumnDelta::CellKind::HCC_NULL);
        DemotionNoPhysicalAllocator allocator;
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
        HybridPropertyDemotionPreparation prepared(batch, pin, fields, merged, memory);
        EXPECT_EQ(prepared.group().memberships, 48U); EXPECT_EQ(prepared.group().columns, 0U);
        ASSERT_EQ(prepared.erasedOwners().size(), 2U); EXPECT_EQ(prepared.extensions().size(), 47U);
        for (unsigned i = 0; i < 2; ++i)
            EXPECT_EQ(prepared.erasedOwners()[i], HybridPropertyDirectoryCodec::ownerKey({EntityPropertyOwnerKind::EPOK_NODE, i + 1U}, 1));
        EXPECT_EQ(prepared.group().live, one->sourceGroup().live);
    }
    EXPECT_EQ(memory->used(), baseline); EXPECT_EQ(io_->fileSize(), size); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), pin.superblock());
}
TEST_F(HybridPropertyReadContextTest, MergedColumnReplacementsApplyEverySlotAndRejectOverlapsWithoutLeaking) {
    seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    const auto next = Generation(pin.superblock().generation.value() + 1);
    const graph::PropertyValue first(int64_t(300)), second(int64_t(400)), unchanged(int64_t(103));
    auto one = HybridPropertyColumnDelta::planExistingColumn(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1, next, memory, &first);
    auto two = HybridPropertyColumnDelta::planExistingColumn(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 2}, 1, next, memory, &second);
    auto noop = HybridPropertyColumnDelta::planExistingColumn(context, pin,
        {EntityPropertyOwnerKind::EPOK_NODE, 4}, 1, next, memory, &unchanged);
    ASSERT_TRUE(one); ASSERT_TRUE(two); ASSERT_TRUE(noop);
    const std::array<const HybridPropertyColumnDelta *, 3> changes{&*one, &*two, &*noop};
    const auto baseline = memory->used();
    {
        auto merged = HybridPropertyColumnDelta::mergeExistingColumnChanges(changes, memory);
        ASSERT_EQ(merged.action(), HybridPropertyColumnDelta::Action::HCD_REPLACE_COLUMN);
        EXPECT_EQ(merged.changedSlots(), 3U); EXPECT_EQ(merged.requestedSlots(), 11U);
        EXPECT_EQ(merged.effects().memberships, 0); EXPECT_EQ(merged.effects().columns, 0);
        EXPECT_EQ(merged.replacementColumn().words[0], 300U); EXPECT_EQ(merged.replacementColumn().words[1], 400U);
        const std::array<const HybridPropertyColumnDelta *, 1> final{&merged};
        HybridPropertyColumnWritePreparation prepared(pin, fields, final, memory);
        ASSERT_EQ(prepared.pages().size(), 1U);
        const auto group = prepared.pages()[0].readGroup(merged.groupReference(), EntityPropertyOwnerKind::EPOK_NODE, 1, next);
        auto identity = merged.sourceColumn().identity; identity.semantic_generation = next;
        const auto column = prepared.pages()[0].readColumn(merged.columnReference(), identity, group, next);
        EXPECT_EQ(column.words[0], 300U); EXPECT_EQ(column.words[1], 400U); EXPECT_EQ(column.words[3], 103U);
        const std::array<const HybridPropertyColumnDelta *, 2> overlap{&merged, &*two};
        const auto held = memory->used();
        EXPECT_THROW(HybridPropertyColumnDelta::mergeExistingColumnChanges(overlap, memory), FormatException);
        EXPECT_THROW((HybridPropertyColumnWritePreparation(pin, fields, overlap, memory)), FormatException);
        EXPECT_EQ(memory->used(), held);
    }
    EXPECT_EQ(memory->used(), baseline);
    const std::array<const HybridPropertyColumnDelta *, 2> duplicate{&*noop, &*noop};
    EXPECT_THROW(HybridPropertyColumnDelta::mergeExistingColumnChanges(duplicate, memory), FormatException);
    const std::array<const HybridPropertyColumnDelta *, 1> noChanges{&*noop};
    {
        auto merged = HybridPropertyColumnDelta::mergeExistingColumnChanges(noChanges, memory);
        EXPECT_EQ(merged.action(), HybridPropertyColumnDelta::Action::HCD_NO_CHANGE); EXPECT_EQ(merged.changedSlots(), 0U);
    }
    auto small = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024);
    EXPECT_THROW(HybridPropertyColumnDelta::mergeExistingColumnChanges(changes, small),
        graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(small->used(), 0U);
    struct Cancel final : CancellationCheckpoint {
        mutable unsigned calls = 0;
        void check() const override { if (++calls == 3) throw std::runtime_error("cancel while merging a column"); }
    } cancellation;
    EXPECT_THROW(HybridPropertyColumnDelta::mergeExistingColumnChanges(changes, memory,
        RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, &cancellation), std::runtime_error);
    EXPECT_EQ(memory->used(), baseline);
    auto moved = std::move(*two);
    const std::array<const HybridPropertyColumnDelta *, 1> movedFrom{&*two}, nullPlan{nullptr};
    EXPECT_THROW(HybridPropertyColumnDelta::mergeExistingColumnChanges(movedFrom, memory), FormatException);
    EXPECT_THROW(HybridPropertyColumnDelta::mergeExistingColumnChanges(nullPlan, memory), FormatException);
    EXPECT_EQ(memory->used(), baseline);
}
TEST_F(HybridPropertyReadContextTest, InverseConversionOwnsActualNativeRowsAndTheirAdmissionWithoutWriting) {
    seed(false, 8192, true); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(128 * 1024);
    const auto size = io_->fileSize(), epoch = io_->dataMutationEpoch();
    {
        HybridPropertyRowPreparation prepared(context, pin, *metadata_, *entities_, EntityPropertyOwnerKind::EPOK_NODE,
            1, memory, large_values_.get());
        EXPECT_EQ(prepared.databaseId(), before.database_id); EXPECT_EQ(prepared.sourceGeneration(), before.generation);
        EXPECT_EQ(prepared.sourceGroup().memberships, 49U); ASSERT_EQ(prepared.rows().size(), 48U);
        EXPECT_GT(memory->used(), 8192U);
        auto moved = std::move(prepared);
        pin.release(); // Returned rows own their values and do not borrow the pin.
        for (size_t i = 0; i < moved.rows().size(); ++i) {
            const auto &row = moved.rows()[i]; EXPECT_EQ(row.owner_entity_id, i + 1U); EXPECT_EQ(row.owner_entity_type, graph::Node::typeId);
            ASSERT_EQ(row.entries.size(), i ? 1U : 2U); EXPECT_EQ(row.entries[0].key_token, 1U);
            EXPECT_EQ(row.entries[0].value, i == 2 ? graph::PropertyValue() : graph::PropertyValue(int64_t(i + 100)));
        }
        EXPECT_EQ(moved.rows()[0].entries[1].value, graph::PropertyValue(std::string(8192, 'x')));
        EXPECT_EQ(io_->fileSize(), size); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
}
TEST_F(HybridPropertyReadContextTest, InverseConversionPreflightRejectsOversizedMembershipsBeforeAnyValueDecode) {
    group_memberships_ = 5000; seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    size_t decoded = 0;
    graph::property_decode::AllocationScope forbid(&decoded, [](void *owner, size_t) {
        ++*static_cast<size_t *>(owner); throw std::logic_error("preflight must not decode values");
    });
    EXPECT_THROW((HybridPropertyRowPreparation(context, pin, *metadata_, *entities_, EntityPropertyOwnerKind::EPOK_NODE,
        1, memory)), std::length_error);
    EXPECT_EQ(decoded, 0U); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyReadContextTest, InverseConversionRejectsWrongGroupQuotaAndCancellationThenRetriesIndependently) {
    seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    const auto size = io_->fileSize(), epoch = io_->dataMutationEpoch();
    EXPECT_THROW((HybridPropertyRowPreparation(context, pin, *metadata_, *entities_, EntityPropertyOwnerKind::EPOK_NODE,
        2, memory)), FormatException);
    EXPECT_THROW((HybridPropertyRowPreparation(context, pin, *metadata_, *entities_, EntityPropertyOwnerKind::EPOK_NODE,
        1, std::make_shared<graph::query::execution::MaterializedByteBudget>(2 * 1024 * 1024))), FormatException);
    auto small = std::make_shared<graph::query::execution::MaterializedByteBudget>(2048);
    EXPECT_THROW((HybridPropertyRowPreparation(context, pin, *metadata_, *entities_, EntityPropertyOwnerKind::EPOK_NODE,
        1, small)), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(small->used(), 0U);
    struct Cancel final : CancellationCheckpoint {
        mutable unsigned calls = 0;
        void check() const override { if (++calls == 20) throw std::runtime_error("cancel inverse source preparation"); }
    } cancellation;
    EXPECT_THROW((HybridPropertyRowPreparation(context, pin, *metadata_, *entities_, EntityPropertyOwnerKind::EPOK_NODE,
        1, memory, nullptr, &cancellation)), std::runtime_error);
    EXPECT_EQ(memory->used(), 0U);
    {
        HybridPropertyRowPreparation retry(context, pin, *metadata_, *entities_, EntityPropertyOwnerKind::EPOK_NODE, 1, memory);
        EXPECT_EQ(retry.rows().size(), 48U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(io_->fileSize(), size); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), pin.superblock());
}
#endif

#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyReadContextTest, MixedGroupMutationsCloneSharedSourcesOnceAndPreserveAllFinalChanges) {
    extra_columns_ = 2; seed();
    const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(262144);
    const auto next = Generation(pin.superblock().generation.value() + 1);
    const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, 1};
    const graph::PropertyValue text("new type"), number(int64_t(789));
    auto removed = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1, next, memory, nullptr);
    auto demoted = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 3, next, memory, &text);
    auto compatible = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 4, next, memory, &number);
    ASSERT_TRUE(removed && demoted && compatible);
    const std::array<const HybridPropertyColumnDelta *, 3> changes{&*compatible, &*removed, &*demoted};
    const auto baseline = memory->used(), epoch = io_->dataMutationEpoch(), fileSize = io_->fileSize();
    DemotionNoPhysicalAllocator allocator;
    MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
    {
        HybridPropertyDemotionPreparation prepared(batch, pin, fields, changes, memory);
        EXPECT_EQ(prepared.group().memberships, 144U); EXPECT_EQ(prepared.group().columns, 1U);
        EXPECT_EQ(prepared.group().live, removed->sourceGroup().live);
        ASSERT_EQ(prepared.erasedColumns().size(), 2U);
        EXPECT_EQ(prepared.erasedColumns()[0], HybridPropertyDirectoryCodec::columnKey(1, 1));
        EXPECT_EQ(prepared.erasedColumns()[1], HybridPropertyDirectoryCodec::columnKey(1, 3));
        EXPECT_THROW(prepared.erasedColumn(), FormatException);
        EXPECT_EQ(prepared.erasedOwners().size(), 1U);
        EXPECT_EQ(prepared.owners().size(), 95U); EXPECT_EQ(prepared.extensions().size(), 95U);
        EXPECT_TRUE(std::is_sorted(prepared.owners().begin(), prepared.owners().end(),
            [](const auto &left, const auto &right) { return left.key < right.key; }));
        EXPECT_TRUE(std::is_sorted(prepared.extensions().begin(), prepared.extensions().end(),
            [](const auto &left, const auto &right) { return left.key < right.key; }));
        const auto sourceId = removed->groupReference().page_id;
        EXPECT_EQ(std::count_if(prepared.pages().begin(), prepared.pages().end(),
            [&](const auto &page) { return page.pageId() == sourceId; }), 1);
        const auto image = std::find_if(prepared.pages().begin(), prepared.pages().end(),
            [&](const auto &page) { return page.pageId() == sourceId; });
        ASSERT_NE(image, prepared.pages().end());
        EXPECT_EQ(image->readGroup(removed->groupReference(), owner.owner_kind, 1, next), prepared.group());
        EXPECT_THROW(image->readRecord(removed->columnReference()), FormatException);
        EXPECT_THROW(image->readRecord(demoted->columnReference()), FormatException);
        const auto finalColumn = image->readColumn(compatible->columnReference(), compatible->replacementColumn().identity,
            prepared.group(), next);
        EXPECT_EQ(finalColumn.words[0], 789U); EXPECT_EQ(finalColumn.words[1], 101U);
        auto original = fields.cloneForMutation(pin, sourceId, next, memory);
        const SlottedSlotReference unrelated{sourceId, 2, 1};
        EXPECT_EQ(image->readRecord(unrelated), original.readRecord(unrelated));
        for (const auto &entry : prepared.extensions()) {
            const auto key = HybridPropertyDirectoryCodec::decodeOwnerKey(entry.key, owner.owner_kind);
            const auto locator = HybridPropertyDirectoryCodec::decodeLocator(entry.value);
            const auto page = std::find_if(prepared.pages().begin(), prepared.pages().end(),
                [&](const auto &value) { return value.pageId() == locator.page_id; });
            ASSERT_NE(page, prepared.pages().end());
            const auto bytes = page->readRecord(locator);
            const auto extension = HybridPropertyExtensionView::open(bytes, key.owner, key.token, next, next, next);
            const auto value = extension.decodeInline([](const auto &) {});
            if (key.owner.entity_id == 1) { EXPECT_EQ(key.token, 3U); EXPECT_EQ(value, text); }
            else if (key.owner.entity_id == 3) EXPECT_EQ(value, graph::PropertyValue());
            else EXPECT_EQ(value, graph::PropertyValue(int64_t(key.owner.entity_id + 99)));
        }
    }
    EXPECT_EQ(memory->used(), baseline);
    const std::array<const HybridPropertyColumnDelta *, 2> overlap{&*removed, &*removed};
    EXPECT_THROW((HybridPropertyDemotionPreparation(batch, pin, fields, overlap, memory)), FormatException);
    EXPECT_EQ(memory->used(), baseline);
    struct CountingCheckpoint final : CancellationCheckpoint {
        mutable size_t calls = 0;
        size_t stop = 0;
        void check() const override {
            if (++calls == stop) throw std::runtime_error("mixed group cancelled");
        }
    } count;
    {
        HybridPropertyDemotionPreparation counted(batch, pin, fields, changes, memory, &count);
    }
    EXPECT_EQ(memory->used(), baseline);
    const auto checkpoints = count.calls;
    ASSERT_GT(checkpoints, 5U);
    for (const auto stop : {size_t(1), checkpoints / 2, checkpoints}) {
        CountingCheckpoint cancellation; cancellation.stop = stop;
        EXPECT_THROW((HybridPropertyDemotionPreparation(batch, pin, fields, changes, memory, &cancellation)), std::runtime_error);
        EXPECT_EQ(memory->used(), baseline);
    }
    {
        HybridPropertyDemotionPreparation retry(batch, pin, fields, changes, memory);
        EXPECT_EQ(retry.extensions().size(), 95U);
    }
    EXPECT_EQ(memory->used(), baseline);
    auto small = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024);
    EXPECT_THROW((HybridPropertyDemotionPreparation(batch, pin, fields, changes, small)), std::exception);
    EXPECT_EQ(small->used(), 0U);
    EXPECT_EQ(io_->fileSize(), fileSize); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(batch.allocatedPageCount(), 0U); EXPECT_EQ(batch.createdPoolCount(), 0U);
}

#endif

#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyReadContextTest, MixedGroupKeepsAQualifiedColumnAfterRemovalAlongsideAnotherFieldDemotion) {
    live_owner_count_ = 49; group_memberships_ = 50; extra_columns_ = 1; seed();
    const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(131072);
    const auto next = Generation(pin.superblock().generation.value() + 1);
    const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, 1};
    const graph::PropertyValue text("new type");
    auto removed = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1, next, memory, nullptr);
    auto demoted = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 3, next, memory, &text);
    ASSERT_TRUE(removed && demoted);
    ASSERT_EQ(removed->action(), HybridPropertyColumnDelta::Action::HCD_REPLACE_COLUMN);
    const std::array<const HybridPropertyColumnDelta *, 2> changes{&*demoted, &*removed};
    DemotionNoPhysicalAllocator allocator;
    MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
    const auto baseline = memory->used(), epoch = io_->dataMutationEpoch();
    {
        HybridPropertyDemotionPreparation prepared(batch, pin, fields, changes, memory);
        EXPECT_EQ(prepared.group().memberships, 98U); EXPECT_EQ(prepared.group().columns, 1U);
        EXPECT_EQ(prepared.erasedColumn(), HybridPropertyDirectoryCodec::columnKey(1, 3));
        ASSERT_EQ(prepared.erasedOwners().size(), 1U);
        EXPECT_EQ(prepared.erasedOwners()[0], HybridPropertyDirectoryCodec::ownerKey(owner, 1));
        EXPECT_EQ(prepared.extensions().size(), 49U);
        const auto image = std::find_if(prepared.pages().begin(), prepared.pages().end(),
            [&](const auto &page) { return page.pageId() == removed->columnReference().page_id; });
        ASSERT_NE(image, prepared.pages().end());
        const auto column = image->readColumn(removed->columnReference(), removed->replacementColumn().identity,
            prepared.group(), next);
        EXPECT_EQ(std::popcount(column.presence), 48); EXPECT_EQ(column.presence & 1U, 0U);
        EXPECT_EQ(column.live, removed->sourceColumn().live);
    }
    EXPECT_EQ(memory->used(), baseline); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(batch.allocatedPageCount(), 0U);
    const graph::PropertyValue same(int64_t(100));
    auto noChange = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1, next, memory, &same);
    ASSERT_TRUE(noChange);
    const auto nextId = batch.candidate().next_logical_page_id;
    {
        HybridPropertyDemotionPreparation prepared(batch, pin, fields, *noChange, memory);
        EXPECT_TRUE(prepared.pages().empty()); EXPECT_TRUE(prepared.owners().empty());
        EXPECT_TRUE(prepared.erasedColumns().empty()); EXPECT_EQ(prepared.group(), noChange->sourceGroup());
    }
    EXPECT_EQ(batch.candidate().next_logical_page_id, nextId);
    const std::array<const HybridPropertyColumnDelta *, 1> nullPlan{nullptr};
    EXPECT_THROW((HybridPropertyDemotionPreparation(batch, pin, fields, nullPlan, memory)), FormatException);
    auto moved = std::move(*removed);
    EXPECT_FALSE(removed->isValid()); EXPECT_TRUE(moved.isValid());
    const std::array<const HybridPropertyColumnDelta *, 1> movedPlan{&*removed};
    EXPECT_THROW((HybridPropertyDemotionPreparation(batch, pin, fields, movedPlan, memory)), FormatException);
    EXPECT_THROW((HybridPropertyColumnWritePreparation(pin, fields, movedPlan, memory)), FormatException);
}
#endif

#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyReadContextTest, WholeOwnerDeletionCoordinatesColumnDemotionExistingExtensionsAndLiveOnce) {
    extra_columns_ = 1; seed(false, 8192, true);
    const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(524288);
    const auto next = Generation(pin.superblock().generation.value() + 1);
    const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, 1};
    const std::array owners{owner};
    const auto source = context.groupRecord(owner);
    ASSERT_TRUE(source);
    auto firstColumn = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1, next, memory, nullptr);
    ASSERT_TRUE(firstColumn);
    const auto baseline = memory->used(), epoch = io_->dataMutationEpoch(), fileSize = io_->fileSize();
    DemotionNoPhysicalAllocator allocator;
    MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
    {
        auto prepared = HybridPropertyDemotionPreparation::prepareOwnerDeletion(batch, context, pin, fields, owners, memory);
        EXPECT_EQ(prepared.group().live, source->live & ~UINT64_C(1));
        EXPECT_EQ(prepared.group().memberships, 94U); EXPECT_EQ(prepared.group().columns, 0U);
        EXPECT_FALSE(prepared.erasedGroup());
        ASSERT_EQ(prepared.erasedColumns().size(), 2U);
        ASSERT_EQ(prepared.erasedExtensions().size(), 1U);
        EXPECT_EQ(prepared.erasedExtensions()[0], HybridPropertyDirectoryCodec::ownerKey(owner, 2));
        ASSERT_EQ(prepared.erasedOwners().size(), 3U);
        for (unsigned i = 0; i < 3; ++i)
            EXPECT_EQ(prepared.erasedOwners()[i], HybridPropertyDirectoryCodec::ownerKey(owner, i + 1));
        EXPECT_EQ(prepared.owners().size(), 94U); EXPECT_EQ(prepared.extensions().size(), 94U);
        const auto sourceId = firstColumn->groupReference().page_id;
        EXPECT_EQ(std::count_if(prepared.pages().begin(), prepared.pages().end(),
            [&](const auto &page) { return page.pageId() == sourceId; }), 1);
        const auto image = std::find_if(prepared.pages().begin(), prepared.pages().end(),
            [&](const auto &page) { return page.pageId() == sourceId; });
        ASSERT_NE(image, prepared.pages().end());
        const SlottedSlotReference oldExtension{sourceId, 2, 1};
        EXPECT_THROW((void)image->readRecord(oldExtension), FormatException);
        auto original = fields.cloneForMutation(pin, sourceId, next, memory);
        EXPECT_NO_THROW((void)original.readRecord(oldExtension));
        EXPECT_EQ(image->readGroup(firstColumn->groupReference(), owner.owner_kind, 1, next), prepared.group());
        for (const auto &entry : prepared.extensions()) {
            const auto key = HybridPropertyDirectoryCodec::decodeOwnerKey(entry.key, owner.owner_kind);
            EXPECT_NE(key.owner.entity_id, 1U);
            const auto locator = HybridPropertyDirectoryCodec::decodeLocator(entry.value);
            const auto page = std::find_if(prepared.pages().begin(), prepared.pages().end(),
                [&](const auto &value) { return value.pageId() == locator.page_id; });
            ASSERT_NE(page, prepared.pages().end());
            const auto bytes = page->readRecord(locator);
            const auto extension = HybridPropertyExtensionView::open(bytes, key.owner, key.token, next, next, next);
            const auto value = extension.decodeInline([](const auto &) {});
            if (key.owner.entity_id == 3) EXPECT_EQ(value, graph::PropertyValue());
            else EXPECT_EQ(value, graph::PropertyValue(int64_t(key.owner.entity_id + 99)));
        }
    }
    EXPECT_EQ(memory->used(), baseline);
    EXPECT_EQ(io_->fileSize(), fileSize); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(batch.allocatedPageCount(), 0U); EXPECT_EQ(batch.current(), pin.superblock());
    EXPECT_EQ(context.count(owner), 3U); // The caller's original source remains live and unchanged.
}
TEST_F(HybridPropertyReadContextTest, WholeOwnerDeletionRetainsQualifiedColumnsAndCanEraseTheFinalGroup) {
    live_owner_count_ = 49; group_memberships_ = 50; extra_columns_ = 1; seed();
    const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(524288);
    const auto next = Generation(pin.superblock().generation.value() + 1);
    const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, 1};
    auto column = HybridPropertyColumnDelta::planExistingColumn(context, pin, owner, 1, next, memory, nullptr);
    ASSERT_TRUE(column);
    DemotionNoPhysicalAllocator allocator;
    const auto baseline = memory->used(), epoch = io_->dataMutationEpoch();
    {
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
        const std::array owners{owner};
        auto prepared = HybridPropertyDemotionPreparation::prepareOwnerDeletion(batch, context, pin, fields, owners, memory);
        EXPECT_EQ(prepared.group().memberships, 96U); EXPECT_EQ(prepared.group().columns, 2U);
        EXPECT_EQ(std::popcount(prepared.group().live), 48);
        EXPECT_TRUE(prepared.erasedColumns().empty()); EXPECT_TRUE(prepared.extensions().empty());
        const auto image = std::find_if(prepared.pages().begin(), prepared.pages().end(),
            [&](const auto &page) { return page.pageId() == column->columnReference().page_id; });
        ASSERT_NE(image, prepared.pages().end());
        const auto record = image->readColumn(column->columnReference(), column->replacementColumn().identity,
            prepared.group(), next);
        EXPECT_EQ(std::popcount(record.presence), 48); EXPECT_EQ(record.presence & 1U, 0U);
        EXPECT_EQ(record.live, prepared.group().live);
    }
    EXPECT_EQ(memory->used(), baseline);
    {
        std::array<EntityPropertyKey, 49> owners{};
        for (unsigned i = 0; i < owners.size(); ++i) owners[i] = {owner.owner_kind, i + 1};
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
        auto prepared = HybridPropertyDemotionPreparation::prepareOwnerDeletion(batch, context, pin, fields, owners, memory);
        EXPECT_EQ(prepared.erasedGroup(), HybridPropertyDirectoryCodec::groupKey(1));
        EXPECT_EQ(prepared.group().live, 0U); EXPECT_EQ(prepared.group().memberships, 0U);
        EXPECT_EQ(prepared.group().columns, 0U); EXPECT_EQ(prepared.erasedColumns().size(), 2U);
        EXPECT_EQ(prepared.erasedOwners().size(), 99U); EXPECT_EQ(prepared.erasedExtensions().size(), 1U);
        EXPECT_TRUE(prepared.owners().empty()); EXPECT_TRUE(prepared.extensions().empty());
        const auto image = std::find_if(prepared.pages().begin(), prepared.pages().end(),
            [&](const auto &page) { return page.pageId() == column->groupReference().page_id; });
        ASSERT_NE(image, prepared.pages().end());
        EXPECT_THROW((void)image->readRecord(column->groupReference()), FormatException);
    }
    EXPECT_EQ(memory->used(), baseline); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
#endif

#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyReadContextTest, WholeOwnerDeletionLeavesAnUninvolvedColumnByteIdenticalAndRefundsFailures) {
    live_owner_count_ = 49; group_memberships_ = 50; extra_columns_ = 1; extra_column_missing_owner_ = 1;
    seed(); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(524288);
    const auto next = Generation(pin.superblock().generation.value() + 1);
    const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, 1};
    const std::array owners{owner};
    const graph::PropertyValue same(int64_t(101));
    auto untouched = HybridPropertyColumnDelta::planExistingColumn(context, pin, {owner.owner_kind, 2}, 3, next, memory, &same);
    ASSERT_TRUE(untouched);
    DemotionNoPhysicalAllocator allocator;
    const auto baseline = memory->used(), epoch = io_->dataMutationEpoch(), fileSize = io_->fileSize();
    struct CountingCheckpoint final : CancellationCheckpoint {
        mutable size_t calls = 0;
        size_t stop = 0;
        void check() const override { if (++calls == stop) throw std::runtime_error("owner deletion cancelled"); }
    } count;
    {
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
        auto prepared = HybridPropertyDemotionPreparation::prepareOwnerDeletion(batch, context, pin, fields, owners, memory, &count);
        EXPECT_EQ(prepared.group().memberships, 96U); EXPECT_EQ(prepared.group().columns, 2U);
        EXPECT_EQ(prepared.erasedOwners().size(), 2U);
        const auto image = std::find_if(prepared.pages().begin(), prepared.pages().end(),
            [&](const auto &page) { return page.pageId() == untouched->columnReference().page_id; });
        ASSERT_NE(image, prepared.pages().end());
        auto source = fields.cloneForMutation(pin, untouched->columnReference().page_id, next, memory);
        EXPECT_EQ(image->readRecord(untouched->columnReference()), source.readRecord(untouched->columnReference()));
        // Its older live snapshot is allowed: current group.live is authority.
        EXPECT_NO_THROW((void)image->readColumn(untouched->columnReference(), untouched->sourceColumn().identity,
            prepared.group(), next));
    }
    EXPECT_EQ(memory->used(), baseline); ASSERT_GT(count.calls, 5U);
    for (const auto stop : {size_t(1), count.calls / 2, count.calls}) {
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
        CountingCheckpoint cancellation; cancellation.stop = stop;
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareOwnerDeletion(batch, context, pin, fields,
            owners, memory, &cancellation), std::runtime_error);
        EXPECT_EQ(batch.current(), pin.superblock());
    }
    EXPECT_EQ(memory->used(), baseline);
    for (const auto &invalid : {std::vector<EntityPropertyKey>{}, std::vector<EntityPropertyKey>{owner, owner},
            std::vector<EntityPropertyKey>{{owner.owner_kind, 50}}, std::vector<EntityPropertyKey>{owner, {owner.owner_kind, 65}}}) {
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareOwnerDeletion(batch, context, pin, fields,
            invalid, memory), FormatException);
    }
    auto small = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024);
    {
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), small);
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareOwnerDeletion(batch, context, pin, fields,
            owners, small), graph::query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(small->used(), 0U);
    {
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
        auto retry = HybridPropertyDemotionPreparation::prepareOwnerDeletion(batch, context, pin, fields, owners, memory);
        EXPECT_EQ(retry.group().memberships, 96U);
    }
    EXPECT_EQ(memory->used(), baseline); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), fileSize);
}
#endif

#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyReadContextTest, ExtensionOnlyOwnerDeletionPreservesSharedLargeValueAndUnchangedColumns) {
    live_owner_count_ = 49; group_memberships_ = 51; column_missing_owner_ = 1; shared_large_owner_ = true;
    seed(false, 8192, true); const auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(131072);
    const auto next = Generation(pin.superblock().generation.value() + 1);
    const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, 1};
    const std::array owners{owner};
    EXPECT_EQ(context.count(owner), 1U);
    const graph::PropertyValue same(int64_t(101));
    auto column = HybridPropertyColumnDelta::planExistingColumn(context, pin, {owner.owner_kind, 2}, 1, next, memory, &same);
    ASSERT_TRUE(column);
    const auto baseline = memory->used(), epoch = io_->dataMutationEpoch();
    DemotionNoPhysicalAllocator allocator;
    {
        MetadataWriteBatch batch(allocator, pin.superblock(), LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1), memory);
        auto prepared = HybridPropertyDemotionPreparation::prepareOwnerDeletion(batch, context, pin, fields, owners, memory);
        EXPECT_EQ(prepared.group().memberships, 49U); EXPECT_EQ(prepared.group().columns, 1U);
        EXPECT_EQ(prepared.group().live & 1U, 0U);
        ASSERT_EQ(prepared.erasedExtensions().size(), 1U); EXPECT_EQ(prepared.erasedOwners().size(), 1U);
        EXPECT_TRUE(prepared.erasedColumns().empty()); EXPECT_TRUE(prepared.extensions().empty());
        ASSERT_EQ(prepared.pages().size(), 1U);
        const auto &image = prepared.pages()[0];
        auto original = fields.cloneForMutation(pin, image.pageId(), next, memory);
        EXPECT_EQ(image.readRecord(column->columnReference()), original.readRecord(column->columnReference()));
        const SlottedSlotReference deleted{image.pageId(), 2, 1}, shared{image.pageId(), 3, 1};
        EXPECT_THROW((void)image.readRecord(deleted), FormatException);
        const auto sharedBytes = image.readRecord(shared);
        EXPECT_EQ(sharedBytes, original.readRecord(shared));
        graph::utils::OwnedHeapReservation credit(memory);
        const auto admit = [&](size_t bytes) { credit.set(bytes); };
        const auto extension = HybridPropertyExtensionView::open(sharedBytes, {owner.owner_kind, 2}, 2,
            next, next, next, std::ref(admit));
        ASSERT_TRUE(extension.reference());
        graph::utils::OwnedHeapReservation manifestCredit(memory);
        const auto admitManifest = [&](size_t bytes) { manifestCredit.set(bytes); };
        EXPECT_NO_THROW((void)large_values_->manifest(pin, *extension.reference(), std::ref(admitManifest)));
        EXPECT_EQ(batch.allocatedPageCount(), 0U);
    }
    EXPECT_EQ(memory->used(), baseline); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(context.count(owner), 1U); EXPECT_EQ(context.count({owner.owner_kind, 2}), 2U);
}
#endif
