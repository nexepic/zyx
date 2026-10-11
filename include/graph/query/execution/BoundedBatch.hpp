/**
 * @file BoundedBatch.hpp
 * @brief Resource-bounded, reusable batch storage for logical operators.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "graph/query/execution/MaterializedByteBudget.hpp"

namespace graph::query::execution {

	// A batch owns only the rows currently in flight.  It centralizes row and
	// byte accounting so every vectorized operator enforces the same bounds.
	template <typename T>
	class BoundedBatch final {
	public:
		using SizeFunction = std::function<size_t(const T &)>;
		struct ChargedValue {
			// Destroy the moved value before releasing its budget charge.
			MaterializedByteReservation reservation;
			T value;
		};

		BoundedBatch(size_t maximumRows, size_t maximumBytes, SizeFunction sizeFunction,
				MaterializedByteBudget *sharedBudget = nullptr) :
			maximum_rows_(maximumRows), maximum_bytes_(maximumBytes), size_function_(std::move(sizeFunction)),
			reservation_(sharedBudget) {
			if (maximum_rows_ == 0U || maximum_bytes_ == 0U || !size_function_) {
				throw std::invalid_argument("bounded batch requires positive limits and a size function");
			}
		}

		BoundedBatch(const BoundedBatch &) = delete;
		BoundedBatch &operator=(const BoundedBatch &) = delete;
		// Copy the estimator before transferring anything: a throwing function
		// copy leaves both owners unchanged. The emptied source stays reusable
		// under its original limits and shared budget.
		BoundedBatch(BoundedBatch &&other) :
			maximum_rows_(other.maximum_rows_), maximum_bytes_(other.maximum_bytes_),
			size_function_(other.size_function_), reservation_(std::move(other.reservation_)),
			values_(std::move(other.values_)), bytes_(std::exchange(other.bytes_, 0U)) {
			other.values_.clear();
			other.reservation_ = reservation_.makeEmptyReservation();
		}
		BoundedBatch &operator=(BoundedBatch &&other) {
			if (this != &other) {
				SizeFunction estimator = other.size_function_;
				clear(); // Destroy old values before refunding their reservation.
				maximum_rows_ = other.maximum_rows_;
				maximum_bytes_ = other.maximum_bytes_;
				size_function_ = std::move(estimator);
				reservation_ = std::move(other.reservation_);
				values_ = std::move(other.values_);
				bytes_ = std::exchange(other.bytes_, 0U);
				other.values_.clear();
				other.reservation_ = reservation_.makeEmptyReservation();
			}
			return *this;
		}

		[[nodiscard]] size_t size() const noexcept { return values_.size(); }
		[[nodiscard]] bool empty() const noexcept { return values_.empty(); }
		[[nodiscard]] size_t bytes() const noexcept { return bytes_; }
		[[nodiscard]] size_t maximumRows() const noexcept { return maximum_rows_; }
		[[nodiscard]] size_t maximumBytes() const noexcept { return maximum_bytes_; }
		[[nodiscard]] const std::vector<T> &values() const noexcept { return values_; }
		[[nodiscard]] std::vector<T> &values() noexcept { return values_; }
		[[nodiscard]] bool wouldExceedBytes(const T &value) const {
			const size_t valueBytes = size_function_(value);
			return valueBytes > maximum_bytes_ - std::min(bytes_, maximum_bytes_) ||
				reservation_.wouldExceed(bytes_ + valueBytes);
		}

		void reserve(size_t capacity) {
			// Even an empty batch must not eagerly allocate slots for the entire
			// row bound when the byte budget can only hold a small subset. Payload
			// accounting remains the caller's size function's responsibility.
			const size_t byteCapacity = maximum_bytes_ / sizeof(T);
			values_.reserve(std::min({capacity, maximum_rows_, byteCapacity}));
		}

		void append(T value) {
			if (values_.size() == maximum_rows_) {
				throw std::runtime_error("bounded batch exceeded its row limit");
			}
			const size_t valueBytes = size_function_(value);
			if (valueBytes > maximum_bytes_ - std::min(bytes_, maximum_bytes_)) {
				throw std::runtime_error("bounded batch exceeded its byte limit");
			}
			reservation_.set(bytes_ + valueBytes);
			try {
				values_.push_back(std::move(value));
			} catch (...) {
				reservation_.set(bytes_);
				throw;
			}
			// Publish the accounting only after vector insertion succeeds; an
			// allocation failure must leave the batch internally consistent.
			bytes_ += valueBytes;
		}

		// Accept a value whose existing shared-budget charge moves with it.
		// The caller must have recalculated the charge after enriching the value.
		void appendCharged(T value, MaterializedByteReservation charge) {
			if (values_.size() == maximum_rows_) {
				throw std::runtime_error("bounded batch exceeded its row limit");
			}
			const size_t valueBytes = size_function_(value);
			if (charge.bytes() != valueBytes) {
				throw std::logic_error("bounded batch received a stale row charge");
			}
			if (valueBytes > maximum_bytes_ - std::min(bytes_, maximum_bytes_)) {
				throw std::runtime_error("bounded batch exceeded its byte limit");
			}
			reservation_.absorb(std::move(charge));
			try {
				values_.push_back(std::move(value));
			} catch (...) {
				reservation_.set(bytes_);
				throw;
			}
			bytes_ += valueBytes;
		}

		// Move a value out and refund its batch charge. A caller that keeps the
		// value live while allocating must hold its own shared-budget charge.
		// Call recalculateBytes first if mutable access enriched the values.
		T popBack() {
			if (values_.empty()) throw std::logic_error("cannot pop an empty bounded batch");
			const size_t valueBytes = size_function_(values_.back());
			if (valueBytes > bytes_) throw std::logic_error("bounded batch requires byte recalculation before pop");
			T value = std::move(values_.back());
			values_.pop_back();
			bytes_ -= valueBytes;
			reservation_.set(bytes_);
			return value;
		}

		// Move a value out while keeping its existing shared-budget charge live.
		// The caller owns both fields until the value is consumed or transferred.
		ChargedValue popBackCharged() {
			if (values_.empty()) throw std::logic_error("cannot pop an empty bounded batch");
			const size_t valueBytes = size_function_(values_.back());
			if (valueBytes > bytes_) throw std::logic_error("bounded batch requires byte recalculation before pop");
			T value = std::move(values_.back());
			values_.pop_back();
			bytes_ -= valueBytes;
			MaterializedByteReservation charge = reservation_.splitOff(valueBytes);
			return {std::move(charge), std::move(value)};
		}

		// Property prefetch can enrich rows after append. Recompute the bound at
		// that boundary so LargeValue payloads cannot bypass the batch budget.
		void recalculateBytes() {
			size_t total = 0U;
			for (const T &value: values_) {
				const size_t valueBytes = size_function_(value);
				if (valueBytes > maximum_bytes_ - std::min(total, maximum_bytes_)) {
					throw std::runtime_error("bounded batch exceeded its byte limit");
				}
				total += valueBytes;
			}
			reservation_.set(total);
			bytes_ = total;
		}

		// Transfer separately admitted enrichment before reconciliation, without
		// refunding live payloads or charging them twice in the shared allowance.
		void recalculateBytes(MaterializedByteReservation &&enrichment) {
			reservation_.absorb(std::move(enrichment));
			recalculateBytes();
		}

		void clear() noexcept {
			values_.clear();
			reservation_.reset();
			bytes_ = 0U;
		}

	private:
		size_t maximum_rows_ = 0U;
		size_t maximum_bytes_ = 0U;
		SizeFunction size_function_;
		// Values are destroyed before their reservation releases the charge.
		MaterializedByteReservation reservation_;
		std::vector<T> values_;
		size_t bytes_ = 0U;
	};

} // namespace graph::query::execution
