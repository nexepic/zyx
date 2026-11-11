#include <cassert>
#include <memory>
#include "query/execution/ConsumedPrefixCompaction.hpp"
struct SwapOnlyRow {
    static inline unsigned defaults = 0U, failDefault = 0U, moves = 0U;
    static inline bool semanticFailure = false;
    std::unique_ptr<int> payload;
    SwapOnlyRow() {
        if (++defaults == failDefault) {
            if (semanticFailure) throw std::runtime_error("empty preparation cancelled");
            throw std::bad_alloc();
        }
    }
    explicit SwapOnlyRow(int value) : payload(std::make_unique<int>(value)) {}
    SwapOnlyRow(SwapOnlyRow &&) noexcept(false) { ++moves; throw std::runtime_error("unsafe move"); }
    SwapOnlyRow(const SwapOnlyRow &) = delete;
    friend void swap(SwapOnlyRow &left, SwapOnlyRow &right) noexcept { left.payload.swap(right.payload); }
};
int main() {
 using graph::utils::OwnedVector;
 using graph::query::execution::detail::compactConsumedPrefix;
 OwnedVector<SwapOnlyRow> rows; rows.reserve(4);
 for(int i=0;i<4;++i) rows.emplace_back(i);
 auto *third=rows[2].payload.get(), *fourth=rows[3].payload.get();
 size_t first=2; assert(compactConsumedPrefix(rows,first,2*sizeof(SwapOnlyRow)));
 assert(first==0 && rows.size()==2 && rows[0].payload.get()==third && rows[1].payload.get()==fourth);
 assert(SwapOnlyRow::moves==0);
}
