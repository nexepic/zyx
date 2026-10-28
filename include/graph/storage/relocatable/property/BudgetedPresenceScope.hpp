#pragma once

namespace graph::storage {
// Confined to one synchronous experimental probe. Legacy ROW paths have
// unmanaged buffers without the probe's admission contract and must decline.
class BudgetedPresenceScope final {
public:
    BudgetedPresenceScope() noexcept : previous_(active_) { active_ = true; }
    ~BudgetedPresenceScope() { active_ = previous_; }
    BudgetedPresenceScope(const BudgetedPresenceScope &) = delete;
    BudgetedPresenceScope &operator=(const BudgetedPresenceScope &) = delete;
    static bool active() noexcept { return active_; }
private:
    bool previous_;
    inline static thread_local bool active_ = false;
};
}
