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

namespace graph::query::execution {

	// A batch owns only the rows currently in flight.  It centralizes row and
	// byte accounting so every vectorized operator enforces the same bounds.
	template <typename T>
	class BoundedBatch final {
	public:
		using SizeFunction = std::function<size_t(const T &)>;

		BoundedBatch(size_t maximumRows, size_t maximumBytes, SizeFunction sizeFunction) :
			maximum_rows_(maximumRows), maximum_bytes_(maximumBytes), size_function_(std::move(sizeFunction)) {
			if (maximum_rows_ == 0U || maximum_bytes_ == 0U || !size_function_) {
				throw std::invalid_argument("bounded batch requires positive limits and a size function");
			}
		}

		[[nodiscard]] size_t size() const noexcept { return values_.size(); }
		[[nodiscard]] bool empty() const noexcept { return values_.empty(); }
		[[nodiscard]] size_t bytes() const noexcept { return bytes_; }
		[[nodiscard]] size_t maximumRows() const noexcept { return maximum_rows_; }
		[[nodiscard]] size_t maximumBytes() const noexcept { return maximum_bytes_; }
		[[nodiscard]] const std::vector<T> &values() const noexcept { return values_; }
		[[nodiscard]] std::vector<T> &values() noexcept { return values_; }

		void reserve(size_t capacity) { values_.reserve(std::min(capacity, maximum_rows_)); }

		void append(T value) {
			if (values_.size() == maximum_rows_) {
				throw std::runtime_error("bounded batch exceeded its row limit");
			}
			const size_t valueBytes = size_function_(value);
			if (valueBytes > maximum_bytes_ - std::min(bytes_, maximum_bytes_)) {
				throw std::runtime_error("bounded batch exceeded its byte limit");
			}
			values_.push_back(std::move(value));
			// Publish the accounting only after vector insertion succeeds; an
			// allocation failure must leave the batch internally consistent.
			bytes_ += valueBytes;
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
			bytes_ = total;
		}

		void clear() noexcept {
			values_.clear();
			bytes_ = 0U;
		}

	private:
		size_t maximum_rows_ = 0U;
		size_t maximum_bytes_ = 0U;
		SizeFunction size_function_;
		std::vector<T> values_;
		size_t bytes_ = 0U;
	};

} // namespace graph::query::execution
