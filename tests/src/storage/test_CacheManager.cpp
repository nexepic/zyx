/**
 * @file test_CacheManager.cpp
 * @author Nexepic
 * @date 2025/12/22
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

#include <gtest/gtest.h>
#include <ranges>
#include <string>
#include <vector>
#include "graph/storage/CacheManager.hpp"

// Test basic put and get operations
TEST(LRUCacheTest, PutAndGet) {
	graph::storage::LRUCache<int, std::string> cache(2);
	cache.put(1, "one");
	cache.put(2, "two");

	EXPECT_EQ(cache.get(1), "one");
	EXPECT_EQ(cache.get(2), "two");
	EXPECT_EQ(cache.size(), 2U);
}

// Test updating an existing value
TEST(LRUCacheTest, UpdateValue) {
	graph::storage::LRUCache<int, std::string> cache(2);
	cache.put(1, "one");
	EXPECT_EQ(cache.get(1), "one");

	cache.put(1, "new_one");
	EXPECT_EQ(cache.get(1), "new_one");
	EXPECT_EQ(cache.size(), 1U);
}

// Test the 'contains' method
TEST(LRUCacheTest, Contains) {
	graph::storage::LRUCache<int, int> cache(2);
	cache.put(1, 10);

	EXPECT_TRUE(cache.contains(1));
	EXPECT_FALSE(cache.contains(2));
}

// Test the Least Recently Used (LRU) eviction policy
TEST(LRUCacheTest, EvictionPolicy) {
	graph::storage::LRUCache<int, int> cache(3);
	cache.put(1, 10); // LRU: 1
	cache.put(2, 20); // LRU: 1, 2
	cache.put(3, 30); // LRU: 1, 2, 3

	// Access 1 to make it the most recently used
	cache.get(1); // LRU: 2, 3, 1

	// Add a new element, which should evict key 2 (the LRU element)
	cache.put(4, 40); // LRU: 3, 1, 4

	EXPECT_FALSE(cache.contains(2));
	EXPECT_TRUE(cache.contains(1));
	EXPECT_TRUE(cache.contains(3));
	EXPECT_TRUE(cache.contains(4));
	EXPECT_EQ(cache.size(), 3U);
}

// Test the 'get' method on a non-existent key
TEST(LRUCacheTest, GetNonExistent) {
	graph::storage::LRUCache<int, int> cache(2);
	cache.put(1, 10);

	// For integers, the default-constructed value is 0
	EXPECT_EQ(cache.get(99), 0);

	graph::storage::LRUCache<int, std::string> str_cache(2);
	str_cache.put(1, "one");
	// For std::string, the default-constructed value is ""
	EXPECT_EQ(str_cache.get(99), "");
}

// Test the 'clear' method
TEST(LRUCacheTest, Clear) {
	graph::storage::LRUCache<int, int> cache(3);
	cache.put(1, 10);
	cache.put(2, 20);

	ASSERT_EQ(cache.size(), 2U);
	ASSERT_TRUE(cache.contains(1));

	cache.clear();

	EXPECT_EQ(cache.size(), 0U);
	EXPECT_FALSE(cache.contains(1));
	EXPECT_FALSE(cache.contains(2));
}

// Test the 'remove' method
TEST(LRUCacheTest, Remove) {
	graph::storage::LRUCache<int, int> cache(3);
	cache.put(1, 10);
	cache.put(2, 20);
	cache.put(3, 30);

	ASSERT_EQ(cache.size(), 3U);

	// Remove an element from the middle of the recency list
	cache.remove(2);
	EXPECT_EQ(cache.size(), 2U);
	EXPECT_FALSE(cache.contains(2));
	EXPECT_TRUE(cache.contains(1));
	EXPECT_TRUE(cache.contains(3));

	// Try to remove an element that doesn't exist
	cache.remove(99);
	EXPECT_EQ(cache.size(), 2U);
}

// Test the 'peek' method to ensure it doesn't alter LRU order
TEST(LRUCacheTest, Peek) {
	graph::storage::LRUCache<int, int> cache(3);
	cache.put(1, 10); // LRU: 1
	cache.put(2, 20); // LRU: 1, 2
	cache.put(3, 30); // LRU: 1, 2, 3

	// Peek at the least recently used element
	EXPECT_EQ(cache.peek(1), 10);

	// Add a new element. If peek updated the order, 2 would be evicted.
	// Since peek should NOT update order, 1 should be evicted.
	cache.put(4, 40);

	EXPECT_FALSE(cache.contains(1)); // Verify 1 was evicted
	EXPECT_TRUE(cache.contains(2));
	EXPECT_TRUE(cache.contains(3));
	EXPECT_TRUE(cache.contains(4));
}

// Test 'peek' on a non-existent key
TEST(LRUCacheTest, PeekNonExistent) {
	graph::storage::LRUCache<int, int> cache(2);
	cache.put(1, 10);
	EXPECT_EQ(cache.peek(99), 0); // Returns default value
}

// Test cache with a capacity of 1
TEST(LRUCacheTest, CapacityOne) {
	graph::storage::LRUCache<int, std::string> cache(1);
	cache.put(1, "one");
	EXPECT_EQ(cache.get(1), "one");
	EXPECT_EQ(cache.size(), 1U);

	cache.put(2, "two"); // This should evict key 1
	EXPECT_EQ(cache.size(), 1U);
	EXPECT_FALSE(cache.contains(1));
	EXPECT_TRUE(cache.contains(2));
	EXPECT_EQ(cache.get(2), "two");
}

// Test cache with a capacity of 0
TEST(LRUCacheTest, CapacityZero) {
	graph::storage::LRUCache<int, int> cache(0);
	cache.put(1, 10);
	EXPECT_EQ(cache.size(), 0U);
	EXPECT_FALSE(cache.contains(1));
}

// Test iterator functionality
TEST(LRUCacheTest, Iterator) {
	graph::storage::LRUCache<int, int> cache(3);
	cache.put(1, 10);
	cache.put(2, 20);
	cache.put(3, 30);

	// Order should be from most-recently-used to least-recently-used
	std::vector<int> keys;
	for (const auto &key: cache | std::views::keys) {
		keys.push_back(key);
	}

	std::vector<int> expected_keys = {3, 2, 1};
	EXPECT_EQ(keys, expected_keys);

	// Update recency and check order again
	cache.get(1); // 1 is now most recent
	keys.clear();
	for (const auto &key: cache | std::views::keys) {
		keys.push_back(key);
	}
	expected_keys = {1, 3, 2};
	EXPECT_EQ(keys, expected_keys);
}

// Test with a complex key type (std::string)
TEST(LRUCacheTest, StringKey) {
	graph::storage::LRUCache<std::string, int> cache(2);
	cache.put("one", 1);
	cache.put("two", 2);

	EXPECT_TRUE(cache.contains("one"));
	EXPECT_EQ(cache.get("two"), 2);

	cache.put("three", 3); // Evicts "one"
	EXPECT_FALSE(cache.contains("one"));
	EXPECT_TRUE(cache.contains("three"));
}

// Test multiple updates to the same key
TEST(LRUCacheTest, MultipleUpdates) {
	graph::storage::LRUCache<int, int> cache(2);
	cache.put(1, 10);
	cache.put(1, 20);
	cache.put(1, 30);

	EXPECT_EQ(cache.get(1), 30);
	EXPECT_EQ(cache.size(), 1U);
}

// Test get on empty cache
TEST(LRUCacheTest, GetFromEmptyCache) {
	graph::storage::LRUCache<int, int> cache(3);
	EXPECT_EQ(cache.get(1), 0);
	EXPECT_EQ(cache.size(), 0U);
}

// Test peek on capacity 0 cache
TEST(LRUCacheTest, PeekCapacityZero) {
	graph::storage::LRUCache<int, int> cache(0);
	cache.put(1, 10);
	EXPECT_EQ(cache.peek(1), 0);
}

// Test remove from empty cache
TEST(LRUCacheTest, RemoveFromEmpty) {
	graph::storage::LRUCache<int, int> cache(2);
	cache.remove(1); // Should not crash
	EXPECT_EQ(cache.size(), 0U);
}

// Test clear on empty cache
TEST(LRUCacheTest, ClearEmpty) {
	graph::storage::LRUCache<int, int> cache(2);
	cache.clear(); // Should not crash
	EXPECT_EQ(cache.size(), 0U);
}

// Test eviction with multiple accesses
TEST(LRUCacheTest, ComplexEvictionPattern) {
	graph::storage::LRUCache<int, int> cache(3);
	cache.put(1, 10); // [1]
	cache.put(2, 20); // [2, 1]
	cache.put(3, 30); // [3, 2, 1]

	cache.get(1); // [1, 3, 2]
	cache.get(2); // [2, 1, 3]

	cache.put(4, 40); // [4, 2, 1] - evicts 3

	EXPECT_FALSE(cache.contains(3));
	EXPECT_TRUE(cache.contains(1));
	EXPECT_TRUE(cache.contains(2));
	EXPECT_TRUE(cache.contains(4));
}

// Test update moves to front
TEST(LRUCacheTest, UpdateMovesToFront) {
	graph::storage::LRUCache<int, int> cache(3);
	cache.put(1, 10);
	cache.put(2, 20);
	cache.put(3, 30);

	cache.put(1, 15); // Update should move to front

	cache.put(4, 40); // Should evict 2, not 1

	EXPECT_TRUE(cache.contains(1));
	EXPECT_FALSE(cache.contains(2));
	EXPECT_TRUE(cache.contains(3));
	EXPECT_TRUE(cache.contains(4));
}

// Test iterator on empty cache
TEST(LRUCacheTest, IteratorEmpty) {
	graph::storage::LRUCache<int, int> cache(3);
	std::vector<int> keys;

	for (const auto &key: cache | std::views::keys) {
		keys.push_back(key);
	}

	EXPECT_TRUE(keys.empty());
}

// Test iterator after clear
TEST(LRUCacheTest, IteratorAfterClear) {
	graph::storage::LRUCache<int, int> cache(3);
	cache.put(1, 10);
	cache.put(2, 20);
	cache.clear();

	std::vector<int> keys;
	for (const auto &key: cache | std::views::keys) {
		keys.push_back(key);
	}

	EXPECT_TRUE(keys.empty());
}

// Test with custom struct as value
TEST(LRUCacheTest, CustomValueType) {
	struct Data {
		int x = 0;
		std::string y;
		bool operator==(const Data &other) const { return x == other.x && y == other.y; }
	};

	graph::storage::LRUCache<int, Data> cache(2);
	cache.put(1, {10, "test"});

	auto result = cache.get(1);
	EXPECT_EQ(result.x, 10);
	EXPECT_EQ(result.y, "test");
}

// Test full capacity behavior
TEST(LRUCacheTest, FullCapacityBehavior) {
	graph::storage::LRUCache<int, int> cache(2);
	cache.put(1, 10);
	cache.put(2, 20);

	EXPECT_EQ(cache.size(), 2U);

	cache.put(3, 30); // Should evict oldest

	EXPECT_EQ(cache.size(), 2U);
	EXPECT_FALSE(cache.contains(1));
}

// Test peek doesn't affect contains
TEST(LRUCacheTest, PeekDoesNotAffectContains) {
	graph::storage::LRUCache<int, int> cache(2);
	cache.put(1, 10);
	cache.put(2, 20);

	(void) cache.peek(1);
	cache.put(3, 30);

	// If peek affected order, 2 would be evicted
	// Since it doesn't, 1 should be evicted
	EXPECT_FALSE(cache.contains(1));
	EXPECT_TRUE(cache.contains(2));
}
