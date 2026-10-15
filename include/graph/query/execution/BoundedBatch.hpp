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
#include <memory_resource>
#include <memory>
#include <optional>
#include <type_traits>

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
				MaterializedByteBudget *sharedBudget = nullptr, bool accountVectorCapacity = true,size_t embeddedObjectBytes=0U) :
			maximum_rows_(maximumRows), maximum_bytes_(maximumBytes), embedded_object_bytes_(sharedBudget && accountVectorCapacity?embeddedObjectBytes:0U), size_function_(std::move(sizeFunction)),
			reservation_(sharedBudget),
				values_(sharedBudget && accountVectorCapacity ? static_cast<std::pmr::memory_resource *>(sharedBudget) : std::pmr::get_default_resource()) {
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
            embedded_object_bytes_(other.embedded_object_bytes_),
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
				embedded_object_bytes_=other.embedded_object_bytes_;
				size_function_ = std::move(estimator);
				reservation_ = std::move(other.reservation_);
				std::destroy_at(&values_);
				std::construct_at(&values_,std::move(other.values_));
				bytes_ = std::exchange(other.bytes_, 0U);
				other.values_.clear();
				other.reservation_ = reservation_.makeEmptyReservation();
			}
			return *this;
		}

		[[nodiscard]] size_t size() const noexcept { return values_.size(); }
		[[nodiscard]] bool empty() const noexcept { return values_.empty(); }
		[[nodiscard]] size_t bytes() const noexcept { return bytes_; }
        // Enrichment may pre-admit payload growth before mutating values().
        // Reconcile with recalculateBytes() before removing or appending rows.
        [[nodiscard]] MaterializedByteReservation &enrichmentReservation() noexcept { return reservation_; }
		[[nodiscard]] size_t maximumRows() const noexcept { return maximum_rows_; }
		[[nodiscard]] size_t maximumBytes() const noexcept { return maximum_bytes_; }
		[[nodiscard]] const std::pmr::vector<T> &values() const noexcept { return values_; }
		[[nodiscard]] std::pmr::vector<T> &values() noexcept { return values_; }
		[[nodiscard]] bool wouldExceedBytes(const T &value) const {
			const size_t valueBytes = size_function_(value);
			return valueBytes > maximum_bytes_ - std::min(bytes_, maximum_bytes_) ||
				reservation_.wouldExceed(bytes_ + valueBytes);
		}

		void reserve(size_t capacity) {
			// Even an empty batch must not eagerly allocate slots for the entire
			// row bound when the byte budget can only hold a small subset. Payload
			// accounting remains the caller's size function's responsibility.
			auto *allocationBudget=dynamic_cast<MaterializedByteBudget *>(values_.get_allocator().resource());
			const size_t byteCapacity=allocationBudget
				? std::min(maximum_bytes_/(4U*sizeof(T)),allocationBudget->available()/(4U*sizeof(T)))
				: maximum_bytes_/sizeof(T);
			values_.reserve(std::min({capacity, maximum_rows_, byteCapacity,allocationBudget?size_t{1U}:capacity}));
		}

		void append(T value) {
			if (values_.size() == maximum_rows_) {
				throw std::runtime_error("bounded batch exceeded its row limit");
			}
			const size_t valueBytes = size_function_(value);
			if (valueBytes > maximum_bytes_ - std::min(bytes_, maximum_bytes_)) {
				throw std::runtime_error("bounded batch exceeded its byte limit");
			}
			reservation_.setAccounting(bytes_ + valueBytes,(values_.size()+1U)*embedded_object_bytes_);
			try {
				values_.push_back(std::move(value));
			} catch (...) {
				reservation_.setAccounting(bytes_,values_.size()*embedded_object_bytes_);
				throw;
			}
			// Publish the accounting only after vector insertion succeeds; an
			// allocation failure must leave the batch internally consistent.
			bytes_ += valueBytes;
		}

		// Accept a value whose existing shared-budget charge moves with it.
		// The caller must have recalculated the charge after enriching the value.
        void appendCharged(T value, MaterializedByteReservation charge) {
            if constexpr (std::is_nothrow_move_constructible_v<T>) {
                // Native rows have nonthrowing ownership moves. Put their payload
                // after its charge so validation failures destroy bytes first.
                MaterializedByteReservation retained(std::move(charge));
                std::optional<T> candidate(std::in_place, std::move(value));
                appendChargedValue(*candidate, retained, [&] { candidate.reset(); });
            } else {
                appendChargedValue(value, charge, [] {});
            }
        }

        // Destroy a rejected value before refunding its payload ownership. This
        // avoids creating an independently charged stack value just to discard it.
        void discardBack() {
            if (values_.empty()) throw std::logic_error("cannot discard from an empty bounded batch");
            const size_t valueBytes = size_function_(values_.back());
            if (valueBytes > bytes_) throw std::logic_error("bounded batch requires byte recalculation before discard");
            values_.pop_back();
            if (values_.empty()) releaseEmptyCapacity();
            bytes_ -= valueBytes;
            reservation_.setAccounting(bytes_, values_.size() * embedded_object_bytes_);
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
			if(values_.empty())releaseEmptyCapacity();
			bytes_ -= valueBytes;
			reservation_.setAccounting(bytes_,values_.size()*embedded_object_bytes_);
			return value;
		}

		// Move a value out while keeping its existing shared-budget charge live.
		// The caller owns both fields until the value is consumed or transferred.
        ChargedValue popBackCharged() {
            if (values_.empty()) throw std::logic_error("cannot pop an empty bounded batch");
            const size_t valueBytes = size_function_(values_.back());
            if (valueBytes > bytes_) throw std::logic_error("bounded batch requires byte recalculation before pop");
            auto charge = reservation_.splitOffAccounting(valueBytes, embedded_object_bytes_);
            try {
                // The value escapes the vector's charged capacity. Admit its
                // independent ownership before mutating the published batch.
                charge.setAccounting(valueBytes, 0U);
            } catch (...) {
                reservation_.absorb(std::move(charge));
                throw;
            }
            T value = [&]() {
                try { return T(std::move(values_.back())); }
                catch (...) {
                    // Restore ownership if construction fails before removal.
                    charge.setAccounting(valueBytes, embedded_object_bytes_);
                    reservation_.absorb(std::move(charge));
                    throw;
                }
            }();
            values_.pop_back();
            if (values_.empty()) releaseEmptyCapacity();
            bytes_ -= valueBytes;
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
			reservation_.setAccounting(total,values_.size()*embedded_object_bytes_);
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
			releaseEmptyCapacity();
			reservation_.reset();
			bytes_ = 0U;
		}

	private:
        template<class DestroyRejectedValue>
        void appendChargedValue(T &value, MaterializedByteReservation &charge,
                                DestroyRejectedValue destroyRejectedValue) {
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
			try {
				reservation_.absorb(std::move(charge));
				reservation_.setAccounting(bytes_+valueBytes,(values_.size()+1U)*embedded_object_bytes_);
				values_.push_back(std::move(value));
			} catch (...) {
                destroyRejectedValue();
				reservation_.setAccounting(bytes_,values_.size()*embedded_object_bytes_);
				throw;
			}
			bytes_ += valueBytes;
        }

		void releaseEmptyCapacity() noexcept {
			if(dynamic_cast<MaterializedByteBudget *>(values_.get_allocator().resource())!=nullptr)
				std::pmr::vector<T>(values_.get_allocator().resource()).swap(values_);
		}
		size_t maximum_rows_ = 0U;
		size_t maximum_bytes_ = 0U;
		size_t embedded_object_bytes_ = 0U;
		SizeFunction size_function_;
		// Values are destroyed before their reservation releases the charge.
		MaterializedByteReservation reservation_;
		std::pmr::vector<T> values_;
		size_t bytes_ = 0U;
	};

} // namespace graph::query::execution
