#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include "graph/storage/relocatable/index/BTreeIndex.hpp"

#include "graph/storage/relocatable/entity/EntityPage.hpp"

namespace graph::storage {

// Edge estimates retain the existing millisecond fit. Node estimates compare
// bytes of record decoding, page validation and index keys, using a bounded
// snapshot-owned page-locality sample when counts alone favor scanning.
// Edge costs estimate dispersed page occupancy and
// cursor-batch revisits using codec capacity and the real cursor batch size.
// A fitted residual margin favors scanning near ties. Unknown/inconsistent
// estimates and a scan exceeding the caller budget retain candidates.
// Without locality/fragmentation statistics the policy is conservative;
// estimates never prune results or answer queries. See p2-occupancy-cost-fit.
class NativeMembershipAccessCost final {
public:
 [[nodiscard]] static bool preferCandidates(uint64_t total, uint64_t candidates,
                                          size_t maximumRecords, bool edges) noexcept {
  // A scan would exceed the caller's budget. Unknown/inconsistent estimates
  // and empty estimates retain the authenticated candidate cursor as well.
  if (total == 0U || candidates == 0U || candidates > total || total > maximumRecords)
   return true;
  if (edges) {
   const uint64_t capacity = EntityPage::recordCapacity(EntityPageRecordKind::EPRK_EDGE);
   const uint64_t pages = total / capacity + (total % capacity != 0U ? 1U : 0U);
   // Uniform occupancy estimates unique touches; batch boundaries can
   // revisit one page. The fitted residual is a conservative cost margin.
   const long double uniquePages = pages == 1U ? 1.0L :
       static_cast<long double>(pages) * -std::expm1(-static_cast<long double>(candidates) / pages);
   const long double gatheredPages = uniquePages +
       (candidates - 1U) / BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES;
   const long double scan = 0.050L + 0.0001109749L * total;
   const long double indexed = 0.250L + 0.0001890586L * candidates + 0.01043849L * gatheredPages;
   return indexed + 0.250L < scan;
  }
  return preferNodeCandidates(total, candidates, maximumRecords);
 }
 // A sample affects access selection only. Cardinalities never answer a query;
 // candidates still authenticate primary records and every requested label.
 [[nodiscard]] static bool preferNodeCandidates(uint64_t total, uint64_t candidates,
     size_t maximumRecords, size_t sampleRows = 0U, size_t samplePages = 0U,
     size_t indexKeyBytes = 2U * sizeof(uint64_t) + 1U,
     size_t batchRows = BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES) noexcept {
  if (total == 0U || candidates == 0U || candidates > total || total > maximumRecords) return true;
  if (candidates == total) return false;
  const auto capacity = EntityPage::recordCapacity(EntityPageRecordKind::EPRK_NODE);
  const long double scanPages = total / capacity + (total % capacity != 0U);
  // Without an authenticated sample, charge a separate page per candidate.
  // This upper bound permits very sparse probes without paying for sampling.
  const long double candidatePages = sampleRows != 0U && samplePages != 0U && samplePages <= sampleRows
      ? std::ceil(static_cast<long double>(candidates) * samplePages / sampleRows) +
          (candidates - 1U) / std::max<size_t>(1U, batchRows)
      : static_cast<long double>(candidates);
  const long double scan = static_cast<long double>(total) * RSS_ENTITY_NODE_RECORD_BYTES + scanPages * RSS_PAGE_SIZE;
  const long double indexed = static_cast<long double>(candidates) *
      (RSS_ENTITY_NODE_RECORD_BYTES + static_cast<long double>(indexKeyBytes)) +
      (candidatePages + 2.0L) * RSS_PAGE_SIZE;
  return indexed < scan;
 }

};

} // namespace graph::storage
