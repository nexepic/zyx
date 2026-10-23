#include <gtest/gtest.h>
#include "graph/storage/relocatable/metadata/HybridPropertyConversionPreparation.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/property/HybridPropertyFieldView.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
#include "storage/relocatable/property/HybridPropertyConversionPreparationTestAccess.hpp"
namespace {
using Access = HybridPropertyConversionPreparationTestAccess;
using Plan = HybridPropertyGroupConversion;
constexpr auto NODE = EntityPropertyOwnerKind::EPOK_NODE;
class NoPhysicalAllocator final : public MetadataPageAllocator {
public:
    MetadataPageId allocateMetadataPageId() override { throw std::logic_error("unexpected physical metadata allocation"); }
    MetadataPageAllocation allocatePage(Generation, LogSequenceNumber, std::span<const PhysicalSlotId>) override {
        throw std::logic_error("unexpected physical metadata allocation");
    }
    void abandonPage(PhysicalPageRef) noexcept override {}
};
Superblock sourceRoot() {
    Superblock result; result.database_id = DatabaseId(33); result.generation = Generation(8);
    result.checkpoint_lsn = LogSequenceNumber(8); result.next_logical_page_id = 100; return result;
}
std::vector<PropertyCollectionRoot> inputRows(size_t fields, bool mixed = false) {
    std::vector<PropertyCollectionRoot> roots;
    for (size_t id = 1; id <= 64; ++id) {
        PropertyCollectionRoot row; row.owner_entity_id = id; row.owner_entity_type = graph::Node::typeId;
        for (size_t token = 1; token <= fields; ++token)
            row.entries.push_back({uint32_t(token), mixed && token == fields ? PropertyValue(std::string(100, 'x'))
                : PropertyValue(int64_t(id + token))});
        roots.push_back(std::move(row));
    }
    return roots;
}
TEST(HybridPropertyConversionPreparationTest, FormatFlagControlsPrivatePreparationWithoutPublishingAnIdReservation) {
    NoPhysicalAllocator allocator; const auto current = sourceRoot();
    MetadataWriteBatch batch(allocator, current, LogSequenceNumber(9));
    const auto roots = inputRows(1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    Plan plan(memory, roots, NODE, 1, batch.generation(), 3952);
    if constexpr (RSS_HYBRID_PROPERTY_FORMAT_ENABLED) {
        const auto prepared = Access::prepare(batch, plan);
        ASSERT_EQ(prepared->pages().size(), 1U); EXPECT_EQ(prepared->pages()[0].pageId(), PageId(100));
        EXPECT_EQ(batch.candidate().next_logical_page_id, 101U);
        EXPECT_EQ(prepared->owners().size(), 64U); EXPECT_EQ(prepared->columns().size(), 1U);
    } else {
        try { static_cast<void>(Access::prepare(batch, plan)); FAIL() << "default format accepted native fields"; }
        catch (const FormatException &error) { EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION); }
        EXPECT_EQ(batch.candidate().next_logical_page_id, 100U);
    }
    EXPECT_EQ(batch.current(), current); EXPECT_EQ(current.next_logical_page_id, 100U);
    EXPECT_EQ(batch.allocatedPageCount(), 0U); EXPECT_EQ(batch.createdPoolCount(), 0U);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
const HybridPropertyPageImage &imageFor(const HybridPropertyConversionPreparation &prepared, PageId id) {
    const auto found = std::find_if(prepared.pages().begin(), prepared.pages().end(),
        [id](const auto &image) { return image.pageId() == id; });
    if (found == prepared.pages().end()) throw std::logic_error("unbound native locator");
    return *found;
}
TEST(HybridPropertyConversionPreparationTest, MetadataAssignedIdsBindActualTypedRecordsAndAllFinalDirectories) {
    NoPhysicalAllocator allocator; MetadataWriteBatch batch(allocator, sourceRoot(), LogSequenceNumber(9));
    const auto roots = inputRows(3, true); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    Plan plan(memory, roots, NODE, 1, batch.generation(), 3952);
    const auto prepared = Access::prepare(batch, plan);
    ASSERT_GT(prepared->pages().size(), 1U); ASSERT_EQ(prepared->owners().size(), 192U);
    ASSERT_EQ(prepared->columns().size(), 2U); ASSERT_EQ(prepared->extensions().size(), 64U);
    const auto groupSlot = HybridPropertyDirectoryCodec::decodeLocator(prepared->group().value);
    EXPECT_EQ(imageFor(*prepared, groupSlot.page_id).readGroup(groupSlot, NODE, 1, batch.generation()), plan.group());
    for (size_t i = 0; i < prepared->pages().size(); ++i) EXPECT_EQ(prepared->pages()[i].pageId(), PageId(100 + i));
    EXPECT_EQ(batch.candidate().next_logical_page_id, 100 + prepared->pages().size());
    for (size_t i = 0; i < prepared->columns().size(); ++i) {
        const auto &entry = prepared->columns()[i]; const auto slot = HybridPropertyDirectoryCodec::decodeLocator(entry.value);
        EXPECT_EQ(HybridPropertyDirectoryCodec::decodeColumnKey(entry.key).token, i + 1);
        EXPECT_EQ(imageFor(*prepared, slot.page_id).readColumn(slot, plan.columns()[i].identity,
            plan.group(), batch.generation()), plan.columns()[i]);
    }
    for (const auto &entry : prepared->extensions()) {
        const auto owner = HybridPropertyDirectoryCodec::decodeOwnerKey(entry.key, NODE);
        const auto slot = HybridPropertyDirectoryCodec::decodeLocator(entry.value);
        const auto bytes = imageFor(*prepared, slot.page_id).readRecord(slot);
        const auto view = HybridPropertyExtensionView::open(bytes, owner.owner, owner.token,
            batch.generation(), batch.generation(), batch.generation());
        EXPECT_EQ(view.decodeInline([](const auto &) {}), roots[owner.owner.entity_id - 1].entries[2].value);
    }
    for (const auto &entry : prepared->owners()) {
        const auto owner = HybridPropertyDirectoryCodec::decodeOwnerKey(entry.key, NODE);
        EXPECT_EQ(HybridPropertyDirectoryCodec::decodeSource(entry.value), owner.token == 3 ?
            HybridPropertySource::HPS_EXTENSION : HybridPropertySource::HPS_COLUMN);
    }
    EXPECT_EQ(batch.allocatedPageCount(), 0U); EXPECT_EQ(batch.createdPoolCount(), 0U);
}
TEST(HybridPropertyConversionPreparationTest, CompleteMembershipBoundPacksIntoActualSlottedPageCapacity) {
    NoPhysicalAllocator allocator; MetadataWriteBatch batch(allocator, sourceRoot(), LogSequenceNumber(9));
    const auto roots = inputRows(64); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        Plan plan(memory, roots, NODE, 1, batch.generation(), 3952);
        const auto prepared = Access::prepare(batch, plan);
        EXPECT_EQ(prepared->owners().size(), 4096U); EXPECT_EQ(prepared->columns().size(), 64U);
        EXPECT_TRUE(prepared->extensions().empty()); EXPECT_EQ(prepared->pages().size(), 11U);
        EXPECT_EQ(batch.candidate().next_logical_page_id, 111U);
        EXPECT_LE(memory->used(), 1024U * 1024U);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyConversionPreparationTest, PreparedBytesOutliveBorrowedRowsAndRetainTheirAllocationOwner) {
    NoPhysicalAllocator allocator; MetadataWriteBatch batch(allocator, sourceRoot(), LogSequenceNumber(9));
    auto roots = inputRows(2, true); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    std::weak_ptr<MaterializedByteBudget> weak = memory;
    std::unique_ptr<HybridPropertyConversionPreparation> prepared;
    { Plan plan(memory, roots, NODE, 1, batch.generation(), 3952); prepared = Access::prepare(batch, plan); }
    roots.clear(); roots.shrink_to_fit(); memory.reset(); EXPECT_FALSE(weak.expired());
    const auto slot = HybridPropertyDirectoryCodec::decodeLocator(prepared->extensions()[0].value);
    {
        const auto bytes = imageFor(*prepared, slot.page_id).readRecord(slot);
        EXPECT_EQ(HybridPropertyExtensionView::open(bytes, {NODE, 1}, 2, batch.generation(), batch.generation(),
            batch.generation()).decodeInline([](const auto &) {}), PropertyValue(std::string(100, 'x')));
        prepared.reset(); EXPECT_FALSE(weak.expired()); // escaped record owns the account
    }
    EXPECT_TRUE(weak.expired());
}
TEST(HybridPropertyConversionPreparationTest, StaleGenerationAndOversizedInlineValueRefuseWithoutPublishing) {
    NoPhysicalAllocator allocator; const auto current = sourceRoot();
    auto roots = inputRows(1, true); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        MetadataWriteBatch batch(allocator, current, LogSequenceNumber(9));
        Plan stale(memory, roots, NODE, 1, Generation(8), 3952);
        EXPECT_THROW(Access::prepare(batch, stale), FormatException);
        EXPECT_EQ(batch.candidate().next_logical_page_id, 100U);
    }
    EXPECT_EQ(memory->used(), 0U);
    roots[0].entries[0].value = PropertyValue(std::string(8192, 'x'));
    {
        MetadataWriteBatch batch(allocator, current, LogSequenceNumber(9));
        Plan plan(memory, roots, NODE, 1, batch.generation(), 3952); const auto charge = memory->used();
        EXPECT_THROW(Access::prepare(batch, plan), FormatException);
        EXPECT_EQ(memory->used(), charge); EXPECT_EQ(batch.current(), current);
        EXPECT_EQ(batch.allocatedPageCount(), 0U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(current.next_logical_page_id, 100U);
    roots[0].entries[0].value = PropertyValue(std::string(100, 'x'));
    MetadataWriteBatch retry(allocator, current, LogSequenceNumber(9));
    Plan plan(memory, roots, NODE, 1, retry.generation(), 3952);
    EXPECT_EQ(Access::prepare(retry, plan)->pages()[0].pageId(), PageId(100));
}
class FailingResource final : public std::pmr::memory_resource {
public:
    std::shared_ptr<MaterializedByteBudget> budget = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    size_t calls = 0, fail_on = 0;
private:
    void *do_allocate(size_t bytes, size_t alignment) override {
        if (++calls == fail_on) throw std::bad_alloc();
        return budget->allocate(bytes, alignment);
    }
    void do_deallocate(void *pointer, size_t bytes, size_t alignment) override { budget->deallocate(pointer, bytes, alignment); }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
TEST(HybridPropertyConversionPreparationTest, AllocationFailureAndCancellationAfterIdAssignmentDiscardOnlyThePrivateCandidate) {
    NoPhysicalAllocator allocator; const auto current = sourceRoot(); const auto roots = inputRows(3, true);
    auto memory = std::make_shared<FailingResource>();
    Plan plan(memory, roots, NODE, 1, Generation(9), 3952); const auto charge = memory->budget->used();
    {
        MetadataWriteBatch batch(allocator, current, LogSequenceNumber(9));
        // Three exact directory reservations, one page holder, then the first
        // page buffer: fail after the batch has assigned its logical identity.
        memory->fail_on = memory->calls + 5;
        EXPECT_THROW(Access::prepare(batch, plan), std::bad_alloc);
        EXPECT_EQ(batch.candidate().next_logical_page_id, 101U);
        EXPECT_EQ(batch.current(), current); EXPECT_EQ(batch.allocatedPageCount(), 0U);
        EXPECT_EQ(memory->budget->used(), charge);
    }
    memory->fail_on = 0;
    {
        MetadataWriteBatch batch(allocator, current, LogSequenceNumber(9)); size_t seen = 0;
        EXPECT_THROW(Access::prepare(batch, plan, [&] {
            if (++seen == 2) throw std::runtime_error("cancel prepared native fields");
        }), std::runtime_error);
        EXPECT_EQ(batch.candidate().next_logical_page_id, 101U);
        EXPECT_EQ(batch.current(), current); EXPECT_EQ(memory->budget->used(), charge);
    }
    MetadataWriteBatch retry(allocator, current, LogSequenceNumber(9));
    const auto prepared = Access::prepare(retry, plan);
    EXPECT_EQ(prepared->pages()[0].pageId(), PageId(100));
    EXPECT_EQ(roots, inputRows(3, true)); EXPECT_EQ(current.next_logical_page_id, 100U);
}
TEST(HybridPropertyConversionPreparationTest, ColdPreparationBindsWholeRowsAndHotLocatorsWithoutPerFieldDirectories) {
    NoPhysicalAllocator allocator; const auto current = sourceRoot();
    MetadataWriteBatch batch(allocator, current, LogSequenceNumber(9));
    const auto roots = inputRows(3, true); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        Plan plan(memory, roots, NODE, 1, batch.generation(), 3952);
        const auto prepared = Access::prepareCold(batch, plan);
        EXPECT_TRUE(prepared->owners().empty()); EXPECT_TRUE(prepared->columns().empty()); EXPECT_TRUE(prepared->extensions().empty());
        const auto groupSlot = HybridPropertyDirectoryCodec::decodeLocator(prepared->group().value);
        const auto &groupPage = imageFor(*prepared, groupSlot.page_id);
        const auto group = groupPage.readGroup(groupSlot, NODE, 1, batch.generation());
        EXPECT_EQ(group.mode, HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER); EXPECT_EQ(group.memberships, 192U); EXPECT_EQ(group.columns, 2U);
        const auto matrix = groupPage.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.cold_fields, 64U); EXPECT_EQ(matrix.cold_rows.owners, UINT64_MAX);
        size_t coldRows = 0, blocks = 0; uint64_t remaining = matrix.cold_rows.owners;
        while (remaining) {
            const auto slot = unsigned(std::countr_zero(remaining)); const auto reference = matrix.cold_rows.roots[slot];
            const auto bytes = imageFor(*prepared, reference.page_id).readRecord(reference);
            const auto block = HybridPropertyFieldView::decodeColdRows(bytes, reference, group, matrix,
                batch.generation(), batch.generation(), memory);
            remaining &= ~block.owners; ++blocks;
            for (unsigned row = 0; row < 64; ++row) if (const auto value = block.row(row)) {
                EXPECT_FALSE(value->large_value_reference); const auto decoded = EntityPropertyCollectionRootCodec::decode(value->root);
                EXPECT_EQ(decoded.owner_entity_id, row + 1U); ASSERT_EQ(decoded.entries.size(), 1U);
                EXPECT_EQ(decoded.entries[0].key_token, 3U); EXPECT_EQ(decoded.entries[0].value, PropertyValue(std::string(100, 'x'))); ++coldRows;
            }
        }
        EXPECT_EQ(coldRows, 64U); EXPECT_GT(blocks, 1U);
        for (const auto &column : plan.columns()) {
            const auto reference = matrix.columnLocator(column.identity.token);
            EXPECT_EQ(imageFor(*prepared, reference.page_id).readColumn(reference, column.identity, group, batch.generation()), column);
        }
        EXPECT_EQ(group.column_membership.page_id, groupSlot.page_id); EXPECT_NE(group.column_membership.slot_id, groupSlot.slot_id);
        EXPECT_EQ(batch.current(), current); EXPECT_EQ(batch.allocatedPageCount(), 0U); EXPECT_EQ(batch.createdPoolCount(), 0U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(roots, inputRows(3, true));
}
TEST(HybridPropertyConversionPreparationTest, ColdOnlyAndPropertylessPreparationKeepNativeOwnerIdentityWithoutSelectors) {
    for (const bool propertyless : {false, true}) {
        NoPhysicalAllocator allocator; MetadataWriteBatch batch(allocator, sourceRoot(), LogSequenceNumber(9));
        auto roots = inputRows(1, true); if (propertyless) for (auto &root : roots) root.entries.clear();
        auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
        {
            Plan plan(memory, roots, NODE, 1, batch.generation(), 3952); const auto prepared = Access::prepareCold(batch, plan);
            const auto ref = HybridPropertyDirectoryCodec::decodeLocator(prepared->group().value);
            const auto &page = imageFor(*prepared, ref.page_id); const auto group = page.readGroup(ref, NODE, 1, batch.generation());
            const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
            EXPECT_EQ(group.mode, HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER); EXPECT_EQ(group.columns, 0U);
            EXPECT_EQ(matrix.cold_fields, propertyless ? 0U : 64U); EXPECT_EQ(matrix.cold_rows.owners, propertyless ? 0U : UINT64_MAX);
            EXPECT_TRUE(prepared->owners().empty()); EXPECT_TRUE(prepared->extensions().empty());
        }
        EXPECT_EQ(memory->used(), 0U);
    }
}
TEST(HybridPropertyConversionPreparationTest, OversizedColdRowsRequireSameBatchResolverAndNeverPublishIndependentValues) {
    NoPhysicalAllocator allocator; const auto current = sourceRoot(); MetadataWriteBatch batch(allocator, current, LogSequenceNumber(9));
    auto roots = inputRows(1, true); for (auto &root : roots) root.entries[0].value = PropertyValue(std::string(8193, 'x'));
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        Plan plan(memory, roots, NODE, 1, batch.generation(), 3952); const auto retained = memory->used();
        EXPECT_THROW(Access::prepareCold(batch, plan), std::length_error);
        EXPECT_EQ(batch.current(), current); EXPECT_EQ(batch.candidate().next_logical_page_id, 100U);
        EXPECT_EQ(batch.allocatedPageCount(), 0U); EXPECT_EQ(batch.createdPoolCount(), 0U); EXPECT_EQ(memory->used(), retained);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyConversionPreparationTest, ColdPreparationCancellationAfterPrivateIdsRefundsAndRetriesSameSource) {
    NoPhysicalAllocator allocator; const auto current = sourceRoot(); const auto roots = inputRows(3, true);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        Plan plan(memory, roots, NODE, 1, Generation(9), 3952); const auto retained = memory->used();
        MetadataWriteBatch batch(allocator, current, LogSequenceNumber(9));
        EXPECT_THROW(Access::prepareCold(batch, plan, [&] {
            if (batch.candidate().next_logical_page_id > current.next_logical_page_id) throw std::runtime_error("cancel after private identity");
        }), std::runtime_error);
        EXPECT_EQ(batch.current(), current); EXPECT_EQ(memory->used(), retained);
        MetadataWriteBatch retry(allocator, current, LogSequenceNumber(9)); const auto prepared = Access::prepareCold(retry, plan);
        EXPECT_EQ(prepared->pages()[0].pageId(), PageId(100));
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyConversionPreparationTest, IncompressibleRowsThatIndividuallyFitSplitWithoutRequiringLargeValues) {
    NoPhysicalAllocator allocator; MetadataWriteBatch batch(allocator, sourceRoot(), LogSequenceNumber(9));
    auto roots = inputRows(1); uint32_t random = 0x6584a39bU;
    for (auto &root : roots) {
        PropertyValue::Bytes bytes(1925);
        for (auto &byte : bytes) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; byte = std::byte(random & 255U); }
        root.entries[0].value = PropertyValue(std::move(bytes));
    }
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        Plan plan(memory, roots, NODE, 1, batch.generation(), 3952); const auto prepared = Access::prepareCold(batch, plan);
        const auto ref = HybridPropertyDirectoryCodec::decodeLocator(prepared->group().value);
        const auto &page = imageFor(*prepared, ref.page_id); const auto group = page.readGroup(ref, NODE, 1, batch.generation());
        const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
        for (unsigned owner = 0; owner < 64; ++owner) {
            const auto rootRef = matrix.cold_rows.roots[owner]; const auto encoded = imageFor(*prepared, rootRef.page_id).readRecord(rootRef);
            EXPECT_LE(encoded.size(), RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES);
            const auto block = HybridPropertyFieldView::decodeColdRows(encoded, rootRef, group, matrix,
                batch.generation(), batch.generation(), memory);
            const auto row = block.row(owner); ASSERT_TRUE(row); EXPECT_FALSE(row->large_value_reference);
            EXPECT_EQ(EntityPropertyCollectionRootCodec::decode(row->root), roots[owner]);
        }
    }
    EXPECT_EQ(memory->used(), 0U);
}
#endif
} // namespace
