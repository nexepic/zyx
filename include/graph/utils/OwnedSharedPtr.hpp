/**
 * @file OwnedSharedPtr.hpp
 * @brief Shared control blocks that retain their allocation authority.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <memory>
#include <utility>
#include "graph/utils/OwnedPmrAllocator.hpp"

namespace graph::utils {
inline std::shared_ptr<std::pmr::memory_resource> sharedAllocationOwner() {
    auto owner = OwnedAllocationScope::owner();
    // Preserve ordinary make_shared's global-new behavior for legacy callers;
    // never borrow an unrelated ambient default PMR resource.
    if (!owner) owner = std::shared_ptr<std::pmr::memory_resource>(
        std::shared_ptr<std::pmr::memory_resource>{}, std::pmr::new_delete_resource());
    return owner;
}
template<class T, class... Args>
std::shared_ptr<T> makeSharedOwned(Args &&...args) {
    return std::allocate_shared<T>(OwnedPmrAllocator<T>(sharedAllocationOwner()), std::forward<Args>(args)...);
}
template<class T>
std::shared_ptr<T> shareOwned(std::unique_ptr<T> value) {
    if (!value) return {};
    auto allocator = OwnedPmrAllocator<T>(sharedAllocationOwner());
    // shared_ptr's allocator constructor invokes the deleter if allocating its
    // control block fails. Release the unique owner only for that constructor.
    return std::shared_ptr<T>(value.release(), std::default_delete<T>{}, std::move(allocator));
}
} // namespace graph::utils
