/**
 * @file SingleWriterSaturatingCounter.hpp
 * @brief Lock-free saturating metric counter for one serialized producer.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace graph::storage {

	// Maintenance-loop task execution is serialized, while metrics readers may
	// observe it concurrently. The producer publishes task-side effects before
	// it advances a completion metric, so acquire/release makes an observed
	// metric value a reliable completion boundary without claiming multi-writer
	// support.
	class SingleWriterSaturatingCounter final {
	public:
		explicit SingleWriterSaturatingCounter(uint64_t initial = 0U) noexcept : value_(initial) {}

		void increment() noexcept { add(1U); }

		void add(uint64_t increment) noexcept {
			const uint64_t current = value_.load(std::memory_order_relaxed);
			const uint64_t updated = increment > std::numeric_limits<uint64_t>::max() - current
							 ? std::numeric_limits<uint64_t>::max()
							 : current + increment;
			value_.store(updated, std::memory_order_release);
		}

		[[nodiscard]] uint64_t load() const noexcept { return value_.load(std::memory_order_acquire); }

	private:
		std::atomic<uint64_t> value_;
	};

} // namespace graph::storage
