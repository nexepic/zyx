#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
#include "graph/storage/relocatable/property/HybridPropertyColumnDelta.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyColumnWritePreparation.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class PendingOwnerNoPhysicalAllocator final : public MetadataPageAllocator {
public:
    MetadataPageId allocateMetadataPageId() override { throw std::logic_error("unexpected physical metadata allocation"); }
    MetadataPageAllocation allocatePage(Generation, LogSequenceNumber, std::span<const PhysicalSlotId>) override {
        throw std::logic_error("unexpected physical metadata allocation");
    }
    void abandonPage(PhysicalPageRef) noexcept override {}
};
class HybridPropertyOwnerInsertionPlanTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    void install(const std::shared_ptr<MaterializedByteBudget> &memory) {
        auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; run(conversion);
    }
};
}
TEST_F(HybridPropertyOwnerInsertionPlanTest, DisabledOrRowOwnerCannotAcquirePendingColumnImages) {
    seed(NODE, 63, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto source = metadata_->openSnapshot(); const auto epoch = io_->dataMutationEpoch();
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, source); const PropertyValue value(int64_t(64));
    EXPECT_THROW((void)HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, source, {NODE, 64}, 1,
        Generation(source.superblock().generation.value() + 1), memory, value), FormatException);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyOwnerInsertionPlanTest, PendingOwnerColumnCarriesSeparateLivenessWithoutPublishingAnEntity) {
    seed(NODE, 63, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); install(memory);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        const auto source = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, source);
        const PropertyValue value(int64_t(64));
        auto plan = HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, source, {NODE, 64}, 1,
            Generation(before.generation.value() + 1), memory, value); ASSERT_TRUE(plan);
        EXPECT_EQ(plan->action(), HybridPropertyColumnDelta::Action::HCD_REPLACE_COLUMN);
        EXPECT_EQ(plan->effects().memberships, 1); EXPECT_EQ(plan->effects().set_live, UINT64_C(1) << 63);
        EXPECT_EQ(plan->replacementColumn().presence, UINT64_MAX); EXPECT_EQ(plan->replacementColumn().live, UINT64_MAX);
        EXPECT_EQ(plan->replacementColumn().words[63], 64U); EXPECT_FALSE(plan->sourceGroup().live & (UINT64_C(1) << 63));
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(source, 64));
        EXPECT_EQ(context.visitField({NODE, 64}, 1, [](const auto &) {}, [](const auto &) {}), HybridPropertyReadContext::FieldRoute::HFR_MISSING);
    }
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerInsertionPlanTest, NetZeroMembershipMergeCannotBypassTheExistingLivenessPublicationGuard) {
    seed(NODE, 63, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); install(memory);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        const auto source = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, source);
        const auto next = Generation(before.generation.value() + 1); const PropertyValue value(int64_t(64));
        auto inserted = HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, source, {NODE, 64}, 1, next, memory, value);
        auto erased = HybridPropertyColumnDelta::planExistingColumn(context, source, {NODE, 1}, 1, next, memory, nullptr);
        ASSERT_TRUE(inserted); ASSERT_TRUE(erased);
        const std::array<const HybridPropertyColumnDelta *, 2> changes{&*inserted, &*erased};
        auto merged = HybridPropertyColumnDelta::mergeExistingColumnChanges(changes, memory);
        EXPECT_EQ(merged.effects().memberships, 0); EXPECT_EQ(merged.effects().set_live, UINT64_C(1) << 63);
        EXPECT_EQ(merged.replacementColumn().live, UINT64_MAX); EXPECT_FALSE(merged.replacementColumn().presence & 1U);
        HybridPropertyFieldPageStore fields(io_, *metadata_, source);
        const std::array<const HybridPropertyColumnDelta *, 1> final{&merged};
        EXPECT_THROW((void)HybridPropertyColumnWritePreparation(source, fields, final, memory), FormatException);
    }
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerInsertionPlanTest, IncompatiblePendingValueDemotesOnlyItsFieldAndKeepsBorrowedValuesAlive) {
    seed(NODE, 63, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); install(memory);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        const auto source = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, source);
        const auto next = Generation(before.generation.value() + 1); const PropertyValue value("pending"), changed(int64_t(999));
        auto inserted = HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, source, {NODE, 64}, 1, next, memory, value);
        auto updated = HybridPropertyColumnDelta::planExistingColumn(context, source, {NODE, 1}, 1, next, memory, &changed);
        ASSERT_TRUE(inserted); ASSERT_TRUE(updated);
        const std::array<const HybridPropertyColumnDelta *, 2> changes{&*inserted, &*updated};
        auto merged = HybridPropertyColumnDelta::mergeExistingColumnChanges(changes, memory);
        EXPECT_EQ(merged.action(), HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD);
        EXPECT_EQ(merged.effects().columns, -1); EXPECT_EQ(merged.effects().memberships, 1);
        EXPECT_EQ(merged.effects().set_live, UINT64_C(1) << 63); EXPECT_EQ(merged.demotedCells()[63].extension, &value);
        EXPECT_EQ(merged.demotedCells()[0].bits, 999U); EXPECT_EQ(properties_->readEntityPropertyOptional(source, {NODE, 1}, 2)->value, PropertyValue(int64_t(2)));
        EXPECT_THROW((void)HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, source, {NODE, 1}, 1, next, memory, value), FormatException);
        auto tiny = std::make_shared<MaterializedByteBudget>(1024);
        EXPECT_THROW((void)HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, source, {NODE, 64}, 1, next, tiny, value), graph::query::execution::MaterializedByteLimitExceeded);
        EXPECT_EQ(tiny->used(), 0U);
    }
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerInsertionPlanTest, PendingGroupPreparationCombinesColumnAndLiveBitWithoutPhysicalAllocation) {
    seed(NODE, 63, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); install(memory);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        const auto source = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, source);
        HybridPropertyFieldPageStore fields(io_, *metadata_, source); PendingOwnerNoPhysicalAllocator allocator;
        MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1), memory);
        const PropertyValue value(int64_t(64)); auto plan = HybridPropertyColumnDelta::planPendingOwnerColumnMember(
            context, source, {NODE, 64}, 1, batch.generation(), memory, value); ASSERT_TRUE(plan);
        const std::array<const HybridPropertyColumnDelta *, 1> changes{&*plan}; const std::array<EntityPropertyKey, 1> owners{{{NODE, 64}}};
        auto prepared = HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, source, fields, changes, owners, memory);
        EXPECT_EQ(prepared.group().live, UINT64_MAX); EXPECT_EQ(prepared.group().memberships, 64U);
        ASSERT_EQ(prepared.pages().size(), 1U); ASSERT_EQ(prepared.owners().size(), 1U); EXPECT_TRUE(prepared.extensions().empty());
        EXPECT_EQ(prepared.pages()[0].readColumn(plan->columnReference(), plan->replacementColumn().identity,
            prepared.group(), batch.generation()).words[63], 64U);
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(source, 64));
    }
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerInsertionPlanTest, PropertylessPendingOwnerPreservesOlderColumnBytesAndRejectsDuplicateDeclarations) {
    seed(NODE, 63, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); install(memory);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        const auto source = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, source);
        HybridPropertyFieldPageStore fields(io_, *metadata_, source); PendingOwnerNoPhysicalAllocator allocator;
        MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1), memory);
        const PropertyValue value(int64_t(1)); auto unchanged = HybridPropertyColumnDelta::planExistingColumn(
            context, source, {NODE, 1}, 1, batch.generation(), memory, &value); ASSERT_TRUE(unchanged);
        const std::array<EntityPropertyKey, 1> owners{{{NODE, 64}}};
        for (const bool includeNoop : {false, true}) {
            const std::array<const HybridPropertyColumnDelta *, 1> noops{&*unchanged};
            const auto changes = includeNoop ? std::span<const HybridPropertyColumnDelta *const>(noops) : std::span<const HybridPropertyColumnDelta *const>{};
            auto prepared = HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, source, fields, changes, owners, memory);
            EXPECT_EQ(prepared.group().live, UINT64_MAX); EXPECT_EQ(prepared.group().memberships, 63U); EXPECT_TRUE(prepared.owners().empty());
            ASSERT_EQ(prepared.pages().size(), 1U); EXPECT_EQ(prepared.pages()[0].readColumn(unchanged->columnReference(),
                unchanged->sourceColumn().identity, prepared.group(), batch.generation()), unchanged->sourceColumn());
        }
        const std::array<EntityPropertyKey, 2> duplicates{{{NODE, 64}, {NODE, 64}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, source, fields, {}, duplicates, memory), FormatException);
        const std::array<EntityPropertyKey, 1> live{{{NODE, 1}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, source, fields, {}, live, memory), FormatException);
    }
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOwnerInsertionPlanTest, PendingDemotionProducesCompleteDirectoriesAndRefundsPartialPreparationOnCancel) {
    seed(NODE, 63, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); install(memory);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        const auto source = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, source);
        HybridPropertyFieldPageStore fields(io_, *metadata_, source); PendingOwnerNoPhysicalAllocator allocator;
        const PropertyValue value("pending"); auto plan = HybridPropertyColumnDelta::planPendingOwnerColumnMember(
            context, source, {NODE, 64}, 1, Generation(before.generation.value() + 1), memory, value); ASSERT_TRUE(plan);
        const std::array<const HybridPropertyColumnDelta *, 1> changes{&*plan}; const std::array<EntityPropertyKey, 1> owners{{{NODE, 64}}};
        const auto retained = memory->used();
        {
            MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1), memory);
            auto prepared = HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, source, fields, changes, owners, memory);
            EXPECT_EQ(prepared.group().live, UINT64_MAX); EXPECT_EQ(prepared.group().memberships, 127U); EXPECT_EQ(prepared.group().columns, 1U);
            EXPECT_EQ(prepared.owners().size(), 64U); EXPECT_EQ(prepared.extensions().size(), 64U); EXPECT_EQ(prepared.erasedColumns().size(), 1U);
        }
        EXPECT_EQ(memory->used(), retained);
        class Cancel final : public CancellationCheckpoint { public: mutable size_t checks = 0;
            void check() const override { if (++checks == 7) throw std::runtime_error("cancel after private clone"); } } cancel;
        {
            MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1), memory);
            EXPECT_THROW((void)HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, source, fields, changes, owners, memory, &cancel), std::runtime_error);
            EXPECT_EQ(cancel.checks, 7U);
        }
        EXPECT_EQ(memory->used(), retained);
        auto tiny = std::make_shared<MaterializedByteBudget>(1024);
        {
            MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1), tiny);
            EXPECT_THROW((void)HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, source, fields, changes, owners, tiny), graph::query::execution::MaterializedByteLimitExceeded);
        }
        EXPECT_EQ(tiny->used(), 0U);
    }
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
#endif

#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyOwnerInsertionPlanTest, PendingNodeAndEdgeGroupsShareOneBoundedPrivatePreparation) {
    seed(EDGE, 63, 1); seed(NODE, 61, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    install(memory); auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true; run(conversion);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        const auto source = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, source);
        HybridPropertyFieldPageStore fields(io_, *metadata_, source); PendingOwnerNoPhysicalAllocator allocator;
        MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1), memory);
        const std::array<EntityPropertyKey, 2> owners{{{NODE, 64}, {EDGE, 64}}};
        const PropertyValue value(int64_t(64)), changed(int64_t(900));
        auto node = HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, source, owners[0], 1, batch.generation(), memory, value);
        auto edge = HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, source, owners[1], 1, batch.generation(), memory, value);
        auto existing = HybridPropertyColumnDelta::planExistingColumn(context, source, {NODE, 3}, 1, batch.generation(), memory, &changed);
        ASSERT_TRUE(node); ASSERT_TRUE(edge); ASSERT_TRUE(existing);
        const std::array<const HybridPropertyColumnDelta *, 2> nodeChanges{&*node, &*existing};
        const auto merged = HybridPropertyColumnDelta::mergeExistingColumnChanges(nodeChanges, memory);
        const std::array<const HybridPropertyColumnDelta *, 2> changes{&merged, &*edge};
        for (const bool withFields : {false, true}) {
            auto prepared = HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, source, fields,
                withFields ? std::span<const HybridPropertyColumnDelta *const>(changes) : std::span<const HybridPropertyColumnDelta *const>{}, owners, memory);
            for (size_t i = 0; i < owners.size(); ++i) {
                SlottedSlotReference reference; const auto old = context.groupRecord(owners[i], {}, &reference); ASSERT_TRUE(old);
                const auto image = std::ranges::find(prepared.pages(), reference.page_id, &HybridPropertyPageImage::pageId); ASSERT_NE(image, prepared.pages().end());
                const auto group = image->readGroup(reference, owners[i].owner_kind, 1, batch.generation());
                EXPECT_EQ(group.live, UINT64_MAX); EXPECT_EQ(group.memberships, old->memberships + size_t(withFields));
                if (withFields) {
                    const auto &plan = i == 0 ? merged : *edge;
                    const auto column = image->readColumn(plan.columnReference(), plan.replacementColumn().identity, group, batch.generation());
                    EXPECT_EQ(column.words[63], 64U); if (!i) EXPECT_EQ(column.words[2], 900U);
                }
            }
        }
    }
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
#endif
