#include "graph/query/execution/LogicalAggregateAccumulator.hpp"
#include <chrono>
#include <cstdio>
using namespace graph;
using namespace graph::query::execution;
int main() {
 for(size_t count: {2000U,4000U,8000U}) {
  CollectAccumulator accumulator;
  const auto start=std::chrono::steady_clock::now();
  size_t checksum=0;
  for(size_t i=0;i<count;++i) {
   checksum+=accumulator.retainedBytesForAdmission();
   accumulator.updateAdmitted(PropertyValue(int64_t(i)),[](size_t){});
   checksum+=accumulator.retainedBytesForAdmission();
  }
  const auto nanos=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
  if(accumulator.retainedBytesForAdmission()!=accumulator.retainedBytes()) return 2;
  std::printf("rows=%zu nanos=%lld checksum=%zu retained=%zu\n",count,(long long)nanos,checksum,accumulator.retainedBytes());
 }
}
