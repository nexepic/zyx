/**
 * @file PlanCache.hpp
 * @brief LRU cache for optimized logical query plans.
 **/

#pragma once

#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include "graph/query/QueryPlan.hpp"

namespace graph::query::cache {

/**
 * @class PlanCache
 * @brief Thread-safe LRU cache for optimized QueryPlan objects.
 *
 * Caches post-optimization, pre-physical-conversion logical plans with
 * their mutation flags. Most effective with parameterized queries where
 * the same template is executed with different parameter values.
 */
class PlanCache {
public:
	explicit PlanCache(size_t maxSize = 128) : maxSize_(maxSize) {}

	/**
	 * @brief Returns a cloned plan if cached, nullopt on miss.
	 */
	[[nodiscard]] std::optional<QueryPlan> get(const std::string &query) const {
		std::lock_guard lock(mutex_);
		auto it = index_.find(query);
		if (it == index_.end()) {
			++misses_;
			return std::nullopt;
		}
		// Move accessed entry to front (most recently used)
		entries_.splice(entries_.begin(), entries_, it->second);
		++hits_;
		return it->second->second.clone();
	}

	/**
	 * @brief Stores a clone of the plan. Evicts LRU entry if at capacity.
	 */
	void put(const std::string &query, const QueryPlan &plan) {
		std::lock_guard lock(mutex_);
		auto it = index_.find(query);
		if (it != index_.end()) {
			// Update existing entry
			it->second->second = plan.clone();
			entries_.splice(entries_.begin(), entries_, it->second);
			return;
		}
		// Evict if at capacity
		if (entries_.size() >= maxSize_) {
			auto &last = entries_.back();
			index_.erase(last.first);
			entries_.pop_back();
		}
		// Insert new entry at front
		entries_.emplace_front(query, plan.clone());
		index_[query] = entries_.begin();
	}

	void clear() {
		std::lock_guard lock(mutex_);
		entries_.clear();
		index_.clear();
	}

	[[nodiscard]] size_t size() const {
		std::lock_guard lock(mutex_);
		return entries_.size();
	}

	[[nodiscard]] size_t hits() const {
		std::lock_guard lock(mutex_);
		return hits_;
	}

	[[nodiscard]] size_t misses() const {
		std::lock_guard lock(mutex_);
		return misses_;
	}

private:
	size_t maxSize_;
	mutable std::mutex mutex_;
	mutable std::list<std::pair<std::string, QueryPlan>> entries_;
	mutable std::unordered_map<std::string, decltype(entries_)::iterator> index_;
	mutable size_t hits_ = 0;
	mutable size_t misses_ = 0;
};

} // namespace graph::query::cache
