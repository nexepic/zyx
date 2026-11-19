#pragma once
#include "graph/debug/PerfTrace.hpp"

namespace graph::debug {
// Immediate construction prevents a borrowed label from referring to a
// runtime string or automatic array. Dynamic labels use ScopedPerfTimer.
class StaticPerfLabel final {
public:
    explicit consteval StaticPerfLabel(std::string_view key) : key_(key) {}
private:
    friend class StaticPerfTimer;
    std::string_view key_;
};

class StaticPerfTimer final {
public:
    explicit StaticPerfTimer(StaticPerfLabel label)
        : key_(label.key_), active_(PerfTrace::isEnabled() && !key_.empty()) {
        if (active_) start_ = Clock::now();
    }
    StaticPerfTimer(const StaticPerfTimer &) = delete;
    StaticPerfTimer &operator=(const StaticPerfTimer &) = delete;
    ~StaticPerfTimer() {
        if (active_) PerfTrace::addDuration(key_, static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count()));
    }
private:
    using Clock = std::chrono::steady_clock;
    std::string_view key_;
    Clock::time_point start_{};
    bool active_;
};
}
