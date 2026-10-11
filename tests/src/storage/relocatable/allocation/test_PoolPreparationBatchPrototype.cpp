#include <gtest/gtest.h>
#include <array>
#include "benchmarks/graphdb-comparison/native-write-batch-cost/PoolPreparationRun.hpp"
using namespace graph::storage;

TEST(PoolPreparationBatchPrototypeTest, OwnsPayloadAndScattersOnlyAfterOneSuccessfulCallback) {
 CatalogEntry pool; pool.segment_id = SegmentId(3U); pool.generation = Generation(2U);
 benchmark::PoolPreparationBatch batch(pool, 1U);
 std::array<SharedPagePoolPreparedAppend, 2> pages;
 std::array<PageLocation, 2> outputs;
 for (size_t i = 0; i < 2; ++i) {
  pages[i].owner.page_id = PageId(i + 10U); pages[i].owner.payload_bytes = 1U;
  pages[i].payload = {std::byte(i + 1U)};
  outputs[i] = {pages[i].owner.page_id, pool.segment_id, 0U, pool.generation, 0U};
  batch.append(pool, i + 1U, std::span<const SharedPagePoolPreparedAppend>(&pages[i], 1U),
   std::span<PageLocation>(&outputs[i], 1U));
 }
 const auto before = outputs; pages[0].payload[0] = std::byte{99};
 size_t calls = 0U;
 batch.flush([&](const CatalogEntry &, auto pending, auto results, size_t prefix) {
  ++calls; EXPECT_EQ(pending.size(), 2U); EXPECT_EQ(pending[0].payload[0], std::byte{1});
  EXPECT_EQ(outputs, before);
  for (size_t i = 0; i < results.size(); ++i) results[i].page_index = prefix + i + 1U;
 });
 EXPECT_EQ(calls, 1U); EXPECT_EQ(outputs[0].page_index, 2U); EXPECT_EQ(outputs[1].page_index, 3U);
 EXPECT_THROW(batch.flush([](auto &&...) {}), std::logic_error);
}

TEST(PoolPreparationBatchPrototypeTest, CallbackFailureAndWrongIdentityLeaveOriginalOutputsUnchanged) {
 CatalogEntry pool; pool.segment_id = SegmentId(3U); pool.generation = Generation(2U);
 SharedPagePoolPreparedAppend page; page.owner.page_id = PageId(10U);
 PageLocation output{page.owner.page_id, pool.segment_id, 0U, pool.generation, 0U};
 benchmark::PoolPreparationBatch batch(pool, 0U);
 batch.append(pool, 0U, std::span<const SharedPagePoolPreparedAppend>(&page, 1U), std::span<PageLocation>(&output, 1U));
 const auto before = output;
 EXPECT_THROW(batch.flush([](auto &, auto, auto results, size_t) {
  results[0].page_index = 1U; throw std::runtime_error("injected callback failure");
 }), std::runtime_error);
 EXPECT_EQ(output, before);
 EXPECT_THROW(batch.flush([](auto &, auto, auto results, size_t) {
  results[0].page_index = 1U; results[0].generation = Generation(99U);
 }), std::runtime_error);
 EXPECT_EQ(output, before);
 EXPECT_EQ(batch.pageCount(), 1U);
}

TEST(PoolPreparationBatchPrototypeTest, RejectsDuplicateAndOversizedFragmentsBeforeChangingPendingBatch) {
 CatalogEntry pool; pool.segment_id = SegmentId(3U); pool.generation = Generation(2U);
 SharedPagePoolPreparedAppend page; page.owner.page_id = PageId(10U);
 PageLocation output{page.owner.page_id, pool.segment_id, 0U, pool.generation, 0U};
 benchmark::PoolPreparationBatch batch(pool, 0U);
 auto append = [&](size_t prefix) { batch.append(pool, prefix,
  std::span<const SharedPagePoolPreparedAppend>(&page, 1U), std::span<PageLocation>(&output, 1U)); };
 page.payload.resize(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 1U);
 page.owner.payload_bytes = page.payload.size();
 EXPECT_THROW(append(0U), std::invalid_argument); EXPECT_EQ(batch.pageCount(), 0U);
 page.payload.clear(); page.owner.payload_bytes = 0U;
 append(0U);
 EXPECT_THROW(append(1U), std::invalid_argument); EXPECT_EQ(batch.pageCount(), 1U);
 EXPECT_EQ(output.page_index, 0U);
}
