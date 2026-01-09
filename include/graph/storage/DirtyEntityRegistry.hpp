/**
 * @file DirtyEntityRegistry.hpp
 * @author Nexepic
 * @date 2025/12/1
 *
 * @copyright Copyright (c) 2025 Nexepic
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
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include "data/DirtyEntityInfo.hpp"

namespace graph::storage {

	/**
	 * @brief Manages dirty entities of a specific type.
	 * Implements a double-buffering strategy to allow non-blocking writes during flush operations.
	 */
	template<typename EntityType>
	class DirtyEntityRegistry {
	public:
		using DirtyMap = std::unordered_map<int64_t, DirtyEntityInfo<EntityType>>;

		DirtyEntityRegistry() = default;
		~DirtyEntityRegistry() = default;

		/**
		 * @brief Adds or updates a dirty entity.
		 * Thread-safe: Uses unique_lock for writing.
		 */
		void upsert(const DirtyEntityInfo<EntityType> &info) {
			std::unique_lock<std::shared_mutex> lock(mutex_);
			if (info.backup.has_value()) {
				activeMap_[info.backup->getId()] = info;
				updateCount();
			}
		}

		/**
		 * @brief Attempts to remove an entity from the active dirty registry.
		 * Used to undo an ADD operation that hasn't been persisted yet.
		 *
		 * @param id The ID of the entity to remove.
		 * @return true If the entity was successfully removed from the active map and is NOT currently flushing.
		 * @return false If the entity is currently involved in a flush operation (in flushingMap_).
		 *               In this case, removal is unsafe as it would cause data inconsistency between memory and disk.
		 */
		bool remove(int64_t id) {
			std::unique_lock<std::shared_mutex> lock(mutex_);

			// SAFETY CHECK:
			// If the entity is in the flushing map, the IO thread is currently writing it to disk
			// (or is about to). We cannot simply "disappear" it from memory, because the disk
			// will soon contain this entity.
			if (flushingMap_.contains(id)) {
				return false;
			}

			// Safe to remove from active map (if it exists there)
			activeMap_.erase(id);
			updateCount();
			return true;
		}

		/**
		 * @brief Retrieves the dirty info wrapper if it exists (Active or Flushing).
		 */
		std::optional<DirtyEntityInfo<EntityType>> getInfo(int64_t id) const {
			std::shared_lock<std::shared_mutex> lock(mutex_);

			// 1. Check active map (most recent changes)
			auto it = activeMap_.find(id);
			if (it != activeMap_.end()) {
				return it->second;
			}

			// 2. Check flushing map (snapshot currently being written)
			auto fit = flushingMap_.find(id);
			if (fit != flushingMap_.end()) {
				return fit->second;
			}

			return std::nullopt;
		}

		std::vector<DirtyEntityInfo<EntityType>> getAllDirtyInfos() const {
			std::shared_lock<std::shared_mutex> lock(mutex_);
			std::vector<DirtyEntityInfo<EntityType>> result;

			// Reserve approximate size
			result.reserve(activeMap_.size() + flushingMap_.size());

			// 1. Add all Active entities (Newest data)
			for (const auto &[id, info]: activeMap_) {
				result.push_back(info);
			}

			// 2. Add Flushing entities ONLY if they are not in Active map
			//    (Active map has newer version if ID exists in both)
			for (const auto &[id, info]: flushingMap_) {
				if (!activeMap_.contains(id)) {
					result.push_back(info);
				}
			}

			return result;
		}

		/**
		 * @brief Checks if an entity is marked as dirty.
		 */
		bool contains(int64_t id) const {
			std::shared_lock<std::shared_mutex> lock(mutex_);
			return activeMap_.contains(id) || flushingMap_.contains(id);
		}

		/**
		 * @brief Swaps the active buffer to the flushing buffer.
		 * This "freezes" the current state for FileStorage to write, while opening a new
		 * active map for incoming updates.
		 */
		DirtyMap createFlushSnapshot() {
			std::unique_lock<std::shared_mutex> lock(mutex_);

			// If previous flush wasn't committed, merge active into flushing (should not happen if logic is correct)
			if (!flushingMap_.empty()) {
				for (auto &[id, info]: activeMap_) {
					flushingMap_[id] = info;
				}
				activeMap_.clear();
			} else {
				flushingMap_ = std::move(activeMap_);
				activeMap_ = DirtyMap(); // Reset active
			}

			updateCount();
			return flushingMap_;
		}

		/**
		 * @brief Clears the flushing buffer after successful IO.
		 */
		void commitFlush() {
			std::unique_lock<std::shared_mutex> lock(mutex_);
			flushingMap_.clear();
			updateCount();
		}

		size_t size() const { return dirtyCount_.load(std::memory_order_relaxed); }

		void clear() {
			std::unique_lock<std::shared_mutex> lock(mutex_);
			activeMap_.clear();
			flushingMap_.clear();
			updateCount();
		}

	private:
		void updateCount() { dirtyCount_.store(activeMap_.size() + flushingMap_.size(), std::memory_order_relaxed); }

		mutable std::shared_mutex mutex_;
		DirtyMap activeMap_; // Buffer A: Receiving new updates
		DirtyMap flushingMap_; // Buffer B: Being written to disk
		std::atomic<size_t> dirtyCount_{0};
	};

} // namespace graph::storage
