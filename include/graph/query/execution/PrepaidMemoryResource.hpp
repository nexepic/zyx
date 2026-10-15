#pragma once

#include <atomic>
#include <exception>
#include <utility>
#include "graph/query/execution/ResourceCoordinator.hpp"
#include "graph/utils/UnmanagedAllocationResource.hpp"

namespace graph::query::execution::detail {
// Admit a finite allocation grant outside publication/cache locks. Its entire
// capacity stays charged to the parent, including unused bytes. Consumers can
// then account actual PMR requests without acquiring the process resource mutex.
// The resource must outlive its containers; an upstream resource must outlive it.
class PrepaidMemoryResource final : public utils::UnmanagedAllocationResource {
public:
	PrepaidMemoryResource(std::shared_ptr<ResourceAccount> account, size_t capacity,
		std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
		: grant_(std::move(account)), capacity_(capacity), upstream_(upstream) {
		if (!upstream || capacity == 0U) throw std::invalid_argument("prepaid allocation requires a finite positive grant and upstream");
		grant_.set(capacity);
	}
	~PrepaidMemoryResource() override {
		// Refunding a grant while its allocated buffers remain live would let a
		// later owner reuse the same quota. Such lifetime misuse must fail closed.
		if (used_.load(std::memory_order_relaxed) != 0U) std::terminate();
	}
	PrepaidMemoryResource(const PrepaidMemoryResource &) = delete;
	PrepaidMemoryResource &operator=(const PrepaidMemoryResource &) = delete;
    // Reserve an audited upper bound before adopting an existing non-PMR
    // buffer. Declare this owner before the buffer so teardown frees the
    // buffer first. These bytes are admitted capacity, not an RSS measurement.
    class ExternalReservation final {
    public:
        ExternalReservation(std::shared_ptr<PrepaidMemoryResource> resource, size_t bytes)
            : resource_(std::move(resource)), bytes_(bytes) {
            if (!resource_) throw std::invalid_argument("external buffer reservation requires an allocation grant");
            resource_->reserveBytes(bytes_);
        }
        ~ExternalReservation() { if (resource_) resource_->releaseBytes(bytes_); }
        ExternalReservation(const ExternalReservation &) = delete;
        ExternalReservation &operator=(const ExternalReservation &) = delete;
        ExternalReservation(ExternalReservation &&other) noexcept
            : resource_(std::move(other.resource_)), bytes_(std::exchange(other.bytes_, 0U)) {}
        ExternalReservation &operator=(ExternalReservation &&) = delete;
        // Growing admission is transactional; shrinking is only valid after
        // the caller has freed the corresponding external storage.
        void set(size_t bytes) {
            if (!resource_) throw std::logic_error("cannot resize a moved external reservation");
            if (bytes > bytes_) resource_->reserveBytes(bytes - bytes_);
            else resource_->releaseBytes(bytes_ - bytes);
            bytes_ = bytes;
        }
        [[nodiscard]] size_t bytes() const noexcept { return bytes_; }
    private:
        std::shared_ptr<PrepaidMemoryResource> resource_;
        size_t bytes_;
    };
	[[nodiscard]] size_t capacity() const noexcept { return capacity_; }
	[[nodiscard]] size_t used() const noexcept { return used_.load(std::memory_order_relaxed); }
	[[nodiscard]] size_t peakAdmitted() const noexcept { return peak_.load(std::memory_order_relaxed); }
	[[nodiscard]] size_t denials() const noexcept { return denials_.load(std::memory_order_relaxed); }
    void reserveUnmanaged(size_t bytes) override { reserveBytes(bytes); }
    void releaseUnmanaged(size_t bytes) noexcept override { releaseBytes(bytes); }
private:
	void reserveBytes(size_t bytes) {
		size_t prior = used_.load(std::memory_order_relaxed);
		for (;;) {
			if (bytes > capacity_ - prior) {
				denials_.fetch_add(1U, std::memory_order_relaxed);
				throw ResourceAdmissionDenied();
			}
			if (used_.compare_exchange_weak(prior, prior + bytes, std::memory_order_relaxed)) break;
		}
		size_t peak = peak_.load(std::memory_order_relaxed);
		while (peak < prior + bytes && !peak_.compare_exchange_weak(peak, prior + bytes, std::memory_order_relaxed)) {}
	}
	void releaseBytes(size_t bytes) noexcept { used_.fetch_sub(bytes, std::memory_order_relaxed); }
	void *do_allocate(size_t bytes, size_t alignment) override {
		reserveBytes(bytes);
		try { return upstream_->allocate(bytes, alignment); }
		catch (...) { releaseBytes(bytes); throw; }
	}
	void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
		upstream_->deallocate(pointer, bytes, alignment);
		releaseBytes(bytes);
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
	// Last member destroyed, after the allocation counters and upstream binding.
	ResourceLease grant_;
	const size_t capacity_;
	std::pmr::memory_resource *upstream_;
	std::atomic<size_t> used_{0U}, peak_{0U}, denials_{0U};
};
} // namespace graph::query::execution::detail
