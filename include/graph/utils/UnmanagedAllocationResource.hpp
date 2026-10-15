/**
 * @file UnmanagedAllocationResource.hpp
 * @brief Admission authority for heaps that retain standard-library allocators.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <cstddef>
#include <memory_resource>

namespace graph::utils {
// The caller owns the real allocation and must release it before refunding its
// admission. No backing/dummy allocation is made through the PMR interface.
class UnmanagedAllocationResource : public std::pmr::memory_resource {
public:
    virtual void reserveUnmanaged(size_t bytes) = 0;
    virtual void releaseUnmanaged(size_t bytes) noexcept = 0;
};
} // namespace graph::utils
