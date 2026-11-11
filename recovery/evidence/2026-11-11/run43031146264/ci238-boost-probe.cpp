#include <cassert>
#include <cstdio>
#include <memory>
#include <boost/version.hpp>
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
    SwapOnlyRow &operator=(const SwapOnlyRow &) = delete;
    // Boost instantiates assignment paths during resize; executing one is still unsafe.
    SwapOnlyRow &operator=(SwapOnlyRow &&) noexcept(false) {
        ++moves; throw std::runtime_error("unsafe move assignment");
    }
    friend void swap(SwapOnlyRow &left, SwapOnlyRow &right) noexcept { left.payload.swap(right.payload); }
};
int main() {
 using graph::utils::OwnedVector;
 using graph::query::execution::detail::compactConsumedPrefix;
 for(unsigned scenario=0;scenario<3;++scenario) {
  SwapOnlyRow::defaults=SwapOnlyRow::moves=0;
  SwapOnlyRow::failDefault=scenario ? 2 : 0;
  SwapOnlyRow::semanticFailure=scenario==2;
  OwnedVector<SwapOnlyRow> rows; rows.reserve(4);
  for(int i=0;i<4;++i) rows.emplace_back(i);
  auto *third=rows[2].payload.get(), *fourth=rows[3].payload.get();
  size_t first=2;
  if(scenario==2) {
   bool caught=false;
   try { compactConsumedPrefix(rows,first,2*sizeof(SwapOnlyRow)); }
   catch(const std::runtime_error &e) { caught=std::string_view(e.what())=="empty preparation cancelled"; }
   assert(caught && first==2 && rows.size()==4 && rows[2].payload.get()==third);
  } else {
   bool changed=compactConsumedPrefix(rows,first,2*sizeof(SwapOnlyRow));
   if(scenario==1) {
    assert(!changed && first==2 && rows.size()==4 && rows[2].payload.get()==third);
    SwapOnlyRow::failDefault=0;
    changed=compactConsumedPrefix(rows,first,2*sizeof(SwapOnlyRow));
   }
   assert(changed && first==0 && rows.size()==2);
   assert(rows[0].payload.get()==third && rows[1].payload.get()==fourth);
  }
  assert(SwapOnlyRow::moves==0);
 }
 std::printf("CI_BOOST_COMPACTION version=%d scenarios=3 move_operations=0 identity_retry_semantics=passed native_windows=0\n", BOOST_VERSION);
}
