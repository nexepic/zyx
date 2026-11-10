#pragma once
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/OwnedVector.hpp"

namespace graph::query::execution::detail {
// Optional compaction before constructing the next output. The caller supplies
// headroom after charging every existing source and result owner. Old and new
// buffers coexist until the swap; element payload ownership only moves.
template<class Row>
bool compactConsumedPrefix(utils::OwnedVector<Row> &rows, size_t &first, size_t headroom) {
    if constexpr (!std::is_nothrow_move_constructible_v<Row>) return false;
    else {
        if (first == 0U || first >= rows.size()) return false;
        const size_t remaining = rows.size() - first;
        // Reclaim after at least a quarter is consumed, while temporary space
        // can still fit. Geometric compaction moves fewer than 3*N elements.
        if (first < remaining / 3U + (remaining % 3U != 0U) || rows.capacity() <= remaining ||
                remaining > headroom / sizeof(Row)) return false;
        utils::OwnedVector<Row> compacted(rows.get_allocator());
        try { compacted.reserve(remaining); }
        catch (const MaterializedByteLimitExceeded &) { return false; }
        catch (const ResourceAdmissionDenied &) { return false; }
        catch (const std::bad_alloc &) { return false; }
        for (size_t index = first; index < rows.size(); ++index)
            compacted.emplace_back(std::move(rows[index]));
        rows.swap(compacted);
        first = 0U;
        return true;
    }
}
}
