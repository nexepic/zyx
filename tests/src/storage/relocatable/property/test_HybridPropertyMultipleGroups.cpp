#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyMultipleGroupsTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    void seedBothKinds(const std::shared_ptr<MaterializedByteBudget> &memory) {
        seed(NODE, 64, 2); seed(EDGE, 64, 2); convert(NODE, 1, memory); convert(EDGE, 1, memory);
    }
    void attachPropertyIndex(IndexDefinition &definition) {
        NativeGraphWritePlannerOptions placement; placement.first_slot = PhysicalSlotId(40); placement.last_slot = PhysicalSlotId(80);
        placement.entity_pool_owner_id = 901; placement.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
        NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement);
        SystemCatalog catalog(*btree_pages_, *metadata_); auto tx = writer.beginTransaction(); auto session = catalog.beginWrite(tx);
        ASSERT_EQ(session.ensurePropertyToken("p"), 1U); ASSERT_EQ(session.ensurePropertyToken("q"), 2U);
        definition = IndexDefinition{}; definition.name = "global_p"; definition.kind = IndexDefinitionKind::IDK_PROPERTY;
        definition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE; definition.property_names = {"p"};
        definition = session.createIndexDefinition(definition); session.setGraphCardinalityStatistics({128, 0}); session.finish();
        std::vector<BTreePageEntry> entries;
        for (uint64_t id = 1; id <= 128; ++id) {
            const auto key = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(id)), id); BTreePageEntry entry;
            entry.key.assign(key.begin(), key.end()); entry.value_or_child = id; entries.push_back(std::move(entry));
        }
        tx.stageScalarIndexCreate(*btree_pages_, definition.index_id, entries); ASSERT_TRUE(tx.commit().committed());
    }
    Superblock writeAcrossGroups(std::span<const NativeHybridPropertyColumnReplacement> cells,
        const std::shared_ptr<MaterializedByteBudget> &memory, const MetadataPublishOptions &options = {}) {
        const auto source = metadata_->openSnapshot().superblock();
        NativeHybridPropertyColumnWrite write{source.database_id, source.generation, source.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
        return metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(source.checkpoint_lsn.value() + 1), options);
    }
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
TEST_F(HybridPropertyMultipleGroupsTest, LifecycleSetUpdates64ExistingColumnCellsAcrossTwoGroupsAtomically) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    convert(NODE, 1, memory); convert(NODE, 2, memory); const auto old = metadata_->openSnapshot();
    std::array<NativeHybridPropertyColumnReplacement, 64> cells;
    for (size_t i = 0; i < cells.size(); ++i) {
        const uint64_t id = i < 32 ? i + 1 : i + 33;
        cells[i] = {{NODE, id}, 1, int64_t(1000 + id)};
    }
    ASSERT_NO_THROW((void)writeAcrossGroups(cells, memory)); const auto current = metadata_->openSnapshot();
    for (const auto &cell : cells) {
        EXPECT_EQ(properties_->readEntityPropertyOptional(current, cell.owner, 1)->value, PropertyValue(int64_t(1000 + cell.owner.entity_id)));
        EXPECT_EQ(properties_->readEntityPropertyOptional(old, cell.owner, 1)->value, PropertyValue(int64_t(cell.owner.entity_id)));
        EXPECT_EQ(properties_->readEntityPropertyOptional(current, cell.owner, 2)->value, PropertyValue(int64_t(cell.owner.entity_id + 1)));
    }
    const auto before = current.superblock(); const auto epoch = io_->dataMutationEpoch();
    EXPECT_EQ(writeAcrossGroups(cells, memory), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    auto reopened = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore roots(pages, *reopened); const auto pin = reopened->openSnapshot();
    EXPECT_EQ(roots.readEntityPropertyOptional(pin, {NODE, 1}, 1)->value, PropertyValue(int64_t(1001)));
    EXPECT_EQ(roots.readEntityPropertyOptional(pin, {NODE, 65}, 1)->value, PropertyValue(int64_t(1065)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, MapReplacementCanChangeColumnsInTwoNativeGroupsInOneTransaction) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    convert(NODE, 1, memory); convert(NODE, 2, memory); const auto old = metadata_->openSnapshot();
    NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    std::array<PropertyCollectionRoot, 2> roots{properties_->readEntity(old, {NODE, 1}), properties_->readEntity(old, {NODE, 65})};
    roots[0].entries[0].value = PropertyValue(int64_t(1001)); roots[1].entries[0].value = PropertyValue(int64_t(1065));
    auto tx = writer.beginTransaction(); tx.stageHybridPropertyMaps(roots, NativeHybridPropertyMapMode::NHPMM_REPLACE, memory);
    ASSERT_TRUE(tx.commit().committed()); const auto current = metadata_->openSnapshot();
    EXPECT_EQ(properties_->readEntity(current, {NODE, 1}), roots[0]); EXPECT_EQ(properties_->readEntity(current, {NODE, 65}), roots[1]);
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, 1}, 1)->value, PropertyValue(int64_t(1)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, 65}, 1)->value, PropertyValue(int64_t(65)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, CrossGroupColumnChangesRecoverTogetherAcrossBothBootstrapInterruptions) {
    seed(EDGE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    convert(EDGE, 1, memory); convert(EDGE, 2, memory); const auto old = metadata_->openSnapshot();
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{EDGE, 1}, 1, int64_t(1001)}, {{EDGE, 65}, 1, int64_t(1065)}}};
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions options; options.bootstrap_failpoint = point;
        EXPECT_THROW((void)writeAcrossGroups(cells, memory, options), BootstrapPublishInterrupted);
        auto reopened = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *reopened); const auto pin = reopened->openSnapshot();
        const bool complete = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(roots.readEntityPropertyOptional(pin, {EDGE, 1}, 1)->value, PropertyValue(int64_t(complete ? 1001 : 1)));
        EXPECT_EQ(roots.readEntityPropertyOptional(pin, {EDGE, 65}, 1)->value, PropertyValue(int64_t(complete ? 1065 : 65)));
        EXPECT_EQ(properties_->readEntityPropertyOptional(old, {EDGE, 1}, 1)->value, PropertyValue(int64_t(1)));
        EXPECT_EQ(properties_->readEntityPropertyOptional(old, {EDGE, 65}, 1)->value, PropertyValue(int64_t(65)));
        EXPECT_EQ(memory->used(), 0U);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyMultipleGroupsTest, CrossGroupDemotionChangesOnlyItsFieldAndPreservesTheOtherGroup) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    convert(NODE, 1, memory); convert(NODE, 2, memory);
    std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 1, int64_t(1001)}, {{NODE, 65}, 1, int64_t(1065)}}};
    (void)writeAcrossGroups(cells, memory); const auto old = metadata_->openSnapshot();
    { graph::utils::OwnedAllocationScope allocation(memory); cells[0].value = graph::OwnedPropertyValue(PropertyValue("demotion")); }
    ASSERT_NO_THROW((void)writeAcrossGroups(cells, memory)); const auto current = metadata_->openSnapshot();
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, 1}, 1)->value, PropertyValue("demotion"));
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, 65}, 1)->value, PropertyValue(int64_t(1065)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, 1}, 1)->value, PropertyValue(int64_t(1001)));
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current); bool firstExtension = false, secondColumn = false;
    (void)context.visitField({NODE, 1}, 1, [](const auto &) {}, [&](const auto &) { firstExtension = true; });
    (void)context.visitField({NODE, 65}, 1, [&](const auto &) { secondColumn = true; }, [](const auto &) {});
    EXPECT_TRUE(firstExtension); EXPECT_TRUE(secondColumn);
    cells[0].value = int64_t(1001); EXPECT_EQ(memory->used(), 0U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyMultipleGroupsTest, CrossGroupFieldsAndTheirSharedScalarIndexRecoverInOneGeneration) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    IndexDefinition definition; attachPropertyIndex(definition);
    SystemCatalog catalog(*btree_pages_, *metadata_);
    convert(NODE, 1, memory); convert(NODE, 2, memory); const auto old = metadata_->openSnapshot();
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 1, int64_t(1001)}, {{NODE, 65}, 1, int64_t(1065)}}};
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions options; options.bootstrap_failpoint = point;
        EXPECT_THROW((void)writeAcrossGroups(cells, memory, options), BootstrapPublishInterrupted);
        auto reopened = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        BTreePageStore trees(pages, *reopened); PropertyCollectionRootStore roots(pages, *reopened); const auto source = reopened->openSnapshot();
        const bool complete = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        for (const auto id : {uint64_t(1), uint64_t(65)}) {
            const auto value = PropertyValue(int64_t(complete ? 1000 + id : id));
            EXPECT_EQ(roots.readEntityPropertyOptional(source, {NODE, id}, 1)->value, value);
            EXPECT_EQ(BTreeIndex(definition.index_id, trees, *reopened).find(source, IndexKeyCodec::encodeEntryKey(value, id)), std::optional<uint64_t>(id));
            const auto absent = PropertyValue(int64_t(complete ? id : 1000 + id));
            EXPECT_FALSE(BTreeIndex(definition.index_id, trees, *reopened).find(source, IndexKeyCodec::encodeEntryKey(absent, id)));
            EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, id}, 1)->value, PropertyValue(int64_t(id)));
        }
        EXPECT_EQ(catalog.readGraphCardinalityStatistics(old), std::optional(NativeGraphCardinalityStatistics{128, 0}));
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyMultipleGroupsTest, CrossGroupColumnQuotaRefusesWithoutChangingEitherGroup) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    convert(NODE, 1, memory); convert(NODE, 2, memory);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 1, int64_t(1001)}, {{NODE, 65}, 1, int64_t(1065)}}};
    auto tiny = std::make_shared<MaterializedByteBudget>(128);
    EXPECT_THROW((void)writeAcrossGroups(cells, tiny), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyMultipleGroupsTest, MembershipRemovalAndReinsertionMergeEachGroupsCounters) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    convert(NODE, 1, memory); convert(NODE, 2, memory); const auto old = metadata_->openSnapshot();
    std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 1, std::monostate{}, true}, {{NODE, 65}, 1, std::monostate{}, true}}};
    ASSERT_NO_THROW((void)writeAcrossGroups(cells, memory)); const auto absent = metadata_->openSnapshot();
    for (const auto id : {uint64_t(1), uint64_t(65)}) {
        EXPECT_FALSE(properties_->readEntityPropertyOptional(absent, {NODE, id}, 1));
        EXPECT_EQ(HybridPropertyReadContext(io_, *slotted_pages_, *metadata_, absent).groupRecord({NODE, id})->memberships, 127U);
    }
    cells[0] = {{NODE, 1}, 1, int64_t(1001)}; cells[1] = {{NODE, 65}, 1, int64_t(1065)};
    ASSERT_NO_THROW((void)writeAcrossGroups(cells, memory)); const auto current = metadata_->openSnapshot();
    for (const auto id : {uint64_t(1), uint64_t(65)}) {
        EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, id}, 1)->value, PropertyValue(int64_t(1000 + id)));
        EXPECT_EQ(HybridPropertyReadContext(io_, *slotted_pages_, *metadata_, current).groupRecord({NODE, id})->memberships, 128U);
        EXPECT_FALSE(properties_->readEntityPropertyOptional(absent, {NODE, id}, 1));
        EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, id}, 1)->value, PropertyValue(int64_t(id)));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyMultipleGroupsTest, TwoGroupsDemoteTogetherAndLaterIntegersRemainExtensions) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    convert(NODE, 1, memory); convert(NODE, 2, memory); const auto old = metadata_->openSnapshot();
    std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 1, graph::OwnedPropertyValue(PropertyValue("first"))},
        {{NODE, 65}, 1, graph::OwnedPropertyValue(PropertyValue("second"))}}};
    ASSERT_NO_THROW((void)writeAcrossGroups(cells, memory)); const auto demoted = metadata_->openSnapshot();
    cells[0].value = int64_t(1001); cells[1].value = int64_t(1065); ASSERT_NO_THROW((void)writeAcrossGroups(cells, memory));
    const auto current = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
    for (const auto id : {uint64_t(1), uint64_t(65)}) {
        bool extension = false; (void)context.visitField({NODE, id}, 1, [](const auto &) {}, [&](const auto &) { extension = true; }); EXPECT_TRUE(extension);
        EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, id}, 1)->value, PropertyValue(int64_t(1000 + id)));
        EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, id}, 1)->value, PropertyValue(int64_t(id)));
        EXPECT_EQ(context.groupRecord({NODE, id})->columns, 1U); EXPECT_EQ(context.groupRecord({NODE, id})->memberships, 128U);
    }
    EXPECT_EQ(properties_->readEntityPropertyOptional(demoted, {NODE, 1}, 1)->value, PropertyValue("first"));
    EXPECT_EQ(properties_->readEntityPropertyOptional(demoted, {NODE, 65}, 1)->value, PropertyValue("second"));
    EXPECT_EQ(memory->used(), 0U); EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyMultipleGroupsTest, SharedExtensionPageSurvivesDemotionRemovalAndGrowthInDifferentGroups) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    convert(NODE, 1, memory); convert(NODE, 2, memory);
    std::array<NativeHybridPropertyColumnReplacement, 2> inserted{{{{NODE, 1}, 3, graph::OwnedPropertyValue(PropertyValue("first"))},
        {{NODE, 65}, 3, graph::OwnedPropertyValue(PropertyValue("second"))}}};
    ASSERT_NO_THROW((void)writeAcrossGroups(inserted, memory)); const auto old = metadata_->openSnapshot();
    const std::string grown(300, 'g');
    std::array<NativeHybridPropertyColumnReplacement, 3> changes{{{{NODE, 1}, 1, graph::OwnedPropertyValue(PropertyValue("demoted"))},
        {{NODE, 1}, 3, std::monostate{}, true}, {{NODE, 65}, 3, graph::OwnedPropertyValue(PropertyValue(grown))}}};
    ASSERT_NO_THROW((void)writeAcrossGroups(changes, memory)); const auto current = metadata_->openSnapshot();
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, 1}, 1)->value, PropertyValue("demoted"));
    EXPECT_FALSE(properties_->readEntityPropertyOptional(current, {NODE, 1}, 3));
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, 65}, 3)->value, PropertyValue(grown));
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, 65}, 1)->value, PropertyValue(int64_t(65)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, 1}, 3)->value, PropertyValue("first"));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, 65}, 3)->value, PropertyValue("second"));
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
    EXPECT_EQ(context.groupRecord({NODE, 1})->memberships, 128U); EXPECT_EQ(context.groupRecord({NODE, 65})->memberships, 129U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, CrossGroupDemotionRecoversAllOwnerColumnAndExtensionRootsTogether) {
    seed(EDGE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    convert(EDGE, 1, memory); convert(EDGE, 2, memory); const auto old = metadata_->openSnapshot();
    const std::array<NativeHybridPropertyColumnReplacement, 2> changes{{{{EDGE, 1}, 1, graph::OwnedPropertyValue(PropertyValue("first"))},
        {{EDGE, 65}, 1, graph::OwnedPropertyValue(PropertyValue("second"))}}};
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions options; options.bootstrap_failpoint = point;
        EXPECT_THROW((void)writeAcrossGroups(changes, memory, options), BootstrapPublishInterrupted);
        auto reopened = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *reopened); const auto pin = reopened->openSnapshot(); const bool complete = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(roots.readEntityPropertyOptional(pin, {EDGE, 1}, 1)->value, complete ? PropertyValue("first") : PropertyValue(int64_t(1)));
        EXPECT_EQ(roots.readEntityPropertyOptional(pin, {EDGE, 65}, 1)->value, complete ? PropertyValue("second") : PropertyValue(int64_t(65)));
        EXPECT_EQ(properties_->readEntityPropertyOptional(old, {EDGE, 1}, 1)->value, PropertyValue(int64_t(1)));
        EXPECT_EQ(properties_->readEntityPropertyOptional(old, {EDGE, 65}, 1)->value, PropertyValue(int64_t(65)));
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyMultipleGroupsTest, CrossGroupMapReplacementCombinesDemotionRemovalAndNewExtensions) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    convert(NODE, 1, memory); convert(NODE, 2, memory); const auto old = metadata_->openSnapshot();
    NativeGraphWritePlannerOptions placement; placement.first_slot = PhysicalSlotId(40); placement.last_slot = PhysicalSlotId(80);
    placement.entity_pool_owner_id = 901; placement.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement);
    const std::array<PropertyCollectionRoot, 2> roots{{{1, graph::Node::typeId, {{1, PropertyValue("demoted")}, {3, PropertyValue(true)}}},
        {65, graph::Node::typeId, {{2, PropertyValue(int64_t(9000))}, {3, PropertyValue("new")}}}}};
    auto tx = writer.beginTransaction(); tx.stageHybridPropertyMaps(roots, NativeHybridPropertyMapMode::NHPMM_REPLACE, memory);
    ASSERT_TRUE(tx.commit().committed()); const auto current = metadata_->openSnapshot();
    EXPECT_EQ(properties_->readEntity(current, {NODE, 1}), roots[0]); EXPECT_EQ(properties_->readEntity(current, {NODE, 65}), roots[1]);
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, 2}, 1)->value, PropertyValue(int64_t(2)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, 66}, 1)->value, PropertyValue(int64_t(66)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, 1}, 1)->value, PropertyValue(int64_t(1)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, 65}, 1)->value, PropertyValue(int64_t(65)));
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
    EXPECT_EQ(context.groupRecord({NODE, 1})->memberships, 128U); EXPECT_EQ(context.groupRecord({NODE, 65})->memberships, 128U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, CrossGroupLargeValueDedupPreservesTheOtherOwnerAndOldPinsOnRemoval) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    convert(NODE, 1, memory); convert(NODE, 2, memory); const auto old = metadata_->openSnapshot();
    LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_); const std::string payload(70000, 'v');
    std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 3, graph::OwnedPropertyValue(PropertyValue(payload))},
        {{NODE, 65}, 3, graph::OwnedPropertyValue(PropertyValue(payload))}}};
    ASSERT_NO_THROW((void)writeAcrossGroups(cells, memory)); const auto inserted = metadata_->openSnapshot();
    EXPECT_EQ(inserted.superblock().next_large_value_id, old.superblock().next_large_value_id + 1U);
    std::array<LargeValueId, 2> identities{}; size_t index = 0;
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, inserted);
    graph::utils::OwnedHeapReservation referenceCredit(memory); const auto admitReference = [&](size_t bytes) { referenceCredit.set(bytes); };
    for (const auto id : {uint64_t(1), uint64_t(65)}) {
        EXPECT_EQ(roots.readEntityPropertyOptional(inserted, {NODE, id}, 3)->value, PropertyValue(payload));
        EXPECT_FALSE(roots.readEntityPropertyOptional(old, {NODE, id}, 3));
        (void)context.visitField({NODE, id}, 3, [](const auto &) {}, [&](const auto &field) {
            ASSERT_TRUE(field.reference()); identities[index++] = field.reference()->value_id;
        }, {}, std::ref(admitReference)); referenceCredit.set(0);
    }
    ASSERT_EQ(index, 2U); EXPECT_EQ(identities[0], identities[1]);
    cells[0] = {{NODE, 1}, 3, std::monostate{}, true}; ASSERT_NO_THROW((void)writeAcrossGroups(std::span(cells).first(1), memory));
    const auto one = metadata_->openSnapshot(); EXPECT_FALSE(roots.readEntityPropertyOptional(one, {NODE, 1}, 3));
    EXPECT_EQ(roots.readEntityPropertyOptional(one, {NODE, 65}, 3)->value, PropertyValue(payload));
    EXPECT_EQ(roots.readEntityPropertyOptional(inserted, {NODE, 1}, 3)->value, PropertyValue(payload));
    cells[1] = {{NODE, 65}, 3, std::monostate{}, true}; ASSERT_NO_THROW((void)writeAcrossGroups(cells, memory));
    const auto current = metadata_->openSnapshot(); EXPECT_FALSE(roots.readEntityPropertyOptional(current, {NODE, 1}, 3));
    EXPECT_FALSE(roots.readEntityPropertyOptional(current, {NODE, 65}, 3));
    EXPECT_EQ(roots.readEntityPropertyOptional(one, {NODE, 65}, 3)->value, PropertyValue(payload));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, CrossGroupDemotionUpdatesOneScalarIndexAndPreservesUntouchedEntries) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    IndexDefinition definition; attachPropertyIndex(definition);
    SystemCatalog catalog(*btree_pages_, *metadata_);
    convert(NODE, 1, memory); convert(NODE, 2, memory); const auto old = metadata_->openSnapshot();
    const std::array<NativeHybridPropertyColumnReplacement, 2> changes{{{{NODE, 1}, 1, graph::OwnedPropertyValue(PropertyValue("first"))},
        {{NODE, 65}, 1, graph::OwnedPropertyValue(PropertyValue("second"))}}};
    ASSERT_NO_THROW((void)writeAcrossGroups(changes, memory)); const auto current = metadata_->openSnapshot();
    BTreeIndex index(definition.index_id, *btree_pages_, *metadata_);
    for (const auto id : {uint64_t(1), uint64_t(65)}) {
        const auto value = PropertyValue(id == 1 ? "first" : "second"); const auto oldValue = PropertyValue(int64_t(id));
        EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, id}, 1)->value, value);
        EXPECT_EQ(index.find(current, IndexKeyCodec::encodeEntryKey(value, id)), std::optional<uint64_t>(id));
        EXPECT_FALSE(index.find(current, IndexKeyCodec::encodeEntryKey(oldValue, id)));
        EXPECT_EQ(index.find(old, IndexKeyCodec::encodeEntryKey(oldValue, id)), std::optional<uint64_t>(id));
        EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, id}, 1)->value, oldValue);
    }
    EXPECT_EQ(index.find(current, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(66)), 66)), std::optional<uint64_t>(66));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, SameNodeAndEdgeIdsKeepCompatibleColumnValuesInSeparateNamespaces) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedBothKinds(memory); const auto old = metadata_->openSnapshot();
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 1, int64_t(111)}, {{EDGE, 1}, 1, int64_t(222)}}};
    ASSERT_NO_THROW((void)writeAcrossGroups(cells, memory)); const auto current = metadata_->openSnapshot();
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, 1}, 1)->value, PropertyValue(int64_t(111)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {EDGE, 1}, 1)->value, PropertyValue(int64_t(222)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, 1}, 1)->value, PropertyValue(int64_t(1)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {EDGE, 1}, 1)->value, PropertyValue(int64_t(1)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, MixedNodeEdgeMapReplacementKeepsDemotionAndMembershipDirectoriesSeparate) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedBothKinds(memory); const auto old = metadata_->openSnapshot();
    NativeGraphWritePlannerOptions placement; placement.first_slot = PhysicalSlotId(40); placement.last_slot = PhysicalSlotId(80);
    placement.entity_pool_owner_id = 901; placement.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement);
    const std::array<PropertyCollectionRoot, 2> roots{{{1, graph::Node::typeId, {{1, PropertyValue("node")}, {3, PropertyValue(true)}}},
        {1, graph::Edge::typeId, {{1, PropertyValue(false)}, {2, PropertyValue(int64_t(9000))}}}}};
    auto tx = writer.beginTransaction(); tx.stageHybridPropertyMaps(roots, NativeHybridPropertyMapMode::NHPMM_REPLACE, memory);
    ASSERT_TRUE(tx.commit().committed()); const auto current = metadata_->openSnapshot();
    EXPECT_EQ(properties_->readEntity(current, {NODE, 1}), roots[0]); EXPECT_EQ(properties_->readEntity(current, {EDGE, 1}), roots[1]);
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, 2}, 1)->value, PropertyValue(int64_t(2)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {EDGE, 2}, 1)->value, PropertyValue(int64_t(2)));
    EXPECT_FALSE(properties_->readEntityPropertyOptional(current, {NODE, 1}, 2));
    EXPECT_FALSE(properties_->readEntityPropertyOptional(current, {EDGE, 1}, 3));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, 1}, 1)->value, PropertyValue(int64_t(1)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {EDGE, 1}, 1)->value, PropertyValue(int64_t(1)));
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
    EXPECT_EQ(context.groupRecord({NODE, 1})->memberships, 128U); EXPECT_EQ(context.groupRecord({EDGE, 1})->memberships, 128U);
    EXPECT_EQ(context.groupRecord({NODE, 1})->columns, 1U); EXPECT_EQ(context.groupRecord({EDGE, 1})->columns, 1U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, MixedNodeEdgeExtensionsUseDifferentDirectoriesWithSharedFieldPages) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedBothKinds(memory); const auto old = metadata_->openSnapshot();
    std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 3, graph::OwnedPropertyValue(PropertyValue("node"))},
        {{EDGE, 1}, 3, graph::OwnedPropertyValue(PropertyValue("edge"))}}};
    ASSERT_NO_THROW((void)writeAcrossGroups(cells, memory)); const auto inserted = metadata_->openSnapshot();
    cells[0] = {{NODE, 1}, 3, std::monostate{}, true}; cells[1].value = graph::OwnedPropertyValue(PropertyValue(std::string(300, 'e')));
    ASSERT_NO_THROW((void)writeAcrossGroups(cells, memory)); const auto current = metadata_->openSnapshot();
    EXPECT_FALSE(properties_->readEntityPropertyOptional(current, {NODE, 1}, 3));
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {EDGE, 1}, 3)->value, PropertyValue(std::string(300, 'e')));
    EXPECT_EQ(properties_->readEntityPropertyOptional(inserted, {NODE, 1}, 3)->value, PropertyValue("node"));
    EXPECT_EQ(properties_->readEntityPropertyOptional(inserted, {EDGE, 1}, 3)->value, PropertyValue("edge"));
    EXPECT_FALSE(properties_->readEntityPropertyOptional(old, {NODE, 1}, 3)); EXPECT_FALSE(properties_->readEntityPropertyOptional(old, {EDGE, 1}, 3));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, MixedNodeEdgeDemotionRecoversBothDirectoryNamespacesTogether) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedBothKinds(memory); const auto old = metadata_->openSnapshot();
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 1, graph::OwnedPropertyValue(PropertyValue("node"))},
        {{EDGE, 1}, 1, graph::OwnedPropertyValue(PropertyValue("edge"))}}};
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions options; options.bootstrap_failpoint = point;
        EXPECT_THROW((void)writeAcrossGroups(cells, memory, options), BootstrapPublishInterrupted);
        auto reopened = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *reopened); const auto pin = reopened->openSnapshot(); const bool complete = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(roots.readEntityPropertyOptional(pin, {NODE, 1}, 1)->value, complete ? PropertyValue("node") : PropertyValue(int64_t(1)));
        EXPECT_EQ(roots.readEntityPropertyOptional(pin, {EDGE, 1}, 1)->value, complete ? PropertyValue("edge") : PropertyValue(int64_t(1)));
        EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, 1}, 1)->value, PropertyValue(int64_t(1)));
        EXPECT_EQ(properties_->readEntityPropertyOptional(old, {EDGE, 1}, 1)->value, PropertyValue(int64_t(1)));
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyMultipleGroupsTest, MixedNodeEdgeMembershipErasureAndInsertionDoNotAliasIdenticalKeys) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedBothKinds(memory); const auto old = metadata_->openSnapshot();
    std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 1, std::monostate{}, true}, {{EDGE, 1}, 1, std::monostate{}, true}}};
    ASSERT_NO_THROW((void)writeAcrossGroups(cells, memory)); const auto absent = metadata_->openSnapshot();
    EXPECT_FALSE(properties_->readEntityPropertyOptional(absent, {NODE, 1}, 1)); EXPECT_FALSE(properties_->readEntityPropertyOptional(absent, {EDGE, 1}, 1));
    cells[0] = {{NODE, 1}, 1, int64_t(111)}; cells[1] = {{EDGE, 1}, 1, int64_t(222)};
    ASSERT_NO_THROW((void)writeAcrossGroups(cells, memory)); const auto current = metadata_->openSnapshot();
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, 1}, 1)->value, PropertyValue(int64_t(111)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {EDGE, 1}, 1)->value, PropertyValue(int64_t(222)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, 1}, 1)->value, PropertyValue(int64_t(1)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {EDGE, 1}, 1)->value, PropertyValue(int64_t(1)));
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
    EXPECT_EQ(context.groupRecord({NODE, 1})->memberships, 128U); EXPECT_EQ(context.groupRecord({EDGE, 1})->memberships, 128U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMultipleGroupsTest, MixedNodeEdgeLargeValueUsesOneIdentityAndRetainsItsOtherNamespaceOwner) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedBothKinds(memory); const auto old = metadata_->openSnapshot();
    LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_); const std::string payload(70000, 'm');
    std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 3, graph::OwnedPropertyValue(PropertyValue(payload))},
        {{EDGE, 1}, 3, graph::OwnedPropertyValue(PropertyValue(payload))}}};
    ASSERT_NO_THROW((void)writeAcrossGroups(cells, memory)); const auto inserted = metadata_->openSnapshot();
    EXPECT_EQ(inserted.superblock().next_large_value_id, old.superblock().next_large_value_id + 1U);
    EXPECT_EQ(roots.readEntityPropertyOptional(inserted, {NODE, 1}, 3)->value, PropertyValue(payload));
    EXPECT_EQ(roots.readEntityPropertyOptional(inserted, {EDGE, 1}, 3)->value, PropertyValue(payload));
    cells[0] = {{NODE, 1}, 3, std::monostate{}, true}; ASSERT_NO_THROW((void)writeAcrossGroups(std::span(cells).first(1), memory));
    const auto current = metadata_->openSnapshot(); EXPECT_FALSE(roots.readEntityPropertyOptional(current, {NODE, 1}, 3));
    EXPECT_EQ(roots.readEntityPropertyOptional(current, {EDGE, 1}, 3)->value, PropertyValue(payload));
    EXPECT_EQ(roots.readEntityPropertyOptional(inserted, {NODE, 1}, 3)->value, PropertyValue(payload));
    EXPECT_FALSE(roots.readEntityPropertyOptional(old, {NODE, 1}, 3)); EXPECT_FALSE(roots.readEntityPropertyOptional(old, {EDGE, 1}, 3));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
#endif
