/**
 * @file PerfTrace.cpp
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

#include "graph/debug/PerfTrace.hpp"

#include <atomic>
#include <mutex>
#include <utility>

namespace graph::debug {
	namespace {
		struct Collector {
			std::atomic<bool> enabled{false};
			std::mutex mutex;
			PerfTrace::Snapshot data;
		};

		Collector gCollector;
	}

	void PerfTrace::setEnabled(bool enabled) {
		gCollector.enabled.store(enabled, std::memory_order_relaxed);
	}

	bool PerfTrace::isEnabled() {
		return gCollector.enabled.load(std::memory_order_relaxed);
	}

	void PerfTrace::addDuration(const std::string_view key, const uint64_t durationNs) {
		if (!isEnabled() || key.empty()) {
			return;
		}

		std::lock_guard<std::mutex> lock(gCollector.mutex);
		auto &entry = gCollector.data[std::string(key)];
		entry.totalNs += durationNs;
		entry.calls += 1;
	}

	void PerfTrace::reset() {
		std::lock_guard<std::mutex> lock(gCollector.mutex);
		gCollector.data.clear();
	}

	PerfTrace::Snapshot PerfTrace::snapshotAndReset() {
		std::lock_guard<std::mutex> lock(gCollector.mutex);
		auto snapshot = std::move(gCollector.data);
		gCollector.data.clear();
		return snapshot;
	}

} // namespace graph::debug

