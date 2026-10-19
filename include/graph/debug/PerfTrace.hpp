/**
 * @file PerfTrace.hpp
 * @author Nexepic
 * @date 2026/3/26
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace graph::debug {

	class PerfTrace {
	public:
		struct Entry {
			uint64_t totalNs = 0;
			uint64_t calls = 0;
			int64_t totalValue = 0;
			uint64_t valueCalls = 0;
			// Maximum single observed value; meaningful only when valueCalls is nonzero.
			int64_t maximumValue = 0;
		};

		using Snapshot = std::unordered_map<std::string, Entry>;

        // Synchronous thread-local observation. No global toggle/reset and no
        // allocation: the caller owns bounded storage and a noexcept receiver.
        class ScopedObserver final {
        public:
            using Receiver = void (*)(void *, std::string_view, int64_t, bool) noexcept;
            ScopedObserver(void *owner, Receiver receiver) noexcept
                : owner_(owner), receiver_(receiver), previous_(active_) { active_ = this; }
            ~ScopedObserver() { active_ = previous_; }
            ScopedObserver(const ScopedObserver &) = delete;
            ScopedObserver &operator=(const ScopedObserver &) = delete;
        private:
            friend class PerfTrace;
            void *owner_;
            Receiver receiver_;
            ScopedObserver *previous_;
            inline static thread_local ScopedObserver *active_ = nullptr;
        };

		// Maintains the legacy all-tracing toggle for callers that collect both durations and values.
		static void setEnabled(bool enabled);
		[[nodiscard]] static bool isEnabled();
		static void setValueCollectionEnabled(bool enabled);
		[[nodiscard]] static bool areValuesEnabled();

		static void addDuration(std::string_view key, uint64_t durationNs);
		static void addDurationBatch(std::string_view key, uint64_t durationNs, uint64_t calls);
		static void addValue(std::string_view key, int64_t value);
		// Physical pin identities are deduplicated across threads until reset.
		// Overflow is reported explicitly; diagnostics never reject a query.
		static constexpr size_t MAXIMUM_DISTINCT_PAGE_PINS = 65536U;
		static void addPagePin(const void *ioIdentity, uint64_t offset, uint64_t generation) noexcept;

		static void reset();
		[[nodiscard]] static Snapshot snapshotAndReset();
	};

	class ScopedPerfTimer {
	public:
		explicit ScopedPerfTimer(std::string_view key) :
			active_(PerfTrace::isEnabled() && !key.empty()) {
			if (active_) {
				key_.assign(key);
				start_ = Clock::now();
			}
		}

		ScopedPerfTimer(const ScopedPerfTimer &) = delete;
		ScopedPerfTimer &operator=(const ScopedPerfTimer &) = delete;

		~ScopedPerfTimer() {
			if (!active_) {
				return;
			}
			PerfTrace::addDuration(
				key_,
					static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_)
												 .count()));
		}

	private:
		using Clock = std::chrono::steady_clock;

		std::string key_;
		Clock::time_point start_{};
		bool active_ = false;
	};

} // namespace graph::debug
