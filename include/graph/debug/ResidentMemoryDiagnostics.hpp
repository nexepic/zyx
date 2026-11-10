#pragma once
// Optional counters only. Never substitute them for the guard's actual RSS.
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "graph/debug/SanitizerAllocationSnapshot.hpp"
#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#endif
#if defined(__clang__) && (defined(__APPLE__) || defined(__linux__))
#if __has_feature(address_sanitizer)
#if defined(__APPLE__)
#define ZYX_RSS_WEAK __attribute__((weak_import))
#else
#define ZYX_RSS_WEAK __attribute__((weak))
#endif
extern "C" size_t __sanitizer_get_free_bytes() ZYX_RSS_WEAK;
extern "C" void __asan_print_accumulated_stats() ZYX_RSS_WEAK;
#undef ZYX_RSS_WEAK
#define ZYX_RSS_ASAN_COUNTERS 1
#endif
#endif
namespace graph::debug {
inline std::atomic<unsigned> ciResidentMemoryReports{0U};
static inline void traceResidentMemory(const char *phase, [[maybe_unused]] bool printAllocatorStats = true) noexcept {
    if (!std::getenv("ZYX_CI_RSS_TRACE")) return;
    const auto index = ciResidentMemoryReports.fetch_add(1U, std::memory_order_relaxed);
    if (index >= 24U) {
        if (index == 24U) std::fputs("CI_RSS_TRUNCATED process_limit=24\n",stderr);
        return;
    }
    const auto allocations = captureSanitizerAllocations();
    std::fprintf(stderr,"CI_RSS_PHASE phase=%s asan_available=%d live=%zu heap=%zu\n",phase,allocations.available,allocations.live_bytes,allocations.heap_bytes);
#if defined(__APPLE__)
    task_vm_info_data_t vm{};mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
    const auto status=task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&vm),&count);
    if(status==KERN_SUCCESS && count>=TASK_VM_INFO_REV1_COUNT)
        std::fprintf(stderr,"CI_RSS_MACH phase=%s resident=%llu internal=%llu external=%llu reusable=%llu compressed=%llu footprint=%llu\n",phase,
            static_cast<unsigned long long>(vm.resident_size),static_cast<unsigned long long>(vm.internal),
            static_cast<unsigned long long>(vm.external),static_cast<unsigned long long>(vm.reusable),
            static_cast<unsigned long long>(vm.compressed),static_cast<unsigned long long>(vm.phys_footprint));
    else std::fprintf(stderr,"CI_RSS_MACH unavailable status=%d count=%u\n",status,count);
#elif defined(__linux__)
    char bytes[8192];const int fd=::open("/proc/self/status",O_RDONLY|O_CLOEXEC);
    const auto n=fd<0 ? -1 : ::read(fd,bytes,sizeof(bytes));if(fd>=0)::close(fd);
    if(n>0)for(char *p=bytes;p<bytes+n;){char *e=static_cast<char*>(std::memchr(p,'\n',static_cast<size_t>(bytes+n-p)));if(!e)break;
        if(std::strncmp(p,"RssAnon:",8)==0||std::strncmp(p,"RssFile:",8)==0||std::strncmp(p,"RssShmem:",9)==0)
            std::fprintf(stderr,"CI_RSS_LINUX phase=%s %.*s\n",phase,static_cast<int>(e-p),p);
        p=e+1;}
#endif
#ifdef ZYX_RSS_ASAN_COUNTERS
    if(__sanitizer_get_free_bytes)std::fprintf(stderr,"CI_RSS_ASAN phase=%s free=%zu\n",phase,__sanitizer_get_free_bytes());
    else std::fputs("CI_RSS_ASAN free_counter_unavailable\n",stderr);
    // Do not warm the large statistics/reporting path before the RSS baseline.
    if(printAllocatorStats && std::strcmp(phase,"before-query")!=0) {
        if(__asan_print_accumulated_stats)__asan_print_accumulated_stats();
        else std::fputs("CI_RSS_ASAN statistics_unavailable\n",stderr);
    }
#endif
    std::fflush(stderr);
}
}
#undef ZYX_RSS_ASAN_COUNTERS
#endif
