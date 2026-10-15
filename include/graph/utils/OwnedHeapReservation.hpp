/**
 * @file OwnedHeapReservation.hpp
 * @brief Escaping ownership of admission for standard-library heap requests.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <memory>
#include <limits>
#include <stdexcept>
#include <utility>
#include "graph/utils/UnmanagedAllocationResource.hpp"

namespace graph::utils {
// Declare this before the data it governs, so data is destroyed before credit.
// Unknown/legacy PMR providers retain their lifetime but supply no authority for
// unmanaged allocations; native budget resources implement the explicit API.
class OwnedHeapReservation {
public:
    explicit OwnedHeapReservation(std::shared_ptr<std::pmr::memory_resource> owner = {}, size_t bytes = 0U)
        : owner_(std::move(owner)), authority_(dynamic_cast<UnmanagedAllocationResource *>(owner_.get())) {
        set(bytes);
    }
    ~OwnedHeapReservation() { if (authority_ && bytes_ != 0U) authority_->releaseUnmanaged(bytes_); }
    OwnedHeapReservation(const OwnedHeapReservation &) = delete;
    OwnedHeapReservation &operator=(const OwnedHeapReservation &) = delete;
    OwnedHeapReservation(OwnedHeapReservation &&other) noexcept
        : owner_(std::move(other.owner_)), authority_(std::exchange(other.authority_, nullptr)),
          bytes_(std::exchange(other.bytes_, 0U)), valid_(std::exchange(other.valid_, false)) {}
    OwnedHeapReservation &operator=(OwnedHeapReservation &&) = delete;
    void set(size_t bytes) {
        if (!valid_) throw std::logic_error("cannot resize a moved heap reservation");
        if (bytes == bytes_) return;
        if (authority_) {
            if (bytes > bytes_) authority_->reserveUnmanaged(bytes - bytes_);
            else authority_->releaseUnmanaged(bytes_ - bytes);
        }
        bytes_ = bytes;
    }
    // Transfer already-admitted credit without a refund/re-admit gap. The
    // corresponding storage must have changed owners in the same authority.
    void absorb(OwnedHeapReservation &other) {
        if (!valid_ || !other.valid_ || this == &other || owner_ != other.owner_ || authority_ != other.authority_)
            throw std::logic_error("heap reservation transfer requires matching live authorities");
        if (other.bytes_ > (std::numeric_limits<size_t>::max)() - bytes_)
            throw std::overflow_error("heap reservation transfer overflowed");
        bytes_ += other.bytes_;
        other.bytes_ = 0U;
    }
    void swap(OwnedHeapReservation &other) noexcept {
        owner_.swap(other.owner_);
        std::swap(authority_, other.authority_);
        std::swap(bytes_, other.bytes_);
        std::swap(valid_, other.valid_);
    }
    [[nodiscard]] size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool governed() const noexcept { return authority_ != nullptr; }
    [[nodiscard]] const std::shared_ptr<std::pmr::memory_resource> &owner() const noexcept { return owner_; }
private:
    std::shared_ptr<std::pmr::memory_resource> owner_;
    UnmanagedAllocationResource *authority_;
    size_t bytes_ = 0U;
    bool valid_ = true;
};
} // namespace graph::utils
