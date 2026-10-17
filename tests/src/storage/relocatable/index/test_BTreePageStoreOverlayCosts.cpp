#include "RelocatableBTreePageStoreTestUtils.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/utils/UnmanagedAllocationResource.hpp"

namespace {
using namespace relocatable_btree_test;

class OverlayAdmissionCounter final : public graph::utils::UnmanagedAllocationResource {
public:
 size_t total = 0U;
 size_t retained = 0U;
 void reserveUnmanaged(size_t bytes) override { total += bytes; retained += bytes; }
 void releaseUnmanaged(size_t bytes) noexcept override { retained -= bytes; }
private:
 void *do_allocate(size_t bytes, size_t alignment) override {
  void *result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
  reserveUnmanaged(bytes);
  return result;
 }
 void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
  std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
  releaseUnmanaged(bytes);
 }
 bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

std::vector<std::byte> overlayCostKey(uint16_t ordinal) {
 std::vector<std::byte> result(32U, std::byte{'p'});
 result.push_back(static_cast<std::byte>(ordinal >> 8U));
 result.push_back(static_cast<std::byte>(ordinal));
 return result;
}

TEST_F(RelocatableBTreePageStoreTest, OrderedOverlayAppendDoesNotCopyEveryExistingLeafKey) {
 auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
 SlottedPagePool pages(io, *metadata);
 BTreePageStore store(pages, *metadata);
 const auto request = allocationRequest();
 ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
                           LogSequenceNumber(2)).has_value());
 const IndexRootLocation root{17U, request.page_id, request.generation};
 static_cast<void>(store.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3)));
 constexpr size_t count = 1000U;
 std::vector<BTreeScalarIndexOperation> operations;
 for (uint16_t i = 0U; i < count; ++i)
  operations.push_back({BTreeScalarIndexOperationKind::BSIOP_UPSERT, overlayCostKey(i), i + 1U});
 auto snapshot = metadata->openSnapshot();
 auto counter = std::make_shared<OverlayAdmissionCounter>();
 {
  graph::utils::OwnedAllocationScope admission(counter);
  const auto plan = store.planScalarIndexTransactionMutation(snapshot, 17U, operations);
  EXPECT_FALSE(plan.final_tree_empty);
  EXPECT_FALSE(plan.pool_requests.empty());
 }
 // Geometric backing growth, split copies and bounded append-size previews
 // need linear credit. Recopying a whole leaf on every append exceeds this
 // deliberately loose bound by an order of magnitude on the old path.
 const size_t linearAllowance = count * (sizeof(BTreePageEntry) + overlayCostKey(0).size()) * 32U;
 RecordProperty("cumulative_admitted_bytes", std::to_string(counter->total));
 EXPECT_LT(counter->total, linearAllowance);
 snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, OverlayLeafRedistributionDoesNotMaterializeEveryCandidateSplit) {
 auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
 SlottedPagePool pages(io, *metadata);
 NativeBTreeWriteContext writes(io, *metadata, pages);
 BTreePageStore store(pages, *metadata);
 const auto request = allocationRequest();
 ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
                           LogSequenceNumber(2)).has_value());
 const IndexRootLocation root{17U, request.page_id, request.generation};
 static_cast<void>(store.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3)));
 constexpr uint16_t count = 210U;
 static_cast<void>(store.materializeAndPublishTree(request.segment_id, 17U, 3U, 0U, LogSequenceNumber(4),
  [&](const CatalogEntry &pool, std::span<const PageId> ids) {
   std::vector<BTreePage> result;
   for (size_t page = 0U; page < 3U; ++page) {
    BTreePageHeader header;
    header.index_id = 17U; header.page_id = ids[page]; header.generation = pool.generation;
    std::vector<BTreePageEntry> entries;
    if (page == 0U) {
     header.kind = BTreeNodeKind::BPN_INTERNAL; header.level = 1U; header.leftmost_child = ids[1];
     entries.push_back({overlayCostKey(10U), ids[2].value()});
    } else {
     if (page == 1U) header.right_sibling = ids[2]; else header.left_sibling = ids[1];
     for (uint16_t i = page == 1U ? 0U : 10U; i < (page == 1U ? 10U : count); ++i)
      entries.push_back({overlayCostKey(i), UINT64_MAX});
    }
    result.push_back(BTreePage::createWithEntries(treeOwner(pool, ids[page], LogSequenceNumber(4)), header, entries));
   }
   return result;
  }));
 auto old = metadata->openSnapshot();
 auto transaction = writes.beginTransaction();
 transaction.stageScalarIndexErase(store, 17U, overlayCostKey(9U));
 const bool valuesEnabled = graph::debug::PerfTrace::areValuesEnabled();
 graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
 const bool committed = transaction.commit().committed();
 const auto trace = graph::debug::PerfTrace::snapshotAndReset();
 graph::debug::PerfTrace::setValueCollectionEnabled(valuesEnabled);
 ASSERT_TRUE(committed);
 ASSERT_TRUE(trace.contains("native_btree.read.packed_page_decodes"));
 const auto decodes = trace.at("native_btree.read.packed_page_decodes").totalValue;
 RecordProperty("packed_page_decodes", std::to_string(decodes));
 // Only three source pages and the selected replacement pages need codecs;
 // the number of candidate boundaries must not multiply page decodes.
 EXPECT_LE(decodes, 3U * 16U);
 BTreeIndex index(17U, store, *metadata);
 auto current = metadata->openSnapshot();
 for (uint16_t i = 0U; i < count; ++i) {
  EXPECT_EQ(index.find(old, overlayCostKey(i)), std::optional<uint64_t>(UINT64_MAX));
  EXPECT_EQ(index.find(current, overlayCostKey(i)), i == 9U ? std::nullopt : std::optional<uint64_t>(UINT64_MAX));
 }
 current.release(); old.release();
 auto reopened = RelocatableMetadataStore::open(io);
 SlottedPagePool reopenedPages(io, *reopened);
 BTreePageStore reopenedStore(reopenedPages, *reopened);
 BTreeIndex reopenedIndex(17U, reopenedStore, *reopened);
 auto durable = reopened->openSnapshot();
 EXPECT_EQ(reopenedIndex.scan(durable, {}, count + 1U).size(), count - 1U);
 durable.release();
}
} // namespace

