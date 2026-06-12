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

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace graph::debug {
	namespace {
		constexpr size_t kShardCount = 64;

		struct Shard {
			std::mutex mutex;
			PerfTrace::Snapshot data;
		};

		struct Collector {
			std::atomic<bool> enabled{false};
			std::array<Shard, kShardCount> shards;
		};

		Collector gCollector;

		Shard &currentThreadShard() {
			const auto hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
			return gCollector.shards[hash % gCollector.shards.size()];
		}

		void mergeInto(PerfTrace::Snapshot &target, const PerfTrace::Snapshot &source) {
			for (const auto &[key, sourceEntry]: source) {
				auto &targetEntry = target[key];
				targetEntry.totalNs += sourceEntry.totalNs;
				targetEntry.calls += sourceEntry.calls;
			}
		}
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

		auto &shard = currentThreadShard();
		std::lock_guard<std::mutex> lock(shard.mutex);
		auto &entry = shard.data[std::string(key)];
		entry.totalNs += durationNs;
		entry.calls += 1;
	}

	void PerfTrace::reset() {
		for (auto &shard: gCollector.shards) {
			std::lock_guard<std::mutex> lock(shard.mutex);
			shard.data.clear();
		}
	}

	PerfTrace::Snapshot PerfTrace::snapshotAndReset() {
		Snapshot snapshot;
		for (auto &shard: gCollector.shards) {
			std::lock_guard<std::mutex> lock(shard.mutex);
			mergeInto(snapshot, shard.data);
			shard.data.clear();
		}
		return snapshot;
	}

} // namespace graph::debug
