/**
 * @file CacheManager.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <list>
#include <unordered_map>
#include <functional>
#include "graph/utils/PairHash.h"

namespace graph::storage {

	template<typename K, typename V>
	class LRUCache {
	public:
		explicit LRUCache(size_t capacity) : capacity_(capacity) {}

		bool contains(const K& key) const {
			return cache_map_.find(key) != cache_map_.end();
		}

		V get(const K& key) {
			auto it = cache_map_.find(key);
			if (it == cache_map_.end()) {
				return V();
			}

			// Move to front (most recently used)
			cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
			return it->second->second;
		}

		void put(const K& key, const V& value) {
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
				const auto& last = cache_list_.back();
				cache_map_.erase(last.first);
				cache_list_.pop_back();
			}

			// Insert new entry at front
			cache_list_.emplace_front(std::make_pair(key, value));
			cache_map_[key] = cache_list_.begin();
		}

		// Peek method to look at the value without affecting the cache order
		V peek(const K& key) const {
			auto it = cache_map_.find(key);
			if (it == cache_map_.end()) {
				return V();
			}
			return it->second->second;
		}

		void remove(const K& key) {
			auto it = cache_map_.find(key);
			if (it != cache_map_.end()) {
				cache_list_.erase(it->second);
				cache_map_.erase(it);
			}
		}

		void clear() {
			cache_map_.clear();
			cache_list_.clear();
		}

		[[nodiscard]] size_t size() const {
			return cache_list_.size();
		}

	private:
		size_t capacity_;
		std::list<std::pair<K, V>> cache_list_;
		// std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> cache_map_;

		using Hash = std::conditional_t<std::is_same_v<K, std::pair<uint64_t, std::string>>,
					utils::PairHash,
					std::hash<K>>;

		std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator, Hash> cache_map_;
	};

} // namespace graph::storage