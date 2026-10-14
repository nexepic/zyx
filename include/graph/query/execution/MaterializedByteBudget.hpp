/**
 * @file MaterializedByteBudget.hpp
 * @brief Shared logical byte accounting for simultaneously retained execution data.
 */
#pragma once

#include <cstddef>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <utility>

namespace graph::query::execution {
	// Optional execution kernels may decline admission without swallowing an
	// allocator failure, cancellation, or an unrelated semantic exception.
	class MaterializedByteLimitExceeded final : public std::runtime_error {
	public:
		MaterializedByteLimitExceeded() : std::runtime_error("shared materialization exceeded its byte bound") {}
	};

	// Counts the sizes supplied by owners, not allocator capacity or RSS. Owners
	// must keep this object alive until all their reservations have been released.
	class MaterializedByteBudget final : public std::pmr::memory_resource {
	public:
		explicit MaterializedByteBudget(size_t maximumBytes) : maximum_bytes_(maximumBytes), upstream_(std::pmr::get_default_resource()) {
			if (maximumBytes == 0U) throw std::invalid_argument("materialized byte budget requires a positive limit");
		}
		MaterializedByteBudget(const MaterializedByteBudget &) = delete;
		MaterializedByteBudget &operator=(const MaterializedByteBudget &) = delete;
		[[nodiscard]] size_t available() const noexcept { return maximum_bytes_ - used_bytes_; }
		[[nodiscard]] size_t used() const noexcept { return used_bytes_; }

	private:
		friend class MaterializedByteReservation;
		void replace(size_t previousBytes, size_t nextBytes) {
			if (nextBytes > previousBytes && nextBytes - previousBytes > available()) {
				throw MaterializedByteLimitExceeded();
			}
			used_bytes_ = used_bytes_ - previousBytes + nextBytes;
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
		size_t maximum_bytes_;
		size_t used_bytes_ = 0U;
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
			if(budget_)budget_->replace(bytes_-embedded_bytes_,bytes-embedded);
			bytes_=bytes;embedded_bytes_=embedded;
		}

		void reset() noexcept {
			if (budget_ != nullptr) budget_->replace(bytes_-embedded_bytes_, 0U);
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
