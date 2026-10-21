#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::OwnedPropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyInlineMutationTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    NativeGraphWritePlannerOptions placement() {
        NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
        options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903}; return options;
    }
    Superblock write(std::span<const NativeHybridPropertyColumnReplacement> cells,
        std::shared_ptr<std::pmr::memory_resource> memory, const MetadataPublishOptions &options = {}) {
        const auto before = metadata_->openSnapshot().superblock();
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            cells, std::move(memory), PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
        return metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(before.checkpoint_lsn.value() + 1), options);
    }
    void convert(EntityPropertyOwnerKind kind, const std::shared_ptr<MaterializedByteBudget> &memory) {
        auto conversion = request(kind, memory); conversion.conversion->publish_candidate = true; run(conversion);
    }
};
}
TEST_F(HybridPropertyInlineMutationTest, DisabledOrUnconvertedComplexWritesAreRejectedWithoutIo) {
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue("abc"))}}};
    EXPECT_THROW((void)write(cells, memory), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyInlineMutationTest, ComplexReplacementDemotesOnlyItsColumnAndKeepsOldPinsAndReopen) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    const auto old = metadata_->openSnapshot(); const auto original = properties_->readEntity(old, {NODE, 1});
    PropertyValue::MapType map{{"list", PropertyValue(std::vector<PropertyValue>{PropertyValue("nested"), PropertyValue(), PropertyValue(int64_t(7))})},
        {"bytes", PropertyValue(PropertyValue::Bytes{std::byte{0}, std::byte{255}})}};
    const PropertyValue replacement(map);
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{
        {{NODE, 1}, 1, OwnedPropertyValue(replacement)}, {{NODE, 49}, 3, OwnedPropertyValue(PropertyValue("new field"))}}};
    ASSERT_NO_THROW((void)write(cells, memory)); EXPECT_EQ(memory->used(), 0U);
    const auto pin = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.columns, 1U); EXPECT_EQ(report.extensions, 49U); EXPECT_EQ(report.memberships, 97U);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}).entries[0].value, replacement);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 2}), properties_->readEntity(old, {NODE, 2}));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    auto reopened = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore roots(pages, *reopened);
    EXPECT_EQ(roots.readEntity(reopened->openSnapshot(), {NODE, 1}).entries[0].value, replacement);
    auto inverse = request(NODE, memory); inverse.conversion->publish_candidate = true;
    inverse.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(inverse));
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, replacement);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}).entries[0].value, replacement);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyInlineMutationTest, InlineKindsRoundTripAndNoopUsesRecursiveRowEquality) {
    seed(EDGE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(EDGE, memory));
    std::vector<PropertyValue> values{PropertyValue(std::string(900, 'x')), PropertyValue(PropertyValue::Bytes{std::byte{42}}),
        PropertyValue(graph::TemporalDate{123}), PropertyValue(graph::TemporalDateTime{456}), PropertyValue(graph::TemporalDuration{7, 8, 9}),
        PropertyValue(std::vector<PropertyValue>{PropertyValue("a"), PropertyValue(-0.0)})};
    for (const auto &value : values) {
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{EDGE, 1}, 1, OwnedPropertyValue(value)}}};
        ASSERT_NO_THROW((void)write(cells, memory)); const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
        EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {EDGE, 1}).entries[0].value, value);
        EXPECT_EQ(write(cells, memory), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
    }
    constexpr uint64_t nanBits = UINT64_C(0x7ff80000abc12345);
    const PropertyValue nested(std::vector<PropertyValue>{PropertyValue(std::bit_cast<double>(nanBits))});
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{EDGE, 1}, 1, OwnedPropertyValue(nested)}}};
    ASSERT_NO_THROW((void)write(cells, memory)); const auto before = metadata_->openSnapshot().superblock();
    EXPECT_GT(write(cells, memory).generation.value(), before.generation.value());
    const auto root = properties_->readEntity(metadata_->openSnapshot(), {EDGE, 1});
    const auto &list = std::get<std::vector<PropertyValue>>(root.entries[0].value.getVariant());
    EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(list[0].getVariant())), nanBits);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyInlineMutationTest, NativeStagingOwnsPayloadAcrossMoveConflictAndRollback) {
    seed(NODE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
    std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(std::string(900, 'a')))}}};
    auto first = planner.beginTransaction(); auto second = planner.beginTransaction();
    first.stageHybridPropertyColumns(cells, memory, nullptr, true); second.stageHybridPropertyColumns(cells, memory, nullptr, true);
    EXPECT_GT(memory->used(), 1800U); cells[0].value = OwnedPropertyValue(PropertyValue("changed caller"));
    NativeGraphWriteTransaction moved(std::move(first)); ASSERT_TRUE(moved.commit().committed());
    EXPECT_FALSE(second.commit().committed()); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(std::string(900, 'a')));
    auto tx = planner.beginTransaction(); tx.stageHybridPropertyColumns(cells, memory, nullptr, true);
    EXPECT_GT(memory->used(), 0U); tx.rollback(); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyInlineMutationTest, BeyondAccountAndHeapHeavyCellsRefuseBeforeIoAndRefundStaging) {
    seed(NODE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(std::string(1024 * 1024, 'a')))}}};
    EXPECT_THROW((void)write(cells, memory), graph::query::execution::MaterializedByteLimitExceeded);
    auto tx = planner.beginTransaction(); EXPECT_THROW(tx.stageHybridPropertyColumns(cells, memory, nullptr, true), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(memory->used(), 0U); EXPECT_TRUE(tx.isActive());
    cells[0].value = OwnedPropertyValue(PropertyValue(std::vector<PropertyValue>(300, PropertyValue())));
    auto tiny = std::make_shared<MaterializedByteBudget>(4096);
    EXPECT_THROW(tx.stageHybridPropertyColumns(cells, tiny, nullptr, true), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U); EXPECT_TRUE(tx.isActive());
    EXPECT_THROW((void)write(cells, tiny), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    cells[0].value = OwnedPropertyValue(PropertyValue("retry")); tx.stageHybridPropertyColumns(cells, memory, nullptr, true);
    ASSERT_TRUE(tx.commit().committed()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyInlineMutationTest, ComplexWriteABRecoveryAndErasePreserveAtomicFields) {
    seed(NODE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    const auto old = metadata_->openSnapshot();
    std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue("atomic"))}}};
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions options; options.bootstrap_failpoint = point;
        EXPECT_THROW((void)write(cells, memory, options), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *recovered); const auto pin = recovered->openSnapshot();
        if (point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE)
            EXPECT_EQ(roots.readEntity(pin, {NODE, 1}).entries[0].value, PropertyValue("atomic"));
        else EXPECT_FALSE(roots.readEntityOptional(pin, {NODE, 1}));
        EXPECT_FALSE(properties_->readEntityOptional(old, {NODE, 1})); EXPECT_EQ(memory->used(), 0U);
    }
    cells[0].erase = true; ASSERT_NO_THROW((void)write(cells, memory));
    EXPECT_FALSE(properties_->readEntityOptional(metadata_->openSnapshot(), {NODE, 1}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyInlineMutationTest, CancellationAfterPrivatePreparationRefundsComplexPayloadAndAllowsRetry) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("inline mutation cancelled") {} };
    struct Cancellation final : CancellationCheckpoint {
        bool prepared = false;
        void check() const override { if (prepared) throw Cancelled(); }
    } cancellation;
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1,
        OwnedPropertyValue(PropertyValue(std::vector<PropertyValue>{PropertyValue(std::string(900, 'x'))}))}}};
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    NativeHybridPropertyColumnWrite mutation{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, &cancellation, true};
    {
        graph::debug::PerfTrace::ScopedObserver observer(&cancellation,
            [](void *owner, std::string_view key, int64_t, bool) noexcept {
                if (key == "native_graph.hybrid.private_map_images") static_cast<Cancellation *>(owner)->prepared = true;
            });
        EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(mutation,
            LogSequenceNumber(before.checkpoint_lsn.value() + 1)), Cancelled);
    }
    EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    mutation.cancellation = nullptr;
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(mutation, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyInlineMutationTest, RepeatedSmallWritesReusePhysicalFramesWithoutGrowingPropertyPools) {
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    auto old = metadata_->openSnapshot(); const auto original = properties_->readEntity(old, {NODE, 1});
    const auto countPools = [&] {
        size_t count = 0; metadata_->visitCatalogEntries(metadata_->openSnapshot(), [&](const CatalogEntry &pool) {
            if (pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && pool.owner_id == 903 && pool.state == SegmentState::SGS_SEALED) ++count;
        }); return count;
    };
    const auto count = countPools(); ASSERT_EQ(count, 1U);
    for (int64_t value = 100; value < 170; ++value) {
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(value))}}};
        ASSERT_NO_THROW((void)write(cells, memory));
    }
    EXPECT_EQ(countPools(), count); EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(int64_t(169)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
#endif
