/**
 * @file test_BTreePageStoreAppendSize.cpp
 * @brief Exact transaction-local append sizing across structural and mixed writes.
 */
#include <array>
#include <map>
#include "RelocatableBTreePageStoreTestUtils.hpp"
#include "graph/debug/PerfTrace.hpp"

namespace {
using namespace relocatable_btree_test;

TEST_F(RelocatableBTreePageStoreTest, CachedAppendSizePreservesMixedWritesAndOldSnapshots) {
 auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
 SlottedPagePool pages(io, *metadata);
 NativeBTreeWriteContext writes(io, *metadata, pages);
 BTreePageStore store(pages, *metadata);
 const auto request = allocationRequest();
 ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
                           LogSequenceNumber(2)).has_value());
 const IndexRootLocation root{17, request.page_id, request.generation};
 static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
 auto before = metadata->openSnapshot();
 const auto key = [](uint16_t ordinal) {
  std::vector<std::byte> bytes(300U, std::byte{'k'});
  bytes.push_back(static_cast<std::byte>(ordinal >> 8U));
  bytes.push_back(static_cast<std::byte>(ordinal));
  return bytes;
 };
 const std::array<uint64_t, 6> values{1U, 127U, 128U, 16383U, 16384U, UINT64_MAX};
 std::map<std::vector<std::byte>, uint64_t> expected;
 auto transaction = writes.beginTransaction();
 for (uint16_t i = 0; i < 400; ++i) {
  const auto bytes = key(i);
  const auto value = values[i % values.size()];
  transaction.stageScalarIndexUpsert(store, 17, bytes, value);
  expected[bytes] = value;
 }
 // Value replacement must discard the previous append sizing proof, even if
 // the entry count stays constant and the new value needs a wider varint.
 transaction.stageScalarIndexUpsert(store, 17, key(399), UINT64_MAX);
 expected[key(399)] = UINT64_MAX;
 transaction.stageScalarIndexErase(store, 17, key(31));
 expected.erase(key(31));
 transaction.stageScalarIndexUpsert(store, 17, key(31), 128U);
 expected[key(31)] = 128U;
 for (uint16_t i = 400; i < 450; ++i) {
  transaction.stageScalarIndexUpsert(store, 17, key(i), values[i % values.size()]);
  expected[key(i)] = values[i % values.size()];
 }
 const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
 graph::debug::PerfTrace::reset();
 graph::debug::PerfTrace::setValueCollectionEnabled(true);
 bool committed = false;
 EXPECT_NO_THROW(committed = transaction.commit().committed());
 const auto trace = graph::debug::PerfTrace::snapshotAndReset();
 graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
 ASSERT_TRUE(committed);
 ASSERT_TRUE(trace.contains("native_btree.plan.cached_leaf_append_size"));
 EXPECT_GT(trace.at("native_btree.plan.cached_leaf_append_size").totalValue, 200);
 BTreeIndex index(17, store, *metadata);
 EXPECT_TRUE(index.scan(before, {}, 500).empty());
 auto current = metadata->openSnapshot();
 std::vector<BTreePageEntry> entries;
 for (const auto &[bytes, value] : expected) entries.push_back({bytes, value});
 EXPECT_EQ(index.scan(current, {}, 500), entries);
 EXPECT_EQ(index.find(current, key(31)), std::optional<uint64_t>(128U));
 EXPECT_EQ(index.find(current, key(399)), std::optional<uint64_t>(UINT64_MAX));
 current.release();
 before.release();
}

TEST_F(RelocatableBTreePageStoreTest, OrderedUpsertRunsPreserveDuplicateStatementsAndEraseBarriers) {
 auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
 SlottedPagePool pages(io, *metadata);
 NativeBTreeWriteContext writes(io, *metadata, pages);
 BTreePageStore store(pages, *metadata);
 const auto request = allocationRequest();
 ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
  LogSequenceNumber(2)).has_value());
 const IndexRootLocation root{17, request.page_id, request.generation};
 static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
 auto before = metadata->openSnapshot();
 const auto key = [](uint16_t i) {
  std::vector<std::byte> bytes(300U, std::byte{'k'});
  bytes.push_back(static_cast<std::byte>(i >> 8U));
  bytes.push_back(static_cast<std::byte>(i));
  return bytes;
 };
 std::map<std::vector<std::byte>, uint64_t> expected;
 auto transaction = writes.beginTransaction();
 for (uint16_t i = 0; i < 450; ++i) {
  const auto bytes = key((i * 137U) % 450U); // full deterministic permutation
  transaction.stageScalarIndexUpsert(store, 17, bytes, i + 1U);
  expected[bytes] = i + 1U;
 }
 transaction.stageScalarIndexUpsert(store, 17, key(31), UINT64_MAX);
 transaction.stageScalarIndexUpsert(store, 17, key(31), 128U);
 expected[key(31)] = 128U;
 transaction.stageScalarIndexErase(store, 17, key(31));
 expected.erase(key(31));
 transaction.stageScalarIndexUpsert(store, 17, key(400), 16384U);
 expected[key(400)] = 16384U;
 transaction.stageScalarIndexUpsert(store, 17, key(31), 1U);
 expected[key(31)] = 1U;
 const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
 graph::debug::PerfTrace::reset();
 graph::debug::PerfTrace::setValueCollectionEnabled(true);
 bool committed = false;
 EXPECT_NO_THROW(committed = transaction.commit().committed());
 const auto trace = graph::debug::PerfTrace::snapshotAndReset();
 graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
 ASSERT_TRUE(committed);
 EXPECT_GE(trace.at("native_btree.plan.ordered_upsert_run_entries").totalValue, 452);
 EXPECT_GT(trace.at("native_btree.plan.cached_leaf_append_size").totalValue, 350);
 BTreeIndex index(17, store, *metadata);
 EXPECT_TRUE(index.scan(before, {}, 500).empty());
 auto current = metadata->openSnapshot();
 std::vector<BTreePageEntry> entries;
 for (const auto &[bytes, value] : expected) entries.push_back({bytes, value});
 EXPECT_EQ(index.scan(current, {}, 500), entries);
 EXPECT_EQ(index.find(current, key(31)), std::optional<uint64_t>(1U));
}
TEST_F(RelocatableBTreePageStoreTest, ReclaimsOnlyInvisibleVersionsBeforeRebuildingAnExhaustedOverlay) {
 auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
 SlottedPagePool pages(io, *metadata);
 BTreePageStore store(pages, *metadata);
 NativeBTreeWriteContext writes(io, *metadata, pages);
 const auto request = allocationRequest();
 ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
                           LogSequenceNumber(2)).has_value());
 const IndexRootLocation root{17U, request.page_id, request.generation};
 static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
 const std::vector<std::byte> key{std::byte{'a'}};
 std::vector<MetadataSnapshotPin> pinned;
 uint64_t value = 0U;
 uint64_t lsn = 4U;
 while (true) {
  auto current = metadata->openSnapshot();
  if (pages.availableFrameCount(current, request.segment_id) == 0U) break;
  pinned.push_back(std::move(current));
  static_cast<void>(store.upsertRootLeaf(17U, key, ++value, LogSequenceNumber(lsn++)));
 }
 ASSERT_GT(pinned.size(), 2U);
 const std::array operations = {
  BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT, key, value + 1U},
 };
 // With every old version still pinned, reclamation must leave the pool full
 // and preserve the fresh-tree fallback instead of reusing any visible frame.
 auto fullSnapshot = metadata->openSnapshot();
 const auto fullPlan = store.planScalarIndexTransactionMutation(fullSnapshot, 17U, operations, true);
 EXPECT_TRUE(fullPlan.new_pool_mutation.has_value());
 EXPECT_TRUE(fullPlan.replacement_root.has_value());
 EXPECT_EQ(pages.availableFrameCount(fullSnapshot, request.segment_id), 0U);
 fullSnapshot.release();
 BTreeIndex index(17U, store, *metadata);
 EXPECT_EQ(index.find(pinned[1], key), std::optional<uint64_t>(1U));
 for (size_t i = 0; i < pinned.size(); ++i) {
  if (i != 1U) pinned[i].release();
 }
 auto before = metadata->openSnapshot();
 ASSERT_EQ(pages.availableFrameCount(before, request.segment_id), 0U);
 const auto plan = store.planScalarIndexTransactionMutation(before, 17U, operations, true);
 ASSERT_EQ(plan.pool_requests.size(), 1U);
 EXPECT_FALSE(plan.new_pool_mutation.has_value());
 EXPECT_FALSE(plan.replacement_root.has_value());
 ASSERT_EQ(plan.pool_requests.front().expected_replacements.size(), 1U);
 EXPECT_EQ(plan.pool_requests.front().expected_replacements.front().page_id, root.root_page_id);
 EXPECT_TRUE(plan.pool_requests.front().expected_retirements.empty());
 auto transaction = writes.beginTransaction();
 transaction.stageScalarIndexUpsert(store, 17U, key, value + 1U);
 ASSERT_TRUE(transaction.commit().committed());
 auto after = metadata->openSnapshot();
 EXPECT_EQ(index.find(pinned[1], key), std::optional<uint64_t>(1U));
 EXPECT_EQ(index.find(before, key), std::optional<uint64_t>(value));
 EXPECT_EQ(index.find(after, key), std::optional<uint64_t>(value + 1U));
 after.release();
 before.release();
 pinned[1].release();
 auto reopened = RelocatableMetadataStore::open(io);
 SlottedPagePool reopenedPages(io, *reopened);
 BTreePageStore reopenedStore(reopenedPages, *reopened);
 BTreeIndex reopenedIndex(17U, reopenedStore, *reopened);
 auto durable = reopened->openSnapshot();
 EXPECT_EQ(reopenedIndex.find(durable, key), std::optional<uint64_t>(value + 1U));
 durable.release();
}

TEST_F(RelocatableBTreePageStoreTest, VirtualInternalChildrenReserveTheirPersistedVarintWidth) {
 auto initial = superblock();
 initial.next_logical_page_id = uint64_t{1} << 63U;
 auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
 SlottedPagePool pages(io, *metadata);
 NativeBTreeWriteContext writes(io, *metadata, pages);
 BTreePageStore store(pages, *metadata);
 const auto request = allocationRequest();
 ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
  LogSequenceNumber(2)).has_value());
 const IndexRootLocation root{17U, request.page_id, request.generation};
 static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
 auto before = metadata->openSnapshot();
 std::vector<std::vector<std::byte>> keys;
 for (uint8_t ordinal = 0U; ordinal < 6U; ++ordinal)
  keys.emplace_back(1908U, static_cast<std::byte>('a' + ordinal));
 auto transaction = writes.beginTransaction();
 for (size_t ordinal = 0U; ordinal < keys.size(); ++ordinal)
  transaction.stageScalarIndexUpsert(store, 17U, keys[ordinal], ordinal + 1U);
 bool committed = false;
 ASSERT_NO_THROW(committed = transaction.commit().committed());
 ASSERT_TRUE(committed);
 BTreeIndex index(17U, store, *metadata);
 EXPECT_TRUE(index.scan(before, {}, keys.size() + 1U).empty());
 auto after = metadata->openSnapshot();
 const auto rows = index.scan(after, {}, keys.size() + 1U);
 ASSERT_EQ(rows.size(), keys.size());
 for (size_t ordinal = 0U; ordinal < keys.size(); ++ordinal) {
  EXPECT_EQ(rows[ordinal].key, keys[ordinal]);
  EXPECT_EQ(rows[ordinal].value_or_child, ordinal + 1U);
 }
 const auto newRoot = metadata->findIndexRoot(after, 17U);
 EXPECT_GE(newRoot.root_page_id.value(), uint64_t{1} << 63U);
 after.release(); before.release();
 auto reopened = RelocatableMetadataStore::open(io);
 SlottedPagePool durablePages(io, *reopened);
 BTreePageStore durableStore(durablePages, *reopened);
 BTreeIndex durableIndex(17U, durableStore, *reopened);
 auto durable = reopened->openSnapshot();
 EXPECT_EQ(durableIndex.scan(durable, {}, keys.size() + 1U), rows);
 durable.release();
}

} // namespace
