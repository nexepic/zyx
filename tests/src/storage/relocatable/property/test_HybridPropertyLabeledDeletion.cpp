#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
using namespace graph::storage;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyLabeledDeletionTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    const std::vector<uint64_t> first{1, 3}, second{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, third{2, 4, 6, 8, 10, 12, 14, 16};
    NativeGraphWritePlannerOptions placement() {
        NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
        options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903}; return options;
    }
    void seedLabels() {
        seed(NODE, 3, 2);
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
        auto firstTx = planner.beginTransaction(); firstTx.replaceNodeLabels(1, first); firstTx.replaceNodeLabels(2, second);
        ASSERT_TRUE(firstTx.commit().committed());
        auto next = planner.beginTransaction(); next.replaceNodeLabels(3, third); ASSERT_TRUE(next.commit().committed());
        auto pin = metadata_->openSnapshot();
        ASSERT_EQ(entities_->readNode(pin, 2).label_overflow.page_id, entities_->readNode(pin, 3).label_overflow.page_id);
    }
    void convert(const std::shared_ptr<MaterializedByteBudget> &memory) {
        auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; run(conversion);
    }
    void erase(std::span<const EntityPropertyKey> owners, const std::shared_ptr<MaterializedByteBudget> &memory,
        const MetadataPublishOptions &options = {}, const CancellationCheckpoint *cancellation = nullptr) {
        const auto source = metadata_->openSnapshot().superblock();
        NativeHybridPropertyColumnWrite write{source.database_id, source.generation, source.hybrid_property_descriptor_root,
            {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, cancellation, true, owners};
        (void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(source.checkpoint_lsn.value() + 1), options);
    }
    std::vector<uint64_t> readLabels(const MetadataSnapshotPin &pin, uint64_t id) {
        return labels_->readNodeLabels(pin, entities_->readNode(pin, id));
    }
};
}
TEST_F(HybridPropertyLabeledDeletionTest, UnconvertedOwnerDeletionRetainsLabelsAndAllRoutes) {
    seedLabels(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    const std::array<EntityPropertyKey, 1> owners{{{NODE, 2}}}; EXPECT_THROW(erase(owners, memory), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(readLabels(metadata_->openSnapshot(), 2), second); EXPECT_EQ(memory->used(), 0U);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyLabeledDeletionTest, ErasingOneOverflowOwnerCopiesItsLabelFrameAndPreservesTheSharedNeighborSlot) {
    seedLabels(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    auto old = metadata_->openSnapshot(); const auto removed = entities_->readNode(old, 2).label_overflow;
    const auto neighbor = entities_->readNode(old, 3).label_overflow;
    const auto source = metadata_->findPageLocation(old, removed.page_id);
    const std::array<EntityPropertyKey, 1> owners{{{NODE, 2}}}; ASSERT_NO_THROW(erase(owners, memory));
    auto pin = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 2));
    EXPECT_EQ(entities_->readNode(pin, 3).label_overflow, neighbor); EXPECT_EQ(readLabels(pin, 3), third); EXPECT_EQ(readLabels(pin, 1), first);
    const auto moved = metadata_->findPageLocation(pin, neighbor.page_id);
    EXPECT_TRUE(moved.segment_id != source.segment_id || moved.page_index != source.page_index || moved.generation != source.generation);
    EXPECT_THROW((void)labels_->read(pin, removed), FormatException);
    EXPECT_EQ(readLabels(old, 2), second); EXPECT_EQ(readLabels(old, 3), third);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLabeledDeletionTest, TwoRemovalsCoalesceTheSharedLabelPageAndRemoveItsEmptyRoute) {
    seedLabels(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    auto old = metadata_->openSnapshot(); const auto page = entities_->readNode(old, 2).label_overflow.page_id;
    const std::array<EntityPropertyKey, 2> owners{{{NODE, 2}, {NODE, 3}}}; ASSERT_NO_THROW(erase(owners, memory));
    auto pin = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->findPageLocationOptional(pin, page));
    EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 2)); EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 3));
    EXPECT_EQ(readLabels(pin, 1), first); EXPECT_EQ(readLabels(old, 2), second); EXPECT_EQ(readLabels(old, 3), third);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLabeledDeletionTest, OldPinsProtectDetachedEntityAndLabelPoolsUntilEveryOwnerIsRemoved) {
    seedLabels(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    auto old = metadata_->openSnapshot(); const auto page = entities_->readNode(old, 2).label_overflow.page_id;
    const std::array<EntityPropertyKey, 3> owners{{{NODE, 1}, {NODE, 2}, {NODE, 3}}}; ASSERT_NO_THROW(erase(owners, memory));
    EXPECT_FALSE(metadata_->findPageLocationOptional(metadata_->openSnapshot(), page));
    EXPECT_EQ(readLabels(old, 2), second); EXPECT_EQ(readLabels(old, 3), third);
    std::vector<CatalogSegmentRetirementRequest> requests;
    metadata_->visitRetireQueueEntries(metadata_->openSnapshot(), [&](const auto &entry) {
        const auto pool = metadata_->findCatalog(metadata_->openSnapshot(), entry.segment_id); requests.push_back({pool.segment_id, pool.generation});
    }); ASSERT_FALSE(requests.empty());
    const auto state = metadata_->openSnapshot().superblock();
    EXPECT_FALSE(metadata_->tryReleaseRetiredCatalogSegments(requests, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    old.release(); ASSERT_TRUE(metadata_->tryReleaseRetiredCatalogSegments(requests, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLabeledDeletionTest, QuotaAndPrivateCancellationRefundWithoutPublishingHalfALabelDeletion) {
    seedLabels(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    const std::array<EntityPropertyKey, 1> owners{{{NODE, 2}}}; auto tiny = std::make_shared<MaterializedByteBudget>(128);
    EXPECT_THROW(erase(owners, tiny), graph::query::execution::MaterializedByteLimitExceeded); EXPECT_EQ(tiny->used(), 0U);
    struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("cancel labeled owner deletion") {} };
    struct Cancellation final : CancellationCheckpoint { bool prepared = false; void check() const override { if (prepared) throw Cancelled(); } } cancellation;
    {
        graph::debug::PerfTrace::ScopedObserver observer(&cancellation, [](void *owner, std::string_view key, int64_t count, bool) noexcept {
            if (key == "native_graph.hybrid.private_map_images" && count > 0) static_cast<Cancellation *>(owner)->prepared = true;
        });
        EXPECT_THROW(erase(owners, memory, {}, &cancellation), Cancelled);
    }
    EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(readLabels(metadata_->openSnapshot(), 2), second); EXPECT_EQ(readLabels(metadata_->openSnapshot(), 3), third);
    ASSERT_NO_THROW(erase(owners, memory)); EXPECT_EQ(readLabels(metadata_->openSnapshot(), 3), third);
}
TEST_F(HybridPropertyLabeledDeletionTest, ABRecoverySelectsMatchingNodeLabelAndNativeFieldRoots) {
    seedLabels(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    auto old = metadata_->openSnapshot(); const std::array<EntityPropertyKey, 1> owners{{{NODE, 2}}};
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions interruption; interruption.bootstrap_failpoint = point;
        EXPECT_THROW(erase(owners, memory, interruption), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); auto pin = recovered->openSnapshot();
        SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        EntityLabelSetStore labels(io_, *recovered, pages, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        EntityPageStore entities(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        const bool installed = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(bool(recovered->findNodePageLocationOptional(pin, 2)), !installed);
        if (!installed) EXPECT_EQ(labels.readNodeLabels(pin, entities.readNode(pin, 2)), second);
        EXPECT_EQ(labels.readNodeLabels(pin, entities.readNode(pin, 3)), third);
        EXPECT_EQ(readLabels(old, 2), second); EXPECT_EQ(readLabels(old, 3), third);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
#endif
