#pragma once
#include <stdexcept>
#include <cstddef>
#include <span>
#include <vector>
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/metadata/Catalog.hpp"

namespace graph::storage::detail {
// Plan for coalescing a contiguous, allocator-reserved new-pool suffix.
// It does not authenticate bytes or write storage; the allocator still owns those duties.
class PoolPreparationRun {
public:
 explicit PoolPreparationRun(CatalogEntry pool, size_t prefix) : pool_(pool), prefix_(prefix) {
  if (prefix > RSS_PAGE_POOL_DATA_FRAME_COUNT) throw std::invalid_argument("invalid pool prefix");
 }
 void append(const CatalogEntry &pool, size_t prefix, size_t count) {
  const bool same = pool.segment_id == pool_.segment_id && pool.physical_slot == pool_.physical_slot &&
   pool.kind == pool_.kind && pool.owner_id == pool_.owner_id && pool.generation == pool_.generation &&
   pool.state == pool_.state && pool.committed_lsn == pool_.committed_lsn;
  if (!same || !count || prefix != prefix_ + count_ || count > RSS_PAGE_POOL_DATA_FRAME_COUNT - prefix_ - count_)
   throw std::invalid_argument("noncontiguous or incompatible pool preparation fragment");
  count_ += count;
 }
 [[nodiscard]] size_t initialPrefix() const { return prefix_; }
 [[nodiscard]] size_t pageCount() const { return count_; }
private:
 CatalogEntry pool_;
 size_t prefix_, count_ = 0U;
};
// Output spans must outlive the batch. Only checked allocator callbacks may
// write durable bytes; a failed flush is aborted by the metadata coordinator.
class PoolPreparationBatch {
public:
 PoolPreparationBatch(CatalogEntry pool, size_t prefix) : pool_(pool), run_(pool, prefix) {
  pages_.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
  locations_.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
  outputs_.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
 }
 void append(const CatalogEntry &pool, size_t prefix,
             std::span<const SharedPagePoolPreparedAppend> pages, std::span<PageLocation> outputs) {
  if (finished_ || pages.size() != outputs.size()) throw std::invalid_argument("invalid preparation shape");
  auto next = run_; next.append(pool, prefix, pages.size());
  for (size_t i = 0; i < pages.size(); ++i) {
   const auto &p = pages[i]; const auto &l = outputs[i];
   if (p.payload.size() > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES || p.owner.payload_bytes != p.payload.size() ||
       p.owner.page_id != l.page_id || l.segment_id != pool.segment_id || l.generation != pool.generation ||
       l.page_index != 0U) throw std::invalid_argument("invalid prepared page association");
   for (const auto &prior : pages_) if (prior.owner.page_id == p.owner.page_id)
    throw std::invalid_argument("duplicate prepared page");
   for (size_t j = 0; j < i; ++j) if (pages[j].owner.page_id == p.owner.page_id)
    throw std::invalid_argument("duplicate prepared page");
  }
  // Complete potentially throwing copies before changing the pending run.
  std::vector<SharedPagePoolPreparedAppend> copied(pages.begin(), pages.end());
  for (size_t i = 0; i < copied.size(); ++i) {
   pages_.push_back(std::move(copied[i])); locations_.push_back(outputs[i]); outputs_.push_back(&outputs[i]);
  }
  run_ = next;
 }
 template<typename Writer> void flush(Writer writer) {
  if (finished_ || pages_.empty()) throw std::logic_error("preparation batch is not pending");
  auto result = locations_;
  writer(pool_, std::span<const SharedPagePoolPreparedAppend>(pages_), std::span<PageLocation>(result), run_.initialPrefix());
  for (size_t i = 0; i < result.size(); ++i) {
   if (result[i].page_id != locations_[i].page_id || result[i].segment_id != pool_.segment_id ||
       result[i].generation != pool_.generation || result[i].page_index != run_.initialPrefix() + i + 1U ||
       result[i].free_bytes != pages_[i].free_bytes)
    throw std::runtime_error("allocator changed preparation identity or frame order");
  }
  for (size_t i = 0; i < result.size(); ++i) *outputs_[i] = result[i];
  finished_ = true; pages_.clear(); outputs_.clear(); locations_.clear();
 }
 [[nodiscard]] size_t pageCount() const { return run_.pageCount(); }
 [[nodiscard]] SegmentId segmentId() const { return pool_.segment_id; }
private:
 CatalogEntry pool_;
 PoolPreparationRun run_;
 std::vector<SharedPagePoolPreparedAppend> pages_;
 std::vector<PageLocation> locations_;
 std::vector<PageLocation *> outputs_;
 bool finished_ = false;
};

}
