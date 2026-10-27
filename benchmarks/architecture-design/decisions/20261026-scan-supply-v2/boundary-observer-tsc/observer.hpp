#pragma once
// Linux-only attribution executable; never included by production sources.
#include <time.h>
#include <sys/resource.h>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
namespace boundary {
inline thread_local bool enabled = false;
inline thread_local unsigned depth = 0;
inline thread_local uint64_t source_ns = 0, callback_ns = 0, calls = 0, callbacks = 0;
inline uint64_t ticks() noexcept {
 unsigned lo=0,hi=0;
 asm volatile("lfence; rdtsc; lfence" : "=a"(lo),"=d"(hi) :: "memory");
 return (uint64_t(hi)<<32U)|lo;
}
inline uint64_t rawNow() noexcept {
 timespec t{}; clock_gettime(CLOCK_MONOTONIC_RAW,&t);
 return uint64_t(t.tv_sec)*1000000000ULL+uint64_t(t.tv_nsec);
}
inline double nanosecondsPerTick() noexcept {
 static const double scale=[] {
  const auto start=rawNow(),first=ticks();
  timespec pause{0,10000000}; nanosleep(&pause,nullptr);
  const auto last=ticks(),end=rawNow();
  return double(end-start)/double(last-first);
 }();
 return scale;
}
inline uint64_t now() noexcept { return uint64_t(double(ticks())*nanosecondsPerTick()); }
inline void audit() noexcept {
 const auto start=rawNow(),first=ticks();timespec pause{0,10000000};nanosleep(&pause,nullptr);
 const auto last=ticks(),end=rawNow();
 const double observed=double(end-start)/double(last-first);
 std::fprintf(stderr,"{\"tsc_calibration_ratio\":%.12f}\n",observed/nanosecondsPerTick());
}
inline void initialize() noexcept {
 static const bool once=[] { (void)nanosecondsPerTick();std::atexit(audit);return true; }();
 (void)once;
}


struct Callback {
 uint64_t start = now();
 Callback() { ++callbacks; }
 ~Callback() { callback_ns += now()-start; }
};
struct Source {
 uint64_t start = now();
 Source() { ++depth; ++calls; }
 ~Source() { source_ns += now()-start; --depth; }
};
}
namespace cpu_sample {
inline bool collect = false;
inline unsigned calls = 0;
inline double last_user=0,last_system=0;
inline long last_involuntary=0,last_faults=0;
inline rusage before{};
inline double seconds(timeval t) { return t.tv_sec+t.tv_usec/1000000.0; }
inline void begin() {
 const unsigned ordinal=calls++;
 uint64_t x=uint64_t(ordinal/2U)+0x92873ULL;
 x=(x^(x>>30U))*0xbf58476d1ce4e5b9ULL;
 x=(x^(x>>27U))*0x94d049bb133111ebULL;x^=x>>31U;
 collect=std::getenv("ZYX_CPU_SAMPLE_ENABLE") && bool((x&1U)^(ordinal&1U));
 boundary::source_ns=boundary::callback_ns=boundary::calls=boundary::callbacks=0;
 boundary::enabled=collect;getrusage(RUSAGE_SELF,&before);
}
inline void end() {
 boundary::enabled=false;rusage after{};getrusage(RUSAGE_SELF,&after);
 last_user=seconds(after.ru_utime)-seconds(before.ru_utime);
 last_system=seconds(after.ru_stime)-seconds(before.ru_stime);
 last_involuntary=after.ru_nivcsw-before.ru_nivcsw;last_faults=after.ru_majflt-before.ru_majflt;
}
}
