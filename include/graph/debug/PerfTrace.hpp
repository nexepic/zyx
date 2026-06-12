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
		};

		using Snapshot = std::unordered_map<std::string, Entry>;

		static void setEnabled(bool enabled);
		[[nodiscard]] static bool isEnabled();

		static void addDuration(std::string_view key, uint64_t durationNs);

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
