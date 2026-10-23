#include "storage/relocatable/property/HybridPropertyColdRowRoutingTestFixture.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyRowPreparation.hpp"
#include <algorithm>
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "storage/relocatable/property/HybridPropertyNativeWriteTestAccess.hpp"
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class RejectColdHotPhysicalAllocation final : public MetadataPageAllocator {
    MetadataPageId allocateMetadataPageId() override { throw std::logic_error("hot preparation allocated metadata"); }
    MetadataPageAllocation allocatePage(Generation, LogSequenceNumber, std::span<const PhysicalSlotId>) override {
        throw std::logic_error("hot preparation allocated physical storage");
    }
    void abandonPage(PhysicalPageRef) noexcept override {}
};
class HybridPropertyColdCompatibleHotTest : public graph::storage::test::HybridPropertyColdRowRoutingTest {
protected:
    void expectInversePrepared(HybridPropertyReadContext &context, const MetadataSnapshotPin &pin,
        const std::shared_ptr<MaterializedByteBudget> &memory, int64_t newValues, bool hasRows = true) {
        NativeGraphHybridPropertyConversion conversion{owner_kind_, 1, memory, nullptr,
            PhysicalSlotId(40), PhysicalSlotId(80), 902};
        conversion.direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
        struct Counts { int64_t rows = -1, values = -1; } counts;
        graph::debug::PerfTrace::ScopedObserver observe(&counts, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            auto &c = *static_cast<Counts *>(p);
            if (key == "native_graph.hybrid.inverse_private_new_large_values") c.values = value;
            if (key == "native_graph.hybrid.inverse_private_row_frames") c.rows = value;
        });
        try { HybridPropertyNativeWriteTestAccess::prepareInverse(*metadata_, pin, context, conversion); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
            EXPECT_NE(std::string(error.what()).find("inverse native ROW preparation completed"), std::string::npos) << error.what();
        }
        if (hasRows) EXPECT_GT(counts.rows, 0); else EXPECT_EQ(counts.rows, 0);
        EXPECT_EQ(counts.values, newValues);
    }
    const HybridPropertyPageImage &image(const HybridPropertyDemotionPreparation &prepared, PageId id) {
        for (const auto &page : prepared.pages()) if (page.pageId() == id) return page;
        throw std::logic_error("hot preparation lost its page");
    }
};
}
TEST_F(HybridPropertyColdCompatibleHotTest, CompatibleHotValuePreservesUnvisitedColdBytesAndPublicMutationGuard) {
    bad_cold_crc_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const auto before = pin.superblock();
    const auto epoch = io_->dataMutationEpoch(); int64_t decodes = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        EXPECT_THROW(context.groupRecord({NODE, 1}), FormatException);
        EXPECT_THROW(context.visitField({NODE, 1}, 1, [](const auto &) {}, [](const auto &) {}, {}, {},
            [](const auto &, const auto &, const auto &, const auto &) {}), FormatException);
        graph::debug::PerfTrace::ScopedObserver observer(&decodes, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.cold_block_decodes") *static_cast<int64_t *>(p) += value;
        });
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const PropertyValue replacement(int64_t(444));
        auto change = HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1, batch.generation(), memory, &replacement);
        ASSERT_TRUE(change); EXPECT_EQ(change->action(), HybridPropertyColumnDelta::Action::HCD_REPLACE_COLUMN);
        HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
        const HybridPropertyDemotionPreparation prepared(batch, pin, fields, *change, memory);
        ASSERT_EQ(prepared.pages().size(), 1U); EXPECT_TRUE(prepared.owners().empty()); EXPECT_TRUE(prepared.extensions().empty());
        EXPECT_TRUE(prepared.erasedOwners().empty());
        const auto &page = image(prepared, change->groupReference().page_id);
        const auto group = page.readGroup(change->groupReference(), NODE, 1, batch.generation());
        const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
        const auto column = image(prepared, change->columnReference().page_id).readColumn(change->columnReference(),
            change->replacementColumn().identity, group, batch.generation());
        EXPECT_EQ(column.words[0], 444U); EXPECT_EQ(group.memberships, 51U); EXPECT_EQ(matrix.cold_fields, 3U);
        EXPECT_EQ(matrix.cold_rows.roots[0], cold_reference_);
        EXPECT_TRUE(std::ranges::equal(image(prepared, cold_reference_.page_id).readRecord(cold_reference_), cold_encoded_bytes_));
        const auto location = metadata_->findPageLocation(pin, field_page_id_);
        const auto pool = metadata_->findCatalog(pin, location.segment_id);
        const PageOwnerHeader owner{field_page_id_, pool.owner_id, PageOwnerKind::POK_HYBRID_PROPERTY, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
        // Only an actual FieldPageStore clone carries source provenance. A
        // public serialized reopen cannot use it to bypass corrupt cold bytes.
        auto untrusted = HybridPropertyPageImage::open(page.serialize(), field_page_id_, pool.generation, batch.generation(), memory);
        EXPECT_THROW(std::move(untrusted).bindPrivateOwner(owner, pool, batch.checkpointLsn()), FormatException);
        auto trusted = page.clone(batch.generation(), memory);
        const auto bound = std::move(trusted).bindPrivateOwner(owner, pool, batch.checkpointLsn());
        EXPECT_TRUE(std::ranges::equal(bound.read(cold_reference_), cold_encoded_bytes_));
        EXPECT_THROW(HybridPropertyColdRowBlock::decode(bound.read(cold_reference_), NODE, 1, batch.generation(), memory), FormatException);
        EXPECT_EQ(batch.candidate().next_logical_page_id, before.next_logical_page_id); EXPECT_EQ(batch.allocatedPageCount(), 0U);
        EXPECT_EQ(HybridPropertyValueReader::read(context, pin, {NODE, 1}, 1).value->value, PropertyValue(int64_t(100)));
    }
    EXPECT_EQ(decodes, 0); EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, MissingHotNullInsertionOnEdgeKeepsColdBindingAndExactTotals) {
    owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; raw_cold_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const PropertyValue null;
        auto change = HybridPropertyColumnDelta::planMissingColumnMember(context, pin, {owner_kind_, 64}, 1, batch.generation(), memory, null);
        ASSERT_TRUE(change); EXPECT_EQ(change->effects().memberships, 1);
        HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
        const HybridPropertyDemotionPreparation prepared(batch, pin, fields, *change, memory);
        const auto &page = image(prepared, change->groupReference().page_id);
        const auto group = page.readGroup(change->groupReference(), owner_kind_, 1, batch.generation());
        const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
        const auto column = image(prepared, change->columnReference().page_id).readColumn(change->columnReference(),
            change->replacementColumn().identity, group, batch.generation());
        EXPECT_EQ(group.memberships, 52U); EXPECT_EQ(matrix.totalMemberships(), 52U); EXPECT_EQ(matrix.cold_fields, 3U);
        EXPECT_TRUE(matrix.contains({owner_kind_, 64}, 1)); EXPECT_TRUE(column.nulls & (UINT64_C(1) << 63)); EXPECT_EQ(column.words[63], 0U);
        EXPECT_EQ(matrix.cold_rows.roots[0], cold_reference_);
        EXPECT_TRUE(std::ranges::equal(image(prepared, cold_reference_.page_id).readRecord(cold_reference_), cold_encoded_bytes_));
        EXPECT_TRUE(prepared.owners().empty()); EXPECT_TRUE(prepared.erasedOwners().empty()); EXPECT_TRUE(prepared.extensions().empty());
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NoopAndColdOrUnsupportedChangesCannotEscapeAsMutationAuthority) {
    bad_cold_crc_ = true; seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const Generation next(before.generation.value() + 1); const PropertyValue same(int64_t(100)), text("incompatible-hot");
        auto unchanged = HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1, next, memory, &same);
        ASSERT_TRUE(unchanged); EXPECT_EQ(unchanged->action(), HybridPropertyColumnDelta::Action::HCD_NO_CHANGE);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
        const HybridPropertyDemotionPreparation prepared(batch, pin, fields, *unchanged, memory); EXPECT_TRUE(prepared.pages().empty());
        EXPECT_THROW(HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1, next, memory, &text), FormatException);
        EXPECT_THROW(HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1, next, memory, nullptr), FormatException);
        EXPECT_THROW(HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1, next, memory, nullptr, true), FormatException);
        EXPECT_FALSE(HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 2, next, memory, &same));
        EXPECT_FALSE(HybridPropertyColumnDelta::planMissingColumnMember(context, pin, {NODE, 1}, 9, next, memory, same));
        EXPECT_EQ(batch.candidate().next_logical_page_id, before.next_logical_page_id);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, MergedIndividuallyCompatibleRemovalsCannotBecomeUnsupportedDemotion) {
    hot_owner_count_ = 64; seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const Generation next(before.generation.value() + 1);
        std::vector<HybridPropertyColumnDelta> plans; plans.reserve(17);
        for (uint64_t id = 1; id <= 17; ++id) {
            auto change = HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, id}, 1, next, memory, nullptr);
            ASSERT_TRUE(change); EXPECT_EQ(change->action(), HybridPropertyColumnDelta::Action::HCD_REPLACE_COLUMN);
            plans.push_back(std::move(*change));
        }
        std::array<const HybridPropertyColumnDelta *, 17> inputs{};
        for (size_t i = 0; i < plans.size(); ++i) inputs[i] = &plans[i];
        auto merged = HybridPropertyColumnDelta::mergeExistingColumnChanges(std::span(inputs).first(16), memory);
        EXPECT_EQ(merged.action(), HybridPropertyColumnDelta::Action::HCD_REPLACE_COLUMN);
        EXPECT_EQ(std::popcount(merged.replacementColumn().presence), 48);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
        const HybridPropertyDemotionPreparation prepared(batch, pin, fields, merged, memory);
        const auto &page = image(prepared, merged.groupReference().page_id);
        const auto group = page.readGroup(merged.groupReference(), NODE, 1, batch.generation());
        const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(group.memberships, 51U); EXPECT_EQ(matrix.totalMemberships(), 51U); EXPECT_EQ(matrix.cold_fields, 3U);
        EXPECT_TRUE(prepared.owners().empty()); EXPECT_TRUE(prepared.erasedOwners().empty());
        EXPECT_TRUE(std::ranges::equal(image(prepared, cold_reference_.page_id).readRecord(cold_reference_), cold_encoded_bytes_));
        const auto retained = memory->used();
        EXPECT_THROW(HybridPropertyColumnDelta::mergeExistingColumnChanges(inputs, memory), FormatException);
        EXPECT_EQ(memory->used(), retained);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
}
TEST_F(HybridPropertyColdCompatibleHotTest, FreshAndReusedColdSlotsCannotInheritUnchangedSourceProof) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch(); int64_t skipped = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const Generation next(before.generation.value() + 1); const PropertyValue same(int64_t(100));
        const auto change = HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1, next, memory, &same);
        ASSERT_TRUE(change);
        HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
        for (const bool fresh : {false, true}) {
        auto page = fields.cloneForMutation(pin, field_page_id_, next, memory);
        const auto previous = change->sourceGroup(); auto replacement = previous; replacement.semantic_generation = next;
        auto matrix = page.readColumnMembership(previous.column_membership, previous, next); matrix.semantic_generation = next;
        if (!fresh) page.erase(cold_reference_);
        const auto inserted = page.appendColdRows(cold_encoded_bytes_, NODE, 1);
        if (fresh) page.erase(cold_reference_);
        EXPECT_NE(inserted, cold_reference_);
        matrix.cold_rows.roots[0] = matrix.cold_rows.roots[63] = inserted;
        page.replaceColumnMembership(previous.column_membership, matrix, previous, replacement, next);
        page.replaceGroup(change->groupReference(), replacement, next);
        const auto location = metadata_->findPageLocation(pin, field_page_id_); const auto pool = metadata_->findCatalog(pin, location.segment_id);
        const LogSequenceNumber lsn(before.checkpoint_lsn.value() + 1);
        const PageOwnerHeader owner{field_page_id_, pool.owner_id, PageOwnerKind::POK_HYBRID_PROPERTY, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, lsn};
        graph::debug::PerfTrace::ScopedObserver observer(&skipped, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.unchanged_cold_bindings") *static_cast<int64_t *>(p) += value;
        });
        const auto bound = std::move(page).bindPrivateOwner(owner, pool, lsn);
        const auto block = HybridPropertyFieldView::decodeColdRows(bound.read(inserted), inserted, replacement, matrix, next, next, memory);
        EXPECT_EQ(block.fields, 3U); EXPECT_EQ(block.owners, UINT64_C(1) | (UINT64_C(1) << 63));
        }
    }
    EXPECT_EQ(skipped, 0); EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}

TEST_F(HybridPropertyColdCompatibleHotTest, ColdBlockReplacementUpdatesExactMatrixAndRetainsOtherOwnerBytes) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const auto root = EntityPropertyCollectionRootCodec::encode({1, graph::Node::typeId, {{2, PropertyValue("replaced")}}});
        const std::array<HybridColdRowInput, 1> changes{{{0, 1, false, root}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, changes, memory);
        ASSERT_EQ(prepared.pages().size(), 1U); const auto group = prepared.group();
        const auto &page = image(prepared, field_page_id_);
        const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(group.memberships, 50U); EXPECT_EQ(matrix.cold_fields, 2U); EXPECT_EQ(matrix.live, UINT64_MAX);
        EXPECT_EQ(matrix.cold_rows.roots[0], cold_reference_); EXPECT_EQ(matrix.cold_rows.roots[63], cold_reference_);
        const auto block = HybridPropertyFieldView::decodeColdRows(page.readRecord(cold_reference_), cold_reference_, group, matrix,
            batch.generation(), batch.generation(), memory);
        EXPECT_TRUE(std::ranges::equal(block.row(0)->root, root));
        const auto old = HybridPropertyColdRowBlock::decode(cold_encoded_bytes_, NODE, 1, before.generation, memory);
        EXPECT_TRUE(std::ranges::equal(block.row(63)->root, old.row(63)->root));
        const auto location = metadata_->findPageLocation(pin, field_page_id_);
        const auto pool = metadata_->findCatalog(pin, location.segment_id);
        const PageOwnerHeader owner{field_page_id_, pool.owner_id, PageOwnerKind::POK_HYBRID_PROPERTY, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
        const auto bound = std::move(page.clone(batch.generation(), memory)).bindPrivateOwner(owner, pool, batch.checkpointLsn());
        EXPECT_TRUE(std::ranges::equal(bound.read(cold_reference_), page.readRecord(cold_reference_)));
        EXPECT_TRUE(prepared.owners().empty()); EXPECT_TRUE(prepared.extensions().empty());
        EXPECT_EQ(batch.allocatedPageCount(), 0U); EXPECT_EQ(batch.candidate().next_logical_page_id, before.next_logical_page_id);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ClearingAllColdRowsErasesBlockWithoutChangingOwnerLiveness) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<HybridColdRowInput, 2> changes{{{0, 0, false, {}}, {63, 0, false, {}}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, changes, memory);
        const auto group = prepared.group(); const auto &page = image(prepared, field_page_id_);
        const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(group.memberships, 48U); EXPECT_EQ(group.live, UINT64_MAX);
        EXPECT_EQ(matrix.cold_fields, 0U); EXPECT_EQ(matrix.cold_rows.owners, 0U);
        EXPECT_EQ(matrix.cold_rows.roots[0], SlottedSlotReference{});
        EXPECT_EQ(matrix.cold_rows.roots[63], SlottedSlotReference{});
        EXPECT_THROW((void)page.readRecord(cold_reference_), FormatException);
        EXPECT_TRUE(prepared.erasedGroups(NODE).empty());
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdBlockPreparationDoesNotDecodeUnrelatedDamagedBlock) {
    split_cold_ = true; bad_cold_crc_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const PropertyValue same(int64_t(100));
        const auto hot = HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1, batch.generation(), memory, &same);
        ASSERT_TRUE(hot); HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
        const auto original = fields.cloneForMutation(pin, field_page_id_, batch.generation(), memory);
        const auto matrix = original.readColumnMembership(hot->sourceGroup().column_membership, hot->sourceGroup(), batch.generation());
        const auto target = matrix.cold_rows.roots[63];
        const auto root = EntityPropertyCollectionRootCodec::encode({64, graph::Node::typeId, {{2, PropertyValue()}}});
        const std::array<HybridColdRowInput, 1> changes{{{63, 1, false, root}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 64}, target, changes, memory);
        const auto &page = image(prepared, field_page_id_);
        EXPECT_TRUE(std::ranges::equal(page.readRecord(cold_reference_), cold_encoded_bytes_));
        EXPECT_THROW(HybridPropertyColdRowBlock::decode(page.readRecord(cold_reference_), NODE, 1, batch.generation(), memory), FormatException);
        EXPECT_EQ(prepared.group().memberships, 51U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdBlockPreparationRejectsHotTokenAndDuplicateChangesWithoutWrites) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const auto root = EntityPropertyCollectionRootCodec::encode({1, graph::Node::typeId, {{1, PropertyValue(int64_t(9))}}});
        const std::array<HybridColdRowInput, 1> changes{{{0, 1, false, root}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, changes, memory), FormatException);
        const std::array<HybridColdRowInput, 2> duplicate{{{0, 0, false, {}}, {0, 0, false, {}}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, duplicate, memory), FormatException);
        EXPECT_EQ(batch.candidate().next_logical_page_id, before.next_logical_page_id);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdBlockPreparationAuthenticatesExistingActualLargeRootBeforeReplacement) {
    seed(true); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const auto root = EntityPropertyCollectionRootCodec::encode({1, graph::Node::typeId, {{2, PropertyValue()}}});
        const std::array<HybridColdRowInput, 1> changes{{{0, 1, false, root}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, changes, memory, values_.get());
        EXPECT_EQ(prepared.group().memberships, 50U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}

TEST_F(HybridPropertyColdCompatibleHotTest, ColdBlockInsertionOnExistingEdgePreservesEntityLivenessAndHotNull) {
    owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const auto root = EntityPropertyCollectionRootCodec::encode({2, graph::Edge::typeId, {{2, PropertyValue()}}});
        const std::array<HybridColdRowInput, 1> changes{{{1, 1, false, root}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {owner_kind_, 1}, cold_reference_, changes, memory);
        const auto group = prepared.group(); const auto &page = image(prepared, field_page_id_);
        const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(group.live, UINT64_MAX); EXPECT_EQ(group.memberships, 52U); EXPECT_EQ(matrix.cold_fields, 4U);
        EXPECT_EQ(matrix.cold_rows.owners, UINT64_C(3) | (UINT64_C(1) << 63));
        const auto block = HybridPropertyFieldView::decodeColdRows(page.readRecord(cold_reference_), cold_reference_, group, matrix,
            batch.generation(), batch.generation(), memory);
        ASSERT_TRUE(block.row(1)); EXPECT_TRUE(std::ranges::equal(block.row(1)->root, root));
        EXPECT_EQ(batch.allocatedPageCount(), 0U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdOnlyClearRetainsPropertylessLiveOwners) {
    cold_only_ = true; seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<HybridColdRowInput, 2> changes{{{0, 0, false, {}}, {63, 0, false, {}}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, changes, memory);
        const auto group = prepared.group(); const auto &page = image(prepared, field_page_id_);
        const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(group.memberships, 0U); EXPECT_EQ(group.columns, 0U); EXPECT_EQ(group.live, UINT64_MAX);
        EXPECT_EQ(matrix.cold_rows.owners, 0U); EXPECT_TRUE(prepared.erasedGroups(NODE).empty());
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdBlockPreparationRejectsCompetingOrdinaryRow) {
    competing_row_ = true; seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<HybridColdRowInput, 1> changes{{{0, 0, false, {}}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, changes, memory), FormatException);
        EXPECT_EQ(batch.allocatedPageCount(), 0U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}

TEST_F(HybridPropertyColdCompatibleHotTest, SameBatchColdReplacementRetainsHotValueAndPresenceChangesOnSharedPage) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        for (const bool addPresence : {false, true}) {
            RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
            const PropertyValue value(int64_t(444)), null;
            auto hot = addPresence ? HybridPropertyColumnDelta::planMissingColumnMember(context, pin, {NODE, 64}, 1, batch.generation(), memory, null) :
                HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1, batch.generation(), memory, &value);
            ASSERT_TRUE(hot); const std::array<const HybridPropertyColumnDelta *, 1> hotChanges{&*hot};
            const auto root = EntityPropertyCollectionRootCodec::encode({1, graph::Node::typeId, {{2, PropertyValue()}}});
            const std::array<HybridColdRowInput, 1> changes{{{0, 1, false, root}}};
            const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlockWithHot(batch, context, hotChanges,
                {NODE, 1}, cold_reference_, changes, memory);
            ASSERT_EQ(prepared.pages().size(), 1U); const auto group = prepared.group(); const auto &page = image(prepared, field_page_id_);
            const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
            EXPECT_EQ(group.memberships, addPresence ? 51U : 50U); EXPECT_EQ(matrix.cold_fields, 2U);
            EXPECT_EQ(group.live, UINT64_MAX); EXPECT_EQ(matrix.totalMemberships(), group.memberships);
            const auto column = page.readColumn(hot->columnReference(), hot->replacementColumn().identity, group, batch.generation());
            if (addPresence) {
                EXPECT_TRUE(column.presence & (UINT64_C(1) << 63)); EXPECT_TRUE(column.nulls & (UINT64_C(1) << 63));
                EXPECT_TRUE(matrix.contains({NODE, 64}, 1)); EXPECT_EQ(column.words[0], 100U);
            } else EXPECT_EQ(column.words[0], 444U);
            const auto block = HybridPropertyFieldView::decodeColdRows(page.readRecord(cold_reference_), cold_reference_, group, matrix,
                batch.generation(), batch.generation(), memory);
            EXPECT_TRUE(std::ranges::equal(block.row(0)->root, root));
            EXPECT_EQ(matrix.cold_rows.roots[0], cold_reference_); EXPECT_EQ(batch.allocatedPageCount(), 0U);
        }
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}

TEST_F(HybridPropertyColdCompatibleHotTest, ColdGrowthSplitsAndRelocatesMatrixWithGroupDirectoryEdit) {
    split_cold_ = true; raw_cold_ = true; last_cold_text_bytes_ = 2200; seed();
    auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    std::vector<std::vector<std::byte>> roots; std::vector<HybridColdRowInput> changes;
    roots.reserve(63); changes.reserve(63);
    for (unsigned slot = 0; slot < 63; ++slot) {
        std::string text(2100, 'x'); uint32_t state = 17321 + slot;
        for (auto &c : text) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; c = char(33 + state % 90); }
        roots.push_back(EntityPropertyCollectionRootCodec::encode({slot + 1U, graph::Node::typeId, {{2, PropertyValue(text)}}}));
        changes.push_back({uint8_t(slot), 1, false, roots.back()});
    }
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const PropertyValue same(int64_t(100)); const Generation next(before.generation.value() + 1);
        const auto probe = HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1, next, memory, &same);
        ASSERT_TRUE(probe); HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
        const auto initialImage = fields.cloneForMutation(pin, field_page_id_, next, memory);
        const auto initialMatrix = initialImage.readColumnMembership(probe->sourceGroup().column_membership, probe->sourceGroup(), next);
        const auto lastRoot = EntityPropertyCollectionRootCodec::encode({64, graph::Node::typeId, {{2, PropertyValue()}}});
        const std::array<HybridColdRowInput, 1> lastChange{{{63, 1, false, lastRoot}}};
        for (const bool modifySecond : {false, true}) {
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const auto prepared = [&] {
            if (!modifySecond) return HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, changes, memory);
            const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 2> blocks{{
                {{NODE, 1}, cold_reference_, changes}, {{NODE, 64}, initialMatrix.cold_rows.roots[63], lastChange}}};
            return HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory);
        }();
        const auto group = prepared.group();
        ASSERT_EQ(prepared.groups(NODE).size(), 1U); EXPECT_TRUE(prepared.groups(EntityPropertyOwnerKind::EPOK_EDGE).empty());
        EXPECT_NE(group.column_membership.page_id, field_page_id_);
        const auto groupReference = HybridPropertyDirectoryCodec::decodeLocator(prepared.groups(NODE).front().value);
        EXPECT_EQ(groupReference.page_id, group.column_membership.page_id);
        const auto &groupPage = image(prepared, groupReference.page_id);
        EXPECT_EQ(groupPage.readGroup(groupReference, NODE, 1, batch.generation()), group);
        const auto matrix = groupPage.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.cold_fields, 64U); EXPECT_EQ(matrix.cold_rows.owners, UINT64_MAX);
        EXPECT_EQ(group.memberships, 112U); EXPECT_EQ(group.live, UINT64_MAX);
        for (unsigned slot = 0; slot < 63; ++slot) {
            const auto reference = matrix.cold_rows.roots[slot]; EXPECT_NE(reference.page_id, field_page_id_);
            const auto &page = image(prepared, reference.page_id);
            const auto block = HybridPropertyFieldView::decodeColdRows(page.readRecord(reference), reference, group, matrix,
                batch.generation(), batch.generation(), memory);
            ASSERT_TRUE(block.row(slot)); EXPECT_TRUE(std::ranges::equal(block.row(slot)->root, roots[slot]));
        }
        EXPECT_EQ(matrix.cold_rows.roots[63].page_id, field_page_id_);
        if (modifySecond) {
            const auto reference = matrix.cold_rows.roots[63];
            const auto block = HybridPropertyFieldView::decodeColdRows(image(prepared, reference.page_id).readRecord(reference), reference,
                group, matrix, batch.generation(), batch.generation(), memory);
            EXPECT_TRUE(std::ranges::equal(block.row(63)->root, lastRoot));
        }
        EXPECT_THROW((void)image(prepared, field_page_id_).readRecord(cold_reference_), FormatException);
        EXPECT_EQ(batch.allocatedPageCount(), 0U);
        EXPECT_GT(batch.candidate().next_logical_page_id, before.next_logical_page_id);
        }
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}

TEST_F(HybridPropertyColdCompatibleHotTest, MultipleColdBlocksAndHotPresenceShareOneFinalMatrix) {
    split_cold_ = true; seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const PropertyValue null;
        auto hot = HybridPropertyColumnDelta::planMissingColumnMember(context, pin, {NODE, 64}, 1, batch.generation(), memory, null);
        ASSERT_TRUE(hot); const std::array<const HybridPropertyColumnDelta *, 1> hotChanges{&*hot};
        HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
        const auto initial = fields.cloneForMutation(pin, field_page_id_, batch.generation(), memory);
        const auto sourceMatrix = initial.readColumnMembership(hot->sourceGroup().column_membership, hot->sourceGroup(), batch.generation());
        const std::array<HybridColdRowInput, 1> first{{{0, 0, false, {}}}}, last{{{63, 0, false, {}}}};
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 2> blocks{{
            {{NODE, 1}, cold_reference_, first}, {{NODE, 64}, sourceMatrix.cold_rows.roots[63], last}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory, hotChanges);
        const auto group = prepared.group(); const auto &page = image(prepared, field_page_id_);
        const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(group.memberships, 49U); EXPECT_EQ(matrix.totalMemberships(), 49U);
        EXPECT_EQ(matrix.cold_fields, 0U); EXPECT_EQ(matrix.cold_rows.owners, 0U); EXPECT_TRUE(matrix.contains({NODE, 64}, 1));
        EXPECT_EQ(group.live, UINT64_MAX); EXPECT_EQ(prepared.pages().size(), 1U);
        const std::array duplicate{blocks[0], blocks[0]};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, duplicate, memory), FormatException);
        EXPECT_EQ(batch.allocatedPageCount(), 0U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}

TEST_F(HybridPropertyColdCompatibleHotTest, FreshColdBlockInsertionRetainsExistingColdOwnersAndHotAuthority) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const auto root = EntityPropertyCollectionRootCodec::encode({2, graph::Node::typeId, {{2, PropertyValue()}}});
        const std::array<HybridColdRowInput, 1> changes{{{1, 1, false, root}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 2}, {}, changes, memory);
        const auto group = prepared.group();
        const auto &groupPage = image(prepared, group.column_membership.page_id);
        const auto matrix = groupPage.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(group.memberships, 52U); EXPECT_EQ(matrix.cold_fields, 4U); EXPECT_EQ(group.live, UINT64_MAX);
        EXPECT_EQ(matrix.cold_rows.roots[0], cold_reference_); EXPECT_EQ(matrix.cold_rows.roots[63], cold_reference_);
        EXPECT_TRUE(matrix.contains({NODE, 2}, 1)); const auto reference = matrix.cold_rows.roots[1];
        EXPECT_NE(reference.page_id, field_page_id_);
        const auto block = HybridPropertyFieldView::decodeColdRows(image(prepared, reference.page_id).readRecord(reference), reference,
            group, matrix, batch.generation(), batch.generation(), memory);
        EXPECT_EQ(block.owners, 2U); EXPECT_TRUE(std::ranges::equal(block.row(1)->root, root));
        EXPECT_TRUE(std::ranges::equal(image(prepared, field_page_id_).readRecord(cold_reference_), cold_encoded_bytes_));
        const std::array<HybridColdRowInput, 1> overwrite{{{0, 1, false, root}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, {}, overwrite, memory), FormatException);
        EXPECT_EQ(batch.allocatedPageCount(), 0U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}

TEST_F(HybridPropertyColdCompatibleHotTest, FirstColdFieldOnPropertylessLiveGroupNeedsNoFabricatedSourceBlock) {
    omit_cold_ = true; cold_only_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const auto root = EntityPropertyCollectionRootCodec::encode({2, graph::Node::typeId, {{7, PropertyValue()}}});
        const std::array<HybridColdRowInput, 1> changes{{{1, 1, false, root}}};
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 1> blocks{{{{NODE, 2}, {}, changes}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory);
        const auto group = prepared.group(); const auto &page = image(prepared, group.column_membership.page_id);
        const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(group.memberships, 1U); EXPECT_EQ(group.columns, 0U); EXPECT_EQ(group.live, UINT64_MAX);
        EXPECT_EQ(matrix.cold_rows.owners, 2U); EXPECT_EQ(matrix.cold_fields, 1U);
        EXPECT_EQ(context.hasFields({NODE, 2}), std::optional<bool>(false)); EXPECT_EQ(batch.allocatedPageCount(), 0U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}

TEST_F(HybridPropertyColdCompatibleHotTest, ColdInverseProjectionRetainsActualLargeNullAndPropertylessNodeRows) {
    seed(true); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const HybridPropertyRowPreparation projection(context, pin, *metadata_, *entities_, NODE, 1, memory, values_.get());
        ASSERT_EQ(projection.rows().size(), 64U); EXPECT_EQ(projection.sourceGroup().memberships, 51U);
        size_t fields = 0;
        for (size_t i = 0; i < projection.rows().size(); ++i) {
            const auto &row = projection.rows()[i]; EXPECT_EQ(row.owner_entity_id, i + 1U);
            EXPECT_EQ(row.owner_entity_type, graph::Node::typeId); fields += row.entries.size();
        }
        EXPECT_EQ(fields, 51U); ASSERT_EQ(projection.rows()[0].entries.size(), 3U);
        EXPECT_EQ(projection.rows()[0].entries[2].key_token, 4U);
        EXPECT_EQ(std::get<std::string>(projection.rows()[0].entries[2].value.getVariant()).size(), 8193U);
        ASSERT_EQ(projection.rows()[2].entries.size(), 1U);
        EXPECT_TRUE(std::holds_alternative<std::monostate>(projection.rows()[2].entries[0].value.getVariant()));
        EXPECT_TRUE(projection.rows()[48].entries.empty());
        EXPECT_THROW((void)context.groupRecord({NODE, 1}), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdInverseProjectionPreservesEdgeOwnerNamespaceAndExactCounts) {
    owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const HybridPropertyRowPreparation projection(context, pin, *metadata_, *entities_, owner_kind_, 1, memory);
        ASSERT_EQ(projection.rows().size(), 64U); size_t fields = 0;
        for (const auto &row : projection.rows()) { EXPECT_EQ(row.owner_entity_type, graph::Edge::typeId); fields += row.entries.size(); }
        EXPECT_EQ(fields, 51U); EXPECT_EQ(projection.rows().back().owner_entity_id, 64U);
        EXPECT_EQ(projection.sourceGroup().owner_kind, owner_kind_);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdInverseProjectionRetainsAllPropertylessLiveOwners) {
    cold_only_ = true; omit_cold_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const HybridPropertyRowPreparation projection(context, pin, *metadata_, *entities_, NODE, 1, memory);
        ASSERT_EQ(projection.rows().size(), 64U);
        for (const auto &row : projection.rows()) EXPECT_TRUE(row.entries.empty());
        EXPECT_EQ(projection.sourceGroup().memberships, 0U); EXPECT_EQ(projection.sourceGroup().live, UINT64_MAX);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdInverseProjectionRejectsAccessedCorruptionAndRefunds) {
    bad_cold_crc_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        EXPECT_THROW((void)HybridPropertyRowPreparation(context, pin, *metadata_, *entities_, NODE, 1, memory), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}

TEST_F(HybridPropertyColdCompatibleHotTest, ColdRootUsesSameBatchImmutableProposalWithoutReadingUnpublishedManifest) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        graph::utils::OwnedHeapReservation stores(memory, 16U * 1024U);
        LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const PropertyCollectionRoot root{1, graph::Node::typeId, {{2, PropertyValue("new-id")}, {4, PropertyValue(std::string(8193, 'y'))}}};
        const auto proposal = HybridPropertyLargeFieldPreparation::prepareRoot(*metadata_, pin, values, {NODE, 1}, root,
            LargeValueId(before.next_large_value_id), memory);
        ASSERT_FALSE(proposal.reused()); const std::span proofs(&proposal, 1);
        const auto encoded = EntityPropertyLargeValueReferenceCodec::encode({1, graph::Node::typeId, proposal.reference()});
        const std::array<HybridColdRowInput, 1> changes{{{0, 2, true, encoded}}};
        for (const bool supplied : {false, true}) {
            RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
            EXPECT_EQ(batch.allocateLargeValueId(), proposal.reference().value_id);
            if (!supplied) {
                EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_,
                    changes, memory, &values), FormatException);
                continue;
            }
            const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_,
                changes, memory, &values, nullptr, proofs);
            const auto group = prepared.group(); const auto &page = image(prepared, field_page_id_);
            const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
            const auto block = HybridPropertyFieldView::decodeColdRows(page.readRecord(cold_reference_), cold_reference_, group, matrix,
                batch.generation(), batch.generation(), memory);
            ASSERT_TRUE(block.row(0)); EXPECT_TRUE(block.row(0)->large_value_reference);
            EXPECT_TRUE(std::ranges::equal(block.row(0)->root, encoded)); EXPECT_EQ(group.memberships, 51U);
            EXPECT_EQ(batch.allocatedPageCount(), 0U);
        }
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdRootCarrierProofPreservesFieldRootDedupButRejectsHotTokenAndUnreservedIds) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        graph::utils::OwnedHeapReservation stores(memory, 16U * 1024U);
        LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        for (const uint32_t token : {4U, 1U}) {
            const PropertyValue value(std::string(8193, 'z'));
            const auto proposal = HybridPropertyLargeFieldPreparation::prepare(*metadata_, pin, values, {NODE, 1}, token, value,
                LargeValueId(before.next_large_value_id), memory);
            ASSERT_FALSE(proposal.reused()); const std::span proofs(&proposal, 1);
            const auto encoded = EntityPropertyLargeValueReferenceCodec::encode({1, graph::Node::typeId, proposal.reference()});
            const std::array<HybridColdRowInput, 1> changes{{{0, 1, true, encoded}}};
            {
                RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch unreserved(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
                EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlock(unreserved, context, {NODE, 1}, cold_reference_,
                    changes, memory, &values, nullptr, proofs), FormatException);
            }
            RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
            EXPECT_EQ(batch.allocateLargeValueId(), proposal.reference().value_id);
            if (token == 1U) {
                EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_,
                    changes, memory, &values, nullptr, proofs), FormatException);
            } else {
                // Field and whole-root carriers use identical owner-neutral
                // bytes. Original batch dedup may select this field proposal.
                const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_,
                    changes, memory, &values, nullptr, proofs);
                EXPECT_EQ(prepared.group().memberships, 50U);
            }
            EXPECT_EQ(batch.allocatedPageCount(), 0U);
        }
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeCoordinatorPreparesCompatibleHotFieldsAndRefusesBeforePhysicalWrites) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    const auto epoch = io_->dataMutationEpoch(); const auto originalBytes = io_->fileSize();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        for (const uint64_t owner : {1U, 49U}) {
            const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, owner}, 1, int64_t(444)}}};
            NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
                before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
            request.field_lifecycle = true;
            int64_t prepared = 0;
            graph::debug::PerfTrace::ScopedObserver observe(&prepared, [](void *p, std::string_view key, int64_t count, bool) noexcept {
                if (key == "native_graph.hybrid.private_field_coordinator_frames") *static_cast<int64_t *>(p) += count;
            });
            try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
            catch (const FormatException &error) {
                EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
                EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
            }
            EXPECT_GT(prepared, 0);
            EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
            EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), originalBytes);
            EXPECT_THROW(context.groupRecord({NODE, owner}), FormatException);
        }
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeCoordinatorPrivateSourceRejectsAnotherPinAndAccessedEntityConflict) {
    competing_row_ = true; seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); auto otherPin = metadata_->openSnapshot();
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, int64_t(444)}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true; int64_t prepared = 0;
        graph::debug::PerfTrace::ScopedObserver observe(&prepared, [](void *p, std::string_view key, int64_t count, bool) noexcept {
            if (key == "native_graph.hybrid.private_field_coordinator_frames") *static_cast<int64_t *>(p) += count;
        });
        EXPECT_THROW(HybridPropertyNativeWriteTestAccess::prepare(*metadata_, otherPin, context, request), FormatException);
        EXPECT_THROW(HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request), FormatException);
        EXPECT_EQ(prepared, 0); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
        EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeCoordinatorMergesColdChangesWithHotNullMissingAndBlockRemoval) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<NativeHybridPropertyColumnReplacement, 4> mixed{{
            {{NODE, 1}, 1, int64_t(777)}, {{NODE, 2}, 1, std::monostate{}},
            {{NODE, 1}, 2, int64_t(333)}, {{NODE, 1}, 4, std::monostate{}, true}}};
        const std::array<NativeHybridPropertyColumnReplacement, 1> fresh{{{{NODE, 49}, 5, std::monostate{}}}};
        const std::array<NativeHybridPropertyColumnReplacement, 2> removed{{
            {{NODE, 1}, 2, std::monostate{}, true}, {{NODE, 1}, 4, std::monostate{}, true}}};
        const auto prepare = [&](std::span<const NativeHybridPropertyColumnReplacement> cells) {
            NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
                before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
            request.field_lifecycle = true; int64_t frames = 0;
            graph::debug::PerfTrace::ScopedObserver observe(&frames, [](void *p, std::string_view key, int64_t count, bool) noexcept {
                if (key == "native_graph.hybrid.private_field_coordinator_frames") *static_cast<int64_t *>(p) += count;
            });
            try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
            catch (const FormatException &error) {
                EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
                EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
            }
            EXPECT_GT(frames, 0);
        };
        prepare(mixed); prepare(fresh); prepare(removed);
        const std::array<NativeHybridPropertyColumnReplacement, 1> noop{{{{NODE, 49}, 5, std::monostate{}, true}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, noop, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true;
        EXPECT_NO_THROW(HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request));
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
        const auto old = HybridPropertyValueReader::readCollection(context, pin, {NODE, 1}, graph::Node::typeId);
        ASSERT_TRUE(old.value); EXPECT_EQ(old.value->entries[0].value, PropertyValue(int64_t(100)));
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeCoordinatorSharesOneNewColdRootAcrossOwnersInOriginalImmutablePlanning) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    const auto fileBytes = io_->fileSize(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const graph::OwnedPropertyValue value(PropertyValue(std::string(8193, 'q')));
        const std::array<NativeHybridPropertyColumnReplacement, 3> cells{{
            {{NODE, 1}, 2, value}, {{NODE, 1}, 4, std::monostate{}, true}, {{NODE, 64}, 2, value}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true;
        struct Counts { int64_t values = 0, frames = 0; } counts;
        graph::debug::PerfTrace::ScopedObserver observe(&counts, [](void *p, std::string_view key, int64_t count, bool) noexcept {
            auto &c = *static_cast<Counts *>(p);
            if (key == "native_graph.hybrid.private_new_field_values") c.values = count;
            if (key == "native_graph.hybrid.private_field_coordinator_frames") c.frames = count;
        });
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_EQ(counts.values, 1); EXPECT_GT(counts.frames, 0);
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
        EXPECT_EQ(io_->fileSize(), fileBytes);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeCoordinatorRetainsGroupOnlyDirectoryEditsAfterColdGrowth) {
    split_cold_ = true; raw_cold_ = true; last_cold_text_bytes_ = 2200;
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        graph::utils::OwnedVector<NativeHybridPropertyColumnReplacement> cells{
            graph::utils::OwnedPmrAllocator<NativeHybridPropertyColumnReplacement>(memory)};
        cells.reserve(64);
        for (uint64_t id = 1; id <= 63; ++id) {
            std::string text(2100, 'x'); uint32_t state = uint32_t(17320 + id);
            for (auto &c : text) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; c = char(33 + state % 90); }
            cells.push_back({{NODE, id}, 2, graph::OwnedPropertyValue(PropertyValue(std::move(text)))});
        }
        cells.push_back({{NODE, 1}, 4, std::monostate{}, true});
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true;
        struct Counts { int64_t frames = 0, directories = 0; } counts;
        graph::debug::PerfTrace::ScopedObserver observe(&counts, [](void *p, std::string_view key, int64_t count, bool) noexcept {
            auto &c = *static_cast<Counts *>(p);
            if (key == "native_graph.hybrid.private_field_coordinator_frames") c.frames = count;
            if (key == "native_graph.hybrid.private_field_coordinator_directories") c.directories = count;
        });
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_GE(counts.frames, 63); EXPECT_GT(counts.directories, 0);
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeCoordinatorUsesColdSourceForRealCatalogIndexesAndRequiredFields) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    SystemCatalog catalog(*btree_pages_, *metadata_); uint64_t person = 0;
    {
        RecordProperty("schema_stage", "tokens");
        auto tx = planner.beginTransaction(); auto schema = catalog.beginWrite(tx);
        EXPECT_EQ(schema.ensurePropertyToken("hot"), 1U); EXPECT_EQ(schema.ensurePropertyToken("cold"), 2U);
        EXPECT_EQ(schema.ensurePropertyToken("unused"), 3U); EXPECT_EQ(schema.ensurePropertyToken("nullable"), 4U);
        person = schema.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
        schema.setGraphCardinalityStatistics({64, 0}); schema.finish(); ASSERT_TRUE(tx.commit().committed());
    }
    {
        RecordProperty("schema_stage", "labels");
        auto tx = planner.beginTransaction(); const std::array labels{person}; tx.replaceNodeLabels(1, labels);
        ASSERT_TRUE(tx.commit().committed());
    }
    IndexDefinition hot, cold;
    {
        RecordProperty("schema_stage", "definitions-and-keys");
        auto tx = planner.beginTransaction(); auto schema = catalog.beginWrite(tx);
        hot.name = "hot_values"; hot.kind = IndexDefinitionKind::IDK_PROPERTY; hot.property_names = {"hot"};
        cold.name = "cold_values"; cold.kind = IndexDefinitionKind::IDK_PROPERTY; cold.property_names = {"cold"};
        RecordProperty("schema_stage", "index-definitions");
        hot = schema.createIndexDefinition(hot); cold = schema.createIndexDefinition(cold);
        ConstraintDefinition required; required.name = "required_cold"; required.kind = ConstraintDefinitionKind::CDK_NOT_NULL;
        required.schema_token_id = person; required.property_names = {"cold"};
        RecordProperty("schema_stage", "constraint-definition");
        static_cast<void>(schema.createConstraintDefinition(required)); schema.finish();
        RecordProperty("schema_stage", "index-keys");
        // Populate the actual index trees with this candidate's authenticated
        // field values. This is a coherent private candidate, not public adoption.
        std::vector<BTreePageEntry> hotEntries, coldEntries;
        for (uint64_t id = 1; id <= 48; ++id) if (id != 3) {
            const auto key = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(id + 99)), id);
            hotEntries.push_back({graph::utils::OwnedStdVector<std::byte>(key.begin(), key.end()), id});
        }
        for (const uint64_t id : {1U, 64U}) {
            const auto key = IndexKeyCodec::encodeEntryKey(PropertyValue(id == 1 ? "user-000000" : "user-000063"), id);
            coldEntries.push_back({graph::utils::OwnedStdVector<std::byte>(key.begin(), key.end()), id});
        }
        tx.stageScalarIndexCreate(*btree_pages_, hot.index_id, hotEntries);
        tx.stageScalarIndexCreate(*btree_pages_, cold.index_id, coldEntries);
        RecordProperty("schema_stage", "definition-commit");
        ASSERT_TRUE(tx.commit().committed());
    }
    RecordProperty("schema_stage", "coordinator");
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    const auto fileBytes = io_->fileSize(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    descriptor_.semantic_generation = before.generation;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const auto old = HybridPropertyValueReader::read(context, pin, {NODE, 1}, 2);
        ASSERT_TRUE(old.value); EXPECT_EQ(old.value->value, PropertyValue("user-000000"));
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, int64_t(444)}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true;
        struct Counts { int64_t scalar = 0, frames = 0; } counts;
        {
            graph::debug::PerfTrace::ScopedObserver observe(&counts, [](void *p, std::string_view key, int64_t count, bool) noexcept {
                auto &c = *static_cast<Counts *>(p);
                if (key == "native_graph.hybrid.private_scalar_index_images") c.scalar = count;
                if (key == "native_graph.hybrid.private_field_coordinator_frames") c.frames = count;
            });
            try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
            catch (const FormatException &error) {
                EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
                EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
            }
        }
        EXPECT_GT(counts.scalar, 0); EXPECT_GT(counts.frames, 0);
        const std::array<NativeHybridPropertyColumnReplacement, 1> remove{{{{NODE, 1}, 2, std::monostate{}, true}}};
        request.replacements = remove;
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const std::runtime_error &error) {
            EXPECT_NE(std::string(error.what()).find("Constraint violation"), std::string::npos) << error.what();
        }
        const auto oldKey = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(100)), 1);
        const auto newKey = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(444)), 1);
        EXPECT_EQ(BTreeIndex(hot.index_id, *btree_pages_, *metadata_).find(pin, oldKey), std::optional<uint64_t>(1));
        EXPECT_FALSE(BTreeIndex(hot.index_id, *btree_pages_, *metadata_).find(pin, newKey));
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
        EXPECT_EQ(io_->fileSize(), fileBytes); EXPECT_THROW(context.groupRecord({NODE, 1}), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdCompatibleHotTest, WholeColumnDemotionPreservesAll64OwnerValuesAndColdBindings) {
    hot_owner_count_ = 64; seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    const auto epoch = io_->dataMutationEpoch(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const PropertyValue value("new-category");
        EXPECT_THROW((void)HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1,
            Generation(before.generation.value() + 1), memory, &value), FormatException);
        auto plan = HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1,
            Generation(before.generation.value() + 1), memory, &value, false, RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, true);
        ASSERT_TRUE(plan); ASSERT_EQ(plan->action(), HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD);
        const std::array<const HybridPropertyColumnDelta *, 1> demotions{&*plan};
        graph::utils::OwnedVector<std::pmr::vector<std::byte>> encoded{
            graph::utils::OwnedPmrAllocator<std::pmr::vector<std::byte>>(memory)};
        graph::utils::OwnedVector<HybridColdRowInput> original, fresh;
        encoded.reserve(64); original.reserve(2); fresh.reserve(62);
        for (uint64_t id = 1; id <= 64; ++id) {
            const PropertyValue scalar = id == 1 ? value : id == 3 ? PropertyValue() : PropertyValue(int64_t(id + 99));
            const PropertyValue name(id == 1 ? "user-000000" : "user-000063"), null;
            std::array<BorrowedPropertyCollectionRootEntry, 3> entries{{{1, &scalar}, {2, &name}, {4, &null}}};
            const size_t count = id == 1 ? 3U : id == 64 ? 2U : 1U;
            encoded.push_back(EntityPropertyCollectionRootCodec::encodeBorrowed(id, graph::Node::typeId,
                std::span(entries).first(count), *memory));
            HybridColdRowInput row{uint8_t(id - 1), uint32_t(count), false, encoded.back()};
            (id == 1 || id == 64 ? original : fresh).push_back(row);
        }
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 2> blocks{{
            {{NODE, 1}, cold_reference_, original}, {{NODE, 2}, {}, fresh}}};
        {
            RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before,
                LogSequenceNumber(before.checkpoint_lsn.value() + 1));
            const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks,
                memory, {}, nullptr, nullptr, {}, demotions);
            const auto group = prepared.group(); EXPECT_EQ(group.columns, 0U); EXPECT_EQ(group.memberships, 67U);
            EXPECT_EQ(group.live, UINT64_MAX);
            const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(
                group.column_membership, group, batch.generation());
            EXPECT_EQ(matrix.column_count, 0U); EXPECT_EQ(matrix.cold_fields, 67U); EXPECT_EQ(matrix.cold_rows.owners, UINT64_MAX);
            EXPECT_THROW(image(prepared, plan->columnReference().page_id).readRecord(plan->columnReference()), FormatException);
            for (unsigned slot = 0; slot < 64; ++slot) {
                const auto ref = matrix.cold_rows.roots[slot];
                const auto block = HybridPropertyFieldView::decodeColdRows(image(prepared, ref.page_id).readRecord(ref), ref,
                    group, matrix, batch.generation(), batch.generation(), memory);
                ASSERT_TRUE(block.row(slot)); const auto root = EntityPropertyCollectionRootCodec::decode(block.row(slot)->root);
                EXPECT_EQ(root.owner_entity_id, slot + 1U);
                ASSERT_FALSE(root.entries.empty()); EXPECT_EQ(root.entries[0].key_token, 1U);
                EXPECT_EQ(root.entries[0].value, slot == 0 ? value : slot == 2 ? PropertyValue() : PropertyValue(int64_t(slot + 100)));
                EXPECT_EQ(root.entries.size(), slot == 0 ? 3U : slot == 63 ? 2U : 1U);
            }
        }
        {
            const PropertyValue invalidValue(int64_t(77));
            const std::array<BorrowedPropertyCollectionRootEntry, 1> invalidFields{{{2, &invalidValue}}};
            const auto wrong = EntityPropertyCollectionRootCodec::encodeBorrowed(2, graph::Node::typeId, invalidFields, *memory);
            const auto saved = fresh.front(); fresh.front().root = wrong;
            RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before,
                LogSequenceNumber(before.checkpoint_lsn.value() + 1));
            try {
                static_cast<void>(HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks,
                    memory, {}, nullptr, nullptr, {}, demotions)); FAIL();
            } catch (const FormatException &error) {
                EXPECT_NE(std::string(error.what()).find("actual final column membership"), std::string::npos) << error.what();
            }
            fresh.front() = saved;
        }
        {
            const PropertyValue invalidValue(int64_t(-1));
            const std::array<BorrowedPropertyCollectionRootEntry, 1> invalidFields{{{1, &invalidValue}}};
            const auto wrong = EntityPropertyCollectionRootCodec::encodeBorrowed(2, graph::Node::typeId, invalidFields, *memory);
            const auto saved = fresh.front(); fresh.front().root = wrong;
            RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before,
                LogSequenceNumber(before.checkpoint_lsn.value() + 1));
            try {
                static_cast<void>(HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks,
                    memory, {}, nullptr, nullptr, {}, demotions)); FAIL();
            } catch (const FormatException &error) {
                EXPECT_NE(std::string(error.what()).find("authenticated final column value"), std::string::npos) << error.what();
            }
            fresh.front() = saved;
        }
        {
            RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before,
                LogSequenceNumber(before.checkpoint_lsn.value() + 1));
            const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 1> incomplete{{{{NODE, 2}, {}, fresh}}};
            EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, incomplete,
                memory, {}, nullptr, nullptr, {}, demotions), FormatException);
        }
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeWholeColumnDemotionCovers64OwnersAndMergedThresholdRemoval) {
    hot_owner_count_ = 64; seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const graph::OwnedPropertyValue changed(PropertyValue("new-category"));
        const std::array<NativeHybridPropertyColumnReplacement, 1> replacement{{{{NODE, 1}, 1, changed}}};
        const graph::OwnedPropertyValue large(PropertyValue(std::string(8193, 'z')));
        const std::array<NativeHybridPropertyColumnReplacement, 1> largeReplacement{{{{NODE, 1}, 1, large}}};
        std::vector<NativeHybridPropertyColumnReplacement> removed;
        for (uint64_t id = 1; id <= 17; ++id) removed.push_back({{NODE, id}, 1, std::monostate{}, true});
        for (auto cells : {std::span<const NativeHybridPropertyColumnReplacement>(replacement),
            std::span<const NativeHybridPropertyColumnReplacement>(largeReplacement),
            std::span<const NativeHybridPropertyColumnReplacement>(removed)}) {
            NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
                before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
            request.field_lifecycle = true; int64_t frames = 0;
            graph::debug::PerfTrace::ScopedObserver observe(&frames, [](void *p, std::string_view key, int64_t count, bool) noexcept {
                if (key == "native_graph.hybrid.private_field_coordinator_frames") *static_cast<int64_t *>(p) = count;
            });
            try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
            catch (const FormatException &error) {
                EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
                EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
            }
            EXPECT_GT(frames, 0);
        }
        EXPECT_THROW(context.groupRecord({NODE, 1}), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeMissingHotInsertionCanDemoteTheWholeExistingColumn) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const graph::OwnedPropertyValue changed(PropertyValue("new-category"));
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 49}, 1, changed}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true; int64_t frames = 0;
        graph::debug::PerfTrace::ScopedObserver observe(&frames, [](void *p, std::string_view key, int64_t count, bool) noexcept {
            if (key == "native_graph.hybrid.private_field_coordinator_frames") *static_cast<int64_t *>(p) = count;
        });
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_GT(frames, 0);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, GlobalDemotionCannotLaunderForbiddenHotTokenInUnrepackedColdOwner) {
    hot_in_cold_ = true; split_cold_ = true; seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const graph::OwnedPropertyValue changed(PropertyValue("new-category"));
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, changed}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true; int64_t frames = 0;
        graph::debug::PerfTrace::ScopedObserver observe(&frames, [](void *p, std::string_view key, int64_t count, bool) noexcept {
            if (key == "native_graph.hybrid.private_field_coordinator_frames") *static_cast<int64_t *>(p) = count;
        });
        EXPECT_THROW(HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request), FormatException);
        EXPECT_EQ(frames, 0);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeRemovingEveryHotFieldRetainsLivePropertylessGroupWithoutColdBlocks) {
    hot_owner_count_ = 64; omit_cold_ = true; seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        graph::utils::OwnedVector<NativeHybridPropertyColumnReplacement> cells{
            graph::utils::OwnedPmrAllocator<NativeHybridPropertyColumnReplacement>(memory)};
        cells.reserve(64);
        for (uint64_t id = 1; id <= 64; ++id) cells.push_back({{NODE, id}, 1, std::monostate{}, true});
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true; int64_t frames = 0;
        graph::debug::PerfTrace::ScopedObserver observe(&frames, [](void *p, std::string_view key, int64_t count, bool) noexcept {
            if (key == "native_graph.hybrid.private_field_coordinator_frames") *static_cast<int64_t *>(p) = count;
        });
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_GT(frames, 0);
        for (uint64_t id : {1U, 3U, 64U}) EXPECT_NO_THROW((void)entities_->readNode(pin, id));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeGlobalDemotionRetainsActualEdgeNamespace) {
    owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; hot_owner_count_ = 64;
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const graph::OwnedPropertyValue changed(PropertyValue("new-category"));
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{owner_kind_, 1}, 1, changed}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true; int64_t frames = 0;
        graph::debug::PerfTrace::ScopedObserver observe(&frames, [](void *p, std::string_view key, int64_t count, bool) noexcept {
            if (key == "native_graph.hybrid.private_field_coordinator_frames") *static_cast<int64_t *>(p) = count;
        });
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_GT(frames, 0);
        EXPECT_NO_THROW((void)entities_->readEdge(pin, 64));
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeInversePlansColdCensusRowInstallationAndRetirementWithoutWrites) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    const auto fileBytes = io_->fileSize(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        expectInversePrepared(context, pin, memory, 0);
        EXPECT_THROW(context.groupRecord({NODE, 1}), FormatException);
        NativeGraphHybridPropertyConversion conversion{NODE, 1, memory, nullptr, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        conversion.direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
        conversion.publish_candidate = true;
        EXPECT_THROW(HybridPropertyNativeWriteTestAccess::prepareInverse(*metadata_, pin, context, conversion), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), fileBytes);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeInverseAddsHotFieldsToOneActualNewWholeRowCarrier) {
    seed(true); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        expectInversePrepared(context, pin, memory, 1);
        const auto old = HybridPropertyValueReader::read(context, pin, {NODE, 1}, 4, values_.get());
        ASSERT_TRUE(old.value); EXPECT_EQ(old.value->value, PropertyValue(std::string(8193, 'x')));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeInverseReusesActualCompleteColdCarrierWithoutAllocatingAnotherValue) {
    cold_only_ = true; seed(true); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        expectInversePrepared(context, pin, memory, 0);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeInverseRetainsActualEdgeNamespaceWithSameLogicalNodeIds) {
    owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; seed();
    HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        expectInversePrepared(context, pin, memory, 0);
        EXPECT_NO_THROW((void)entities_->readEdge(pin, 64)); EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeColdMutationReadsExistingLargeRootUnderItsAlreadyHeldWriter) {
    seed(true); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const graph::OwnedPropertyValue changed(PropertyValue("new-id"));
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 2, changed}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true;
        struct Counts { int64_t frames = 0, values = 0; } counts;
        graph::debug::PerfTrace::ScopedObserver observe(&counts, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            auto &c = *static_cast<Counts *>(p);
            if (key == "native_graph.hybrid.private_field_coordinator_frames") c.frames = value;
            if (key == "native_graph.hybrid.private_new_field_values") c.values = value;
        });
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_GT(counts.frames, 0); EXPECT_EQ(counts.values, 1);
        const auto original = HybridPropertyValueReader::read(context, pin, {NODE, 1}, 2, values_.get());
        ASSERT_TRUE(original.value); EXPECT_EQ(original.value->value, PropertyValue("user-000000"));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeInverseOfLivePropertylessGroupNeedsNoFabricatedRowFrames) {
    cold_only_ = true; omit_cold_ = true; seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        expectInversePrepared(context, pin, memory, 0, false);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeInverseQuotaAndPreparedCancellationDiscardPrivateIdsAndPermitRetry) {
    struct Cancelled final : std::runtime_error { Cancelled() : std::runtime_error("cold inverse cancelled") {} };
    struct Cancel final : CancellationCheckpoint { bool prepared = false; void check() const override {
        if (prepared) throw Cancelled();
    }} cancellation;
    seed(true); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    const auto bytes = io_->fileSize(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_, &cancellation);
        NativeGraphHybridPropertyConversion conversion{NODE, 1, memory, &cancellation, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        conversion.direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
        {
            ASSERT_GT(memory->available(), 96U * 1024U);
            graph::utils::OwnedHeapReservation pressure(memory, memory->available() - 96U * 1024U);
            EXPECT_THROW(HybridPropertyNativeWriteTestAccess::prepareInverse(*metadata_, pin, context, conversion),
                graph::query::execution::MaterializedByteLimitExceeded);
        }
        {
            graph::debug::PerfTrace::ScopedObserver observe(&cancellation, [](void *p, std::string_view key, int64_t value, bool) noexcept {
                if (key == "native_graph.hybrid.inverse_private_new_large_values" && value == 1)
                    static_cast<Cancel *>(p)->prepared = true;
            });
            EXPECT_THROW(HybridPropertyNativeWriteTestAccess::prepareInverse(*metadata_, pin, context, conversion), Cancelled);
        }
        ASSERT_TRUE(cancellation.prepared); cancellation.prepared = false;
        expectInversePrepared(context, pin, memory, 1);
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
        EXPECT_EQ(io_->fileSize(), bytes);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdOwnerLivenessChangesOnlyAfterItsActualRootRemoval) {
    cold_only_ = true; seed(); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<EntityPropertyKey, 1> deleted{{{NODE, 64}}};
        const std::array<HybridColdRowInput, 1> rows{{{63, 0, false, {}}}};
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 1> blocks{{{{NODE, 64}, cold_reference_, rows}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, {}, memory,
            {}, values_.get(), nullptr, {}, {}, deleted), FormatException);
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get(), nullptr, {}, {}, deleted);
        const auto group = prepared.group(); EXPECT_EQ(group.live, UINT64_MAX >> 1); EXPECT_EQ(group.memberships, 2U);
        const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(
            group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.live, group.live); EXPECT_EQ(matrix.cold_rows.owners, 1U); EXPECT_EQ(matrix.cold_fields, 2U);
        EXPECT_FALSE(prepared.erasedGroup()); EXPECT_TRUE(prepared.owners().empty());
        EXPECT_NO_THROW((void)entities_->readNode(pin, 64));
        const auto original = HybridPropertyValueReader::read(context, pin, {NODE, 64}, 2, values_.get());
        ASSERT_TRUE(original.value); EXPECT_EQ(original.value->value, PropertyValue("user-000063"));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdOwnerDeletionCannotSilentlyClearOmittedHotFieldsOrRepeatOwners) {
    seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<EntityPropertyKey, 1> deleted{{{NODE, 1}}};
        const std::array<HybridColdRowInput, 1> rows{{{0, 0, false, {}}}};
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 1> blocks{{{{NODE, 1}, cold_reference_, rows}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get(), nullptr, {}, {}, deleted), FormatException);
        const std::array<EntityPropertyKey, 2> duplicate{{{NODE, 1}, {NODE, 1}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get(), nullptr, {}, {}, duplicate), FormatException);
        const std::array<EntityPropertyKey, 1> foreign{{{EntityPropertyOwnerKind::EPOK_EDGE, 1}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get(), nullptr, {}, {}, foreign), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
}
TEST_F(HybridPropertyColdCompatibleHotTest, DeletingAllLivePropertylessOwnersErasesGroupAndMembershipTogether) {
    cold_only_ = true; omit_cold_ = true; seed(); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        std::array<EntityPropertyKey, 64> deleted{}; for (unsigned i = 0; i < 64; ++i) deleted[i] = {NODE, i + 1U};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, {}, memory,
            {}, values_.get(), nullptr, {}, {}, deleted);
        EXPECT_EQ(prepared.group().live, 0U); EXPECT_EQ(prepared.group().memberships, 0U); EXPECT_TRUE(prepared.erasedGroup());
        ASSERT_EQ(prepared.erasedGroups(NODE).size(), 1U); EXPECT_EQ(prepared.erasedGroups(NODE).front(), HybridPropertyDirectoryCodec::groupKey(1));
        EXPECT_TRUE(prepared.groups(NODE).empty());
        EXPECT_THROW((void)image(prepared, field_page_id_).readRecord(prepared.group().column_membership), FormatException);
        EXPECT_NO_THROW((void)entities_->readNode(pin, 64));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeColdDeletionCoordinatesEntityAndDemotedSurvivingOwners) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch(); const auto bytes = io_->fileSize();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 1> deleted{{{NODE, 1}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true; request.owner_deletions = deleted;
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_NO_THROW((void)entities_->readNode(pin, 1));
        ASSERT_TRUE(HybridPropertyValueReader::read(context, pin, {NODE, 48}, 1, values_.get()).value);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), bytes);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeColdDeletionOfAllOwnersCoordinatesFinalGroupCleanup) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        std::array<EntityPropertyKey, 64> deleted{}; for (unsigned i = 0; i < 64; ++i) deleted[i] = {NODE, i + 1U};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true; request.owner_deletions = deleted;
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_NO_THROW((void)entities_->readNode(pin, 64));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeColdDeletionAuthenticatesExistingLargeRootUnderWriterWithoutNewCarrier) {
    seed(true); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 1> deleted{{{NODE, 1}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true; request.owner_deletions = deleted; int64_t values = -1;
        graph::debug::PerfTrace::ScopedObserver observer(&values, [](void *p, std::string_view key, int64_t n, bool) noexcept {
            if (key == "native_graph.hybrid.private_new_field_values") *static_cast<int64_t *>(p) = n;
        });
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_EQ(values, 0); EXPECT_NO_THROW((void)entities_->readNode(pin, 1));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeColdUniqueChecksAndOwnerDeletionUseTheSameAuthenticatedSource) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    SystemCatalog catalog(*btree_pages_, *metadata_); uint64_t person = 0, support = 0;
    {
        auto tx = planner.beginTransaction(); auto schema = catalog.beginWrite(tx);
        EXPECT_EQ(schema.ensurePropertyToken("hot"), 1U); EXPECT_EQ(schema.ensurePropertyToken("cold"), 2U);
        EXPECT_EQ(schema.ensurePropertyToken("unused"), 3U); EXPECT_EQ(schema.ensurePropertyToken("nullable"), 4U);
        person = schema.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
        schema.setGraphCardinalityStatistics({64, 0}); schema.finish(); ASSERT_TRUE(tx.commit().committed());
    }
    {
        auto tx = planner.beginTransaction(); const std::array names{person};
        tx.replaceNodeLabels(1, names); tx.replaceNodeLabels(64, names); ASSERT_TRUE(tx.commit().committed());
    }
    std::vector<BTreePageEntry> keys;
    {
        auto tx = planner.beginTransaction(); auto schema = catalog.beginWrite(tx);
        ConstraintDefinition unique; unique.name = "unique_cold"; unique.kind = ConstraintDefinitionKind::CDK_UNIQUE;
        unique.schema_token_id = person; unique.property_names = {"cold"};
        unique = schema.createConstraintDefinition(unique); support = unique.constraint_id; schema.finish();
        for (const uint64_t id : {1U, 64U}) {
            const std::array<PropertyValue, 1> values{PropertyValue(id == 1 ? "user-000000" : "user-000063")};
            const auto key = ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(values), id);
            keys.push_back({graph::utils::OwnedStdVector<std::byte>(key.begin(), key.end()), id});
        }
        std::ranges::sort(keys, {}, &BTreePageEntry::key);
        tx.stageScalarIndexCreate(*btree_pages_, support, keys); ASSERT_TRUE(tx.commit().committed());
    }
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    descriptor_.semantic_generation = before.generation; auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const graph::OwnedPropertyValue duplicate(PropertyValue("user-000000"));
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 64}, 2, duplicate}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const std::runtime_error &error) {
            EXPECT_NE(std::string(error.what()).find("Constraint violation: 'unique_cold'"), std::string::npos) << error.what();
        }
        const std::array<EntityPropertyKey, 1> deleted{{{NODE, 1}}}; request.replacements = {}; request.owner_deletions = deleted;
        int64_t constraints = 0;
        graph::debug::PerfTrace::ScopedObserver observer(&constraints, [](void *p, std::string_view key, int64_t n, bool) noexcept {
            if (key == "native_graph.hybrid.private_constraint_index_images") *static_cast<int64_t *>(p) = n;
        });
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_GT(constraints, 0);
        for (const auto &entry : keys) EXPECT_EQ(BTreeIndex(support, *btree_pages_, *metadata_).find(pin, entry.key),
            std::optional(entry.value_or_child));
        EXPECT_NO_THROW((void)entities_->readNode(pin, 1));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdEdgeDeletionPreservesRetainedHotColumnAndOtherOwnerValues) {
    owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<EntityPropertyKey, 1> deleted{{{owner_kind_, 64}}};
        const std::array<HybridColdRowInput, 1> rows{{{63, 0, false, {}}}};
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 1> blocks{{{{owner_kind_, 64}, cold_reference_, rows}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get(), nullptr, {}, {}, deleted);
        const auto group = prepared.group(); EXPECT_EQ(group.live, UINT64_MAX >> 1); EXPECT_EQ(group.columns, 1U); EXPECT_EQ(group.memberships, 50U);
        const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(
            group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.presence(1), (UINT64_C(1) << 48) - 1U); EXPECT_EQ(matrix.live, group.live);
        const PropertyValue same(int64_t(100));
        const auto witness = HybridPropertyColumnDelta::planExistingColumn(context, pin, {owner_kind_, 1}, 1,
            batch.generation(), memory, &same); ASSERT_TRUE(witness);
        const auto ref = matrix.columnLocator(1); const auto column = image(prepared, ref.page_id).readColumn(ref,
            witness->sourceColumn().identity, group, batch.generation());
        EXPECT_EQ(column.words[0], 100U); EXPECT_EQ(column.words[47], 147U);
        EXPECT_EQ(matrix.cold_fields, 2U); EXPECT_EQ(matrix.cold_rows.owners, 1U);
        EXPECT_NO_THROW((void)entities_->readEdge(pin, 64)); EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64));
        const auto original = HybridPropertyValueReader::read(context, pin, {owner_kind_, 64}, 2, values_.get());
        ASSERT_TRUE(original.value); EXPECT_EQ(original.value->value, PropertyValue("user-000063"));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeColdDeletionQuotaFailureRefundsAndAllowsOriginalSourceRetry) {
    seed(true); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch(); const auto bytes = io_->fileSize();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 1> deleted{{{NODE, 1}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true; request.owner_deletions = deleted;
        {
            ASSERT_GT(memory->available(), 96U * 1024U);
            graph::utils::OwnedHeapReservation pressure(memory, memory->available() - 96U * 1024U);
            EXPECT_THROW(HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request),
                graph::query::execution::MaterializedByteLimitExceeded);
        }
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_NO_THROW((void)entities_->readNode(pin, 1));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), bytes);
}
TEST_F(HybridPropertyColdCompatibleHotTest, PendingColdOwnerHotNullUsesActualAbsentEntityAndOnePrivateMembership) {
    live_owner_count_ = 63; seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64)); EXPECT_NO_THROW((void)entities_->readNode(pin, 63));
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const PropertyValue null; const std::array<EntityPropertyKey, 1> pending{{{NODE, 64}}};
        EXPECT_THROW((void)HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, pin, pending[0], 1,
            batch.generation(), memory, null), FormatException);
        auto plan = HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, pin, pending[0], 1,
            batch.generation(), memory, null, true); ASSERT_TRUE(plan);
        EXPECT_EQ(plan->effects().set_live, UINT64_C(1) << 63); EXPECT_EQ(plan->effects().memberships, 1);
        HybridPropertyFieldPageStore fields(io_, *metadata_, pin); const std::array<const HybridPropertyColumnDelta *, 1> plans{&*plan};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, pin, fields,
            plans, pending, memory), FormatException);
        const auto prepared = HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, pin, fields,
            plans, pending, memory, nullptr, {}, true);
        const auto group = prepared.group(); EXPECT_EQ(group.live, UINT64_MAX); EXPECT_EQ(group.memberships, 52U); EXPECT_EQ(group.columns, 1U);
        const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.live, group.live); EXPECT_TRUE(matrix.contains(pending[0], 1)); EXPECT_EQ(matrix.cold_fields, 3U);
        EXPECT_EQ(matrix.cold_rows.owners, 1U | (UINT64_C(1) << 62)); EXPECT_FALSE(matrix.cold_rows.owners & (UINT64_C(1) << 63));
        const auto column = image(prepared, plan->columnReference().page_id).readColumn(plan->columnReference(),
            plan->replacementColumn().identity, group, batch.generation());
        EXPECT_TRUE(column.nulls & (UINT64_C(1) << 63)); EXPECT_EQ(column.words[63], 0U);
        EXPECT_TRUE(std::ranges::equal(image(prepared, cold_reference_.page_id).readRecord(cold_reference_), cold_encoded_bytes_));
        EXPECT_TRUE(prepared.owners().empty()); EXPECT_TRUE(prepared.extensions().empty()); EXPECT_FALSE(prepared.erasedGroup());
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64)); EXPECT_THROW(context.groupRecord(pending[0]), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, PendingPropertylessColdEdgeChangesLivenessWithoutFabricatedFields) {
    live_owner_count_ = 63; owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; seed(); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        EXPECT_FALSE(metadata_->findEdgePageLocationOptional(pin, 64)); EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64));
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        HybridPropertyFieldPageStore fields(io_, *metadata_, pin); const std::array<EntityPropertyKey, 1> pending{{{owner_kind_, 64}}};
        const auto prepared = HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, pin, fields,
            {}, pending, memory, nullptr, {}, true);
        const auto group = prepared.group(); EXPECT_EQ(group.live, UINT64_MAX); EXPECT_EQ(group.memberships, 51U); EXPECT_EQ(group.columns, 1U);
        const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.live, group.live); EXPECT_FALSE(matrix.contains(pending[0], 1)); EXPECT_EQ(matrix.cold_fields, 3U);
        EXPECT_FALSE(matrix.cold_rows.owners & (UINT64_C(1) << 63)); EXPECT_TRUE(prepared.owners().empty()); EXPECT_TRUE(prepared.extensions().empty());
        EXPECT_TRUE(std::ranges::equal(image(prepared, cold_reference_.page_id).readRecord(cold_reference_), cold_encoded_bytes_));
        EXPECT_NO_THROW((void)entities_->readEdge(pin, 63)); EXPECT_FALSE(metadata_->findEdgePageLocationOptional(pin, 64));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdPendingLivenessRejectsActualLiveDuplicateAndForeignOwnerClaims) {
    live_owner_count_ = 63; seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
        for (const auto owner : {EntityPropertyKey{NODE, 1}, EntityPropertyKey{NODE, 65},
            EntityPropertyKey{EntityPropertyOwnerKind::EPOK_EDGE, 64}}) {
            const std::array<EntityPropertyKey, 1> pending{owner};
            EXPECT_THROW((void)HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, pin, fields,
                {}, pending, memory, nullptr, {}, true), FormatException);
        }
        const std::array<EntityPropertyKey, 2> duplicate{{{NODE, 64}, {NODE, 64}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, pin, fields,
            {}, duplicate, memory, nullptr, {}, true), FormatException);
        const PropertyValue value(int64_t(900));
        EXPECT_THROW((void)HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, pin, {NODE, 1}, 1,
            batch.generation(), memory, value, true), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
}
TEST_F(HybridPropertyColdCompatibleHotTest, PendingColdRootsAndHotNullShareOneAuthenticatedPrivateGroup) {
    live_owner_count_ = 63; seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<EntityPropertyKey, 1> pending{{{NODE, 64}}}; const PropertyValue null;
        auto hot = HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, pin, pending[0], 1,
            batch.generation(), memory, null, true); ASSERT_TRUE(hot);
        const std::array<const HybridPropertyColumnDelta *, 1> plans{&*hot};
        const auto root = EntityPropertyCollectionRootCodec::encode({64, graph::Node::typeId,
            {{2, PropertyValue("new-id")}, {4, PropertyValue()}, {9, PropertyValue(int64_t(900))}}});
        const std::array<HybridColdRowInput, 1> rows{{{63, 3, false, root}}};
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 1> blocks{{{pending[0], {}, rows}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get()), FormatException);
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            plans, values_.get(), nullptr, {}, {}, {}, pending);
        const auto group = prepared.group(); EXPECT_EQ(group.live, UINT64_MAX); EXPECT_EQ(group.columns, 1U); EXPECT_EQ(group.memberships, 55U);
        const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_TRUE(matrix.contains(pending[0], 1)); EXPECT_EQ(matrix.cold_fields, 6U); EXPECT_TRUE(matrix.cold_rows.owners & (UINT64_C(1) << 63));
        const auto ref = matrix.cold_rows.roots[63]; const auto block = HybridPropertyColdRowBlock::decode(
            image(prepared, ref.page_id).readRecord(ref), NODE, 1, batch.generation(), memory);
        const auto row = block.row(63); ASSERT_TRUE(row); EXPECT_EQ(row->field_count, 3U);
        EXPECT_EQ(EntityPropertyCollectionRootCodec::decode(row->root).entries,
            EntityPropertyCollectionRootCodec::decode(root).entries);
        EXPECT_TRUE(std::ranges::equal(image(prepared, cold_reference_.page_id).readRecord(cold_reference_), cold_encoded_bytes_));
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64)); EXPECT_TRUE(prepared.owners().empty()); EXPECT_TRUE(prepared.extensions().empty());
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, IncompatiblePendingHotFieldDemotesAllOriginalMembersWithItsNewColdRoot) {
    live_owner_count_ = 63; seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<EntityPropertyKey, 1> pending{{{NODE, 64}}}; const PropertyValue incompatible("new-incompatible");
        auto hot = HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, pin, pending[0], 1,
            batch.generation(), memory, incompatible, true); ASSERT_TRUE(hot);
        ASSERT_EQ(hot->action(), HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD);
        const std::array<const HybridPropertyColumnDelta *, 1> demotions{&*hot};
        std::vector<std::vector<std::byte>> roots; roots.reserve(49); std::vector<HybridColdRowInput> rows; rows.reserve(49);
        for (unsigned slot = 0; slot < 48; ++slot) {
            std::vector<PropertyCollectionRootEntry> entries{{1, slot == 2 ? PropertyValue() : PropertyValue(int64_t(slot + 100))}};
            if (!slot) { entries.push_back({2, PropertyValue("user-000000")}); entries.push_back({4, PropertyValue()}); }
            roots.push_back(EntityPropertyCollectionRootCodec::encode({slot + 1U, graph::Node::typeId, entries}));
            rows.push_back({uint8_t(slot), uint32_t(entries.size()), false, roots.back()});
        }
        roots.push_back(EntityPropertyCollectionRootCodec::encode({64, graph::Node::typeId,
            {{1, incompatible}, {2, PropertyValue("new-cold")}}})); rows.push_back({63, 2, false, roots.back()});
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 2> blocks{{
            {{NODE, 1}, cold_reference_, std::span(rows).first(1)},
            {{NODE, 2}, {}, std::span(rows).subspan(1)}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get(), nullptr, {}, demotions, {}, pending);
        const auto group = prepared.group(); EXPECT_EQ(group.live, UINT64_MAX); EXPECT_EQ(group.columns, 0U); EXPECT_EQ(group.memberships, 53U);
        const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.column_count, 0U); EXPECT_EQ(matrix.cold_fields, 53U);
        for (const auto &expected : rows) {
            const auto ref = matrix.cold_rows.roots[expected.owner_slot]; ASSERT_TRUE(ref.isValid());
            const auto block = HybridPropertyColdRowBlock::decode(image(prepared, ref.page_id).readRecord(ref),
                NODE, 1, batch.generation(), memory); const auto row = block.row(expected.owner_slot); ASSERT_TRUE(row);
            EXPECT_EQ(EntityPropertyCollectionRootCodec::decode(row->root).entries,
                EntityPropertyCollectionRootCodec::decode(expected.root).entries);
        }
        EXPECT_TRUE(matrix.cold_rows.owners & (UINT64_C(1) << 62)); // Original unrelated cold owner survives.
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64)); EXPECT_TRUE(prepared.owners().empty()); EXPECT_TRUE(prepared.extensions().empty());
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, PrivatePendingImagesCannotReplaceSealedNativeEntityCreationAuthority) {
    live_owner_count_ = 63; seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 1> pending{{{NODE, 64}}};
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 64}, 1, int64_t(900)}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true; request.pending_owner_additions = pending;
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_NE(std::string(error.what()).find("sealed new-owner graph publication"), std::string::npos) << error.what();
        }
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64)); EXPECT_NO_THROW((void)entities_->readNode(pin, 63));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdBirthAndDeletionKeepOnlyNewOwnerWithOneFinalRoot) {
    live_owner_count_ = 63; seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<EntityPropertyKey, 1> pending{{{NODE, 64}}}; std::array<EntityPropertyKey, 63> deleted{};
        for (unsigned i = 0; i < 63; ++i) deleted[i] = {NODE, i + 1U};
        graph::utils::OwnedVector<HybridPropertyColumnDelta> plans{graph::utils::OwnedPmrAllocator<HybridPropertyColumnDelta>(memory)};
        plans.reserve(49); std::array<const HybridPropertyColumnDelta *, 49> inputs{};
        for (unsigned i = 0; i < 48; ++i) {
            auto removal = HybridPropertyColumnDelta::planExistingColumn(context, pin, deleted[i], 1, batch.generation(),
                memory, nullptr, false, RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, true); ASSERT_TRUE(removal);
            plans.push_back(std::move(*removal)); inputs[i] = &plans.back();
        }
        const PropertyValue value("new-incompatible");
        auto birth = HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, pin, pending[0], 1,
            batch.generation(), memory, value, true); ASSERT_TRUE(birth);
        plans.push_back(std::move(*birth)); inputs[48] = &plans.back();
        EXPECT_THROW((void)HybridPropertyColumnDelta::mergeExistingColumnChanges(inputs, memory,
            RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, nullptr, true), FormatException);
        auto merged = HybridPropertyColumnDelta::mergeExistingColumnChanges(inputs, memory,
            RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, nullptr, true, true);
        ASSERT_EQ(merged.action(), HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD);
        const std::array<const HybridPropertyColumnDelta *, 1> demotions{&merged};
        const std::array<HybridColdRowInput, 2> removed{{{0, 0, false, {}}, {62, 0, false, {}}}};
        const auto root = EntityPropertyCollectionRootCodec::encode({64, graph::Node::typeId,
            {{1, value}, {2, PropertyValue("new-cold")}}}); const std::array<HybridColdRowInput, 1> added{{{63, 2, false, root}}};
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 2> blocks{{
            {{NODE, 1}, cold_reference_, removed}, {pending[0], {}, added}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get(), nullptr, {}, demotions, deleted, pending);
        const auto group = prepared.group(); EXPECT_EQ(group.live, UINT64_C(1) << 63); EXPECT_EQ(group.columns, 0U); EXPECT_EQ(group.memberships, 2U);
        EXPECT_FALSE(prepared.erasedGroup()); EXPECT_TRUE(prepared.erasedGroups(NODE).empty());
        const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.live, group.live); EXPECT_EQ(matrix.cold_rows.owners, group.live); EXPECT_EQ(matrix.cold_fields, 2U);
        const auto ref = matrix.cold_rows.roots[63]; const auto block = HybridPropertyColdRowBlock::decode(
            image(prepared, ref.page_id).readRecord(ref), NODE, 1, batch.generation(), memory);
        ASSERT_TRUE(block.row(63)); EXPECT_EQ(EntityPropertyCollectionRootCodec::decode(block.row(63)->root).entries,
            EntityPropertyCollectionRootCodec::decode(root).entries);
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64)); EXPECT_NO_THROW((void)entities_->readNode(pin, 63));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
#endif
