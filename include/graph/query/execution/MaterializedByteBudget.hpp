/**
 * @file MaterializedByteBudget.hpp
 * @brief Shared logical byte accounting for simultaneously retained execution data.
 */
#pragma once

#include <cstddef>
#include <limits>
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
	class MaterializedByteBudget final {
	public:
		explicit MaterializedByteBudget(size_t maximumBytes) : maximum_bytes_(maximumBytes) {
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

		size_t maximum_bytes_;
		size_t used_bytes_ = 0U;
	};

	class MaterializedByteReservation final {
	public:
		explicit MaterializedByteReservation(MaterializedByteBudget *budget = nullptr) noexcept : budget_(budget) {}
		~MaterializedByteReservation() { reset(); }
		MaterializedByteReservation(const MaterializedByteReservation &) = delete;
		MaterializedByteReservation &operator=(const MaterializedByteReservation &) = delete;
		MaterializedByteReservation(MaterializedByteReservation &&other) noexcept :
			budget_(std::exchange(other.budget_, nullptr)), bytes_(std::exchange(other.bytes_, 0U)) {}
		MaterializedByteReservation &operator=(MaterializedByteReservation &&other) noexcept {
			if (this != &other) {
				reset();
				budget_ = std::exchange(other.budget_, nullptr);
				bytes_ = std::exchange(other.bytes_, 0U);
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
			if (bytes > bytes_) throw std::logic_error("cannot transfer more materialized bytes than reserved");
			MaterializedByteReservation transferred(budget_);
			transferred.bytes_ = bytes;
			bytes_ -= bytes;
			return transferred;
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
		}


		void set(size_t bytes) {
			if (budget_ != nullptr) budget_->replace(bytes_, bytes);
			bytes_ = bytes;
		}
		void reset() noexcept {
			if (budget_ != nullptr) budget_->replace(bytes_, 0U);
			bytes_ = 0U;
		}
		[[nodiscard]] bool wouldExceed(size_t nextBytes) const noexcept {
			return budget_ != nullptr && nextBytes > bytes_ && nextBytes - bytes_ > budget_->available();
		}

	private:
		MaterializedByteBudget *budget_;
		size_t bytes_ = 0U;
	};
}
