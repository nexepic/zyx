#pragma once
// Explicitly enabled, bounded diagnostics. IDs identify owners, not addresses.
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
namespace graph::debug {
inline std::atomic<size_t> ciNextOwner{1U}, ciTraceEvents{0U}, ciTraceSummaries{0U}, ciSuccessSummaries{0U}, ciTruncationNotices{0U};
class CiBudgetTrace final {
public:
    explicit CiBudgetTrace(const char *kind) noexcept : kind_(kind) {}
    size_t id() const noexcept { return id_; }
    bool active() const noexcept { return events_.load(std::memory_order_relaxed) != 0U; }
    bool allow() const noexcept {
        if (!std::getenv("ZYX_CI_BUDGET_TRACE")) return false;
        const auto local = events_.fetch_add(1U, std::memory_order_relaxed);
        const auto global = ciTraceEvents.fetch_add(1U, std::memory_order_relaxed);
        if (local == 40U || global == 400U) {
            const auto notice = ciTruncationNotices.fetch_add(1U,std::memory_order_relaxed);
            if(notice < 20U)std::fprintf(stderr,"CI_TRACE_TRUNCATED kind=%s owner=%zu owner_limit=40 process_limit=400\n",kind_,id_);
            else if(notice == 20U)std::fputs("CI_TRACE_TRUNCATED process_notice_limit=20\n",stderr);
            std::fflush(stderr);
        }
        if (local >= 40U || global >= 400U) { suppressed_.fetch_add(1U, std::memory_order_relaxed); return false; }
        return true;
    }
    void summary(size_t used, size_t peak, size_t maximum, size_t parent = 0U) const noexcept {
        if (!std::getenv("ZYX_CI_BUDGET_TRACE") || (!active() && peak==0U)) return;
        const bool rejected = active();
        const size_t limit = rejected ? 80U : 20U;
        const auto n = (rejected ? ciTraceSummaries : ciSuccessSummaries).fetch_add(1U, std::memory_order_relaxed);
        if (n < limit)
            std::fprintf(stderr,"CI_BUDGET_SUMMARY kind=%s owner=%zu phase=before_member_release used=%zu peak=%zu maximum=%zu parent_charge=%zu events=%zu suppressed=%zu\n",
                kind_,id_,used,peak,maximum,parent,events_.load(std::memory_order_relaxed),suppressed_.load(std::memory_order_relaxed));
        else if (n == limit) std::fprintf(stderr,"CI_TRACE_TRUNCATED summary_kind=%s process_limit=%zu\n",rejected ? "rejection" : "successful",limit);
        std::fflush(stderr);
    }
private:
    const char *kind_;
    size_t id_ = ciNextOwner.fetch_add(1U,std::memory_order_relaxed);
    mutable std::atomic<size_t> events_{0U}, suppressed_{0U};
};
}
#endif
