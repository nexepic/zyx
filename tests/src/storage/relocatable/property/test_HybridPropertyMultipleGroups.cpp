#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"
using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyMultipleGroupsTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    void convert(EntityPropertyOwnerKind kind, uint64_t group, const std::shared_ptr<MaterializedByteBudget> &memory,
        NativeHybridPropertyConversionDirection direction = NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID,
        const MetadataPublishOptions &options = {}, const CancellationCheckpoint *cancellation = nullptr) {
        auto conversion = request(kind, memory); conversion.conversion->group_id = group;
        conversion.conversion->direction = direction; conversion.conversion->publish_candidate = true; conversion.conversion->cancellation = cancellation;
        const auto source = metadata_->openSnapshot().superblock();
        (void)metadata_->mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, {}, {},
            LogSequenceNumber(source.checkpoint_lsn.value() + 1), [](auto, auto, auto, auto, auto, auto) {}, [](auto &) {},
            {}, {}, {}, {}, {}, options, {}, source.generation, {}, {}, {}, {}, {}, conversion);
    }
};
}
TEST_F(HybridPropertyMultipleGroupsTest, DisabledFormatStillRejectsBeforeWriting) {
#if !ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
    seed(NODE, 1, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    EXPECT_THROW(convert(NODE, 1, memory), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
#endif
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyMultipleGroupsTest, ConvertingASecondGroupPreservesTheFirstAndBothOldSnapshots) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto original = metadata_->openSnapshot(); const auto first = properties_->readEntity(original, {NODE, 1});
    const auto second = properties_->readEntity(original, {NODE, 65});
    ASSERT_NO_THROW(convert(NODE, 1, memory)); auto one = metadata_->openSnapshot();
    ASSERT_NO_THROW(convert(NODE, 2, memory)); auto both = metadata_->openSnapshot();
    for (const auto *pin : {&original, &one, &both}) {
        EXPECT_EQ(properties_->readEntity(*pin, {NODE, 1}), first);
        EXPECT_EQ(properties_->readEntity(*pin, {NODE, 65}), second);
    }
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, both);
    EXPECT_TRUE(context.groupRecord({NODE, 1})); EXPECT_TRUE(context.groupRecord({NODE, 65}));
    EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(both, {NODE, 65}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    const auto before = both.superblock(); const auto epoch = io_->dataMutationEpoch();
    EXPECT_THROW(convert(NODE, 2, memory), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    ASSERT_NO_THROW(convert(NODE, 1, memory, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW));
    auto mixed = metadata_->openSnapshot(); HybridPropertyReadContext partial(io_, *slotted_pages_, *metadata_, mixed);
    EXPECT_FALSE(partial.groupRecord({NODE, 1})); EXPECT_TRUE(partial.groupRecord({NODE, 65}));
    EXPECT_TRUE(metadata_->findEntityPropertyLocationOptional(mixed, {NODE, 1}));
    EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(mixed, {NODE, 65}));
    EXPECT_EQ(properties_->readEntity(mixed, {NODE, 1}), first); EXPECT_EQ(properties_->readEntity(mixed, {NODE, 65}), second);
    EXPECT_EQ(properties_->readEntity(both, {NODE, 1}), first);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    ASSERT_NO_THROW(convert(NODE, 2, memory, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW));
    auto row = metadata_->openSnapshot(); EXPECT_FALSE(row.superblock().hybrid_property_descriptor_root.isValid());
    EXPECT_EQ(properties_->readEntity(row, {NODE, 1}), first); EXPECT_EQ(properties_->readEntity(row, {NODE, 65}), second);
    EXPECT_EQ(properties_->readEntity(both, {NODE, 65}), second);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, WholeGroupOwnerDeletionKeepsOtherGroupsAndTheirSharedDirectories) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    ASSERT_NO_THROW(convert(NODE, 1, memory));
    ASSERT_NO_THROW(convert(NODE, 2, memory));
    auto old = metadata_->openSnapshot(); const auto expected = properties_->readEntity(old, {NODE, 65});
    std::array<EntityPropertyKey, 64> removed{}; for (size_t i = 0; i < removed.size(); ++i) removed[i] = {NODE, i + 1};
    const auto state = old.superblock();
    NativeHybridPropertyColumnWrite deletion{state.database_id, state.generation, state.hybrid_property_descriptor_root,
        {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true, removed};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(deletion, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    auto current = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
    EXPECT_FALSE(context.groupRecord({NODE, 1})); EXPECT_TRUE(context.groupRecord({NODE, 65}));
    EXPECT_EQ(properties_->readEntity(current, {NODE, 65}), expected);
    EXPECT_TRUE(metadata_->findNodePageLocationOptional(old, 1)); EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, 1));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    ASSERT_NO_THROW(convert(NODE, 2, memory, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW));
    EXPECT_FALSE(metadata_->openSnapshot().superblock().hybrid_property_descriptor_root.isValid());
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 65}), expected);
}
TEST_F(HybridPropertyMultipleGroupsTest, NodeAndEdgeNamespacesCanConvertAndReturnIndependently) {
    seed(NODE, 2, 2); seed(EDGE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto original = metadata_->openSnapshot(); const auto node = properties_->readEntity(original, {NODE, 1});
    const auto edge = properties_->readEntity(original, {EDGE, 1});
    ASSERT_NO_THROW(convert(NODE, 1, memory));
    ASSERT_NO_THROW(convert(EDGE, 1, memory));
    auto both = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, both);
    EXPECT_TRUE(context.groupRecord({NODE, 1})); EXPECT_TRUE(context.groupRecord({EDGE, 1}));
    ASSERT_NO_THROW(convert(NODE, 1, memory, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW));
    auto mixed = metadata_->openSnapshot(); HybridPropertyReadContext partial(io_, *slotted_pages_, *metadata_, mixed);
    EXPECT_FALSE(partial.groupRecord({NODE, 1})); EXPECT_TRUE(partial.groupRecord({EDGE, 1}));
    EXPECT_EQ(properties_->readEntity(mixed, {NODE, 1}), node); EXPECT_EQ(properties_->readEntity(mixed, {EDGE, 1}), edge);
    ASSERT_NO_THROW(convert(EDGE, 1, memory, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW));
    auto row = metadata_->openSnapshot(); EXPECT_FALSE(row.superblock().hybrid_property_descriptor_root.isValid());
    EXPECT_EQ(properties_->readEntity(row, {NODE, 1}), node); EXPECT_EQ(properties_->readEntity(row, {EDGE, 1}), edge);
    EXPECT_EQ(properties_->readEntity(both, {NODE, 1}), node); EXPECT_EQ(properties_->readEntity(both, {EDGE, 1}), edge);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, SecondGroupQuotaAndPrivateCancellationLeaveEveryPublishedRouteUnchanged) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    ASSERT_NO_THROW(convert(NODE, 1, memory));
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    auto tiny = std::make_shared<MaterializedByteBudget>(128);
    EXPECT_THROW(convert(NODE, 2, tiny), graph::query::execution::MaterializedByteLimitExceeded); EXPECT_EQ(tiny->used(), 0U);
    struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("cancel shared-directory conversion") {} };
    struct Cancellation final : CancellationCheckpoint { bool prepared = false; void check() const override { if (prepared) throw Cancelled(); } } cancellation;
    {
        graph::debug::PerfTrace::ScopedObserver observer(&cancellation, [](void *owner, std::string_view key, int64_t count, bool) noexcept {
            if (key == "native_graph.hybrid.private_index_frames" && count > 0) static_cast<Cancellation *>(owner)->prepared = true;
        });
        EXPECT_THROW(convert(NODE, 2, memory, NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, {}, &cancellation), Cancelled);
    }
    EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    ASSERT_NO_THROW(convert(NODE, 2, memory));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, SecondGroupABRecoveryNeverDropsTheFirstGroupOrPublishesHalfADirectory) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    ASSERT_NO_THROW(convert(NODE, 1, memory)); auto old = metadata_->openSnapshot();
    const auto first = properties_->readEntity(old, {NODE, 1}); const auto second = properties_->readEntity(old, {NODE, 65});
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions interruption; interruption.bootstrap_failpoint = point;
        EXPECT_THROW(convert(NODE, 2, memory, NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, interruption), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); auto pin = recovered->openSnapshot();
        SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore values(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY); PropertyCollectionRootStore roots(pages, values, *recovered);
        HybridPropertyReadContext context(io_, pages, *recovered, pin);
        EXPECT_TRUE(context.groupRecord({NODE, 1}));
        EXPECT_EQ(bool(context.groupRecord({NODE, 65})), point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE);
        EXPECT_EQ(roots.readEntity(pin, {NODE, 1}), first); EXPECT_EQ(roots.readEntity(pin, {NODE, 65}), second);
        EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), first); EXPECT_EQ(properties_->readEntity(old, {NODE, 65}), second);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyMultipleGroupsTest, IndependentFieldWritesAndGlobalGCRecognizeReferencesInAnotherGroup) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    ASSERT_NO_THROW(convert(NODE, 1, memory));
    ASSERT_NO_THROW(convert(NODE, 2, memory));
    const auto replace = [&](EntityPropertyKey owner, uint32_t token, NativeHybridPropertyColumnReplacement replacement) {
        const auto state = metadata_->openSnapshot().superblock(); replacement.owner = owner; replacement.token = token;
        NativeHybridPropertyColumnWrite mutation{state.database_id, state.generation, state.hybrid_property_descriptor_root,
            std::span(&replacement, 1), memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
        (void)metadata_->mutateHybridPropertyColumns(mutation, LogSequenceNumber(state.checkpoint_lsn.value() + 1));
    };
    auto original = metadata_->openSnapshot(); const auto first = roots.readEntity(original, {NODE, 1});
    ASSERT_NO_THROW(replace({NODE, 65}, 1, {{}, 0, int64_t(999)}));
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 1}), first);
    ASSERT_NO_THROW(replace({NODE, 1}, 3, {{}, 0, graph::OwnedPropertyValue(PropertyValue(std::string(8192, 'x')))}));
    const auto newValue = metadata_->openSnapshot().superblock().next_large_value_id;
    ASSERT_NO_THROW(replace({NODE, 65}, 3, {{}, 0, graph::OwnedPropertyValue(PropertyValue(std::string(8192, 'x')))}));
    EXPECT_EQ(metadata_->openSnapshot().superblock().next_large_value_id, newValue);
    auto shared = metadata_->openSnapshot(); const auto other = roots.readEntity(shared, {NODE, 65});
    ASSERT_NO_THROW(replace({NODE, 1}, 3, {{}, 0, {}, true}));
    LargeValueReachabilityReclaimer reclaimer(values, *slotted_pages_, *metadata_);
    LargeValueReachabilityReclaimOptions options;
    options.maximum_manifest_locations = 32; options.maximum_manifest_metadata_pages = 128; options.maximum_manifest_nodes = 128;
    options.maximum_owner_locations = 512; options.maximum_owner_metadata_pages = 8192; options.maximum_data_pages = 128;
    options.maximum_retirements = 256; options.maximum_page_locations = 1024; options.maximum_pools = 64;
    options.first_index_slot = PhysicalSlotId(40); options.last_index_slot = PhysicalSlotId(80);
    const auto before = metadata_->openSnapshot().superblock();
    const auto report = reclaimer.reclaim(options, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
    EXPECT_EQ(report.reachable_values, 1U); EXPECT_EQ(report.retired_values, 0U);
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 1}), first);
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 65}), other);
    EXPECT_EQ(roots.readEntity(original, {NODE, 1}), first);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, AdjacentGroupsShareRealPropertyAndDirectoryPoolsWhileOldPinsRemainReadable) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    ASSERT_NO_THROW(convert(NODE, 1, memory)); auto old = metadata_->openSnapshot();
    const auto before = properties_->readEntity(old, {NODE, 1});
    const auto poolCounts = [&](const MetadataSnapshotPin &pin) {
        std::array<size_t, 2> counts{};
        metadata_->visitCatalogEntries(pin, [&](const CatalogEntry &pool) {
            if (pool.state != SegmentState::SGS_SEALED) return;
            if (pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && pool.owner_id == 903) ++counts[0];
            if (pool.kind == SegmentKind::SGK_INDEX_PAGE_POOL && isHybridPropertyIndexId(pool.owner_id)) ++counts[1];
        }); return counts;
    };
    const auto first = poolCounts(old); ASSERT_EQ(first[0], 1U);
    ASSERT_NO_THROW(convert(NODE, 2, memory)); auto both = metadata_->openSnapshot();
    EXPECT_EQ(poolCounts(both), first); // actual 128 KiB allocation, not payload bytes
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), before);
    EXPECT_EQ(properties_->readEntity(both, {NODE, 1}), before);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    ASSERT_NO_THROW(convert(NODE, 1, memory, NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW));
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}), before);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, FailedSharedPoolAppendCanBeRetriedWithoutPublishingOrLosingTheFirstGroup) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    ASSERT_NO_THROW(convert(NODE, 1, memory)); auto old = metadata_->openSnapshot();
    const auto first = properties_->readEntity(old, {NODE, 1}); const auto second = properties_->readEntity(old, {NODE, 65});
    MetadataPublishOptions interruption; interruption.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE;
    EXPECT_THROW(convert(NODE, 2, memory, NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, interruption), BootstrapPublishInterrupted);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), old.superblock());
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), first); EXPECT_EQ(properties_->readEntity(old, {NODE, 65}), second);
    ASSERT_NO_THROW(convert(NODE, 2, memory));
    auto both = metadata_->openSnapshot(); EXPECT_EQ(properties_->readEntity(both, {NODE, 1}), first);
    EXPECT_EQ(properties_->readEntity(both, {NODE, 65}), second);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, ReopenedConversionFindsNeighbourCapacityWithoutDependingOnAnInMemoryHint) {
    seed(NODE, 128, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    ASSERT_NO_THROW(convert(NODE, 1, memory)); SegmentId fieldPool;
    metadata_->visitCatalogEntries(metadata_->openSnapshot(), [&](const CatalogEntry &entry) {
        if (entry.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && entry.owner_id == 903 && entry.state == SegmentState::SGS_SEALED) fieldPool = entry.segment_id;
    }); ASSERT_TRUE(fieldPool.isValid());
    vector_manifests_.reset(); vector_pages_.reset(); properties_.reset(); labels_.reset(); btree_pages_.reset(); slotted_pages_.reset(); entities_.reset(); metadata_.reset();
    metadata_ = RelocatableMetadataStore::open(io_);
    ASSERT_NO_THROW(convert(NODE, 2, memory));
    size_t fields = 0; metadata_->visitCatalogEntries(metadata_->openSnapshot(), [&](const CatalogEntry &entry) {
        if (entry.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && entry.owner_id == 903 && entry.state == SegmentState::SGS_SEALED) {
            ++fields; EXPECT_EQ(entry.segment_id, fieldPool);
        }
    }); EXPECT_EQ(fields, 1U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
#endif
