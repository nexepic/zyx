/**
 * @file SnapshotManager.hpp
 * @date 2026/3/27
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

#include <atomic>
#include <memory>
#include <shared_mutex>
#include "CommittedSnapshot.hpp"

namespace graph::storage {

	/**
	 * Manages the current committed snapshot that read-only transactions use.
	 * Thread-safe: writer publishes new snapshots, readers acquire immutable references.
	 */
	class SnapshotManager {
	public:
		SnapshotManager() {
			// Start with an empty snapshot so readers always have something valid
			currentSnapshot_ = std::make_shared<CommittedSnapshot>();
		}

		/**
		 * Called after writer commits — publishes a new snapshot.
		 * @param snap The new committed snapshot
		 */
		void publishSnapshot(std::shared_ptr<CommittedSnapshot> snap) {
			std::unique_lock lock(mutex_);
			snap->snapshotId = nextSnapshotId_.fetch_add(1, std::memory_order_relaxed);
			currentSnapshot_ = std::move(snap);
		}

		/**
		 * Called by readers at begin() — gets an immutable snapshot.
		 * Multiple readers can call this concurrently.
		 * @return shared_ptr to the current snapshot (immutable once published)
		 */
		[[nodiscard]] std::shared_ptr<CommittedSnapshot> acquireSnapshot() const {
			std::shared_lock lock(mutex_);
			return currentSnapshot_;
		}

		/**
		 * @return The next snapshot ID that will be assigned
		 */
		[[nodiscard]] uint64_t getNextSnapshotId() const {
			return nextSnapshotId_.load(std::memory_order_relaxed);
		}

	private:
		mutable std::shared_mutex mutex_;
		std::shared_ptr<CommittedSnapshot> currentSnapshot_;
		std::atomic<uint64_t> nextSnapshotId_{1};
	};

} // namespace graph::storage
