/**
 * @file PropertyDecodeAllocationScope.hpp
 * @brief Borrowed synchronous admission context for decoded property payloads.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <cstddef>
#include <stdexcept>

namespace graph::property_decode {
// The caller retains both context and its credit until decoded values die.
// Function pointers keep installing/consulting this scope allocation-free.
class AllocationScope final {
public:
    using Admission = void (*)(void *, size_t);
    using Checkpoint = void (*)(void *);
    AllocationScope(void *context, Admission admission, Checkpoint checkpoint = nullptr, bool guardedCopies = false)
        : context_(context), admission_(admission), checkpoint_(checkpoint), previous_(active_), guarded_copies_(guardedCopies) {
        if (!admission_) throw std::invalid_argument("property decode scope requires admission");
        active_ = this;
    }
    ~AllocationScope() { active_ = previous_; }
    AllocationScope(const AllocationScope &) = delete;
    AllocationScope &operator=(const AllocationScope &) = delete;
    static AllocationScope *current() noexcept { return active_; }
    void admit(size_t bytes) const { admission_(context_, bytes); }
    [[nodiscard]] bool guardedCopies() const noexcept { return guarded_copies_; }
    void checkpoint() const {
        if (checkpoint_) checkpoint_(context_);
        else if (previous_) previous_->checkpoint();
    }
private:
    void *context_;
    Admission admission_;
    Checkpoint checkpoint_;
    AllocationScope *previous_;
    bool guarded_copies_;
    inline static thread_local AllocationScope *active_ = nullptr;
};
} // namespace graph::property_decode
