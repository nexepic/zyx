#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyFieldRelocationTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    std::vector<PageLocation> fieldPages(const MetadataSnapshotPin &pin, std::optional<SegmentId> segment = {}) {
        std::vector<PageLocation> result;
        static_cast<void>(metadata_->visitPageLocationsUntil(pin, [&](const auto &location) {
            const auto pool = metadata_->findCatalog(pin, location.segment_id);
            if (pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && pool.owner_id == 903 && (!segment || pool.segment_id == *segment)) result.push_back(location);
            return true;
        })); return result;
    }
    NativeHybridPropertyPoolRelocation relocation(const MetadataSnapshotPin &pin, SegmentId segment,
        std::shared_ptr<std::pmr::memory_resource> memory) {
        const auto &state = pin.superblock();
        return {state.database_id, state.generation, state.hybrid_property_descriptor_root, segment,
            PhysicalSlotId(40), PhysicalSlotId(80), std::move(memory)};
    }
};
class RelocationCancelled final : public std::runtime_error { public: RelocationCancelled() : std::runtime_error("field relocation cancelled") {} };
class CancelRelocation final : public CancellationCheckpoint {
public:
    bool prepared = false;
    void check() const override { if (prepared) throw RelocationCancelled(); }
};
}
TEST_F(HybridPropertyFieldRelocationTest, RejectsDisabledOrUnconvertedSourcesBeforeAnyWrite) {
    seed(NODE, 2); const auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    const auto epoch = io_->dataMutationEpoch(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto entity = metadata_->findNodePageLocation(pin, 1);
    auto move = relocation(pin, metadata_->findPageLocation(pin, entity.page_id).segment_id, memory);
    EXPECT_THROW((void)metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_THROW((void)metadata_->relocateHybridPropertyIndexPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_THROW((void)metadata_->relocateHybridPropertyEntityPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyFieldRelocationTest, MovesEveryCurrentFieldPageWithoutChangingLogicalRootsOrOldPins) {
    seed(NODE, 64, 16); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const auto pages = fieldPages(old);
    ASSERT_GE(pages.size(), 3U); const auto pool = metadata_->findCatalog(old, pages[0].segment_id);
    const auto original = properties_->readEntity(old, {NODE, 1}); auto move = relocation(old, pool.segment_id, memory);
    ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root, before.hybrid_property_descriptor_root);
    EXPECT_EQ(metadata_->findCatalog(pin, pool.segment_id).state, SegmentState::SGS_RETIRED);
    for (const auto &previous : pages) {
        const auto current = metadata_->findPageLocation(pin, previous.page_id);
        EXPECT_EQ(current.page_id, previous.page_id); EXPECT_NE(current.segment_id, previous.segment_id);
        EXPECT_EQ(metadata_->findPageLocation(old, previous.page_id), previous);
    }
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}), original); EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 1024U); EXPECT_EQ(report.columns, 16U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    auto reopened = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool reopenedPages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore roots(reopenedPages, *reopened);
    EXPECT_EQ(roots.readEntity(reopened->openSnapshot(), {NODE, 1}), original);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan());
}
TEST_F(HybridPropertyFieldRelocationTest, CopiesCurrentFramesOnlyAfterAColumnUpdateFragmentsTheSourcePool) {
    seed(NODE, 64, 16); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); auto before = old.superblock(); const auto oldPages = fieldPages(old);
    ASSERT_GE(oldPages.size(), 3U); const auto source = oldPages[0].segment_id;
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, int64_t(999)}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto updated = metadata_->openSnapshot(); before = updated.superblock(); const auto remaining = fieldPages(updated, source);
    ASSERT_FALSE(remaining.empty()); ASSERT_LE(remaining.size(), oldPages.size());
    EXPECT_TRUE(std::any_of(oldPages.begin(), oldPages.end(), [&](const auto &page) {
        return metadata_->findPageLocation(updated, page.page_id) != page;
    }));
    const auto allUpdated = fieldPages(updated); auto move = relocation(updated, source, memory); int64_t copied = -1;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&copied, [](void *state, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.relocation_private_field_frames") *static_cast<int64_t *>(state) = value;
        });
        ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    }
    EXPECT_EQ(copied, int64_t(remaining.size())); const auto pin = metadata_->openSnapshot();
    for (const auto &location : allUpdated) {
        const auto current = metadata_->findPageLocation(pin, location.page_id);
        if (location.segment_id == source) EXPECT_NE(current.segment_id, source); else EXPECT_EQ(current, location);
    }
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}).entries[0].value, PropertyValue(int64_t(999)));
    EXPECT_EQ(properties_->readEntity(updated, {NODE, 1}).entries[0].value, PropertyValue(int64_t(999)));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}).entries[0].value, PropertyValue(int64_t(1)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyFieldRelocationTest, ABRecoverySelectsACompletePhysicalRouteAndRetirementGeneration) {
    seed(EDGE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const auto pages = fieldPages(old); ASSERT_FALSE(pages.empty());
    const auto pool = metadata_->findCatalog(old, pages[0].segment_id); const auto original = properties_->readEntity(old, {EDGE, 1});
    const auto edge = entities_->readEdge(old, 1); auto move = relocation(old, pool.segment_id, memory);
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions publication; publication.bootstrap_failpoint = failpoint;
        EXPECT_THROW((void)metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1), publication), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool readPages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(readPages, *recovered); EntityPageStore entities(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        const auto pin = recovered->openSnapshot(); const bool installed = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(pin.superblock().generation.value(), before.generation.value() + size_t(installed));
        EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root, before.hybrid_property_descriptor_root);
        EXPECT_EQ(roots.readEntity(pin, {EDGE, 1}), original); EXPECT_EQ(entities.readEdge(pin, 1), edge);
        EXPECT_EQ(recovered->findCatalog(pin, pool.segment_id).state, installed ? SegmentState::SGS_RETIRED : SegmentState::SGS_SEALED);
        for (const auto &location : pages) EXPECT_EQ(recovered->findPageLocation(pin, location.page_id).segment_id == pool.segment_id, !installed);
        EXPECT_EQ(properties_->readEntity(old, {EDGE, 1}), original);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyFieldRelocationTest, OldPinBlocksReleaseAndFreshLogicalPoolIdentityProtectsPhysicalReuse) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    auto old = metadata_->openSnapshot(); auto before = old.superblock(); const auto pages = fieldPages(old); ASSERT_FALSE(pages.empty());
    const auto pool = metadata_->findCatalog(old, pages[0].segment_id); auto move = relocation(old, pool.segment_id, memory);
    ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const std::array<CatalogSegmentRetirementRequest, 1> retired{{{pool.segment_id, pool.generation}}};
    before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    EXPECT_FALSE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(properties_->readEntity(old, {NODE, 1}).entries[0].value, PropertyValue(int64_t(1)));
    old.release(); ASSERT_TRUE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); const auto currentPool = metadata_->findPageLocation(pin, pages[0].page_id).segment_id;
    move = relocation(pin, currentPool, memory); move.first_slot = move.last_slot = pool.physical_slot;
    ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1)));
    const auto final = metadata_->openSnapshot(); const auto location = metadata_->findPageLocation(final, pages[0].page_id);
    const auto reused = metadata_->findCatalog(final, location.segment_id);
    EXPECT_EQ(reused.physical_slot, pool.physical_slot); EXPECT_NE(reused.segment_id, pool.segment_id);
    const BufferPoolPageKey reusedKey{PhysicalSegmentAllocator::checkedSlotOffset(final.superblock().data_region_offset, reused.physical_slot), reused.generation.value()};
    std::array<std::byte, RSS_PAGE_SIZE> envelope{};
    ASSERT_EQ(io_->readAt(reusedKey, envelope.data(), envelope.size()), envelope.size());
    EXPECT_EQ(SegmentEnvelopeCodec::decode(std::span(envelope).first(RSS_ENVELOPE_BYTES)).segment_id, reused.segment_id);
    EXPECT_NE(SegmentEnvelopeCodec::decode(std::span(envelope).first(RSS_ENVELOPE_BYTES)).segment_id, pool.segment_id);
    EXPECT_EQ(properties_->readEntity(final, {NODE, 1}), properties_->readEntity(pin, {NODE, 1}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyFieldRelocationTest, QuotaCancellationAndStaleFencesRefundBeforeRetry) {
    seed(NODE, 64, 16); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const auto pages = fieldPages(old); ASSERT_FALSE(pages.empty());
    const auto pool = metadata_->findCatalog(old, pages[0].segment_id); auto move = relocation(old, pool.segment_id, memory);
    const auto epoch = io_->dataMutationEpoch(); auto small = std::make_shared<MaterializedByteBudget>(1024); move.memory = small;
    EXPECT_THROW((void)metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(small->used(), 0U); EXPECT_EQ(io_->dataMutationEpoch(), epoch); move.memory = memory;
    move.expected_generation = Generation(before.generation.value() + 1);
    EXPECT_THROW((void)metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); move.expected_generation = before.generation;
    move.first_slot = move.last_slot = pool.physical_slot;
    EXPECT_THROW((void)metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
    move.first_slot = PhysicalSlotId(40); move.last_slot = PhysicalSlotId(80);
    for (const auto boundary : {std::string_view("native_graph.hybrid.relocation_private_field_frames"), std::string_view("native_graph.write.new_pool_prepare_calls")}) {
        CancelRelocation cancellation; move.cancellation = &cancellation;
        struct State { CancelRelocation *cancellation; std::string_view boundary; } state{&cancellation, boundary}; const auto epoch = io_->dataMutationEpoch();
        {
            graph::debug::PerfTrace::ScopedObserver observe(&state, [](void *owner, std::string_view key, int64_t, bool) noexcept {
                auto &state = *static_cast<State *>(owner); if (key == state.boundary) state.cancellation->prepared = true;
            });
            EXPECT_THROW((void)metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), RelocationCancelled);
        }
        EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
        if (boundary == "native_graph.hybrid.relocation_private_field_frames") EXPECT_EQ(io_->dataMutationEpoch(), epoch); else EXPECT_GT(io_->dataMutationEpoch(), epoch);
    }
    move.cancellation = nullptr;
    ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}), properties_->readEntity(old, {NODE, 1}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyFieldRelocationTest, DamagedSourcePoolRefusesBeforeWritingAndCanRetryAfterRestoration) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const auto pages = fieldPages(old); ASSERT_FALSE(pages.empty());
    const auto pool = metadata_->findCatalog(old, pages[0].segment_id); auto move = relocation(old, pool.segment_id, memory);
    const BufferPoolPageKey key{PhysicalSegmentAllocator::checkedSlotOffset(before.data_region_offset, pool.physical_slot), pool.generation.value()};
    std::array<std::byte, RSS_PAGE_SIZE> original{}; ASSERT_EQ(io_->readAt(key, original.data(), original.size()), original.size());
    auto damaged = original; damaged[0] ^= std::byte{1}; io_->writeAt(key, damaged.data(), damaged.size()); const auto epoch = io_->dataMutationEpoch();
    EXPECT_THROW((void)metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
    io_->writeAt(key, original.data(), original.size());
    ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyFieldRelocationTest, DirectoryRelocationSupportsMixedPhysicalEpochWritesAndInverse) {
    seed(NODE, 64, 16); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto index = hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_OWNER);
    const auto root = metadata_->findIndexRoot(old, index); const auto pool = metadata_->findCatalog(old, metadata_->findPageLocation(old, root.root_page_id).segment_id);
    auto move = relocation(old, pool.segment_id, memory);
    ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyIndexPool(move, LogSequenceNumber(old.superblock().checkpoint_lsn.value() + 1)));
    const auto moved = metadata_->openSnapshot(); const auto rebound = metadata_->findIndexRoot(moved, index);
    EXPECT_EQ(rebound.root_page_id, root.root_page_id); EXPECT_EQ(rebound.root_generation.value(), pool.generation.value() + 1);
    const auto movedPool = metadata_->findPageLocation(moved, root.root_page_id).segment_id;
    const auto original = properties_->readEntity(old, {NODE, 1});
    for (const auto [id, token] : {std::pair<uint64_t, uint32_t>{1, 1}}) {
        const auto before = metadata_->openSnapshot().superblock();
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, id}, token, std::monostate{}, true}}};
        NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
        ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    }
    const auto mixed = metadata_->openSnapshot(); EXPECT_EQ(metadata_->findIndexRoot(mixed, index).root_generation, rebound.root_generation);
    EXPECT_EQ(metadata_->findCatalog(mixed, movedPool).state, SegmentState::SGS_SEALED);
    const auto descriptor = mixed.superblock().hybrid_property_descriptor_root;
    move = relocation(mixed, movedPool, memory);
    ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyIndexPool(move, LogSequenceNumber(mixed.superblock().checkpoint_lsn.value() + 1)));
    const auto nonrootMoved = metadata_->openSnapshot();
    const auto afterRoot = metadata_->findIndexRoot(nonrootMoved, index);
    EXPECT_EQ(afterRoot.root_page_id, rebound.root_page_id); EXPECT_EQ(afterRoot.root_generation.value(), rebound.root_generation.value() + 1U);
    EXPECT_NE(nonrootMoved.superblock().hybrid_property_descriptor_root, descriptor);
    for (const auto [id, token] : {std::pair<uint64_t, uint32_t>{64, 16}, {32, 8}}) {
        const auto before = metadata_->openSnapshot().superblock();
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, id}, token, std::monostate{}, true}}};
        NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
        ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    }
    const auto pin = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 1021U); EXPECT_EQ(report.columns, 16U); EXPECT_EQ(report.extensions, 0U);
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original); EXPECT_EQ(properties_->readEntity(moved, {NODE, 1}), original);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion)); EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries.size(), 15U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyFieldRelocationTest, DirectoryABRecoveryBindsTheDescriptorAndRootPhysicalGenerationTogether) {
    seed(EDGE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto index = hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_EDGE_OWNER); const auto root = metadata_->findIndexRoot(old, index);
    const auto pool = metadata_->findCatalog(old, metadata_->findPageLocation(old, root.root_page_id).segment_id);
    const auto original = properties_->readEntity(old, {EDGE, 1}); auto move = relocation(old, pool.segment_id, memory);
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions publication; publication.bootstrap_failpoint = failpoint;
        EXPECT_THROW((void)metadata_->relocateHybridPropertyIndexPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1), publication), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *recovered); const auto pin = recovered->openSnapshot();
        const bool installed = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(recovered->findIndexRoot(pin, index).root_generation.value(), root.root_generation.value() + size_t(installed));
        EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root == before.hybrid_property_descriptor_root, !installed);
        EXPECT_EQ(recovered->findCatalog(pin, pool.segment_id).state, installed ? SegmentState::SGS_RETIRED : SegmentState::SGS_SEALED);
        EXPECT_EQ(roots.readEntity(pin, {EDGE, 1}), original); EXPECT_EQ(properties_->readEntity(old, {EDGE, 1}), original);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyFieldRelocationTest, DirectoryPinsReleaseAndReusePhysicalSlotsAcrossIndependentEpochs) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    auto old = metadata_->openSnapshot(); const auto index = hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_OWNER);
    const auto root = metadata_->findIndexRoot(old, index); const auto pool = metadata_->findCatalog(old, metadata_->findPageLocation(old, root.root_page_id).segment_id);
    auto move = relocation(old, pool.segment_id, memory);
    ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyIndexPool(move, LogSequenceNumber(old.superblock().checkpoint_lsn.value() + 1)));
    const auto before = metadata_->openSnapshot().superblock();
    const std::array<CatalogSegmentRetirementRequest, 1> retired{{{pool.segment_id, pool.generation}}};
    EXPECT_FALSE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    old.release(); ASSERT_TRUE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); const auto current = metadata_->findPageLocation(pin, root.root_page_id);
    move = relocation(pin, current.segment_id, memory); move.first_slot = move.last_slot = pool.physical_slot;
    ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyIndexPool(move, LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1)));
    const auto final = metadata_->openSnapshot(); const auto rebound = metadata_->findIndexRoot(final, index);
    const auto reused = metadata_->findCatalog(final, metadata_->findPageLocation(final, root.root_page_id).segment_id);
    EXPECT_EQ(rebound.root_generation.value(), root.root_generation.value() + 2); EXPECT_EQ(reused.physical_slot, pool.physical_slot);
    EXPECT_NE(reused.segment_id, pool.segment_id); EXPECT_EQ(properties_->readEntity(final, {NODE, 1}), properties_->readEntity(pin, {NODE, 1}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyFieldRelocationTest, DirectoryQuotaCancellationAndWrongPoolKindRefuseBeforeRetry) {
    seed(NODE, 64, 16); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto index = hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_OWNER); const auto root = metadata_->findIndexRoot(old, index);
    const auto pool = metadata_->findCatalog(old, metadata_->findPageLocation(old, root.root_page_id).segment_id);
    auto move = relocation(old, pool.segment_id, memory); const auto epoch = io_->dataMutationEpoch();
    auto small = std::make_shared<MaterializedByteBudget>(1024); move.memory = small;
    EXPECT_THROW((void)metadata_->relocateHybridPropertyIndexPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(small->used(), 0U); EXPECT_EQ(io_->dataMutationEpoch(), epoch); move.memory = memory;
    move.source_segment = fieldPages(old)[0].segment_id;
    EXPECT_THROW((void)metadata_->relocateHybridPropertyIndexPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); move.source_segment = pool.segment_id;
    for (const auto boundary : {std::string_view("native_graph.hybrid.relocation_private_index_frames"), std::string_view("native_graph.write.new_pool_prepare_calls")}) {
        CancelRelocation cancellation; move.cancellation = &cancellation;
        struct State { CancelRelocation *cancellation; std::string_view boundary; } state{&cancellation, boundary}; const auto epoch = io_->dataMutationEpoch();
        {
            graph::debug::PerfTrace::ScopedObserver observe(&state, [](void *owner, std::string_view key, int64_t, bool) noexcept {
                auto &state = *static_cast<State *>(owner); if (key == state.boundary) state.cancellation->prepared = true;
            });
            EXPECT_THROW((void)metadata_->relocateHybridPropertyIndexPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), RelocationCancelled);
        }
        EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
        if (boundary == "native_graph.hybrid.relocation_private_index_frames") EXPECT_EQ(io_->dataMutationEpoch(), epoch); else EXPECT_GT(io_->dataMutationEpoch(), epoch);
    }
    move.cancellation = nullptr;
    ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyIndexPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}), properties_->readEntity(old, {NODE, 1}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyFieldRelocationTest, ValidChecksumFreeBitmapCannotHideAStillRoutedFieldOrDirectoryFrame) {
    seed(NODE, 64, 16); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const auto fields = fieldPages(old); ASSERT_GE(fields.size(), 3U);
    const auto index = hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_OWNER);
    const auto root = metadata_->findIndexRoot(old, index); const auto directory = metadata_->findPageLocation(old, root.root_page_id);
    const auto entity = metadata_->findPageLocation(old, metadata_->findNodePageLocation(old, 1).page_id);
    for (const auto &location : {fields.back(), directory, entity}) {
        const auto pool = metadata_->findCatalog(old, location.segment_id);
        const BufferPoolPageKey key{PhysicalSegmentAllocator::checkedSlotOffset(before.data_region_offset, pool.physical_slot), pool.generation.value()};
        std::array<std::byte, RSS_PAGE_SIZE> original{}; ASSERT_EQ(io_->readAt(key, original.data(), original.size()), original.size());
        auto damaged = original;
        for (size_t control = 0; control < RSS_SHARED_PAGE_POOL_CONTROL_RECORD_COUNT; ++control) {
            const size_t offset = RSS_ENVELOPE_BYTES + control * RSS_SHARED_PAGE_POOL_HEADER_BYTES;
            auto header = SharedPagePoolHeaderCodec::decode(std::span(damaged).subspan(offset, RSS_SHARED_PAGE_POOL_HEADER_BYTES));
            SharedPagePoolFrameMap::release(header, location.page_index);
            const auto bytes = SharedPagePoolHeaderCodec::encode(header);
            std::copy(bytes.begin(), bytes.end(), damaged.begin() + offset);
            EXPECT_NO_THROW((void)SharedPagePoolHeaderCodec::decode(std::span(damaged).subspan(offset, RSS_SHARED_PAGE_POOL_HEADER_BYTES)));
        }
        io_->writeAt(key, damaged.data(), damaged.size()); const auto epoch = io_->dataMutationEpoch(); auto move = relocation(old, pool.segment_id, memory);
        if (pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL) {
            EXPECT_THROW((void)metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
            const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, int64_t(777)}}};
            NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
                cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903};
            EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
        } else if (pool.kind == SegmentKind::SGK_INDEX_PAGE_POOL)
            EXPECT_THROW((void)metadata_->relocateHybridPropertyIndexPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
        else EXPECT_THROW((void)metadata_->relocateHybridPropertyEntityPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
        io_->writeAt(key, original.data(), original.size());
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyFieldRelocationTest, EntityAndOverflowLabelReferencesSurvivePhysicalPoolRepacking) {
    seed(NODE, 64, 16); NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80); options.entity_pool_owner_id = 901;
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    std::array<uint64_t, 32> expectedLabels; for (size_t i = 0; i < expectedLabels.size(); ++i) expectedLabels[i] = i + 1;
    auto tx = planner.beginTransaction(); tx.replaceNodeLabels(1, expectedLabels); ASSERT_TRUE(tx.commit().committed());
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion)); const auto old = metadata_->openSnapshot(); const auto nodeLocation = metadata_->findNodePageLocation(old, 1);
    const auto sourceLocation = metadata_->findPageLocation(old, nodeLocation.page_id); const auto pool = metadata_->findCatalog(old, sourceLocation.segment_id);
    const auto node = entities_->readNode(old, 1); ASSERT_TRUE(node.label_overflow.isValid());
    ASSERT_EQ(metadata_->findPageLocation(old, node.label_overflow.page_id).segment_id, pool.segment_id);
    const auto original = properties_->readEntity(old, {NODE, 1}); auto move = relocation(old, pool.segment_id, memory);
    ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyEntityPool(move, LogSequenceNumber(old.superblock().checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); EXPECT_EQ(metadata_->findNodePageLocation(pin, 1), nodeLocation);
    EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root, old.superblock().hybrid_property_descriptor_root);
    const auto newLocation = metadata_->findPageLocation(pin, nodeLocation.page_id);
    EXPECT_NE(newLocation.segment_id, pool.segment_id); EXPECT_EQ(newLocation.generation.value(), pool.generation.value() + 1U);
    EXPECT_EQ(entities_->readNode(pin, 1), node); EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}), original);
    EXPECT_EQ(labels_->readNodeLabels(pin, entities_->readNode(pin, 1)), (std::vector<uint64_t>(expectedLabels.begin(), expectedLabels.end())));
    EXPECT_EQ(labels_->readNodeLabels(old, entities_->readNode(old, 1)), (std::vector<uint64_t>(expectedLabels.begin(), expectedLabels.end())));
    EXPECT_EQ(metadata_->findPageLocation(pin, node.label_overflow.page_id).segment_id, newLocation.segment_id);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    auto reopened = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool readPages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    EntityPageStore readEntities(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    EntityLabelSetStore readLabels(io_, *reopened, readPages, SharedPagePoolOpenMode::SPPOM_READ_ONLY); const auto reopenedPin = reopened->openSnapshot();
    EXPECT_EQ(readLabels.readNodeLabels(reopenedPin, readEntities.readNode(reopenedPin, 1)), (std::vector<uint64_t>(expectedLabels.begin(), expectedLabels.end())));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan());
}
TEST_F(HybridPropertyFieldRelocationTest, RepackingSharedNodeEdgePoolCopiesCurrentMastersAndDrainsObsoleteEdgeFrames) {
    seed(EDGE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion)); auto old = metadata_->openSnapshot(); const auto nodeLocation = metadata_->findNodePageLocation(old, 1);
    const auto sourceLocation = metadata_->findPageLocation(old, nodeLocation.page_id); const auto pool = metadata_->findCatalog(old, sourceLocation.segment_id);
    ASSERT_EQ(metadata_->findPageLocation(old, metadata_->findEdgePageLocation(old, 1).page_id).segment_id, pool.segment_id);
    const auto before = old.superblock(); const std::array<EntityPropertyKey, 1> owners{{{EDGE, 1}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true, owners};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto changed = metadata_->openSnapshot(); auto move = relocation(changed, pool.segment_id, memory); int64_t copied = -1;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&copied, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.relocation_private_entity_frames") *static_cast<int64_t *>(owner) = value;
        });
        ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyEntityPool(move, LogSequenceNumber(changed.superblock().checkpoint_lsn.value() + 1)));
    }
    size_t currentFrames = 0; static_cast<void>(metadata_->visitPageLocationsUntil(changed, [&](const PageLocation &route) {
        if (route.segment_id == pool.segment_id) ++currentFrames; return true;
    }));
    EXPECT_EQ(copied, int64_t(currentFrames)); const auto pin = metadata_->openSnapshot();
    EXPECT_EQ(entities_->readNode(pin, 1), entities_->readNode(old, 1)); EXPECT_FALSE(metadata_->findEdgePageLocationOptional(pin, 1));
    EXPECT_EQ(entities_->readEdge(pin, 2), entities_->readEdge(changed, 2));
    EXPECT_NO_THROW((void)entities_->readEdge(old, 1));
    EXPECT_EQ(metadata_->findCatalog(pin, pool.segment_id).state, SegmentState::SGS_RETIRED);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyFieldRelocationTest, EntityPoolABRecoverySelectsOneCompleteMainRecordRoute) {
    seed(EDGE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion)); const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto node = metadata_->findNodePageLocation(old, 1); const auto location = metadata_->findPageLocation(old, node.page_id);
    const auto pool = metadata_->findCatalog(old, location.segment_id); auto move = relocation(old, pool.segment_id, memory);
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions publication; publication.bootstrap_failpoint = failpoint;
        EXPECT_THROW((void)metadata_->relocateHybridPropertyEntityPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1), publication), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); EntityPageStore entities(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY); const auto pin = recovered->openSnapshot();
        const bool installed = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(recovered->findPageLocation(pin, node.page_id).segment_id == pool.segment_id, !installed);
        EXPECT_EQ(recovered->findCatalog(pin, pool.segment_id).state, installed ? SegmentState::SGS_RETIRED : SegmentState::SGS_SEALED);
        EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root, before.hybrid_property_descriptor_root);
        EXPECT_EQ(entities.readNode(pin, 1), entities_->readNode(old, 1)); EXPECT_EQ(entities.readEdge(pin, 1), entities_->readEdge(old, 1));
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyFieldRelocationTest, EntityPoolQuotaAndCancellationRefundBeforePhysicalReuse) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion)); auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto node = metadata_->findNodePageLocation(old, 1); const auto pool = metadata_->findCatalog(old, metadata_->findPageLocation(old, node.page_id).segment_id);
    auto move = relocation(old, pool.segment_id, memory); const auto epoch = io_->dataMutationEpoch(); auto small = std::make_shared<MaterializedByteBudget>(1024); move.memory = small;
    EXPECT_THROW((void)metadata_->relocateHybridPropertyEntityPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(small->used(), 0U); EXPECT_EQ(io_->dataMutationEpoch(), epoch); move.memory = memory;
    for (const auto boundary : {std::string_view("native_graph.hybrid.relocation_private_entity_frames"), std::string_view("native_graph.write.new_pool_prepare_calls")}) {
        CancelRelocation cancellation; move.cancellation = &cancellation;
        struct State { CancelRelocation *cancellation; std::string_view boundary; } state{&cancellation, boundary}; const auto epoch = io_->dataMutationEpoch();
        {
            graph::debug::PerfTrace::ScopedObserver observe(&state, [](void *owner, std::string_view key, int64_t, bool) noexcept {
                auto &state = *static_cast<State *>(owner); if (key == state.boundary) state.cancellation->prepared = true;
            }); EXPECT_THROW((void)metadata_->relocateHybridPropertyEntityPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), RelocationCancelled);
        }
        EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
        if (boundary == "native_graph.hybrid.relocation_private_entity_frames") EXPECT_EQ(io_->dataMutationEpoch(), epoch); else EXPECT_GT(io_->dataMutationEpoch(), epoch);
    }
    move.cancellation = nullptr;
    ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyEntityPool(move, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const std::array<CatalogSegmentRetirementRequest, 1> retired{{{pool.segment_id, pool.generation}}}; auto state = metadata_->openSnapshot().superblock();
    EXPECT_FALSE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(state.checkpoint_lsn.value() + 1))); old.release();
    ASSERT_TRUE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); move = relocation(pin, metadata_->findPageLocation(pin, node.page_id).segment_id, memory);
    move.first_slot = move.last_slot = pool.physical_slot;
    ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyEntityPool(move, LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1)));
    const auto final = metadata_->openSnapshot(); const auto current = metadata_->findPageLocation(final, node.page_id);
    EXPECT_EQ(metadata_->findCatalog(final, current.segment_id).physical_slot, pool.physical_slot); EXPECT_EQ(current.generation.value(), pool.generation.value() + 2U);
    EXPECT_EQ(metadata_->findNodePageLocation(final, 1), node);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyFieldRelocationTest, WritableReopenDeletesANodeFromRepackedEntityPagesAndKeepsTheOtherLabels) {
    seed(NODE, 64, 2); NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80); options.entity_pool_owner_id = 901;
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    std::array<uint64_t, 32> labelIds; for (size_t i = 0; i < labelIds.size(); ++i) labelIds[i] = i + 1;
    auto labels = planner.beginTransaction(); labels.replaceNodeLabels(1, labelIds); ASSERT_TRUE(labels.commit().committed());
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    {
        const auto source = metadata_->openSnapshot(); const auto page = metadata_->findNodePageLocation(source, 1).page_id;
        const auto pool = metadata_->findPageLocation(source, page).segment_id; const auto move = relocation(source, pool, memory);
        ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyEntityPool(move, LogSequenceNumber(source.superblock().checkpoint_lsn.value() + 1)));
    }
    auto reopened = RelocatableMetadataStore::open(io_);
    SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION); BTreePageStore indexes(pages, *reopened);
    EntityPageStore entities(io_, *reopened, SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION);
    EntityLabelSetStore labelStore(io_, *reopened, pages, SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION); PropertyCollectionRootStore roots(pages, *reopened);
    NativeGraphWritePlanner writer(*reopened, entities, labelStore, roots, indexes, options);
    const std::array<EntityPropertyKey, 1> owners{{{NODE, 2}}}; auto tx = writer.beginTransaction(); tx.stageHybridPropertyOwnerDeletion(owners, memory); ASSERT_TRUE(tx.commit().committed());
    const auto pin = reopened->openSnapshot(); EXPECT_FALSE(reopened->findNodePageLocationOptional(pin, 2));
    EXPECT_EQ(labelStore.readNodeLabels(pin, entities.readNode(pin, 1)), (std::vector<uint64_t>(labelIds.begin(), labelIds.end())));
    EXPECT_EQ(roots.readEntity(pin, {NODE, 3}).entries[0].value, PropertyValue(int64_t(3)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyFieldRelocationTest, Full31FrameEntityPoolRepackingStaysWithinOneMiBAndKeepsEveryStableRoute) {
    const size_t owners = size_t(RSS_PAGE_POOL_DATA_FRAME_COUNT) * EntityPage::recordCapacity(EntityPageRecordKind::EPRK_NODE);
    seed(NODE, owners, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion)); const auto old = metadata_->openSnapshot(); const auto first = metadata_->findNodePageLocation(old, 1);
    const auto pool = metadata_->findCatalog(old, metadata_->findPageLocation(old, first.page_id).segment_id);
    std::vector<PageLocation> original;
    static_cast<void>(metadata_->visitPageLocationsUntil(old, [&](const auto &location) {
        if (location.segment_id == pool.segment_id) original.push_back(location); return true;
    })); ASSERT_EQ(original.size(), RSS_PAGE_POOL_DATA_FRAME_COUNT);
    const auto move = relocation(old, pool.segment_id, memory);
    ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyEntityPool(move, LogSequenceNumber(old.superblock().checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot();
    for (const auto &location : original) {
        const auto current = metadata_->findPageLocation(pin, location.page_id);
        EXPECT_NE(current.segment_id, pool.segment_id); EXPECT_EQ(current.generation.value(), pool.generation.value() + 1U);
        EXPECT_EQ(metadata_->findPageLocation(old, location.page_id), location);
    }
    for (const auto id : {uint64_t(1), uint64_t(64), uint64_t(owners)}) {
        EXPECT_EQ(metadata_->findNodePageLocation(pin, id), metadata_->findNodePageLocation(old, id));
        EXPECT_EQ(entities_->readNode(pin, id), entities_->readNode(old, id));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
#endif
