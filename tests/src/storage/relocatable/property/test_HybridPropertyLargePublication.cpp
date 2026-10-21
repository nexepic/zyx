#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyLargePublicationTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    NativeGraphWritePlannerOptions placement() {
        NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
        options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902}; return options;
    }
    void seedLarge(LargeValueStore &values, PropertyCollectionRootStore &roots, EntityPropertyOwnerKind kind, size_t count = 2) {
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, roots, *btree_pages_, placement());
        auto tx = planner.beginTransaction(); uint64_t source = 0, target = 0;
        if (kind == EDGE) { source = tx.createNode({}); target = tx.createNode({}); }
        for (size_t i = 0; i < count; ++i) {
            const auto id = kind == NODE ? tx.createNode({}) : tx.createEdge(source, target, 7);
            PropertyCollectionRoot root{id, kind == NODE ? graph::Node::typeId : graph::Edge::typeId,
                {{1, PropertyValue(std::string(8192, 'x'))}, {2, PropertyValue(int64_t(17 + i))}}};
            if (kind == NODE) tx.replaceNodeProperty(std::move(root)); else tx.replaceEdgeProperty(std::move(root));
        }
        ASSERT_TRUE(tx.commit().committed());
    }
    void convert(EntityPropertyOwnerKind kind, std::shared_ptr<MaterializedByteBudget> memory,
        const MetadataPublishOptions &options = {}, const CancellationCheckpoint *cancel = nullptr,
        NativeHybridPropertyConversionDirection direction = NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID) {
        auto conversion = request(kind, memory); conversion.conversion->publish_candidate = true; conversion.conversion->cancellation = cancel;
        conversion.conversion->direction = direction;
        const auto before = metadata_->openSnapshot().superblock(); NativeGraphDerivedStateMutation derived;
        static_cast<void>(metadata_->mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, {}, derived,
            LogSequenceNumber(before.checkpoint_lsn.value() + 1), [](auto, auto, auto, auto, auto, auto) {},
            [](auto &) {}, {}, {}, {}, {}, {}, options, {}, before.generation, {}, {}, {}, {}, {}, conversion));
    }
    void changeBothScalars(EntityPropertyOwnerKind kind, int64_t value, const std::shared_ptr<MaterializedByteBudget> &memory) {
        const auto state = metadata_->openSnapshot().superblock();
        const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{kind, 1}, 2, value}, {{kind, 2}, 2, value}}};
        NativeHybridPropertyColumnWrite write{state.database_id, state.generation, state.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
        static_cast<void>(metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    }
    LargeValueRef reference(const MetadataSnapshotPin &pin, EntityPropertyKey owner) {
        auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
        graph::utils::OwnedHeapReservation boundary(memory), referenceCredit(memory);
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin); std::optional<LargeValueRef> ref;
        const auto route = context.visitField(owner, 1, [](const auto &) {}, [&](const auto &field) { ref = field.reference(); },
            [&](size_t bytes) { boundary.set(bytes); }, [&](size_t bytes) { referenceCredit.set(bytes); });
        if (route != HybridPropertyReadContext::FieldRoute::HFR_FOUND || !ref) throw std::logic_error("missing large field");
        return *ref;
    }
};
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyLargePublicationTest, FreshFieldContentPublishesWithOneDeduplicatedIdentityAndItsIndex) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    seedLarge(values, roots, NODE); const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto original = roots.readEntity(old, {NODE, 1}); const auto oldIndex = metadata_->findIndexRoot(old, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    const auto pin = metadata_->openSnapshot(); EXPECT_EQ(pin.superblock().next_large_value_id, before.next_large_value_id + 1);
    const auto first = reference(pin, {NODE, 1}); const auto second = reference(pin, {NODE, 2}); EXPECT_EQ(first, second);
    EXPECT_EQ(first.value_id.value(), before.next_large_value_id);
    const auto payload = values.read(pin, first);
    EXPECT_EQ(values.findContentDuplicate(pin, payload, first.kind, first.media_type, 8), std::optional(first));
    BTreeIndex index(RSS_LARGE_VALUE_CONTENT_INDEX_ID, values.contentIndex(), *metadata_);
    const auto key = values.contentIndexKey(first); EXPECT_EQ(index.find(pin, key), std::optional(first.value_id.value()));
    EXPECT_FALSE(index.find(old, key)); EXPECT_EQ(metadata_->findIndexRoot(old, RSS_LARGE_VALUE_CONTENT_INDEX_ID), oldIndex);
    EXPECT_EQ(roots.readEntity(pin, {NODE, 1}), original); EXPECT_EQ(roots.readEntity(old, {NODE, 1}), original);
    auto reopened = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore reopenedValues(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY); PropertyCollectionRootStore reopenedRoots(pages, reopenedValues, *reopened);
    EXPECT_EQ(reopenedRoots.readEntity(reopened->openSnapshot(), {NODE, 1}), original);
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, pin, memory, &values).large_values, 2U);
    ASSERT_NO_THROW(convert(NODE, memory, {}, nullptr, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW));
    EXPECT_EQ(metadata_->openSnapshot().superblock().next_large_value_id, pin.superblock().next_large_value_id);
    EXPECT_FALSE(metadata_->openSnapshot().superblock().hybrid_property_descriptor_root.isValid());
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 1}), original);
    EXPECT_EQ(roots.readEntity(pin, {NODE, 1}), original);
    auto restored = placement(); restored.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, roots, *btree_pages_, restored);
    auto update = planner.beginTransaction(); auto changed = original; changed.entries[1].value = PropertyValue(int64_t(999));
    update.replaceNodeProperty(changed); ASSERT_TRUE(update.commit().committed());
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 1}), changed);
    EXPECT_EQ(roots.readEntity(pin, {NODE, 1}), original);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargePublicationTest, EdgeFreshContentKeepsMainTopologyAndIndependentInlineUpdates) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    seedLarge(values, roots, EDGE); const auto old = metadata_->openSnapshot(); const auto edge = entities_->readEdge(old, 1);
    const auto adjacency = metadata_->findIndexRoot(old, RSS_GLOBAL_ADJACENCY_INDEX_ID);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(EDGE, memory));
    auto before = metadata_->openSnapshot().superblock(); const auto native = metadata_->openSnapshot(); const auto ref = reference(native, {EDGE, 1});
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{EDGE, 1}, 2, int64_t(99)}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); EXPECT_EQ(reference(pin, {EDGE, 1}), ref);
    EXPECT_EQ(entities_->readEdge(pin, 1), edge); EXPECT_EQ(metadata_->findIndexRoot(pin, RSS_GLOBAL_ADJACENCY_INDEX_ID), adjacency);
    EXPECT_EQ(roots.readEntity(pin, {EDGE, 1}).entries[1].value, PropertyValue(int64_t(99)));
    EXPECT_EQ(roots.readEntity(native, {EDGE, 1}).entries[1].value, PropertyValue(int64_t(17)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargePublicationTest, FreshContentABRecoverySelectsCompleteFieldsManifestsAndIndex) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    seedLarge(values, roots, NODE); const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto original = roots.readEntity(old, {NODE, 1}); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions options; options.bootstrap_failpoint = point;
        EXPECT_THROW(convert(NODE, memory, options), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); const auto pin = recovered->openSnapshot();
        SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore recoveredValues(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY); PropertyCollectionRootStore recoveredRoots(pages, recoveredValues, *recovered);
        const bool installed = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(pin.superblock().next_large_value_id, before.next_large_value_id + size_t(installed));
        EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root.isValid(), installed);
        EXPECT_EQ(recoveredRoots.readEntity(pin, {NODE, 1}), original); EXPECT_EQ(roots.readEntity(old, {NODE, 1}), original);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyLargePublicationTest, FreshContentQuotaAndCancellationLeaveIdsRootsAndSourceBytesRetryable) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    seedLarge(values, roots, NODE); const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    auto tiny = std::make_shared<MaterializedByteBudget>(128 * 1024);
    EXPECT_THROW(convert(NODE, tiny), graph::query::execution::MaterializedByteLimitExceeded); EXPECT_EQ(tiny->used(), 0U);
    struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("large publication cancelled") {} };
    struct Cancellation final : CancellationCheckpoint {
        bool prepared = false;
        void check() const override { if (prepared) throw Cancelled(); }
    } cancellation;
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        graph::debug::PerfTrace::ScopedObserver observer(&cancellation,
            [](void *owner, std::string_view key, int64_t frames, bool) noexcept {
                if (key == "native_graph.hybrid.private_content_index_frames" && frames > 0)
                    static_cast<Cancellation *>(owner)->prepared = true;
            });
        EXPECT_THROW(convert(NODE, memory, {}, &cancellation), Cancelled);
    }
    EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    ASSERT_NO_THROW(convert(NODE, memory));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargePublicationTest, ExistingMultiLevelContentIndexKeepsUntouchedRoutesAndOtherMedia) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    LargeValueWriteOptions options{PhysicalSlotId(40), PhysicalSlotId(80), PhysicalSlotId(40), PhysicalSlotId(80)};
    options.media_type = "application/" + std::string(116, 'a');
    std::vector<LargeValueRef> unrelated;
    for (uint64_t i = 0; i < 96; ++i) {
        const std::array<std::byte, 8> bytes{std::byte(i)};
        const auto state = metadata_->openSnapshot().superblock();
        unrelated.push_back(values.write(bytes, options, LogSequenceNumber(state.checkpoint_lsn.value() + 1)).reference);
    }
    seedLarge(values, roots, NODE); const auto old = metadata_->openSnapshot();
    const auto oldRoot = metadata_->findIndexRoot(old, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
    ASSERT_GT(values.contentIndex().read(old, oldRoot.root_page_id).header().level, 0U);
    const auto tree = values.contentIndex().planTreeRetirement(old, RSS_LARGE_VALUE_CONTENT_INDEX_ID, 128);
    std::vector<PageLocation> pages;
    for (const auto &pool : tree.pool_requests) for (const auto &page : pool.expected_retirements) pages.push_back(page.location);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    const auto pin = metadata_->openSnapshot(); size_t untouched = 0, changed = 0;
    for (const auto &page : pages) {
        const auto current = metadata_->findPageLocation(pin, page.page_id);
        if (current == page) ++untouched; else ++changed;
    }
    EXPECT_GT(untouched, 0U); EXPECT_GT(changed, 0U);
    BTreeIndex index(RSS_LARGE_VALUE_CONTENT_INDEX_ID, values.contentIndex(), *metadata_);
    for (const auto &ref : unrelated) {
        EXPECT_EQ(index.find(pin, values.contentIndexKey(ref)), std::optional(ref.value_id.value()));
        EXPECT_EQ(values.read(pin, ref), values.read(old, ref));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargePublicationTest, ContentIndexReusesItsPoolAndCannotReleaseItsCurrentDestination) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    seedLarge(values, roots, NODE); auto old = metadata_->openSnapshot();
    const auto root = metadata_->findIndexRoot(old, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
    const auto sourcePage = metadata_->findPageLocation(old, root.root_page_id);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    std::vector<CatalogSegmentRetirementRequest> retirement;
    {
        const auto pin = metadata_->openSnapshot(); const auto pool = metadata_->findCatalog(pin, sourcePage.segment_id);
        ASSERT_EQ(pool.state, SegmentState::SGS_SEALED);
        const auto nextRoot = metadata_->findIndexRoot(pin, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
        const auto nextPage = metadata_->findPageLocation(pin, nextRoot.root_page_id);
        EXPECT_EQ(nextPage.segment_id, sourcePage.segment_id); EXPECT_NE(nextPage, sourcePage);
        retirement.push_back({pool.segment_id, pool.generation});
        EXPECT_THROW((void)metadata_->tryReleaseRetiredCatalogSegments(retirement, LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1)), FormatException);
        EXPECT_EQ(roots.readEntity(old, {NODE, 1}), roots.readEntity(pin, {NODE, 1}));
    }
    old.release(); const auto state = metadata_->openSnapshot().superblock();
    EXPECT_THROW((void)metadata_->tryReleaseRetiredCatalogSegments(retirement, LogSequenceNumber(state.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(std::string(8192, 'x')));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargePublicationTest, ExistingWholeRowCarrierInverseABRecoveryPreservesIdsAndBothPinnedLayouts) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    seedLarge(values, roots, NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    const auto hybrid = metadata_->openSnapshot(); const auto before = hybrid.superblock(); const auto original = roots.readEntity(hybrid, {NODE, 1});
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions options; options.bootstrap_failpoint = point;
        EXPECT_THROW(convert(NODE, memory, options, nullptr, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); const auto pin = recovered->openSnapshot();
        SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore recoveredValues(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY); PropertyCollectionRootStore recoveredRoots(pages, recoveredValues, *recovered);
        EXPECT_EQ(pin.superblock().next_large_value_id, before.next_large_value_id);
        EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root.isValid(), point != BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE);
        EXPECT_EQ(recoveredRoots.readEntity(pin, {NODE, 1}), original); EXPECT_EQ(roots.readEntity(hybrid, {NODE, 1}), original);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyLargePublicationTest, ExistingWholeRowCarrierInverseCancellationRefundsAndRetries) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    seedLarge(values, roots, NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("inverse carrier cancelled") {} };
    struct Cancellation final : CancellationCheckpoint {
        bool prepared = false;
        void check() const override { if (prepared) throw Cancelled(); }
    } cancellation;
    {
        graph::debug::PerfTrace::ScopedObserver observer(&cancellation, [](void *owner, std::string_view key, int64_t count, bool) noexcept {
            if (key == "native_graph.hybrid.inverse_private_row_frames" && count > 0) static_cast<Cancellation *>(owner)->prepared = true;
        });
        EXPECT_THROW(convert(NODE, memory, {}, &cancellation, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW), Cancelled);
    }
    EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    ASSERT_NO_THROW(convert(NODE, memory, {}, nullptr, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW));
    EXPECT_EQ(metadata_->openSnapshot().superblock().next_large_value_id, before.next_large_value_id);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargePublicationTest, FreshInverseCarriersDeduplicateAcrossOwnersAndPreserveNodeAndEdgeAuthority) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    for (const auto kind : {NODE, EDGE}) {
        seedLarge(values, roots, kind); ASSERT_NO_THROW(convert(kind, memory));
        const auto scalar = kind == NODE ? int64_t(999) : int64_t(1234); ASSERT_NO_THROW(changeBothScalars(kind, scalar, memory));
        const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
        const auto expected = roots.readEntity(old, {kind, 1});
        const auto edge = kind == EDGE ? std::optional(entities_->readEdge(old, 1)) : std::nullopt;
        const auto adjacency = metadata_->findIndexRootOptional(old, RSS_GLOBAL_ADJACENCY_INDEX_ID);
        ASSERT_NO_THROW(convert(kind, memory, {}, nullptr, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW));
        const auto pin = metadata_->openSnapshot(); EXPECT_EQ(pin.superblock().next_large_value_id, before.next_large_value_id + 1U);
        EXPECT_FALSE(pin.superblock().hybrid_property_descriptor_root.isValid());
        EXPECT_EQ(roots.readEntity(pin, {kind, 1}), expected); EXPECT_EQ(roots.readEntity(old, {kind, 1}), expected);
        const auto other = roots.readEntity(pin, {kind, 2}); EXPECT_EQ(other.entries, expected.entries);
        size_t currentCarriers = 0;
        metadata_->visitLargeValueManifestLocations(pin, [&](const auto &route) {
            if (route.value_id.value() == before.next_large_value_id) ++currentCarriers;
        });
        EXPECT_EQ(currentCarriers, 1U);
        EXPECT_EQ(metadata_->findIndexRootOptional(pin, RSS_GLOBAL_ADJACENCY_INDEX_ID), adjacency);
        if (edge) EXPECT_EQ(entities_->readEdge(pin, 1), *edge);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyLargePublicationTest, FreshInverseCarrierABRecoverySelectsCompleteIdentityAndContentIndex) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    seedLarge(values, roots, NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    ASSERT_NO_THROW(convert(NODE, memory));
    ASSERT_NO_THROW(changeBothScalars(NODE, 999, memory));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const auto expected = roots.readEntity(old, {NODE, 1});
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions options; options.bootstrap_failpoint = point;
        EXPECT_THROW(convert(NODE, memory, options, nullptr, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); const auto pin = recovered->openSnapshot();
        SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore recoveredValues(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY); PropertyCollectionRootStore recoveredRoots(pages, recoveredValues, *recovered);
        const bool installed = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(pin.superblock().next_large_value_id, before.next_large_value_id + size_t(installed));
        EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root.isValid(), !installed);
        EXPECT_EQ(recoveredRoots.readEntity(pin, {NODE, 1}), expected); EXPECT_EQ(roots.readEntity(old, {NODE, 1}), expected);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyLargePublicationTest, FreshInverseQuotaAndPreparedCancellationRefundCountersAndAllowRetry) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    seedLarge(values, roots, NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    ASSERT_NO_THROW(convert(NODE, memory));
    ASSERT_NO_THROW(changeBothScalars(NODE, 999, memory));
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    auto tiny = std::make_shared<MaterializedByteBudget>(128 * 1024);
    EXPECT_THROW(convert(NODE, tiny, {}, nullptr, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U);
    struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("fresh inverse cancelled") {} };
    struct Cancellation final : CancellationCheckpoint {
        bool prepared = false;
        void check() const override { if (prepared) throw Cancelled(); }
    } cancellation;
    {
        graph::debug::PerfTrace::ScopedObserver observer(&cancellation, [](void *owner, std::string_view key, int64_t count, bool) noexcept {
            if (key == "native_graph.hybrid.inverse_private_new_large_values" && count > 0) static_cast<Cancellation *>(owner)->prepared = true;
        });
        EXPECT_THROW(convert(NODE, memory, {}, &cancellation, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW), Cancelled);
    }
    EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    ASSERT_NO_THROW(convert(NODE, memory, {}, nullptr, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW));
    EXPECT_EQ(metadata_->openSnapshot().superblock().next_large_value_id, before.next_large_value_id + 1U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargePublicationTest, RowReachabilityAfterFreshInverseKeepsSharedCarrierAndOldNativePin) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    seedLarge(values, roots, NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    ASSERT_NO_THROW(convert(NODE, memory));
    ASSERT_NO_THROW(changeBothScalars(NODE, 999, memory));
    auto old = metadata_->openSnapshot(); const auto expected = roots.readEntity(old, {NODE, 1});
    ASSERT_NO_THROW(convert(NODE, memory, {}, nullptr, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW));
    LargeValueReachabilityReclaimer reclaimer(values, *slotted_pages_, *metadata_);
    LargeValueReachabilityReclaimOptions options;
    options.maximum_manifest_locations = 32; options.maximum_manifest_metadata_pages = 128; options.maximum_manifest_nodes = 128;
    options.maximum_owner_locations = 128; options.maximum_owner_metadata_pages = 128; options.maximum_data_pages = 128;
    options.maximum_retirements = 256; options.maximum_page_locations = 1024; options.maximum_pools = 64;
    options.first_index_slot = PhysicalSlotId(40); options.last_index_slot = PhysicalSlotId(80);
    const auto before = metadata_->openSnapshot().superblock();
    const auto report = reclaimer.reclaim(options, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
    EXPECT_EQ(report.reachable_values, 1U); EXPECT_EQ(report.retired_values, 3U);
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 1}), expected);
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 2}).entries, expected.entries);
    EXPECT_EQ(roots.readEntity(old, {NODE, 1}), expected);
    old.release(); const auto next = metadata_->openSnapshot().superblock();
    const auto release = reclaimer.reclaim(options, LogSequenceNumber(next.checkpoint_lsn.value() + 1));
    EXPECT_EQ(release.retired_values, 0U); EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 1}), expected);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
#endif
