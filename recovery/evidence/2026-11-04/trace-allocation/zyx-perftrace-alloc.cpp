#include "graph/debug/PerfTrace.hpp"
#include <cstdlib>
#include <cstdio>
#include <new>
#include <cassert>
static bool track = false;
static size_t count = 0, bytes = 0;
void *operator new(size_t n) {if (track) {++count; bytes += n;} if(void *p=std::malloc(n?n:1))return p; throw std::bad_alloc();}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept {std::free(p);}
int main() {
 using graph::debug::PerfTrace;
 PerfTrace::setEnabled(true);
 const char *key="logical_plan.long_allocation_probe";
 PerfTrace::addValue(key,1); PerfTrace::addDuration(key,1); PerfTrace::addDurationBatch(key,1,2);
 track=true;
 for(size_t i=0;i<100000;i++) { PerfTrace::addValue(key,1); PerfTrace::addDuration(key,1); PerfTrace::addDurationBatch(key,1,2); }
 track=false;
 auto snapshot=PerfTrace::snapshotAndReset();
 auto &v=snapshot.at(key);
 assert(v.valueCalls==100001 && v.calls==300003 && v.totalNs==200002);
 std::printf("allocation_calls=%zu requested_bytes=%zu values=%llu durations=%llu\n",count,bytes,(unsigned long long)v.valueCalls,(unsigned long long)v.calls);
}
