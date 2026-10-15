/**
 * @file PlanCache.hpp
 * @brief LRU cache for optimized logical query plans.
 **/

#pragma once

#include <list>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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
	explicit PlanCache(size_t maxSize = 128, std::shared_ptr<std::pmr::memory_resource> memoryOwner = {})
		: maxSize_(maxSize), configured_owner_(std::move(memoryOwner)), state_(makeState(configured_owner_)) {}

	/**
	 * @brief Returns a cloned plan if cached, nullopt on miss.
	 */
	[[nodiscard]] std::optional<QueryPlan> get(const std::string &query) const {
		std::lock_guard lock(mutex_);
		if (!state_) { ++misses_; return std::nullopt; }
		auto it = state_->index_.find(std::string_view(query));
		if (it == state_->index_.end()) {
			++misses_;
			return std::nullopt;
		}
		// A failed clone must not change recency or claim a successful hit.
		auto cloned = it->second->second.clone();
		// Move accessed entry to front (most recently used)
		state_->entries_.splice(state_->entries_.begin(), state_->entries_, it->second);
		++hits_;
		return cloned;
	}

	/**
	 * @brief Stores a clone of the plan. Evicts LRU entry if at capacity.
	 */
	void put(const std::string &query, const QueryPlan &plan) {
		if (maxSize_ == 0U) return;
        std::lock_guard lock(mutex_);
        if (!state_) state_ = makeState(configured_owner_);
		// Complete the fallible clone before evicting or changing any cached plan.
        // Cached operator objects belong to CACHE; hit clones retain the caller's
        // active allocation owner instead. A legacy cache preserves outer scopes.
        std::optional<utils::OwnedAllocationScope> allocationScope;
        if (state_->memory_owner_) allocationScope.emplace(state_->memory_owner_);
		auto cloned = plan.clone();
		auto it = state_->index_.find(std::string_view(query));
		if (it != state_->index_.end()) {
			// Update existing entry
			it->second->second = std::move(cloned);
			state_->entries_.splice(state_->entries_.begin(), state_->entries_, it->second);
			return;
		}
		state_->entries_.emplace_front(std::pmr::string(query.data(), query.size(), state_->memory_), std::move(cloned));
		try {
			state_->index_.emplace(state_->entries_.front().first, state_->entries_.begin());
		} catch (...) {
			// A failed hash-node/bucket allocation cannot leave an unindexed plan.
			state_->entries_.pop_front();
			throw;
		}
		// Evict only after both ownership containers have accepted the new entry.
		if (state_->entries_.size() > maxSize_) {
			auto &last = state_->entries_.back();
			state_->index_.erase(last.first);
			state_->entries_.pop_back();
		}
	}

	void clear() {
		std::lock_guard lock(mutex_);
		state_.reset();
	}

	[[nodiscard]] size_t size() const {
		std::lock_guard lock(mutex_);
		return state_ ? state_->entries_.size() : 0U;
	}

	[[nodiscard]] size_t hits() const {
		std::lock_guard lock(mutex_);
		return hits_;
	}

	[[nodiscard]] size_t misses() const {
		std::lock_guard lock(mutex_);
		return misses_;
	}

    // Explicit lifecycle reset also drops the allocator authority. Ordinary
    // clear() preserves it so the next insertion cannot bypass its budget.
    void releaseMemoryOwner() {
        std::lock_guard lock(mutex_);
        state_.reset();
        configured_owner_.reset();
    }

    // Reopening a backend can change its allocation domain. Prepare the new
    // empty state before discarding any old cache, retaining old owners until
    // their metadata and operator objects have actually been destroyed.
    void bindMemoryOwner(std::shared_ptr<std::pmr::memory_resource> owner) {
        std::unique_ptr<State> previous;
        {
            std::lock_guard lock(mutex_);
            if (configured_owner_ == owner) return;
            if (owner) previous = makeState(owner);
            state_.swap(previous);
            configured_owner_ = std::move(owner);
            hits_ = misses_ = 0U;
        }
    }

private:
	struct StringHash {
		using is_transparent = void;
		size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
	};
	struct StringEqual {
		using is_transparent = void;
		bool operator()(std::string_view first, std::string_view second) const noexcept { return first == second; }
	};
	using Entries = std::pmr::list<std::pair<std::pmr::string, QueryPlan>>;
    struct State : utils::OwnedPmrObject {
        explicit State(std::shared_ptr<std::pmr::memory_resource> owner)
            : memory_owner_(std::move(owner)),
              memory_(memory_owner_ ? memory_owner_.get() : std::pmr::get_default_resource()),
              entries_(memory_), index_(memory_) {}
        std::shared_ptr<std::pmr::memory_resource> memory_owner_;
        std::pmr::memory_resource *memory_;
        Entries entries_;
        std::pmr::unordered_map<std::pmr::string, Entries::iterator, StringHash, StringEqual> index_;
    };
    static std::unique_ptr<State> makeState(std::shared_ptr<std::pmr::memory_resource> owner) {
        auto allocationOwner = owner;
        return std::unique_ptr<State>(new (std::move(allocationOwner)) State(std::move(owner)));
    }
    const size_t maxSize_;
    mutable std::mutex mutex_;
    std::shared_ptr<std::pmr::memory_resource> configured_owner_;
    std::unique_ptr<State> state_;
	mutable size_t hits_ = 0;
	mutable size_t misses_ = 0;
};

} // namespace graph::query::cache
