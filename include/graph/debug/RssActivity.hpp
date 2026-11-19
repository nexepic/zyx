#pragma once
// Test-only attribution of allocator changes within bounded operation classes.
// Intervals may include other threads; these are not causal RSS measurements.
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include "graph/debug/SanitizerAllocationSnapshot.hpp"
namespace graph::debug {
enum class RssActivityKind { RAK_SCRATCH_READ, RAK_SCRATCH_WRITE, RAK_ROW_DECODE, RAK_ROW_ENCODE, RAK_SCRATCH_SNAPSHOT };
struct RssActivityCounters {
    std::atomic<size_t> calls{0}, live_increase{0}, live_decrease{0}, heap_increase{0}, heap_decrease{0};
    static void add(std::atomic<size_t> &target, size_t amount) noexcept {
        size_t value = target.load(std::memory_order_relaxed);
        for (;;) {
            const size_t next = amount > (std::numeric_limits<size_t>::max)() - value
                ? (std::numeric_limits<size_t>::max)() : value + amount;
            if (target.compare_exchange_weak(value, next, std::memory_order_relaxed)) return;
        }
    }
    void record(SanitizerAllocationSnapshot before, SanitizerAllocationSnapshot after) noexcept {
        if (!before.available || !after.available) return;
        add(calls, 1U);
        if (after.live_bytes >= before.live_bytes) add(live_increase, after.live_bytes - before.live_bytes);
        else add(live_decrease, before.live_bytes - after.live_bytes);
        if (after.heap_bytes >= before.heap_bytes) add(heap_increase, after.heap_bytes - before.heap_bytes);
        else add(heap_decrease, before.heap_bytes - after.heap_bytes);
    }
    void reset() noexcept {
        calls = 0; live_increase = 0; live_decrease = 0; heap_increase = 0; heap_decrease = 0;
    }
};
inline RssActivityCounters rssActivityCounters[5];
class RssActivityScope final {
public:
    explicit RssActivityScope(RssActivityKind kind) noexcept : kind_(kind) {
        if (std::getenv("ZYX_CI_RSS_ATTRIBUTION")) before_ = captureSanitizerAllocations();
    }
    ~RssActivityScope() {
        if (before_.available)
            rssActivityCounters[static_cast<size_t>(kind_)].record(before_, captureSanitizerAllocations());
    }
private:
    RssActivityKind kind_;
    SanitizerAllocationSnapshot before_;
};
inline void reportRssActivity(const char *phase, bool reset) noexcept {
    if (!std::getenv("ZYX_CI_RSS_ATTRIBUTION")) return;
    constexpr const char *names[] = {"scratch_read", "scratch_write", "row_decode", "row_encode", "scratch_snapshot"};
    const bool available = captureSanitizerAllocations().available;
    for (size_t i = 0; i < 5; ++i) {
        auto &c = rssActivityCounters[i];
        if (reset) c.reset();
        std::fprintf(stderr, "CI_RSS_ACTIVITY phase=%s category=%s available=%d calls=%zu live_increase=%zu live_decrease=%zu heap_increase=%zu heap_decrease=%zu\n",
            phase, names[i], available, c.calls.load(), c.live_increase.load(), c.live_decrease.load(), c.heap_increase.load(), c.heap_decrease.load());
    }
}
}
#endif
