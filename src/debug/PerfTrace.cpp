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

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <limits>
#include <thread>
#include <utility>
#include <unordered_set>

namespace graph::debug {
	namespace {
		constexpr size_t kShardCount = 64;

		[[nodiscard]] constexpr uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
			const auto maximum = std::numeric_limits<uint64_t>::max();
			return right > maximum - left ? maximum : left + right;
		}

		[[nodiscard]] constexpr int64_t saturatingAdd(int64_t left, int64_t right) noexcept {
			const auto maximum = std::numeric_limits<int64_t>::max();
			const auto minimum = std::numeric_limits<int64_t>::min();
			if (right > 0 && left > maximum - right) return maximum;
			if (right < 0 && left < minimum - right) return minimum;
			return left + right;
		}

		struct Shard {
			std::mutex mutex;
			PerfTrace::Snapshot data;
		};

		struct Collector {
			std::atomic<bool> durationEnabled{false};
			std::atomic<bool> valueEnabled{false};
			std::array<Shard, kShardCount> shards;
			struct PagePinKey {
				const void *io;
				uint64_t offset, generation;
				bool operator==(const PagePinKey &) const = default;
			};
			struct PagePinHash {
				size_t operator()(const PagePinKey &key) const noexcept {
					return std::hash<const void *>{}(key.io) ^ std::hash<uint64_t>{}(key.offset) ^
						(std::hash<uint64_t>{}(key.generation) << 1U);
				}
			};
			std::mutex pagePinMutex;
			std::unordered_set<PagePinKey, PagePinHash> pagePins;
			bool pagePinsIncomplete = false;
		};

		Collector gCollector;

		Shard &currentThreadShard() {
			const auto hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
			return gCollector.shards[hash % gCollector.shards.size()];
		}

		void mergeInto(PerfTrace::Snapshot &target, const PerfTrace::Snapshot &source) {
			for (const auto &[key, sourceEntry]: source) {
				auto &targetEntry = target[key];
				targetEntry.totalNs = saturatingAdd(targetEntry.totalNs, sourceEntry.totalNs);
				targetEntry.calls = saturatingAdd(targetEntry.calls, sourceEntry.calls);
				targetEntry.totalValue = saturatingAdd(targetEntry.totalValue, sourceEntry.totalValue);
				if (sourceEntry.valueCalls != 0U) {
					targetEntry.maximumValue = targetEntry.valueCalls == 0U ? sourceEntry.maximumValue :
							std::max(targetEntry.maximumValue, sourceEntry.maximumValue);
				}
				targetEntry.valueCalls = saturatingAdd(targetEntry.valueCalls, sourceEntry.valueCalls);
			}
		}
	}

	void PerfTrace::setEnabled(bool enabled) {
		gCollector.durationEnabled.store(enabled, std::memory_order_relaxed);
		gCollector.valueEnabled.store(enabled, std::memory_order_relaxed);
	}

	bool PerfTrace::isEnabled() {
		return gCollector.durationEnabled.load(std::memory_order_relaxed);
	}

	void PerfTrace::setValueCollectionEnabled(const bool enabled) {
		gCollector.valueEnabled.store(enabled, std::memory_order_relaxed);
	}

	bool PerfTrace::areValuesEnabled() {
		return gCollector.valueEnabled.load(std::memory_order_relaxed);
	}

	void PerfTrace::addDuration(const std::string_view key, const uint64_t durationNs) {
		if (!isEnabled() || key.empty()) {
			return;
		}

		auto &shard = currentThreadShard();
		std::lock_guard<std::mutex> lock(shard.mutex);
		auto &entry = shard.data[std::string(key)];
		entry.totalNs = saturatingAdd(entry.totalNs, durationNs);
		entry.calls = saturatingAdd(entry.calls, uint64_t{1});
	}

	void PerfTrace::addDurationBatch(const std::string_view key, const uint64_t durationNs, const uint64_t calls) {
		if (!isEnabled() || key.empty() || calls == 0) {
			return;
		}

		auto &shard = currentThreadShard();
		std::lock_guard<std::mutex> lock(shard.mutex);
		auto &entry = shard.data[std::string(key)];
		entry.totalNs = saturatingAdd(entry.totalNs, durationNs);
		entry.calls = saturatingAdd(entry.calls, calls);
	}

	void PerfTrace::addValue(const std::string_view key, const int64_t value) {
		if (!areValuesEnabled() || key.empty()) {
			return;
		}

		auto &shard = currentThreadShard();
		std::lock_guard<std::mutex> lock(shard.mutex);
		auto &entry = shard.data[std::string(key)];
		entry.maximumValue = entry.valueCalls == 0U ? value : std::max(entry.maximumValue, value);
		entry.totalValue = saturatingAdd(entry.totalValue, value);
		entry.valueCalls = saturatingAdd(entry.valueCalls, uint64_t{1});
	}

	void PerfTrace::addPagePin(const void *ioIdentity, uint64_t offset, uint64_t generation) noexcept {
		if (!areValuesEnabled()) return;
		std::lock_guard<std::mutex> lock(gCollector.pagePinMutex);
		const Collector::PagePinKey key{ioIdentity, offset, generation};
		if (gCollector.pagePins.contains(key)) return;
		if (gCollector.pagePins.size() == MAXIMUM_DISTINCT_PAGE_PINS) {
			gCollector.pagePinsIncomplete = true;
			return;
		}
		try {
			gCollector.pagePins.insert(key);
		} catch (...) {
			gCollector.pagePinsIncomplete = true;
		}
	}

	void PerfTrace::reset() {
		{
			std::lock_guard<std::mutex> lock(gCollector.pagePinMutex);
			gCollector.pagePins.clear();
			gCollector.pagePinsIncomplete = false;
		}
		for (auto &shard: gCollector.shards) {
			std::lock_guard<std::mutex> lock(shard.mutex);
			shard.data.clear();
		}
	}

	PerfTrace::Snapshot PerfTrace::snapshotAndReset() {
		Snapshot snapshot;
		{
			std::lock_guard<std::mutex> lock(gCollector.pagePinMutex);
			if (!gCollector.pagePins.empty() || gCollector.pagePinsIncomplete) {
				snapshot["native_graph.read.distinct_page_pins"].totalValue = static_cast<int64_t>(gCollector.pagePins.size());
				snapshot["native_graph.read.distinct_page_pins"].valueCalls = 1U;
				snapshot["native_graph.read.distinct_page_pins"].maximumValue = snapshot["native_graph.read.distinct_page_pins"].totalValue;
				snapshot["native_graph.read.distinct_page_pins_incomplete"].totalValue = gCollector.pagePinsIncomplete ? 1 : 0;
				snapshot["native_graph.read.distinct_page_pins_incomplete"].valueCalls = 1U;
				snapshot["native_graph.read.distinct_page_pins_incomplete"].maximumValue = snapshot["native_graph.read.distinct_page_pins_incomplete"].totalValue;
			}
			gCollector.pagePins.clear();
			gCollector.pagePinsIncomplete = false;
		}
		for (auto &shard: gCollector.shards) {
			std::lock_guard<std::mutex> lock(shard.mutex);
			mergeInto(snapshot, shard.data);
			shard.data.clear();
		}
		return snapshot;
	}

} // namespace graph::debug
