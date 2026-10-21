#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::OwnedPropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyLargeMutationTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    Superblock write(std::span<const NativeHybridPropertyColumnReplacement> cells,
        std::shared_ptr<MaterializedByteBudget> memory, const MetadataPublishOptions &options = {}, const CancellationCheckpoint *cancel = nullptr) {
        const auto state = metadata_->openSnapshot().superblock();
        NativeHybridPropertyColumnWrite request{state.database_id, state.generation, state.hybrid_property_descriptor_root,
            cells, std::move(memory), PhysicalSlotId(40), PhysicalSlotId(80), 903, cancel, true};
        return metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(state.checkpoint_lsn.value() + 1), options);
    }
    void convert(EntityPropertyOwnerKind kind, const std::shared_ptr<MaterializedByteBudget> &memory) {
        auto conversion = request(kind, memory); conversion.conversion->publish_candidate = true; run(conversion);
    }
    LargeValueRef reference(const MetadataSnapshotPin &pin, EntityPropertyKey owner, uint32_t token) {
        auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
        graph::utils::OwnedHeapReservation boundary(memory), credit(memory); std::optional<LargeValueRef> result;
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
        static_cast<void>(context.visitField(owner, token, [](const auto &) {}, [&](const auto &field) { result = field.reference(); },
            [&](size_t bytes) { boundary.set(bytes); }, [&](size_t bytes) { credit.set(bytes); }));
        if (!result) throw std::logic_error("missing native large reference"); return *result;
    }
    void eraseOwner(std::span<const EntityPropertyKey> owners, std::shared_ptr<MaterializedByteBudget> memory) {
        const auto state = metadata_->openSnapshot().superblock();
        NativeHybridPropertyColumnWrite request{state.database_id, state.generation, state.hybrid_property_descriptor_root,
            {}, std::move(memory), PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true, owners};
        static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    }
};
}
TEST_F(HybridPropertyLargeMutationTest, DisabledOrUnconvertedLargeWritesLeaveTheSourceUntouched) {
    seed(NODE, 2, 0); const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(std::string(8192, 'x')))}}};
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    EXPECT_THROW((void)write(cells, memory), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyLargeMutationTest, MissingLargeFieldsCreateOneContentIdentityAndPreserveOldPinsAndNoop) {
    seed(NODE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    EXPECT_FALSE(metadata_->findIndexRootOptional(old, RSS_LARGE_VALUE_CONTENT_INDEX_ID));
    const OwnedPropertyValue shared(PropertyValue(std::string(8192, 'x')));
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 1, shared}, {{NODE, 2}, 1, shared}}};
    ASSERT_NO_THROW((void)write(cells, memory)); const auto pin = metadata_->openSnapshot();
    EXPECT_EQ(pin.superblock().next_large_value_id, before.next_large_value_id + 1U);
    EXPECT_EQ(reference(pin, {NODE, 1}, 1), reference(pin, {NODE, 2}, 1));
    EXPECT_EQ(roots.readEntity(pin, {NODE, 1}).entries[0].value, shared.get());
    EXPECT_FALSE(roots.readEntityOptional(old, {NODE, 1}));
    const auto epoch = io_->dataMutationEpoch(); EXPECT_EQ(write(cells, memory), pin.superblock()); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    auto reopened = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore reopenedValues(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY); PropertyCollectionRootStore reopenedRoots(pages, reopenedValues, *reopened);
    EXPECT_EQ(reopenedRoots.readEntity(reopened->openSnapshot(), {NODE, 2}).entries[0].value, shared.get());
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargeMutationTest, AColumnDemotesOnlyItsFieldWhenAReplacementNeedsANewLargeValue) {
    seed(NODE, 48, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(std::string(8192, 'x')))}}};
    ASSERT_NO_THROW((void)write(cells, memory)); const auto pin = metadata_->openSnapshot();
    LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory, &values);
    EXPECT_EQ(report.memberships, 96U); EXPECT_EQ(report.columns, 1U); EXPECT_EQ(report.extensions, 48U); EXPECT_EQ(report.large_values, 1U);
    EXPECT_EQ(pin.superblock().next_large_value_id, before.next_large_value_id + 1U);
    EXPECT_EQ(roots.readEntity(pin, {NODE, 1}).entries[0].value, PropertyValue(std::string(8192, 'x')));
    EXPECT_EQ(roots.readEntity(pin, {NODE, 1}).entries[1].value, roots.readEntity(old, {NODE, 1}).entries[1].value);
    EXPECT_EQ(roots.readEntity(pin, {NODE, 2}), roots.readEntity(old, {NODE, 2}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargeMutationTest, ReplacingRemovingAndDeletingOwnersRetainsSharedGlobalContentAndOldPins) {
    seed(EDGE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(EDGE, memory));
    const OwnedPropertyValue shared(PropertyValue(std::string(8192, 'x')));
    std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{EDGE, 1}, 1, shared}, {{EDGE, 2}, 1, shared}}};
    ASSERT_NO_THROW((void)write(cells, memory)); const auto old = metadata_->openSnapshot(); const auto ref = reference(old, {EDGE, 1}, 1);
    const auto master = entities_->readEdge(old, 2);
    LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    const std::array<EntityPropertyKey, 1> first{{{EDGE, 1}}}; ASSERT_NO_THROW(eraseOwner(first, memory));
    EXPECT_FALSE(metadata_->findEdgePageLocationOptional(metadata_->openSnapshot(), 1));
    EXPECT_EQ(reference(metadata_->openSnapshot(), {EDGE, 2}, 1), ref); EXPECT_EQ(entities_->readEdge(metadata_->openSnapshot(), 2), master);
    EXPECT_EQ(roots.readEntity(old, {EDGE, 1}).entries[0].value, shared.get());
    cells[0] = {{EDGE, 2}, 1, int64_t(7)}; ASSERT_NO_THROW((void)write(std::span(cells).first(1), memory));
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {EDGE, 2}).entries[0].value, PropertyValue(int64_t(7)));
    EXPECT_NO_THROW((void)values.manifest(metadata_->openSnapshot(), ref));
    cells[0].value = shared; ASSERT_NO_THROW((void)write(std::span(cells).first(1), memory));
    EXPECT_EQ(reference(metadata_->openSnapshot(), {EDGE, 2}, 1), ref);
    cells[0].erase = true; ASSERT_NO_THROW((void)write(std::span(cells).first(1), memory));
    EXPECT_FALSE(roots.readEntityOptional(metadata_->openSnapshot(), {EDGE, 2}));
    EXPECT_EQ(roots.readEntity(old, {EDGE, 2}).entries[0].value, shared.get());
    EXPECT_NO_THROW((void)values.manifest(metadata_->openSnapshot(), ref));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargeMutationTest, LargeListNoopFollowsRowSignedZeroAndNanEqualityWithoutIdentityChurn) {
    seed(NODE, 1, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    std::vector<PropertyValue> list(1000, PropertyValue(0.0));
    std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(list))}}};
    ASSERT_NO_THROW((void)write(cells, memory)); auto before = metadata_->openSnapshot().superblock(); auto epoch = io_->dataMutationEpoch();
    list[0] = PropertyValue(-0.0); cells[0].value = OwnedPropertyValue(PropertyValue(list));
    EXPECT_EQ(write(cells, memory), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    constexpr uint64_t bits = UINT64_C(0x7ff80000abc12345); list[1] = PropertyValue(std::bit_cast<double>(bits));
    cells[0].value = OwnedPropertyValue(PropertyValue(list)); ASSERT_NO_THROW((void)write(cells, memory));
    const auto old = metadata_->openSnapshot(); const auto ref = reference(old, {NODE, 1}, 1); before = old.superblock(); epoch = io_->dataMutationEpoch();
    EXPECT_EQ(write(cells, memory).generation.value(), before.generation.value() + 1U); EXPECT_GT(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(metadata_->openSnapshot().superblock().next_large_value_id, before.next_large_value_id);
    EXPECT_EQ(reference(metadata_->openSnapshot(), {NODE, 1}, 1), ref);
    LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    const auto root = roots.readEntity(metadata_->openSnapshot(), {NODE, 1}); const auto &stored = std::get<std::vector<PropertyValue>>(root.entries[0].value.getVariant());
    EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(stored[1].getVariant())), bits);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargeMutationTest, LargeWriteABRecoveryAndPreparationCancellationPublishNoPartialIdentity) {
    seed(NODE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(std::string(8192, 'x')))}}};
    auto tiny = std::make_shared<MaterializedByteBudget>(128 * 1024);
    EXPECT_THROW((void)write(cells, tiny), graph::query::execution::MaterializedByteLimitExceeded); EXPECT_EQ(tiny->used(), 0U);
    struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("large field cancelled") {} };
    struct Cancellation final : CancellationCheckpoint { bool prepared = false; void check() const override { if (prepared) throw Cancelled(); } } cancellation;
    {
        graph::debug::PerfTrace::ScopedObserver observer(&cancellation, [](void *owner, std::string_view key, int64_t count, bool) noexcept {
            if (key == "native_graph.hybrid.private_new_field_values" && count > 0) static_cast<Cancellation *>(owner)->prepared = true;
        });
        EXPECT_THROW((void)write(cells, memory, {}, &cancellation), Cancelled);
    }
    EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions options; options.bootstrap_failpoint = point; EXPECT_THROW((void)write(cells, memory, options), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); const auto pin = recovered->openSnapshot();
        SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY); LargeValueStore values(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, values, *recovered); const bool installed = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(pin.superblock().next_large_value_id, before.next_large_value_id + size_t(installed));
        if (installed) EXPECT_EQ(roots.readEntity(pin, {NODE, 1}).entries[0].value, PropertyValue(std::string(8192, 'x')));
        else EXPECT_FALSE(roots.readEntityOptional(pin, {NODE, 1}));
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyLargeMutationTest, NativeTransactionsOwnLargeInputsAcrossMoveConflictRollbackAndPreparedCancellation) {
    seed(NODE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(std::string(8192, 'x')))}}};
    auto first = planner.beginTransaction(); auto second = planner.beginTransaction();
    first.stageHybridPropertyColumns(cells, memory, nullptr, true); second.stageHybridPropertyColumns(cells, memory, nullptr, true);
    EXPECT_GT(memory->used(), 16384U); cells[0].value = OwnedPropertyValue(PropertyValue(std::string(8192, 'y')));
    NativeGraphWriteTransaction moved(std::move(first)); ASSERT_TRUE(moved.commit().committed());
    EXPECT_FALSE(second.commit().committed()); EXPECT_EQ(memory->used(), 0U);
    LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(std::string(8192, 'x')));
    auto rollback = planner.beginTransaction(); rollback.stageHybridPropertyColumns(cells, memory, nullptr, true);
    EXPECT_GT(memory->used(), 8192U); rollback.rollback(); EXPECT_EQ(memory->used(), 0U);
    struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("staged large mutation cancelled") {} };
    struct Cancellation final : CancellationCheckpoint { bool prepared = false; void check() const override { if (prepared) throw Cancelled(); } } cancellation;
    auto cancelled = planner.beginTransaction(); cancelled.stageHybridPropertyColumns(cells, memory, &cancellation, true);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::debug::PerfTrace::ScopedObserver observer(&cancellation, [](void *owner, std::string_view key, int64_t count, bool) noexcept {
            if (key == "native_graph.hybrid.private_new_field_values" && count > 0) static_cast<Cancellation *>(owner)->prepared = true;
        });
        EXPECT_THROW((void)cancelled.commit(), Cancelled);
    }
    EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(cancelled.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    auto retry = planner.beginTransaction(); retry.stageHybridPropertyColumns(cells, memory, nullptr, true); ASSERT_TRUE(retry.commit().committed());
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(std::string(8192, 'y')));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargeMutationTest, CorruptLargeContentCannotBeSilentlyDiscardedByRemoveOrOwnerDeletion) {
    seed(NODE, 1, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(std::string(8192, 'x')))}}};
    ASSERT_NO_THROW((void)write(cells, memory)); const auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    const auto ref = reference(pin, {NODE, 1}, 1); LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    const auto manifest = values.manifest(pin, ref); ASSERT_FALSE(manifest.chunks.empty());
    const auto page = metadata_->findPageLocation(pin, manifest.chunks[0].first_data_page_id);
    const auto pool = metadata_->findCatalog(pin, page.segment_id);
    const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(before.data_region_offset, pool.physical_slot) + uint64_t(page.page_index) * RSS_PAGE_SIZE;
    std::array<std::byte, RSS_PAGE_SIZE> bytes{}; io_->readAt(offset, bytes.data(), bytes.size());
    bytes[RSS_PAGE_OWNER_HEADER_BYTES + RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES] ^= std::byte{1};
    io_->writeAt(offset, bytes.data(), bytes.size()); const auto epoch = io_->dataMutationEpoch();
    cells[0].erase = true; EXPECT_THROW((void)write(cells, memory), FormatException);
    const std::array<EntityPropertyKey, 1> owners{{{NODE, 1}}}; EXPECT_THROW(eraseOwner(owners, memory), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(reference(metadata_->openSnapshot(), {NODE, 1}, 1), ref); EXPECT_EQ(memory->used(), 0U);
}
#endif
