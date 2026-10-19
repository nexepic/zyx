/**
 * @file MaterializedByteBudget.hpp
 * @brief Shared logical byte accounting for simultaneously retained execution data.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory_resource>
#include <optional>
#include "graph/query/execution/ResourceCoordinator.hpp"
#include "graph/utils/UnmanagedAllocationResource.hpp"
#include <stdexcept>
#include <utility>
#include "graph/debug/PerfTrace.hpp"

namespace graph::query::execution {
	// Optional execution kernels may decline admission without swallowing an
	// allocator failure, cancellation, or an unrelated semantic exception.
	class MaterializedByteLimitExceeded final : public std::runtime_error {
	public:
		MaterializedByteLimitExceeded() : std::runtime_error("shared materialization exceeded its byte bound") {}
	};

	// Counts the sizes supplied by owners, not allocator capacity or RSS. Owners
	// must keep this object alive until all their reservations have been released.
	class MaterializedByteBudget final : public utils::UnmanagedAllocationResource {
	public:
		explicit MaterializedByteBudget(size_t maximumBytes) : maximum_bytes_(maximumBytes), upstream_(std::pmr::get_default_resource()) {
			if (maximumBytes == 0U) throw std::invalid_argument("materialized byte budget requires a positive limit");
		}
		// Independent local budgets may share a query account. Parent admission is
		// checked before publishing a local change, for both PMR allocations and
		// manual reservations; rejection leaves every counter unchanged.
		MaterializedByteBudget(size_t maximumBytes, std::shared_ptr<detail::ResourceAccount> account,
			std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
			: MaterializedByteBudget(maximumBytes) {
			if (!upstream) throw std::invalid_argument("materialized allocator requires an upstream resource");
			parent_.emplace(std::move(account));
			upstream_ = upstream;
		}
		MaterializedByteBudget(const MaterializedByteBudget &) = delete;
		MaterializedByteBudget &operator=(const MaterializedByteBudget &) = delete;
		[[nodiscard]] size_t available() const noexcept { return maximum_bytes_ - used_bytes_; }
		// Global headroom is advisory: other queries can consume it before this
		// budget reserves. Keep available() local for existing kernel contracts.
		[[nodiscard]] size_t availableWithParent() const {
			return parent_ ? (std::min)(available(), parent_->available()) : available();
		}
		[[nodiscard]] size_t used() const noexcept { return used_bytes_; }

        void reserveUnmanaged(size_t bytes) override { replace(0U, bytes); }
        void releaseUnmanaged(size_t bytes) noexcept override {
            if (bytes > used_bytes_) std::terminate();
            replace(bytes, 0U);
        }

        [[nodiscard]] detail::ResourceLease detachCharged(size_t bytes) { return detach(bytes); }

        // A synchronous borrowed-row evaluator may return a payload whose local
        // reservation must pass to its caller without a refund/admission gap.
        using ReservationReleaseSink = void (*)(void *, detail::ResourceLease &&) noexcept;
        void setReservationReleaseSink(void *context, ReservationReleaseSink sink) noexcept {
            release_context_ = context; release_sink_ = sink;
        }
        [[nodiscard]] bool canTransferTo(const MaterializedByteBudget &target) const noexcept {
            return parent_ && target.parent_ && parent_->sameAccount(*target.parent_) &&
                parent_->accountMaximum() <= target.maximum_bytes_;
        }
        [[nodiscard]] bool transfersReservations() const noexcept { return release_sink_ != nullptr; }
        void adoptCharged(detail::ResourceLease &&lease) noexcept {
            if (!parent_ || !parent_->sameAccount(lease) || lease.bytes() > available()) std::terminate();
            const size_t bytes = lease.bytes(); parent_->absorb(std::move(lease)); used_bytes_ += bytes; observePeak();
        }

	private:
		friend class MaterializedByteReservation;
		[[nodiscard]] detail::ResourceLease detach(size_t bytes) {
			if (!parent_) throw std::logic_error("materialized budget has no parent account");
			if (bytes > used_bytes_) throw std::logic_error("invalid materialized ownership transfer");
			auto result = parent_->splitOff(bytes);
			used_bytes_ -= bytes;
			return result;
		}
		void replace(size_t previousBytes, size_t nextBytes, bool transferReservation = false) {
			if (nextBytes > previousBytes && nextBytes - previousBytes > available()) {
				throw MaterializedByteLimitExceeded();
			}
			const size_t total = used_bytes_ - previousBytes + nextBytes;
            if (parent_ && release_sink_ && transferReservation && previousBytes > nextBytes) {
                auto released = parent_->splitOff(previousBytes - nextBytes);
                used_bytes_ = total;
                release_sink_(release_context_, std::move(released));
                return;
            }
			if (parent_) parent_->set(total);
			used_bytes_ = total; observePeak();
		}

        void observePeak() noexcept {
            if (used_bytes_ <= peak_bytes_) return;
            peak_bytes_ = used_bytes_;
            try {
                debug::PerfTrace::addValue("logical_plan.materialized_budget_peak_bytes",
                    static_cast<int64_t>((std::min)(peak_bytes_, static_cast<size_t>((std::numeric_limits<int64_t>::max)()))));
            } catch (...) { /* Optional diagnostics cannot invalidate admitted storage. */ }
        }

		void *do_allocate(size_t bytes,size_t alignment) override {
			replace(0U,bytes);
			try{return upstream_->allocate(bytes,alignment);}
			catch(...){replace(bytes,0U);throw;}
		}
		void do_deallocate(void *pointer,size_t bytes,size_t alignment) override {
			upstream_->deallocate(pointer,bytes,alignment);replace(bytes,0U);
		}
		bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {return this==&other;}
		// Destroy the lease after the allocator state; outstanding reservations
		// and PMR allocations still require this budget to outlive them.
		void *release_context_ = nullptr;
		ReservationReleaseSink release_sink_ = nullptr;
		std::optional<detail::ResourceLease> parent_;
		size_t maximum_bytes_;
		size_t used_bytes_ = 0U;
        size_t peak_bytes_ = 0U;
		std::pmr::memory_resource *upstream_;
	};

	class MaterializedByteReservation final {
	public:
		explicit MaterializedByteReservation(MaterializedByteBudget *budget = nullptr) noexcept : budget_(budget) {}
		~MaterializedByteReservation() { reset(); }
		MaterializedByteReservation(const MaterializedByteReservation &) = delete;
		MaterializedByteReservation &operator=(const MaterializedByteReservation &) = delete;
		MaterializedByteReservation(MaterializedByteReservation &&other) noexcept :
			budget_(std::exchange(other.budget_, nullptr)), bytes_(std::exchange(other.bytes_, 0U)), embedded_bytes_(std::exchange(other.embedded_bytes_,0U)) {}
		MaterializedByteReservation &operator=(MaterializedByteReservation &&other) noexcept {
			if (this != &other) {
				reset();
				budget_ = std::exchange(other.budget_, nullptr);
				bytes_ = std::exchange(other.bytes_, 0U);
				embedded_bytes_=std::exchange(other.embedded_bytes_,0U);
			}
			return *this;
		}
		[[nodiscard]] size_t bytes() const noexcept { return bytes_; }
		// The retained payload can outlive its execution-local budget without
		// refunding and re-admitting global memory. Embedded parents must first
		// be reclassified or remain owned by their allocator.
		[[nodiscard]] detail::ResourceLease detachToParent() {
			if (!budget_ || embedded_bytes_ != 0U)
				throw std::logic_error("materialized ownership transfer requires a nonembedded reservation");
			auto result = budget_->detach(bytes_);
			bytes_ = 0U;
			return result;
		}
		// A reusable moved owner needs a zero-charge reservation in the same
		// budget. The budget must outlive both owners, including the empty one.
		[[nodiscard]] MaterializedByteReservation makeEmptyReservation() const noexcept {
			return MaterializedByteReservation(budget_);
		}
		// Transfer part of this owner's charge without changing the shared total.
		[[nodiscard]] MaterializedByteReservation splitOff(size_t bytes) {
			const size_t embedded=embedded_bytes_>bytes_-std::min(bytes,bytes_)?embedded_bytes_-(bytes_-std::min(bytes,bytes_)):0U;
			return splitOffAccounting(bytes,embedded);
		}
		[[nodiscard]] MaterializedByteReservation splitOffAccounting(size_t bytes,size_t embedded) {
			if(bytes>bytes_ || embedded>embedded_bytes_ || embedded>bytes || embedded_bytes_-embedded>bytes_-bytes)
				throw std::logic_error("invalid materialized accounting transfer");
			MaterializedByteReservation result(budget_);result.bytes_=bytes;result.embedded_bytes_=embedded;
			bytes_-=bytes;embedded_bytes_-=embedded;return result;
		}

		// Combine owners of the same allowance without refunding and re-admitting
		// bytes while the corresponding values remain live.
		void absorb(MaterializedByteReservation &&other) {
			if (this == &other) return;
			if (budget_ != other.budget_) {
				throw std::logic_error("cannot combine different materialized byte budgets");
			}
			if (other.bytes_ > std::numeric_limits<size_t>::max() - bytes_) {
				throw std::overflow_error("combined materialized reservation overflowed");
			}
			bytes_ += std::exchange(other.bytes_, 0U);
			embedded_bytes_+=std::exchange(other.embedded_bytes_,0U);
		}


		void set(size_t bytes) {
			setAccounting(bytes,std::min(embedded_bytes_,bytes));
		}
		// Logical size may include a parent already owned by a charged PMR vector.
		// Keep the public size while charging that parent's storage exactly once.
		void setAccounting(size_t bytes,size_t embedded) {
			if(embedded>bytes)throw std::logic_error("embedded materialization exceeds logical size");
			if(budget_)budget_->replace(bytes_-embedded_bytes_,bytes-embedded,true);
			bytes_=bytes;embedded_bytes_=embedded;
		}

		void reset() noexcept {
			if (budget_ != nullptr) budget_->replace(bytes_-embedded_bytes_, 0U,true);
			bytes_ = 0U;embedded_bytes_=0U;
		}
		[[nodiscard]] bool wouldExceed(size_t nextBytes) const noexcept {
			return budget_ != nullptr && nextBytes-std::min(embedded_bytes_,nextBytes)>bytes_-embedded_bytes_ && nextBytes-std::min(embedded_bytes_,nextBytes)-(bytes_-embedded_bytes_)>budget_->available();
		}

	private:
		MaterializedByteBudget *budget_;
		size_t bytes_ = 0U;
		size_t embedded_bytes_ = 0U;
	};
}
