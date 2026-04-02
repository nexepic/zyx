/**
 * @file CacheManager.hpp
 * @author Nexepic
 * @date 2025/3/21
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
#include <list>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace graph::storage {

	template<typename K, typename V>
	class LRUCache {
	public:
		explicit LRUCache(size_t capacity) : capacity_(capacity) {}

		bool contains(const K &key) const {
			std::shared_lock lock(mutex_);
			return cache_map_.contains(key);
		}

		V get(const K &key) {
			std::unique_lock lock(mutex_);
			// Early return if capacity is 0
			if (capacity_ == 0) {
				misses_.fetch_add(1, std::memory_order_relaxed);
				return V();
			}

			auto it = cache_map_.find(key);
			if (it == cache_map_.end()) {
				misses_.fetch_add(1, std::memory_order_relaxed);
				return V();
			}

			hits_.fetch_add(1, std::memory_order_relaxed);
			// Move to front (most recently used)
			cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
			return it->second->second;
		}

		void put(const K &key, const V &value) {
			std::unique_lock lock(mutex_);
			// Early return if capacity is 0
			if (capacity_ == 0) {
				return;
			}

			auto it = cache_map_.find(key);

			if (it != cache_map_.end()) {
				// Update existing entry
				it->second->second = value;
				cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
				return;
			}

			// Check if cache is full
			if (cache_list_.size() >= capacity_) {
				// Remove least recently used
				const auto &last = cache_list_.back();
				cache_map_.erase(last.first);
				cache_list_.pop_back();
			}

			// Insert new entry at front
			cache_list_.emplace_front(std::make_pair(key, value));
			cache_map_[key] = cache_list_.begin();
		}

		// Non-blocking put: attempts to acquire the lock without waiting.
		// If the lock is contended (another thread holds it), silently skips.
		// Ideal for parallel scan paths where blocking on cache is worse than a miss.
		bool tryPut(const K &key, const V &value) {
			std::unique_lock lock(mutex_, std::try_to_lock);
			if (!lock.owns_lock())
				return false;

			if (capacity_ == 0)
				return false;

			auto it = cache_map_.find(key);
			if (it != cache_map_.end()) {
				it->second->second = value;
				cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
				return true;
			}

			if (cache_list_.size() >= capacity_) {
				const auto &last = cache_list_.back();
				cache_map_.erase(last.first);
				cache_list_.pop_back();
			}

			cache_list_.emplace_front(std::make_pair(key, value));
			cache_map_[key] = cache_list_.begin();
			return true;
		}

		// Peek method to look at the value without affecting the cache order
		V peek(const K &key) const {
			std::shared_lock lock(mutex_);
			auto it = cache_map_.find(key);
			if (it == cache_map_.end()) {
				return V();
			}
			return it->second->second;
		}

		// Thread-safe iteration: takes a snapshot of the list
		std::vector<std::pair<K, V>> snapshot() const {
			std::shared_lock lock(mutex_);
			return {cache_list_.begin(), cache_list_.end()};
		}

		// Add iterator support (NOT thread-safe - caller must hold external lock)
		auto begin() const { return cache_list_.begin(); }
		auto end() const { return cache_list_.end(); }

		void remove(const K &key) {
			std::unique_lock lock(mutex_);
			auto it = cache_map_.find(key);
			if (it != cache_map_.end()) {
				cache_list_.erase(it->second);
				cache_map_.erase(it);
			}
		}

		void clear() {
			std::unique_lock lock(mutex_);
			cache_map_.clear();
			cache_list_.clear();
		}

		[[nodiscard]] size_t size() const {
			std::shared_lock lock(mutex_);
			return cache_list_.size();
		}

		[[nodiscard]] uint64_t hits() const { return hits_.load(std::memory_order_relaxed); }
		[[nodiscard]] uint64_t misses() const { return misses_.load(std::memory_order_relaxed); }
		void resetStats() {
			hits_.store(0, std::memory_order_relaxed);
			misses_.store(0, std::memory_order_relaxed);
		}

	private:
		size_t capacity_;
		std::list<std::pair<K, V>> cache_list_;
		std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> cache_map_;
		mutable std::shared_mutex mutex_;
		std::atomic<uint64_t> hits_{0};
		std::atomic<uint64_t> misses_{0};
	};

} // namespace graph::storage
