/**
 * @file TokenRegistry.hpp
 * @author Nexepic
 * @date 2026/1/13
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

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include "graph/core/IndexTreeManager.hpp"
#include "graph/storage/CacheManager.hpp"

namespace graph::storage {
	namespace state {
		class SystemStateManager;
	}
	class DataManager;

	class TokenRegistry {
	public:
		// Capacity for the LRU cache
		static constexpr size_t CACHE_SIZE = 5000;
		static constexpr int64_t NULL_TOKEN_ID = 0;
		static constexpr char STORAGE_KEY[] = "sys.dict.token_registry.root";

		TokenRegistry(std::shared_ptr<DataManager> dataManager,
					  std::shared_ptr<state::SystemStateManager> stateManager,
					  size_t cacheSize = CACHE_SIZE);

		// Get ID for string, creating if missing.
		int64_t getOrCreateTokenId(const std::string &name);

		// Get string for ID.
		std::string resolveTokenName(int64_t tokenId);

		[[nodiscard]] int64_t getRootIndexId() const { return rootIndexId_; }

		void saveState() const;

	private:
		std::shared_ptr<DataManager> dataManager_;
		std::shared_ptr<state::SystemStateManager> stateManager_;
		std::shared_ptr<query::indexes::IndexTreeManager> indexTree_;

		int64_t rootIndexId_ = 0;

		// Use std::mutex. LRU caches modify internal state on read (promotion),
		// so we need exclusive locking anyway.
		mutable std::mutex cacheMutex_;

		// Replaced unordered_map with LRUCache
		LRUCache<std::string, int64_t> stringToIdCache_;
		LRUCache<int64_t, std::string> idToStringCache_;

		void addToCache(const std::string &name, int64_t id);
		void initialize();
	};
} // namespace graph::storage
