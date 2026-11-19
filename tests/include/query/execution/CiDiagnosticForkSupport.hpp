#pragma once
// Test-runner support only: a fork child must not inherit a worker's trace lock.
#include "graph/debug/CiBudgetTrace.hpp"
#include "graph/debug/ResidentMemoryDiagnostics.hpp"
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <process.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif
namespace graph::test::ci_diagnostics {
inline bool initialized=false;
inline std::atomic<bool> preparing{false};
inline unsigned long pid() noexcept {
#ifdef _WIN32
    return static_cast<unsigned long>(::_getpid());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}
inline size_t firstId() noexcept {
    if constexpr(sizeof(size_t)>=8U)return static_cast<size_t>(static_cast<uint64_t>(pid())<<32U)+1U;
    return 1U;
}
#ifndef _WIN32
inline void beforeFork() noexcept {
    preparing.store(true,std::memory_order_release);
    while(debug::ci_budget_detail::state.lock.test_and_set(std::memory_order_acquire)) {}
}
inline void parentAfterFork() noexcept {
    debug::ci_budget_detail::state.lock.clear(std::memory_order_release);
    preparing.store(false,std::memory_order_release);
}
inline void childAfterFork() noexcept {
    auto &s=debug::ci_budget_detail::state;
    for(auto &entry:s.entries)entry={};
    s.next=firstId();s.events=s.summaries=s.successes=s.notices=s.active=0U;
    s.overflow=s.overflow_reported=false;
    s.lock.clear(std::memory_order_release);
    debug::ciResidentMemoryReports.store(0U,std::memory_order_relaxed);
    preparing.store(false,std::memory_order_release);
}
#endif
inline bool initialize() noexcept {
    if(initialized)return true;
#ifndef _WIN32
    if(::pthread_atfork(beforeFork,parentAfterFork,childAfterFork)!=0)return false;
#endif
    {debug::ci_budget_detail::Lock lock;debug::ci_budget_detail::state.next=firstId();}
    initialized=true;
    if(std::getenv("ZYX_CI_BUDGET_TRACE") || std::getenv("ZYX_CI_RSS_TRACE")) {
        std::fprintf(stderr,"CI_DIAGNOSTICS_PROCESS pid=%lu owner_id_namespace=%zu fork_support=ready\n",pid(),firstId());
        std::fflush(stderr);
    }
    return true;
}
}
#endif
