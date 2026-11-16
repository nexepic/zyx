#pragma once
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/OwnedVector.hpp"
#include "graph/utils/OwnedStdVector.hpp"

namespace graph::query::execution::detail {
// Call under the source's allocation scope. Plain hit records own no payload;
// copy the live suffix privately and keep both backing allocations charged.
template<class Hit> requires std::is_trivially_copyable_v<Hit>
bool compactConsumedPrefix(utils::OwnedStdVector<Hit> &hits, size_t &first, size_t headroom) {
    if (first == 0U || first >= hits.size()) return false;
    const size_t remaining = hits.size() - first;
    // Reclaim after a quarter is consumed while copy headroom is still
    // available. Across successful compactions fewer than 3*N hits are copied.
    if (first < remaining / 3U + (remaining % 3U != 0U) || hits.capacity() <= remaining) return false;
    const size_t backing = utils::detail::standardAllocatorRequestBytes(remaining * sizeof(Hit), alignof(Hit));
    const size_t proxy = utils::detail::standardContainerProxyBytes();
    if (backing > headroom || proxy > headroom - backing) return false;
    try {
        utils::OwnedStdVector<Hit> compacted(hits.begin() + first, hits.end());
        hits.swap(compacted);
    } catch (const MaterializedByteLimitExceeded &) { return false; }
      catch (const ResourceAdmissionDenied &) { return false; }
      catch (const std::bad_alloc &) { return false; }
    first = 0U;
    return true;
}

// Optional compaction before constructing the next output. The caller supplies
// headroom after charging every existing source and result owner. Old and new
// buffers coexist until the swap; element payload ownership only moves.
template<class Row>
bool compactConsumedPrefix(utils::OwnedVector<Row> &rows, size_t &first, size_t headroom) {
    if constexpr (!std::is_nothrow_move_constructible_v<Row> &&
        !(std::is_default_constructible_v<Row> && std::is_nothrow_swappable_v<Row>)) return false;
    else {
        if (first == 0U || first >= rows.size()) return false;
        const size_t remaining = rows.size() - first;
        // Reclaim after at least a quarter is consumed, while temporary space
        // can still fit. Geometric compaction moves fewer than 3*N elements.
        if (first < remaining / 3U + (remaining % 3U != 0U) || rows.capacity() <= remaining ||
                remaining > headroom / sizeof(Row)) return false;
        utils::OwnedVector<Row> compacted(rows.get_allocator());
        try {
            compacted.reserve(remaining);
            if constexpr (!std::is_nothrow_move_constructible_v<Row>) compacted.resize(remaining);
        }
        catch (const MaterializedByteLimitExceeded &) { return false; }
        catch (const ResourceAdmissionDenied &) { return false; }
        catch (const std::bad_alloc &) { return false; }
        // Prepare every empty destination before touching the source. A
        // throwing move trait need not prevent a nonthrowing node/credit swap.
        for (size_t index = first; index < rows.size(); ++index) {
            if constexpr (std::is_nothrow_move_constructible_v<Row>)
                compacted.emplace_back(std::move(rows[index]));
            else { using std::swap; swap(compacted[index - first], rows[index]); }
        }
        rows.swap(compacted);
        first = 0U;
        return true;
    }
}
}
