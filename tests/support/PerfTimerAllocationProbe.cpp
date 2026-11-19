// Isolated replacement-new accounting. Collector initialization is excluded;
// this measures repeated timer labels, not all malloc traffic or resident size.
#include <cstdio>
#include <cstdlib>
#include <new>
#include "graph/debug/StaticPerfTimer.hpp"

namespace {
size_t requests = 0U;
bool reject = false;
}
void *operator new(size_t bytes) {
    ++requests;
    if (reject) throw std::bad_alloc();
    if (void *value = std::malloc(bytes ? bytes : 1U)) return value;
    throw std::bad_alloc();
}
void *operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, size_t) noexcept { std::free(value); }
void operator delete[](void *value, size_t) noexcept { std::free(value); }

int main() {
    using namespace graph::debug;
    constexpr std::string_view label = "static.timer.allocation.probe.long.label";
    PerfTrace::setEnabled(true);
    PerfTrace::addDuration(label, 1U); // Warm only the collector's one key.
    const auto before = requests;
    for (unsigned i = 0; i < 1000U; ++i) { ScopedPerfTimer timer(label); }
    const auto dynamicRequests = requests - before;
    const auto afterDynamic = requests;
    reject = true;
    for (unsigned i = 0; i < 1000U; ++i) {
        StaticPerfTimer timer(StaticPerfLabel{"static.timer.allocation.probe.long.label"});
    }
    reject = false;
    const auto staticRequests = requests - afterDynamic;
    const auto snapshot = PerfTrace::snapshotAndReset();
    const auto calls = snapshot.at(std::string(label)).calls;
    const bool passed = dynamicRequests > 0U && staticRequests == 0U && calls == 2001U;
    std::printf("CI_STATIC_TIMER_ALLOCATION dynamic_requests=%zu static_requests=%zu calls=%llu collector_prewarmed=1 cpp_new_only=1 passed=%d\n",
        dynamicRequests, staticRequests, static_cast<unsigned long long>(calls), int(passed));
    return passed ? 0 : 1;
}
