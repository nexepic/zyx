#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"
#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyOwnerDeletionTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    void convertGroup(EntityPropertyOwnerKind kind, uint64_t group, const std::shared_ptr<MaterializedByteBudget> &memory) {
        auto conversion = request(kind, memory); conversion.conversion->group_id = group; conversion.conversion->publish_candidate = true; run(conversion);
    }
    void seedKinds(const std::shared_ptr<MaterializedByteBudget> &memory) {
        seed(NODE, 64, 2); seed(EDGE, 64, 2); convertGroup(NODE, 1, memory); convertGroup(EDGE, 1, memory);
    }
    NativeHybridPropertyColumnWrite deletion(std::span<const EntityPropertyKey> owners,
        std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation = nullptr) {
        const auto state = metadata_->openSnapshot().superblock();
        return {state.database_id, state.generation, state.hybrid_property_descriptor_root, {}, std::move(memory),
            PhysicalSlotId(40), PhysicalSlotId(80), 903, cancellation, true, owners};
    }
    void erase(const NativeHybridPropertyColumnWrite &write, const MetadataPublishOptions &options = {}) {
        static_cast<void>(metadata_->mutateHybridPropertyColumns(write,
            LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1), options));
    }
    NativeGraphWritePlannerOptions placement() {
        NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
        options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903}; return options;
    }
};
class OwnerDeletionCancelled final : public std::runtime_error { public: OwnerDeletionCancelled() : std::runtime_error("owner deletion cancelled") {} };
class CancelOwnerDeletion final : public CancellationCheckpoint {
public:
    bool prepared = false;
    void check() const override { if (prepared) throw OwnerDeletionCancelled(); }
};
}
TEST_F(HybridPropertyOwnerDeletionTest, DisabledOrUnconvertedOwnerDeletionDoesNotWriteOrStageResources) {
    seed(NODE, 2); const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); const std::array<EntityPropertyKey, 1> owners{{{NODE, 1}}};
    EXPECT_THROW(erase(deletion(owners, memory)), FormatException);
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
    auto tx = planner.beginTransaction(); EXPECT_THROW(tx.stageHybridPropertyOwnerDeletion(owners, memory), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, ZeroSuffixSetReadsTheOrdinaryKeyOnlyAdjacencyInBothFormats) {
    seed(EDGE, 2); const auto pin = metadata_->openSnapshot();
    FixedBTreeMap<RSS_GLOBAL_ADJACENCY_KEY_BYTES, 0> set(RSS_GLOBAL_ADJACENCY_INDEX_ID, *btree_pages_, *metadata_);
    const auto key = [](GlobalAdjacencyDirection direction, uint64_t node, uint64_t edge) {
        auto result = AdjacencyKeyCodec::lowerBoundFixed(direction, node);
        for (unsigned i = 0; i < 8; ++i) result[9 + i] = std::byte((edge >> (56U - i * 8U)) & 255U);
        return result;
    };
    EXPECT_TRUE(set.find(pin, key(GlobalAdjacencyDirection::GAD_OUT, 1, 1)));
    EXPECT_TRUE(set.find(pin, key(GlobalAdjacencyDirection::GAD_IN, 2, 1)));
    EXPECT_FALSE(set.find(pin, key(GlobalAdjacencyDirection::GAD_OUT, 2, 1)));
    size_t records = 0; EXPECT_TRUE(set.visitAll(pin, 5, [&](const auto &) { ++records; return true; })); EXPECT_EQ(records, 4U);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyOwnerDeletionTest, NodeRemovalPublishesEntityMapLivenessAndFieldDemotionTogether) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto node = entities_->readNode(old, 1); const auto original = properties_->readEntity(old, {NODE, 1});
    const auto unaffected = properties_->readEntity(old, {NODE, 2}); const std::array<EntityPropertyKey, 1> owners{{{NODE, 1}}};
    ASSERT_NO_THROW(erase(deletion(owners, memory))); const auto pin = metadata_->openSnapshot();
    EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 1)); EXPECT_THROW((void)entities_->readNode(pin, 1), FormatException);
    EXPECT_FALSE(properties_->readEntityOptional(pin, {NODE, 1})); EXPECT_EQ(entities_->readNode(old, 1), node);
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original); EXPECT_EQ(properties_->readEntity(pin, {NODE, 2}), unaffected);
    EXPECT_NO_THROW((void)entities_->readNode(pin, 49)); EXPECT_FALSE(properties_->readEntityOptional(pin, {NODE, 49}));
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 94U); EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.extensions, 94U);
    EXPECT_EQ(std::popcount(context.groupRecord({NODE, 2})->live), 48);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion)); const auto row = metadata_->openSnapshot();
    EXPECT_FALSE(metadata_->findNodePageLocationOptional(row, 1)); EXPECT_FALSE(properties_->readEntityOptional(row, {NODE, 1}));
    EXPECT_EQ(properties_->readEntity(row, {NODE, 2}), unaffected); EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, LastOwnersEraseTheNamespaceAndResumeOrdinaryWritesWithoutIdReuse) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    auto old = metadata_->openSnapshot(); const auto original = properties_->readEntity(old, {NODE, 1});
    std::array<EntityPropertyKey, 49> owners; for (size_t i = 0; i < owners.size(); ++i) owners[i] = {NODE, i + 1};
    ASSERT_NO_THROW(erase(deletion(owners, memory))); auto empty = metadata_->openSnapshot();
    for (const auto owner : owners) EXPECT_FALSE(metadata_->findNodePageLocationOptional(empty, owner.entity_id));
    const auto descriptor = MetadataPageFile(io_, empty.superblock()).readHybridPropertyDescriptor(empty.superblock().hybrid_property_descriptor_root, empty.superblock()).descriptor;
    for (const auto &root : descriptor.roots) EXPECT_FALSE(root.page_id.isValid());
    for (size_t role = 0; role < 4; ++role) EXPECT_FALSE(metadata_->findIndexRootOptional(empty, hybridPropertyIndexId(static_cast<HybridPropertyRootOrdinal>(role))));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    empty.release();
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
    auto tx = planner.beginTransaction(); const auto created = tx.createNode({}); ASSERT_TRUE(tx.commit().committed()); EXPECT_GT(created, 49U);
    EXPECT_NO_THROW((void)entities_->readNode(metadata_->openSnapshot(), created));
    std::vector<CatalogSegmentRetirementRequest> retired;
    metadata_->visitRetireQueueEntries(metadata_->openSnapshot(), [&](const auto &entry) {
        const auto pool = metadata_->findCatalog(metadata_->openSnapshot(), entry.segment_id); retired.push_back({pool.segment_id, pool.generation});
    }); ASSERT_FALSE(retired.empty());
    auto state = metadata_->openSnapshot().superblock();
    EXPECT_FALSE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    old.release(); ASSERT_TRUE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyOwnerDeletionTest, Maximum64Owner4096FieldDeletionUsesTheOriginalOneMiBAccount) {
    seed(NODE, 64, 64); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); std::array<EntityPropertyKey, 64> owners;
    for (size_t i = 0; i < owners.size(); ++i) owners[i] = {NODE, i + 1};
    ASSERT_NO_THROW(erase(deletion(owners, memory))); EXPECT_EQ(memory->used(), 0U);
    const auto pin = metadata_->openSnapshot();
    for (const auto owner : owners) EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, owner.entity_id));
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.groups, 0U); EXPECT_EQ(report.memberships, 0U);
    const auto previous = properties_->readEntity(old, {NODE, 64}); ASSERT_EQ(previous.entries.size(), 64U);
    EXPECT_EQ(previous.entries.back().value, PropertyValue(int64_t(127)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, NativeTransactionCopiesOwnerKeysMovesAndKeepsEarlyCancellationRetryable) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
    std::array<EntityPropertyKey, 1> owners{{{NODE, 1}}}; CancelOwnerDeletion cancellation;
    auto tx = planner.beginTransaction(); tx.stageHybridPropertyOwnerDeletion(owners, memory, &cancellation);
    EXPECT_GT(memory->used(), 0U); owners[0].entity_id = 2;
    EXPECT_THROW((void)tx.createNode({}), FormatException); auto moved = std::move(tx);
    cancellation.prepared = true; EXPECT_THROW((void)moved.commit(), OwnerDeletionCancelled); EXPECT_GT(memory->used(), 0U);
    cancellation.prepared = false; ASSERT_TRUE(moved.commit().committed()); EXPECT_EQ(memory->used(), 0U);
    const auto pin = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 1));
    EXPECT_NO_THROW((void)entities_->readNode(pin, 2));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyOwnerDeletionTest, OwnerDeletionConflictAndRollbackReleaseTheOwnedKeys) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
    const std::array<EntityPropertyKey, 1> owners{{{NODE, 1}}};
    { auto tx = planner.beginTransaction(); tx.stageHybridPropertyOwnerDeletion(owners, memory); EXPECT_GT(memory->used(), 0U); tx.rollback(); EXPECT_EQ(memory->used(), 0U); }
    auto tx = planner.beginTransaction(); tx.stageHybridPropertyOwnerDeletion(owners, memory);
    const auto before = metadata_->openSnapshot().superblock(); const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 3}, 1, int64_t(777)}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    EXPECT_FALSE(tx.commit().committed()); EXPECT_EQ(memory->used(), 0U);
    EXPECT_NO_THROW((void)entities_->readNode(metadata_->openSnapshot(), 1));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyOwnerDeletionTest, ABRecoverySelectsOneCompleteEntityMapAndHybridFieldGeneration) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const auto original = properties_->readEntity(old, {NODE, 1});
    const std::array<EntityPropertyKey, 1> owners{{{NODE, 1}}}; const auto write = deletion(owners, memory);
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions publication; publication.bootstrap_failpoint = failpoint;
        EXPECT_THROW(erase(write, publication), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *recovered); const auto pin = recovered->openSnapshot();
        const bool installed = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(bool(recovered->findNodePageLocationOptional(pin, 1)), !installed); EXPECT_EQ(bool(roots.readEntityOptional(pin, {NODE, 1})), !installed);
        EXPECT_EQ(pin.superblock().generation.value(), before.generation.value() + size_t(installed));
        HybridPropertyReadContext context(io_, pages, *recovered, pin); const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
        EXPECT_EQ(report.memberships, installed ? 94U : 96U); EXPECT_EQ(report.columns, installed ? 0U : 2U);
        EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyOwnerDeletionTest, QuotaAndPrePostWriteCancellationRefundWithoutDeletingHalfAnOwner) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const auto original = properties_->readEntity(old, {NODE, 1});
    const std::array<EntityPropertyKey, 1> owners{{{NODE, 1}}}; auto small = std::make_shared<MaterializedByteBudget>(64 * 1024);
    const auto epoch = io_->dataMutationEpoch(); EXPECT_THROW(erase(deletion(owners, small)), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(small->used(), 0U); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    for (const auto boundary : {std::string_view("native_graph.hybrid.private_map_images"), std::string_view("native_graph.hybrid.pool_frames_prepared")}) {
        CancelOwnerDeletion cancellation; struct State { CancelOwnerDeletion *cancellation; std::string_view boundary; } state{&cancellation, boundary};
        const auto epoch = io_->dataMutationEpoch();
        {
            graph::debug::PerfTrace::ScopedObserver observe(&state, [](void *owner, std::string_view key, int64_t, bool) noexcept {
                auto &state = *static_cast<State *>(owner); if (key == state.boundary) state.cancellation->prepared = true;
            });
            EXPECT_THROW(erase(deletion(owners, memory, &cancellation)), OwnerDeletionCancelled);
        }
        EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(memory->used(), 0U);
        EXPECT_NO_THROW((void)entities_->readNode(metadata_->openSnapshot(), 1)); EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
        if (boundary == "native_graph.hybrid.private_map_images") EXPECT_EQ(io_->dataMutationEpoch(), epoch); else EXPECT_GT(io_->dataMutationEpoch(), epoch);
    }
    ASSERT_NO_THROW(erase(deletion(owners, memory))); EXPECT_FALSE(metadata_->findNodePageLocationOptional(metadata_->openSnapshot(), 1));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, InlineLabeledOwnerRemovalRetainsTheOtherOwnerAndOldPin) {
    seed(NODE, 2); NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
    auto tx = planner.beginTransaction(); const std::array<uint64_t, 1> label{7}; tx.replaceNodeLabels(1, label); ASSERT_TRUE(tx.commit().committed());
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto other = properties_->readEntity(old, {NODE, 2});
    const std::array<EntityPropertyKey, 1> owners{{{NODE, 1}}};
    ASSERT_NO_THROW(erase(deletion(owners, memory)));
    EXPECT_FALSE(metadata_->findNodePageLocationOptional(metadata_->openSnapshot(), 1));
    EXPECT_EQ(entities_->readNode(old, 1).inline_label_count, 1U);
    EXPECT_EQ(labels_->readNodeLabels(old, entities_->readNode(old, 1)), std::vector<uint64_t>{7});
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 2}), other);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, EdgeRemovalPublishesBothDirectionsAndNativeFieldsTogether) {
    seed(EDGE, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto edge = entities_->readEdge(old, 1); const auto unaffected = properties_->readEntity(old, {EDGE, 2});
    const std::array<EntityPropertyKey, 1> owners{{{EDGE, 1}}};
    ASSERT_NO_THROW(erase(deletion(owners, memory)));
    const auto pin = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->findEdgePageLocationOptional(pin, 1)); EXPECT_FALSE(properties_->readEntityOptional(pin, {EDGE, 1}));
    EXPECT_EQ(entities_->readEdge(old, 1), edge); EXPECT_EQ(properties_->readEntity(pin, {EDGE, 2}), unaffected);
    GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
    EXPECT_EQ(adjacency.scan(pin, GlobalAdjacencyDirection::GAD_OUT, 1, 8).edge_ids, (std::vector<uint64_t>{2}));
    EXPECT_EQ(adjacency.scan(pin, GlobalAdjacencyDirection::GAD_IN, 2, 8).edge_ids, (std::vector<uint64_t>{2}));
    EXPECT_EQ(adjacency.scan(old, GlobalAdjacencyDirection::GAD_OUT, 1, 8).edge_ids, (std::vector<uint64_t>{1, 2}));
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin); const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 2U); EXPECT_EQ(report.extensions, 2U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion)); EXPECT_FALSE(metadata_->findEdgePageLocationOptional(metadata_->openSnapshot(), 1));
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {EDGE, 2}), unaffected);
}

TEST_F(HybridPropertyOwnerDeletionTest, ExhaustingPinnedEntityFramesRetiresAndReleasesTheirPoolWithoutLeavingTheCandidate) {
    seed(NODE, 64, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    std::vector<MetadataSnapshotPin> pins;
    for (uint64_t id = 1; id <= 33; ++id) {
        pins.push_back(metadata_->openSnapshot()); const std::array<EntityPropertyKey, 1> owners{{{NODE, id}}};
        ASSERT_NO_THROW(erase(deletion(owners, memory)));
        EXPECT_NO_THROW((void)entities_->readNode(pins.back(), id));
    }
    std::vector<CatalogSegmentRetirementRequest> retired; bool entityPool = false;
    {
        const auto pin = metadata_->openSnapshot(); EXPECT_TRUE(pin.superblock().hybrid_property_descriptor_root.isValid());
        metadata_->visitRetireQueueEntries(pin, [&](const auto &entry) {
            const auto pool = metadata_->findCatalog(pin, entry.segment_id);
            entityPool |= pool.kind == SegmentKind::SGK_ENTITY_PAGE_POOL; retired.push_back({pool.segment_id, pool.generation});
        });
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 33)); EXPECT_NO_THROW((void)entities_->readNode(pin, 64));
    }
    ASSERT_TRUE(entityPool); ASSERT_FALSE(retired.empty());
    const auto state = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    EXPECT_FALSE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_NO_THROW((void)entities_->readNode(pins.front(), 1));
    pins.clear(); ASSERT_TRUE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, PropertylessLastOwnerDropsOnlyItsGroupAndEntityRoutes) {
    seed(NODE, 1, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const std::array<EntityPropertyKey, 1> owners{{{NODE, 1}}};
    ASSERT_NO_THROW(erase(deletion(owners, memory))); const auto pin = metadata_->openSnapshot();
    EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 1));
    EXPECT_NO_THROW((void)entities_->readNode(old, 1));
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.groups, 0U); EXPECT_EQ(report.memberships, 0U); EXPECT_EQ(report.columns, 0U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, LargeValueOwnerRemovalRetainsGlobalContentAndTheOldPin) {
    LargeValueStore largeValues(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, largeValues, *metadata_);
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, roots, *btree_pages_, placement());
    auto tx = planner.beginTransaction(); const auto id = tx.createNode({});
    const PropertyCollectionRoot original{id, graph::Node::typeId, {{1, PropertyValue(std::string(8192, 'x'))}}};
    tx.replaceNodeProperty(original); ASSERT_TRUE(tx.commit().committed());
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const std::array<EntityPropertyKey, 1> owners{{{NODE, id}}}; ASSERT_NO_THROW(erase(deletion(owners, memory)));
    const auto pin = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, id));
    EXPECT_FALSE(roots.readEntityOptional(pin, {NODE, id})); EXPECT_EQ(roots.readEntity(old, {NODE, id}), original);
    EXPECT_NO_THROW((void)largeValues.manifest(pin, LargeValueId(before.next_large_value_id - 1U)));
    EXPECT_NO_THROW((void)entities_->readNode(old, id)); EXPECT_EQ(memory->used(), 0U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyOwnerDeletionTest, LastEdgeOwnersEraseAdjacencyAndKeepEndpointsForNewMonotonicEdges) {
    seed(EDGE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); std::array<EntityPropertyKey, 49> owners;
    for (size_t i = 0; i < owners.size(); ++i) owners[i] = {EDGE, i + 1};
    ASSERT_NO_THROW(erase(deletion(owners, memory))); const auto pin = metadata_->openSnapshot();
    for (const auto owner : owners) EXPECT_FALSE(metadata_->findEdgePageLocationOptional(pin, owner.entity_id));
    EXPECT_FALSE(metadata_->findIndexRootOptional(pin, RSS_GLOBAL_ADJACENCY_INDEX_ID));
    EXPECT_NO_THROW((void)entities_->readNode(pin, 1));
    EXPECT_NO_THROW((void)entities_->readNode(pin, 2));
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin); const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.groups, 0U); EXPECT_EQ(report.memberships, 0U);
    GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
    EXPECT_EQ(adjacency.scan(old, GlobalAdjacencyDirection::GAD_OUT, 1, 64).edge_ids.size(), 49U);
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
    auto tx = planner.beginTransaction(); const auto created = tx.createEdge(1, 2, 0); ASSERT_TRUE(tx.commit().committed()); EXPECT_GT(created, 49U);
    EXPECT_EQ(adjacency.scan(metadata_->openSnapshot(), GlobalAdjacencyDirection::GAD_OUT, 1, 8).edge_ids, (std::vector<uint64_t>{created}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, SelfLoopTypeZeroAndParallelEdgesKeepDistinctDirectionsAndRemainingMaster) {
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement()); auto tx = planner.beginTransaction();
    const auto a = tx.createNode({}), b = tx.createNode({}); const auto loop = tx.createEdge(a, a, 0);
    const auto parallel = tx.createEdge(a, b, 7), remaining = tx.createEdge(a, b, 7);
    for (const auto id : {loop, parallel, remaining}) tx.replaceEdgeProperty({id, graph::Edge::typeId, {{1, PropertyValue(int64_t(id * 10))}}});
    ASSERT_TRUE(tx.commit().committed()); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto master = entities_->readEdge(old, remaining);
    const std::array<EntityPropertyKey, 2> owners{{{EDGE, loop}, {EDGE, parallel}}};
    ASSERT_NO_THROW(erase(deletion(owners, memory)));
    const auto pin = metadata_->openSnapshot(); GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
    EXPECT_EQ(adjacency.scan(pin, GlobalAdjacencyDirection::GAD_OUT, a, 8).edge_ids, (std::vector<uint64_t>{remaining}));
    EXPECT_TRUE(adjacency.scan(pin, GlobalAdjacencyDirection::GAD_IN, a, 8).edge_ids.empty());
    EXPECT_EQ(adjacency.scan(pin, GlobalAdjacencyDirection::GAD_IN, b, 8).edge_ids, (std::vector<uint64_t>{remaining}));
    EXPECT_EQ(entities_->readEdge(pin, remaining), master); EXPECT_EQ(entities_->readEdge(old, loop).type_id, 0U);
    EXPECT_EQ(properties_->readEntity(pin, {EDGE, remaining}).entries[0].value, PropertyValue(int64_t(remaining * 10)));
    EXPECT_NO_THROW((void)entities_->readNode(pin, a));
    EXPECT_NO_THROW((void)entities_->readNode(pin, b));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, EdgeABRecoverySelectsMatchingMasterFieldsAndBothAdjacencyDirections) {
    seed(EDGE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const std::array<EntityPropertyKey, 1> owners{{{EDGE, 1}}}; const auto write = deletion(owners, memory);
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions publication; publication.bootstrap_failpoint = failpoint; EXPECT_THROW(erase(write, publication), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        BTreePageStore indexes(pages, *recovered); PropertyCollectionRootStore roots(pages, *recovered); const auto pin = recovered->openSnapshot();
        const bool installed = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE; GlobalAdjacencyIndex adjacency(indexes, *recovered);
        const auto outgoing = adjacency.scan(pin, GlobalAdjacencyDirection::GAD_OUT, 1, 64).edge_ids;
        const auto incoming = adjacency.scan(pin, GlobalAdjacencyDirection::GAD_IN, 2, 64).edge_ids;
        EXPECT_EQ(outgoing.size(), installed ? 48U : 49U); EXPECT_EQ(incoming, outgoing); EXPECT_EQ(outgoing.front(), installed ? 2U : 1U);
        EXPECT_EQ(bool(recovered->findEdgePageLocationOptional(pin, 1)), !installed); EXPECT_EQ(bool(roots.readEntityOptional(pin, {EDGE, 1})), !installed);
        EXPECT_EQ(pin.superblock().generation.value(), before.generation.value() + size_t(installed));
        EXPECT_NO_THROW((void)entities_->readEdge(old, 1));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyOwnerDeletionTest, EdgeCancellationNeverPublishesOnlyOneDirectionOrDropsTheMasterEarly) {
    seed(EDGE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const std::array<EntityPropertyKey, 1> owners{{{EDGE, 1}}};
    for (const auto boundary : {std::string_view("native_graph.hybrid.private_map_images"), std::string_view("native_graph.hybrid.pool_frames_prepared")}) {
        CancelOwnerDeletion cancellation; struct State { CancelOwnerDeletion *cancellation; std::string_view boundary; } state{&cancellation, boundary}; const auto epoch = io_->dataMutationEpoch();
        {
            graph::debug::PerfTrace::ScopedObserver observe(&state, [](void *owner, std::string_view key, int64_t, bool) noexcept {
                auto &state = *static_cast<State *>(owner); if (key == state.boundary) state.cancellation->prepared = true;
            }); EXPECT_THROW(erase(deletion(owners, memory, &cancellation)), OwnerDeletionCancelled);
        }
        EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(memory->used(), 0U);
        EXPECT_NO_THROW((void)entities_->readEdge(metadata_->openSnapshot(), 1));
        if (boundary == "native_graph.hybrid.private_map_images") EXPECT_EQ(io_->dataMutationEpoch(), epoch); else EXPECT_GT(io_->dataMutationEpoch(), epoch);
    }
    ASSERT_NO_THROW(erase(deletion(owners, memory))); EXPECT_FALSE(metadata_->findEdgePageLocationOptional(metadata_->openSnapshot(), 1));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, ValidChecksumMissingReverseDirectionRejectsBeforeWritingAndCanRetry) {
    seed(EDGE, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const auto root = metadata_->findIndexRoot(old, RSS_GLOBAL_ADJACENCY_INDEX_ID);
    const auto location = metadata_->findPageLocation(old, root.root_page_id); const auto pool = metadata_->findCatalog(old, location.segment_id);
    const BufferPoolPageKey key{PhysicalSegmentAllocator::checkedSlotOffset(before.data_region_offset, pool.physical_slot) + uint64_t(location.page_index) * RSS_PAGE_SIZE, pool.generation.value()};
    std::array<std::byte, RSS_PAGE_SIZE> original{}; ASSERT_EQ(io_->readAt(key, original.data(), original.size()), original.size());
    const auto owner = PageOwnerHeaderCodec::decode(std::span(original).first(RSS_PAGE_OWNER_HEADER_BYTES));
    const auto page = btree_pages_->read(old, root.root_page_id); ASSERT_EQ(page.header().kind, BTreeNodeKind::BPN_LEAF); auto entries = page.entries();
    const auto missing = AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_IN, 2, 1}); const auto wrong = AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_IN, 2, 3});
    auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto &value) { return std::ranges::equal(value.key, missing); }); ASSERT_NE(entry, entries.end());
    entry->key.assign(wrong.begin(), wrong.end()); std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.key < b.key; });
    auto header = page.header(); header.entry_count = 0;
    const auto corrupt = BTreePage::createWithEntries(owner, header, entries); ASSERT_EQ(corrupt.slottedPage().stats().contiguous_free_bytes, location.free_bytes);
    auto damaged = original; const auto bytes = corrupt.serialize(); std::copy(bytes.begin(), bytes.end(), damaged.begin() + RSS_PAGE_OWNER_HEADER_BYTES);
    io_->writeAt(key, damaged.data(), damaged.size()); const auto epoch = io_->dataMutationEpoch(); const std::array<EntityPropertyKey, 1> owners{{{EDGE, 1}}};
    try { erase(deletion(owners, memory)); FAIL() << "missing reverse direction was accepted"; }
    catch (const FormatException &error) { EXPECT_NE(std::string(error.what()).find("missing an authenticated direction"), std::string::npos); }
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
    io_->writeAt(key, original.data(), original.size());
    ASSERT_NO_THROW(erase(deletion(owners, memory)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, NodeDeletionRejectsIncidentEdgesAndAcceptsTheirLogicallyEmptyMapAfterRemoval) {
    seed(EDGE, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const std::array<EntityPropertyKey, 1> nodes{{{NODE, 1}}};
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    try { erase(deletion(nodes, memory)); FAIL() << "Node deletion accepted an incident Edge"; }
    catch (const FormatException &error) { EXPECT_NE(std::string(error.what()).find("incident"), std::string::npos); }
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    const std::array<EntityPropertyKey, 2> edges{{{EDGE, 1}, {EDGE, 2}}};
    ASSERT_NO_THROW(erase(deletion(edges, memory)));
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    ASSERT_NO_THROW(erase(deletion(nodes, memory)));
    const auto pin = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 1));
    EXPECT_NO_THROW((void)entities_->readNode(pin, 2));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, NativeEdgeDeletionStagesOwnedIdsAndKeepsTheRemainingTopology) {
    seed(EDGE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion)); const auto old = metadata_->openSnapshot();
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
    std::array<EntityPropertyKey, 1> owners{{{EDGE, 1}}}; auto tx = planner.beginTransaction(); tx.stageHybridPropertyOwnerDeletion(owners, memory); owners[0].entity_id = 2;
    ASSERT_TRUE(tx.commit().committed()); EXPECT_EQ(memory->used(), 0U); const auto pin = metadata_->openSnapshot();
    EXPECT_FALSE(metadata_->findEdgePageLocationOptional(pin, 1));
    EXPECT_NO_THROW((void)entities_->readEdge(pin, 2));
    EXPECT_NO_THROW((void)entities_->readEdge(old, 1));
    GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_); const auto outgoing = adjacency.scan(pin, GlobalAdjacencyDirection::GAD_OUT, 1, 64).edge_ids;
    EXPECT_EQ(outgoing.size(), 48U); EXPECT_EQ(outgoing.front(), 2U);
    EXPECT_EQ(adjacency.scan(pin, GlobalAdjacencyDirection::GAD_IN, 2, 64).edge_ids, outgoing);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, MaximumEdgeGroupDeletionCoordinates4096FieldsWithinOneMiB) {
    seed(EDGE, 64, 64); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion)); const auto old = metadata_->openSnapshot(); std::array<EntityPropertyKey, 64> owners;
    for (size_t i = 0; i < owners.size(); ++i) owners[i] = {EDGE, i + 1};
    ASSERT_NO_THROW(erase(deletion(owners, memory))); const auto pin = metadata_->openSnapshot();
    for (const auto owner : owners) EXPECT_FALSE(metadata_->findEdgePageLocationOptional(pin, owner.entity_id));
    EXPECT_FALSE(metadata_->findIndexRootOptional(pin, RSS_GLOBAL_ADJACENCY_INDEX_ID));
    EXPECT_NO_THROW((void)entities_->readNode(pin, 1));
    EXPECT_NO_THROW((void)entities_->readNode(pin, 2));
    EXPECT_EQ(properties_->readEntity(old, {EDGE, 64}).entries.size(), 64U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, OwnersInTwoNodeGroupsAreDeletedWithBothLiveMasksAndOldPins) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    convertGroup(NODE, 1, memory); convertGroup(NODE, 2, memory); const auto old = metadata_->openSnapshot();
    const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 65}}};
    ASSERT_NO_THROW(erase(deletion(owners, memory))); const auto current = metadata_->openSnapshot();
    for (const auto owner : owners) {
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, owner.entity_id)); EXPECT_TRUE(metadata_->findNodePageLocationOptional(old, owner.entity_id));
        EXPECT_EQ(properties_->readEntityPropertyOptional(old, owner, 1)->value, PropertyValue(int64_t(owner.entity_id)));
        const auto group = HybridPropertyReadContext(io_, *slotted_pages_, *metadata_, current).groupRecord(owner);
        ASSERT_TRUE(group); EXPECT_EQ(std::popcount(group->live), 63); EXPECT_EQ(group->memberships, 126U);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, NodeAndEdgeWithTheSameIdDeleteTheirOwnEntityAndFieldNamespaces) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedKinds(memory); const auto old = metadata_->openSnapshot();
    const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {EDGE, 1}}};
    ASSERT_NO_THROW(erase(deletion(owners, memory))); const auto current = metadata_->openSnapshot();
    EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, 1)); EXPECT_FALSE(metadata_->findEdgePageLocationOptional(current, 1));
    EXPECT_TRUE(metadata_->findNodePageLocationOptional(old, 1)); EXPECT_TRUE(metadata_->findEdgePageLocationOptional(old, 1));
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, 2}, 1)->value, PropertyValue(int64_t(2)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(current, {EDGE, 2}, 1)->value, PropertyValue(int64_t(2)));
    GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_); const auto outgoing = adjacency.scan(current, GlobalAdjacencyDirection::GAD_OUT, 65, 65).edge_ids;
    EXPECT_EQ(outgoing.size(), 63U); EXPECT_EQ(outgoing.front(), 2U);
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, 1}, 1)->value, PropertyValue(int64_t(1)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {EDGE, 1}, 1)->value, PropertyValue(int64_t(1)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, CrossGroupDemotionThenLastOwnerDeletionRemovesEveryNativeDirectory) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    convertGroup(NODE, 1, memory); convertGroup(NODE, 2, memory); const auto old = metadata_->openSnapshot();
    std::array<EntityPropertyKey, 64> owners;
    for (size_t index = 0; index < owners.size(); ++index) owners[index] = {NODE, index < 32 ? index + 1U : index + 33U};
    ASSERT_NO_THROW(erase(deletion(owners, memory))); const auto middle = metadata_->openSnapshot();
    EXPECT_EQ(HybridPropertyReadContext(io_, *slotted_pages_, *metadata_, middle).groupRecord({NODE, 33})->columns, 0U);
    EXPECT_EQ(HybridPropertyReadContext(io_, *slotted_pages_, *metadata_, middle).groupRecord({NODE, 97})->columns, 0U);
    for (size_t index = 0; index < owners.size(); ++index) owners[index] = {NODE, index < 32 ? index + 33U : index + 65U};
    ASSERT_NO_THROW(erase(deletion(owners, memory))); const auto empty = metadata_->openSnapshot();
    EXPECT_FALSE(HybridPropertyReadContext(io_, *slotted_pages_, *metadata_, empty).hasNativeGroups());
    for (size_t role = 0; role < 4; ++role) EXPECT_FALSE(metadata_->findIndexRootOptional(empty, hybridPropertyIndexId(static_cast<HybridPropertyRootOrdinal>(role))));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {NODE, 1}, 1)->value, PropertyValue(int64_t(1)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(middle, {NODE, 33}, 1)->value, PropertyValue(int64_t(33)));
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
    auto tx = writer.beginTransaction(); const auto id = tx.createNode({}); tx.replaceNodeProperty({id, graph::Node::typeId, {{1, PropertyValue(int64_t(777))}}});
    ASSERT_TRUE(tx.commit().committed()); EXPECT_GT(id, 128U);
    EXPECT_EQ(properties_->readEntityPropertyOptional(metadata_->openSnapshot(), {NODE, id}, 1)->value, PropertyValue(int64_t(777)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, MixedKindDeletionRecoversAllEntityAndFieldRootsAcrossBothHeaders) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedKinds(memory); const auto old = metadata_->openSnapshot();
    const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {EDGE, 1}}};
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions options; options.bootstrap_failpoint = point;
        EXPECT_THROW(erase(deletion(owners, memory), options), BootstrapPublishInterrupted);
        auto reopened = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *reopened); const auto source = reopened->openSnapshot(); const bool complete = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(bool(reopened->findNodePageLocationOptional(source, 1)), !complete); EXPECT_EQ(bool(reopened->findEdgePageLocationOptional(source, 1)), !complete);
        EXPECT_EQ(bool(roots.readEntityPropertyOptional(source, {NODE, 1}, 1)), !complete); EXPECT_EQ(bool(roots.readEntityPropertyOptional(source, {EDGE, 1}, 1)), !complete);
        EXPECT_TRUE(metadata_->findNodePageLocationOptional(old, 1)); EXPECT_TRUE(metadata_->findEdgePageLocationOptional(old, 1));
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyOwnerDeletionTest, IncidentNodeDeletionRequiresEveryParallelEdgeInItsAtomicBatch) {
    seed(EDGE, 2, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    convertGroup(NODE, 1, memory); convertGroup(EDGE, 1, memory); const auto old = metadata_->openSnapshot();
    const std::array<EntityPropertyKey, 2> incomplete{{{NODE, 1}, {EDGE, 1}}};
    const auto before = old.superblock(); const auto epoch = io_->dataMutationEpoch();
    try { erase(deletion(incomplete, memory)); FAIL() << "remaining incident Edge was ignored"; }
    catch (const FormatException &error) { EXPECT_NE(std::string(error.what()).find("incident"), std::string::npos); }
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    const std::array<EntityPropertyKey, 3> complete{{{NODE, 1}, {EDGE, 1}, {EDGE, 2}}};
    ASSERT_NO_THROW(erase(deletion(complete, memory))); const auto current = metadata_->openSnapshot();
    EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, 1)); EXPECT_TRUE(metadata_->findNodePageLocationOptional(current, 2));
    EXPECT_FALSE(metadata_->findEdgePageLocationOptional(current, 1)); EXPECT_FALSE(metadata_->findEdgePageLocationOptional(current, 2));
    EXPECT_FALSE(metadata_->findIndexRootOptional(current, RSS_GLOBAL_ADJACENCY_INDEX_ID));
    EXPECT_TRUE(metadata_->findNodePageLocationOptional(old, 1)); EXPECT_EQ(properties_->readEntity(old, {EDGE, 1}).entries.size(), 2U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerDeletionTest, AtomicNodeDeletionHandlesSelfLoopsAndUntypedIncidentEdges) {
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
    auto tx = writer.beginTransaction(); const auto first = tx.createNode({}); const auto second = tx.createNode({});
    const auto loop = tx.createEdge(first, first, 7); const auto outgoing = tx.createEdge(first, second, 0);
    tx.replaceEdgeProperty({loop, graph::Edge::typeId, {{1, PropertyValue(int64_t(42))}}});
    tx.replaceEdgeProperty({outgoing, graph::Edge::typeId, {{1, PropertyValue(int64_t(43))}}}); ASSERT_TRUE(tx.commit().committed());
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); convertGroup(NODE, 1, memory); convertGroup(EDGE, 1, memory);
    const auto old = metadata_->openSnapshot(); const std::array<EntityPropertyKey, 3> owners{{{NODE, first}, {EDGE, loop}, {EDGE, outgoing}}};
    ASSERT_NO_THROW(erase(deletion(owners, memory))); const auto current = metadata_->openSnapshot();
    EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, first)); EXPECT_TRUE(metadata_->findNodePageLocationOptional(current, second));
    EXPECT_FALSE(metadata_->findEdgePageLocationOptional(current, loop)); EXPECT_FALSE(metadata_->findEdgePageLocationOptional(current, outgoing));
    EXPECT_FALSE(metadata_->findIndexRootOptional(current, RSS_GLOBAL_ADJACENCY_INDEX_ID));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {EDGE, loop}, 1)->value, PropertyValue(int64_t(42)));
    EXPECT_EQ(properties_->readEntityPropertyOptional(old, {EDGE, outgoing}, 1)->value, PropertyValue(int64_t(43)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
#endif
