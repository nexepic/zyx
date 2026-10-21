#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
using namespace graph::storage;
using graph::PropertyValue;
using graph::OwnedPropertyValue;
using graph::query::execution::MaterializedByteBudget;
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
namespace {
class HybridPropertyCompactMembershipLifecycleTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {};
}
TEST_F(HybridPropertyCompactMembershipLifecycleTest, ColumnAuthoritySurvivesMutationDemotionReopenAndInverse) {
    seed(NODE);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto row = metadata_->openSnapshot();
    auto conversion = request(NODE, memory);
    conversion.conversion->publish_candidate = true;
    conversion.conversion->compact_column_memberships = true;
    ASSERT_NO_THROW(run(conversion));
    const auto native = metadata_->openSnapshot();
    {
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, native);
        const auto group = context.groupRecord({NODE, 1}); ASSERT_TRUE(group);
        EXPECT_EQ(group->mode, HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, native, memory);
        EXPECT_EQ(report.columns, 2U); EXPECT_EQ(report.memberships, 96U); EXPECT_EQ(report.directory_selectors, 0U);
    }
    for (uint64_t id = 1; id <= 49; ++id)
        EXPECT_EQ(properties_->readEntityOptional(native, {NODE, id}), properties_->readEntityOptional(row, {NODE, id}));
    const auto write = [&](uint32_t token, const PropertyValue &value) {
        const auto before = metadata_->openSnapshot().superblock();
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, token, OwnedPropertyValue(value)}}};
        NativeHybridPropertyColumnWrite change{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
        return metadata_->mutateHybridPropertyColumns(change, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
    };
    ASSERT_NO_THROW((void)write(1, PropertyValue(int64_t(777))));
    ASSERT_NO_THROW((void)write(1, PropertyValue("demoted")));
    ASSERT_NO_THROW((void)write(2, PropertyValue("last column")));
    const auto changed = metadata_->openSnapshot();
    EXPECT_EQ(properties_->readEntity(changed, {NODE, 1}).entries[0].value, PropertyValue("demoted"));
    EXPECT_EQ(properties_->readEntity(native, {NODE, 1}), properties_->readEntity(row, {NODE, 1}));
    {
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, changed);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, changed, memory);
        EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.memberships, 96U); EXPECT_EQ(report.directory_selectors, 96U);
    }
    auto reopened = RelocatableMetadataStore::openReadOnly(io_);
    SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore roots(pages, *reopened);
    EXPECT_EQ(roots.readEntity(reopened->openSnapshot(), {NODE, 1}), properties_->readEntity(changed, {NODE, 1}));
    auto inverse = request(NODE, memory); inverse.conversion->publish_candidate = true;
    inverse.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(inverse));
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}), properties_->readEntity(changed, {NODE, 1}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyCompactMembershipLifecycleTest, NativeMatrixInversePreservesNodeAndEdgeSnapshots) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    for (const auto kind : {NODE, EDGE}) {
        seed(kind, 64, 4);
        const auto row = metadata_->openSnapshot();
        auto conversion = request(kind, memory); conversion.conversion->publish_candidate = true;
        conversion.conversion->compact_column_memberships = true;
        ASSERT_NO_THROW(run(conversion));
        const auto native = metadata_->openSnapshot();
        conversion = request(kind, memory); conversion.conversion->publish_candidate = true;
        conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
        ASSERT_NO_THROW(run(conversion));
        const auto restored = metadata_->openSnapshot();
        for (uint64_t id = 1; id <= 64; ++id) {
            EXPECT_EQ(properties_->readEntity(native, {kind, id}), properties_->readEntity(row, {kind, id}));
            EXPECT_EQ(properties_->readEntity(restored, {kind, id}), properties_->readEntity(row, {kind, id}));
        }
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
        EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyCompactMembershipLifecycleTest, MatrixCacheRetainsItsOriginalAccountAndIntegrityBypassesIt) {
    seed(NODE, 64, 2);
    auto workspace = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, workspace); conversion.conversion->publish_candidate = true;
    conversion.conversion->compact_column_memberships = true; ASSERT_NO_THROW(run(conversion));
    const auto pin = metadata_->openSnapshot();
    auto retained = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    struct Work { int64_t decodes = 0, hits = 0; } work;
    graph::debug::PerfTrace::ScopedObserver observer(&work, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
        auto &w = *static_cast<Work *>(owner);
        if (key == "native_graph.hybrid.column_membership_decodes") w.decodes += value;
        if (key == "native_graph.hybrid.column_membership_cache_hits") w.hits += value;
    });
    {
        graph::utils::OwnedAllocationScope scope(retained);
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
        EXPECT_EQ(context.count({NODE, 1}), 2U); EXPECT_EQ(context.count({NODE, 2}), 2U);
        EXPECT_EQ(work.decodes, 1); EXPECT_EQ(work.hits, 1); EXPECT_GT(retained->used(), 0U);
        const auto before = work.decodes;
        {
            StorageIO::SequentialReadScope integrity(*io_);
            EXPECT_EQ(context.count({NODE, 1}), 2U); EXPECT_EQ(context.count({NODE, 2}), 2U);
        }
        EXPECT_EQ(work.decodes, before + 2); EXPECT_EQ(work.hits, 1);
        EXPECT_NO_THROW((void)HybridPropertyDirectoryIntegrity::verify(context, pin, workspace));
        EXPECT_EQ(workspace->used(), 0U);
    }
    EXPECT_EQ(retained->used(), 0U);
}

TEST_F(HybridPropertyCompactMembershipLifecycleTest, LegacyAndCompactGroupsShareOneAtomicColumnMutation) {
    seed(NODE, 128, 2);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    for (uint64_t group = 1; group <= 2; ++group) {
        auto conversion = request(NODE, memory); conversion.conversion->group_id = group;
        conversion.conversion->compact_column_memberships = group == 1;
        conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    }
    const auto old = metadata_->openSnapshot();
    {
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, old);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, old, memory);
        EXPECT_EQ(report.groups, 2U); EXPECT_EQ(report.memberships, 256U); EXPECT_EQ(report.directory_selectors, 128U);
    }
    const auto before = old.superblock();
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{
        {{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(int64_t(777)))},
        {{NODE, 65}, 1, OwnedPropertyValue(PropertyValue("legacy demotion"))}}};
    NativeHybridPropertyColumnWrite change{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(change, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto current = metadata_->openSnapshot();
    EXPECT_EQ(properties_->readEntity(current, {NODE, 1}).entries[0].value, PropertyValue(int64_t(777)));
    EXPECT_EQ(properties_->readEntity(current, {NODE, 65}).entries[0].value, PropertyValue("legacy demotion"));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}).entries[0].value, PropertyValue(int64_t(1)));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 65}).entries[0].value, PropertyValue(int64_t(65)));
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory);
    EXPECT_EQ(report.columns, 3U); EXPECT_EQ(report.directory_selectors, 128U); EXPECT_EQ(report.extensions, 64U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyCompactMembershipLifecycleTest, NativeTransactionStagesCompactConversionWithTheSameFenceAndAdmission) {
    seed(NODE, 64, 2);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    NativeGraphWritePlannerOptions placement; placement.first_slot = PhysicalSlotId(40); placement.last_slot = PhysicalSlotId(80);
    placement.entity_pool_owner_id = 901; placement.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement);
    const auto row = metadata_->openSnapshot();
    {
        auto tx = planner.beginTransaction();
        tx.stageHybridPropertyConversion(NODE, 1, memory, nullptr, true,
            NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, true);
        tx.rollback(); EXPECT_EQ(metadata_->openSnapshot().superblock(), row.superblock()); EXPECT_EQ(memory->used(), 0U);
    }
    {
        auto tx = planner.beginTransaction();
        NativeHybridPropertyAllocationAdmission gate; gate.maximum_admitted_peak_bytes = 16U * 1024U * 1024U;
        gate.maximum_admitted_file_bytes = 16U * 1024U * 1024U;
        tx.stageHybridPropertyConversion(NODE, 1, memory, nullptr, true,
            NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, true, gate);
        ASSERT_TRUE(tx.commit().committed());
        // This small shape fails complete allocation admission: no native root
        // or feature becomes visible, even though private COW work was allowed.
        EXPECT_EQ(metadata_->openSnapshot().superblock(), row.superblock()); EXPECT_EQ(memory->used(), 0U);
    }
    auto tx = planner.beginTransaction();
    tx.stageHybridPropertyConversion(NODE, 1, memory, nullptr, true,
        NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, true);
    ASSERT_TRUE(tx.commit().committed());
    const auto native = metadata_->openSnapshot();
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, native);
    ASSERT_TRUE(context.groupRecord({NODE, 1}));
    EXPECT_EQ(context.groupRecord({NODE, 1})->mode, HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP);
    EXPECT_EQ(properties_->readEntity(native, {NODE, 1}), properties_->readEntity(row, {NODE, 1}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
#endif
