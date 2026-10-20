#include <gtest/gtest.h>
#include "graph/storage/relocatable/metadata/HybridPropertyConversionPreparation.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace graph::storage {
class HybridPropertyConversionPreparationTestAccess final {
public:
    static auto prepare(MetadataWriteBatch &batch, const HybridPropertyGroupConversion &plan,
        const std::function<void()> &checkpoint = {}) {
        return std::unique_ptr<HybridPropertyConversionPreparation>(new HybridPropertyConversionPreparation(batch, plan, checkpoint));
    }
};
}
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
#endif
} // namespace
