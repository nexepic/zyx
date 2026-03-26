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
#include <algorithm>
#include <ranges>
#include <string>
#include <vector>
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
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

// =============================================================================
// Tests with graph entity types to improve branch coverage for template
// instantiations across all entity types (Edge, Property, Blob, Index, State)
// =============================================================================

#include "graph/core/Edge.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/Blob.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/State.hpp"

// --- Edge entity cache tests ---

TEST(LRUCacheEntityTest, EdgePutGetEvict) {
	graph::storage::LRUCache<int64_t, graph::Edge> cache(2);

	graph::Edge e1;
	e1.setId(1);
	e1.setSourceNodeId(10);
	e1.setTargetNodeId(20);

	graph::Edge e2;
	e2.setId(2);
	e2.setSourceNodeId(30);
	e2.setTargetNodeId(40);

	cache.put(1, e1);
	cache.put(2, e2);

	EXPECT_TRUE(cache.contains(1));
	EXPECT_TRUE(cache.contains(2));
	EXPECT_EQ(cache.size(), 2U);

	auto retrieved = cache.get(1);
	EXPECT_EQ(retrieved.getId(), 1);
	EXPECT_EQ(retrieved.getSourceNodeId(), 10);

	// Peek should not change LRU order
	auto peeked = cache.peek(2);
	EXPECT_EQ(peeked.getId(), 2);

	// Eviction: get(1) made it MRU, so adding e3 should evict e2
	graph::Edge e3;
	e3.setId(3);
	cache.put(3, e3);

	EXPECT_TRUE(cache.contains(1));
	EXPECT_FALSE(cache.contains(2));
	EXPECT_TRUE(cache.contains(3));
}

TEST(LRUCacheEntityTest, EdgeUpdateRemoveClear) {
	graph::storage::LRUCache<int64_t, graph::Edge> cache(3);

	graph::Edge e1;
	e1.setId(1);
	e1.setSourceNodeId(10);
	cache.put(1, e1);

	// Update existing
	graph::Edge e1_updated;
	e1_updated.setId(1);
	e1_updated.setSourceNodeId(99);
	cache.put(1, e1_updated);
	EXPECT_EQ(cache.get(1).getSourceNodeId(), 99);
	EXPECT_EQ(cache.size(), 1U);

	// Remove
	cache.remove(1);
	EXPECT_FALSE(cache.contains(1));
	EXPECT_EQ(cache.size(), 0U);

	// Remove non-existent
	cache.remove(999);
	EXPECT_EQ(cache.size(), 0U);

	// Clear
	cache.put(1, e1);
	cache.put(2, e1_updated);
	cache.clear();
	EXPECT_EQ(cache.size(), 0U);
}

TEST(LRUCacheEntityTest, EdgeZeroCapacity) {
	graph::storage::LRUCache<int64_t, graph::Edge> cache(0);
	graph::Edge e;
	e.setId(1);
	cache.put(1, e);
	EXPECT_EQ(cache.size(), 0U);
	EXPECT_EQ(cache.get(1).getId(), 0); // default
	EXPECT_EQ(cache.peek(1).getId(), 0); // default
}

TEST(LRUCacheEntityTest, EdgeGetNonExistent) {
	graph::storage::LRUCache<int64_t, graph::Edge> cache(2);
	auto result = cache.get(999);
	EXPECT_EQ(result.getId(), 0); // default-constructed
}

TEST(LRUCacheEntityTest, EdgeIterator) {
	graph::storage::LRUCache<int64_t, graph::Edge> cache(3);
	graph::Edge e1, e2, e3;
	e1.setId(1);
	e2.setId(2);
	e3.setId(3);
	cache.put(1, e1);
	cache.put(2, e2);
	cache.put(3, e3);

	std::vector<int64_t> keys;
	for (const auto &[k, v] : cache) {
		keys.push_back(k);
	}
	EXPECT_EQ(keys, (std::vector<int64_t>{3, 2, 1}));
}

// --- Property entity cache tests ---

TEST(LRUCacheEntityTest, PropertyPutGetEvict) {
	graph::storage::LRUCache<int64_t, graph::Property> cache(2);

	graph::Property p1(1, 100, 0);
	graph::Property p2(2, 200, 1);

	cache.put(1, p1);
	cache.put(2, p2);

	EXPECT_EQ(cache.size(), 2U);
	EXPECT_EQ(cache.get(1).getId(), 1);
	EXPECT_EQ(cache.peek(2).getId(), 2);

	// Evict p1 (LRU after get(1) made it MRU, then get nothing, add p3)
	// Actually: put(1), put(2) -> order: [2,1]. get(1) -> order: [1,2]. put(3) evicts 2.
	cache.get(1);
	graph::Property p3(3, 300, 0);
	cache.put(3, p3);
	EXPECT_TRUE(cache.contains(1));
	EXPECT_FALSE(cache.contains(2));
	EXPECT_TRUE(cache.contains(3));
}

TEST(LRUCacheEntityTest, PropertyUpdateRemoveClear) {
	graph::storage::LRUCache<int64_t, graph::Property> cache(3);

	graph::Property p1(1, 100, 0);
	cache.put(1, p1);

	graph::Property p1_updated(1, 999, 0);
	cache.put(1, p1_updated);
	EXPECT_EQ(cache.size(), 1U);

	cache.remove(1);
	EXPECT_EQ(cache.size(), 0U);

	cache.put(1, p1);
	cache.put(2, p1_updated);
	cache.clear();
	EXPECT_EQ(cache.size(), 0U);
}

TEST(LRUCacheEntityTest, PropertyZeroCapacityAndNonExistent) {
	graph::storage::LRUCache<int64_t, graph::Property> cache(0);
	graph::Property p(1, 100, 0);
	cache.put(1, p);
	EXPECT_EQ(cache.size(), 0U);
	EXPECT_EQ(cache.get(1).getId(), 0);
	EXPECT_EQ(cache.peek(1).getId(), 0);

	graph::storage::LRUCache<int64_t, graph::Property> cache2(2);
	EXPECT_EQ(cache2.get(999).getId(), 0);
}

// --- Blob entity cache tests ---

TEST(LRUCacheEntityTest, BlobPutGetEvict) {
	graph::storage::LRUCache<int64_t, graph::Blob> cache(2);

	graph::Blob b1;
	b1.setId(1);
	graph::Blob b2;
	b2.setId(2);

	cache.put(1, b1);
	cache.put(2, b2);

	EXPECT_EQ(cache.size(), 2U);
	EXPECT_EQ(cache.get(1).getId(), 1);
	EXPECT_EQ(cache.peek(2).getId(), 2);

	cache.get(1); // make 1 MRU
	graph::Blob b3;
	b3.setId(3);
	cache.put(3, b3); // evicts 2
	EXPECT_FALSE(cache.contains(2));
	EXPECT_TRUE(cache.contains(1));
	EXPECT_TRUE(cache.contains(3));
}

TEST(LRUCacheEntityTest, BlobUpdateRemoveClear) {
	graph::storage::LRUCache<int64_t, graph::Blob> cache(3);

	graph::Blob b1;
	b1.setId(1);
	cache.put(1, b1);

	graph::Blob b1_updated;
	b1_updated.setId(1);
	b1_updated.setData("updated");
	cache.put(1, b1_updated);
	EXPECT_EQ(cache.size(), 1U);

	cache.remove(1);
	EXPECT_EQ(cache.size(), 0U);
	cache.remove(999); // non-existent

	cache.put(1, b1);
	cache.clear();
	EXPECT_EQ(cache.size(), 0U);
}

TEST(LRUCacheEntityTest, BlobZeroCapacityAndNonExistent) {
	graph::storage::LRUCache<int64_t, graph::Blob> cache(0);
	graph::Blob b;
	b.setId(1);
	cache.put(1, b);
	EXPECT_EQ(cache.size(), 0U);
	EXPECT_EQ(cache.get(1).getId(), 0);
	EXPECT_EQ(cache.peek(1).getId(), 0);

	graph::storage::LRUCache<int64_t, graph::Blob> cache2(2);
	EXPECT_EQ(cache2.get(999).getId(), 0);
}

// --- Index entity cache tests ---

TEST(LRUCacheEntityTest, IndexPutGetEvict) {
	graph::storage::LRUCache<int64_t, graph::Index> cache(2);

	graph::Index i1;
	i1.setId(1);
	graph::Index i2;
	i2.setId(2);

	cache.put(1, i1);
	cache.put(2, i2);

	EXPECT_EQ(cache.size(), 2U);
	EXPECT_EQ(cache.get(1).getId(), 1);
	EXPECT_EQ(cache.peek(2).getId(), 2);

	cache.get(1);
	graph::Index i3;
	i3.setId(3);
	cache.put(3, i3);
	EXPECT_FALSE(cache.contains(2));
	EXPECT_TRUE(cache.contains(1));
	EXPECT_TRUE(cache.contains(3));
}

TEST(LRUCacheEntityTest, IndexUpdateRemoveClear) {
	graph::storage::LRUCache<int64_t, graph::Index> cache(3);

	graph::Index i1;
	i1.setId(1);
	cache.put(1, i1);

	graph::Index i1_updated;
	i1_updated.setId(1);
	i1_updated.setLevel(5);
	cache.put(1, i1_updated);
	EXPECT_EQ(cache.size(), 1U);
	EXPECT_EQ(cache.get(1).getLevel(), 5);

	cache.remove(1);
	EXPECT_EQ(cache.size(), 0U);

	cache.put(1, i1);
	cache.clear();
	EXPECT_EQ(cache.size(), 0U);
}

TEST(LRUCacheEntityTest, IndexZeroCapacityAndNonExistent) {
	graph::storage::LRUCache<int64_t, graph::Index> cache(0);
	graph::Index i;
	i.setId(1);
	cache.put(1, i);
	EXPECT_EQ(cache.size(), 0U);
	EXPECT_EQ(cache.get(1).getId(), 0);
	EXPECT_EQ(cache.peek(1).getId(), 0);

	graph::storage::LRUCache<int64_t, graph::Index> cache2(2);
	EXPECT_EQ(cache2.get(999).getId(), 0);
}

// --- State entity cache tests ---

TEST(LRUCacheEntityTest, StatePutGetEvict) {
	graph::storage::LRUCache<int64_t, graph::State> cache(2);

	graph::State s1;
	s1.setId(1);
	s1.setKey("key1");
	graph::State s2;
	s2.setId(2);
	s2.setKey("key2");

	cache.put(1, s1);
	cache.put(2, s2);

	EXPECT_EQ(cache.size(), 2U);
	EXPECT_EQ(cache.get(1).getId(), 1);
	EXPECT_EQ(cache.get(1).getKey(), "key1");
	EXPECT_EQ(cache.peek(2).getId(), 2);

	cache.get(1);
	graph::State s3;
	s3.setId(3);
	cache.put(3, s3);
	EXPECT_FALSE(cache.contains(2));
	EXPECT_TRUE(cache.contains(1));
	EXPECT_TRUE(cache.contains(3));
}

TEST(LRUCacheEntityTest, StateUpdateRemoveClear) {
	graph::storage::LRUCache<int64_t, graph::State> cache(3);

	graph::State s1;
	s1.setId(1);
	s1.setKey("original");
	cache.put(1, s1);

	graph::State s1_updated;
	s1_updated.setId(1);
	s1_updated.setKey("updated");
	cache.put(1, s1_updated);
	EXPECT_EQ(cache.size(), 1U);
	EXPECT_EQ(cache.get(1).getKey(), "updated");

	cache.remove(1);
	EXPECT_EQ(cache.size(), 0U);

	cache.put(1, s1);
	cache.clear();
	EXPECT_EQ(cache.size(), 0U);
}

TEST(LRUCacheEntityTest, StateZeroCapacityAndNonExistent) {
	graph::storage::LRUCache<int64_t, graph::State> cache(0);
	graph::State s;
	s.setId(1);
	cache.put(1, s);
	EXPECT_EQ(cache.size(), 0U);
	EXPECT_EQ(cache.get(1).getId(), 0);
	EXPECT_EQ(cache.peek(1).getId(), 0);

	graph::storage::LRUCache<int64_t, graph::State> cache2(2);
	EXPECT_EQ(cache2.get(999).getId(), 0);
}

// --- Node entity cache tests (to complete coverage) ---

#include "graph/core/Node.hpp"

TEST(LRUCacheEntityTest, NodePutGetEvict) {
	graph::storage::LRUCache<int64_t, graph::Node> cache(2);

	graph::Node n1;
	n1.setId(1);
	n1.setLabelId(10);
	graph::Node n2;
	n2.setId(2);
	n2.setLabelId(20);

	cache.put(1, n1);
	cache.put(2, n2);

	EXPECT_EQ(cache.size(), 2U);
	EXPECT_EQ(cache.get(1).getId(), 1);
	EXPECT_EQ(cache.peek(2).getLabelId(), 20);

	cache.get(1);
	graph::Node n3;
	n3.setId(3);
	cache.put(3, n3);
	EXPECT_FALSE(cache.contains(2));
	EXPECT_TRUE(cache.contains(1));
	EXPECT_TRUE(cache.contains(3));
}

TEST(LRUCacheEntityTest, NodeUpdateRemoveClear) {
	graph::storage::LRUCache<int64_t, graph::Node> cache(3);

	graph::Node n1;
	n1.setId(1);
	n1.setLabelId(10);
	cache.put(1, n1);

	graph::Node n1_updated;
	n1_updated.setId(1);
	n1_updated.setLabelId(99);
	cache.put(1, n1_updated);
	EXPECT_EQ(cache.size(), 1U);
	EXPECT_EQ(cache.get(1).getLabelId(), 99);

	cache.remove(1);
	EXPECT_EQ(cache.size(), 0U);

	cache.put(1, n1);
	cache.clear();
	EXPECT_EQ(cache.size(), 0U);
}

TEST(LRUCacheEntityTest, NodeZeroCapacityAndNonExistent) {
	graph::storage::LRUCache<int64_t, graph::Node> cache(0);
	graph::Node n;
	n.setId(1);
	cache.put(1, n);
	EXPECT_EQ(cache.size(), 0U);
	EXPECT_EQ(cache.get(1).getId(), 0);
	EXPECT_EQ(cache.peek(1).getId(), 0);

	graph::storage::LRUCache<int64_t, graph::Node> cache2(2);
	EXPECT_EQ(cache2.get(999).getId(), 0);
}

// =============================================================================
// Typed tests: Instantiate LRUCache with all graph entity types as values
// to ensure template branches are covered for each entity type.
// =============================================================================

template<typename T>
class LRUCacheTypedEntityTest : public ::testing::Test {
protected:
	graph::storage::LRUCache<int64_t, T> cache{5};
};

using EntityTypes = ::testing::Types<graph::Node, graph::Edge, graph::Property, graph::Blob, graph::Index, graph::State>;
TYPED_TEST_SUITE(LRUCacheTypedEntityTest, EntityTypes);

// Test put and get with each entity type
TYPED_TEST(LRUCacheTypedEntityTest, PutAndGet) {
	TypeParam entity;
	entity.setId(42);
	this->cache.put(42, entity);

	EXPECT_TRUE(this->cache.contains(42));
	EXPECT_EQ(this->cache.size(), 1U);

	auto retrieved = this->cache.get(42);
	EXPECT_EQ(retrieved.getId(), 42);
}

// Test get on non-existent key returns default-constructed entity
TYPED_TEST(LRUCacheTypedEntityTest, GetNonExistent) {
	auto result = this->cache.get(999);
	// Default-constructed entity should have id 0
	EXPECT_EQ(result.getId(), 0);
}

// Test get with zero-capacity cache
TYPED_TEST(LRUCacheTypedEntityTest, GetZeroCapacity) {
	graph::storage::LRUCache<int64_t, TypeParam> zeroCache(0);
	TypeParam entity;
	entity.setId(1);
	zeroCache.put(1, entity);
	auto result = zeroCache.get(1);
	EXPECT_EQ(result.getId(), 0);
	EXPECT_EQ(zeroCache.size(), 0U);
}

// Test put with zero-capacity cache
TYPED_TEST(LRUCacheTypedEntityTest, PutZeroCapacity) {
	graph::storage::LRUCache<int64_t, TypeParam> zeroCache(0);
	TypeParam entity;
	entity.setId(1);
	zeroCache.put(1, entity);
	EXPECT_FALSE(zeroCache.contains(1));
	EXPECT_EQ(zeroCache.size(), 0U);
}

// Test updating an existing entry
TYPED_TEST(LRUCacheTypedEntityTest, UpdateExisting) {
	TypeParam entity1;
	entity1.setId(10);
	this->cache.put(10, entity1);

	TypeParam entity2;
	entity2.setId(10);
	this->cache.put(10, entity2);

	EXPECT_EQ(this->cache.size(), 1U);
	auto retrieved = this->cache.get(10);
	EXPECT_EQ(retrieved.getId(), 10);
}

// Test LRU eviction
TYPED_TEST(LRUCacheTypedEntityTest, EvictionPolicy) {
	graph::storage::LRUCache<int64_t, TypeParam> smallCache(2);

	TypeParam e1, e2, e3;
	e1.setId(1);
	e2.setId(2);
	e3.setId(3);

	smallCache.put(1, e1);
	smallCache.put(2, e2);

	// Access e1 to make it most recently used
	smallCache.get(1);

	// Adding e3 should evict e2 (LRU)
	smallCache.put(3, e3);

	EXPECT_TRUE(smallCache.contains(1));
	EXPECT_FALSE(smallCache.contains(2));
	EXPECT_TRUE(smallCache.contains(3));
}

// Test peek does not affect LRU order
TYPED_TEST(LRUCacheTypedEntityTest, PeekDoesNotAffectOrder) {
	graph::storage::LRUCache<int64_t, TypeParam> smallCache(2);

	TypeParam e1, e2, e3;
	e1.setId(1);
	e2.setId(2);
	e3.setId(3);

	smallCache.put(1, e1);
	smallCache.put(2, e2);

	// Peek at e1 - should NOT make it MRU
	auto peeked = smallCache.peek(1);
	EXPECT_EQ(peeked.getId(), 1);

	// Add e3, should evict e1 (still LRU since peek doesn't update order)
	smallCache.put(3, e3);

	EXPECT_FALSE(smallCache.contains(1));
	EXPECT_TRUE(smallCache.contains(2));
	EXPECT_TRUE(smallCache.contains(3));
}

// Test peek on non-existent key
TYPED_TEST(LRUCacheTypedEntityTest, PeekNonExistent) {
	auto result = this->cache.peek(999);
	EXPECT_EQ(result.getId(), 0);
}

// Test remove
TYPED_TEST(LRUCacheTypedEntityTest, Remove) {
	TypeParam entity;
	entity.setId(5);
	this->cache.put(5, entity);

	EXPECT_TRUE(this->cache.contains(5));
	this->cache.remove(5);
	EXPECT_FALSE(this->cache.contains(5));
	EXPECT_EQ(this->cache.size(), 0U);
}

// Test remove non-existent
TYPED_TEST(LRUCacheTypedEntityTest, RemoveNonExistent) {
	this->cache.remove(999);
	EXPECT_EQ(this->cache.size(), 0U);
}

// Test clear
TYPED_TEST(LRUCacheTypedEntityTest, Clear) {
	TypeParam e1, e2;
	e1.setId(1);
	e2.setId(2);
	this->cache.put(1, e1);
	this->cache.put(2, e2);

	EXPECT_EQ(this->cache.size(), 2U);
	this->cache.clear();
	EXPECT_EQ(this->cache.size(), 0U);
	EXPECT_FALSE(this->cache.contains(1));
	EXPECT_FALSE(this->cache.contains(2));
}

// Test iterator with entity values
TYPED_TEST(LRUCacheTypedEntityTest, Iterator) {
	TypeParam e1, e2, e3;
	e1.setId(1);
	e2.setId(2);
	e3.setId(3);

	this->cache.put(1, e1);
	this->cache.put(2, e2);
	this->cache.put(3, e3);

	std::vector<int64_t> keys;
	for (auto it = this->cache.begin(); it != this->cache.end(); ++it) {
		keys.push_back(it->first);
	}

	// Most recently used first
	std::vector<int64_t> expected = {3, 2, 1};
	EXPECT_EQ(keys, expected);
}

// Test capacity-one cache with entity types
TYPED_TEST(LRUCacheTypedEntityTest, CapacityOne) {
	graph::storage::LRUCache<int64_t, TypeParam> oneCache(1);

	TypeParam e1, e2;
	e1.setId(1);
	e2.setId(2);

	oneCache.put(1, e1);
	EXPECT_EQ(oneCache.size(), 1U);
	EXPECT_TRUE(oneCache.contains(1));

	oneCache.put(2, e2);
	EXPECT_EQ(oneCache.size(), 1U);
	EXPECT_FALSE(oneCache.contains(1));
	EXPECT_TRUE(oneCache.contains(2));
}

// Test get on zero-capacity cache returns default
TYPED_TEST(LRUCacheTypedEntityTest, GetOnZeroCapacityReturnsDefault) {
	graph::storage::LRUCache<int64_t, TypeParam> zeroCache(0);
	auto result = zeroCache.get(42);
	EXPECT_EQ(result.getId(), 0);
}

// Test multiple operations sequence
TYPED_TEST(LRUCacheTypedEntityTest, MultipleOperationsSequence) {
	TypeParam e1, e2, e3, e4, e5;
	e1.setId(1);
	e2.setId(2);
	e3.setId(3);
	e4.setId(4);
	e5.setId(5);

	this->cache.put(1, e1);
	this->cache.put(2, e2);
	this->cache.put(3, e3);

	// Remove middle element
	this->cache.remove(2);
	EXPECT_EQ(this->cache.size(), 2U);

	// Add more elements
	this->cache.put(4, e4);
	this->cache.put(5, e5);

	EXPECT_TRUE(this->cache.contains(1));
	EXPECT_FALSE(this->cache.contains(2));
	EXPECT_TRUE(this->cache.contains(3));
	EXPECT_TRUE(this->cache.contains(4));
	EXPECT_TRUE(this->cache.contains(5));

	// Get to update LRU order, then check eviction
	this->cache.get(1);

	TypeParam e6, e7;
	e6.setId(6);
	e7.setId(7);
	this->cache.put(6, e6); // Cache now full: {3, 4, 5, 1, 6} (capacity=5)
	this->cache.put(7, e7); // Should evict LRU (3)

	EXPECT_FALSE(this->cache.contains(3));
	EXPECT_TRUE(this->cache.contains(1));
}

// =============================================================================
// String-keyed LRUCache tests to cover template instantiation branches
// for LRUCache<std::string, V> (capacity==0, cache miss, update, eviction)
// =============================================================================

TEST(LRUCacheStringKeyTest, ZeroCapacityGetReturnsDefault) {
	graph::storage::LRUCache<std::string, int> cache(0);
	cache.put("key1", 42);
	EXPECT_EQ(cache.size(), 0U);
	// get on zero-capacity cache should return default
	EXPECT_EQ(cache.get("key1"), 0);
	EXPECT_EQ(cache.get("nonexistent"), 0);
}

TEST(LRUCacheStringKeyTest, ZeroCapacityPutIsNoop) {
	graph::storage::LRUCache<std::string, std::string> cache(0);
	cache.put("hello", "world");
	EXPECT_EQ(cache.size(), 0U);
	EXPECT_FALSE(cache.contains("hello"));
}

TEST(LRUCacheStringKeyTest, CacheMissReturnsDefault) {
	graph::storage::LRUCache<std::string, int> cache(3);
	cache.put("alpha", 1);
	// Miss for a key that was never inserted
	EXPECT_EQ(cache.get("beta"), 0);
	EXPECT_EQ(cache.get("gamma"), 0);
}

TEST(LRUCacheStringKeyTest, CacheMissStringValue) {
	graph::storage::LRUCache<std::string, std::string> cache(3);
	cache.put("key1", "value1");
	EXPECT_EQ(cache.get("missing"), "");
}

TEST(LRUCacheStringKeyTest, UpdateExistingEntry) {
	graph::storage::LRUCache<std::string, int> cache(3);
	cache.put("key1", 10);
	cache.put("key2", 20);
	cache.put("key3", 30);

	// Update key1 - should update value and move to front
	cache.put("key1", 99);
	EXPECT_EQ(cache.get("key1"), 99);
	EXPECT_EQ(cache.size(), 3U);

	// key1 is now MRU due to update, so adding key4 should evict key2 (LRU)
	cache.put("key4", 40);
	EXPECT_FALSE(cache.contains("key2"));
	EXPECT_TRUE(cache.contains("key1"));
	EXPECT_TRUE(cache.contains("key3"));
	EXPECT_TRUE(cache.contains("key4"));
}

TEST(LRUCacheStringKeyTest, EvictionWithStringKeys) {
	graph::storage::LRUCache<std::string, int> cache(2);
	cache.put("first", 1);
	cache.put("second", 2);

	// Access "first" to make it MRU
	cache.get("first");

	// Adding "third" should evict "second" (LRU)
	cache.put("third", 3);
	EXPECT_TRUE(cache.contains("first"));
	EXPECT_FALSE(cache.contains("second"));
	EXPECT_TRUE(cache.contains("third"));
}

TEST(LRUCacheStringKeyTest, PeekMissAndHit) {
	graph::storage::LRUCache<std::string, int> cache(2);
	cache.put("a", 10);
	EXPECT_EQ(cache.peek("a"), 10);
	EXPECT_EQ(cache.peek("nonexistent"), 0);
}

TEST(LRUCacheStringKeyTest, RemoveStringKey) {
	graph::storage::LRUCache<std::string, int> cache(3);
	cache.put("x", 1);
	cache.put("y", 2);
	cache.remove("x");
	EXPECT_FALSE(cache.contains("x"));
	EXPECT_EQ(cache.size(), 1U);

	// Remove non-existent
	cache.remove("z");
	EXPECT_EQ(cache.size(), 1U);
}

TEST(LRUCacheStringKeyTest, ClearStringKeyCache) {
	graph::storage::LRUCache<std::string, int> cache(3);
	cache.put("a", 1);
	cache.put("b", 2);
	cache.clear();
	EXPECT_EQ(cache.size(), 0U);
	EXPECT_FALSE(cache.contains("a"));
}

TEST(LRUCacheStringKeyTest, IteratorStringKey) {
	graph::storage::LRUCache<std::string, int> cache(3);
	cache.put("first", 1);
	cache.put("second", 2);
	cache.put("third", 3);

	std::vector<std::string> keys;
	for (const auto &[k, v] : cache) {
		keys.push_back(k);
	}
	// MRU to LRU
	EXPECT_EQ(keys, (std::vector<std::string>{"third", "second", "first"}));
}

// String key with entity value types to cover LRUCache<string, Entity> branches
TEST(LRUCacheStringKeyTest, StringKeyNodeValue) {
	graph::storage::LRUCache<std::string, graph::Node> cache(2);
	graph::Node n1;
	n1.setId(1);
	n1.setLabelId(10);
	graph::Node n2;
	n2.setId(2);
	n2.setLabelId(20);

	cache.put("node_1", n1);
	cache.put("node_2", n2);

	EXPECT_EQ(cache.get("node_1").getId(), 1);
	EXPECT_EQ(cache.get("nonexistent").getId(), 0); // miss

	// Update existing
	graph::Node n1_updated;
	n1_updated.setId(1);
	n1_updated.setLabelId(99);
	cache.put("node_1", n1_updated);
	EXPECT_EQ(cache.get("node_1").getLabelId(), 99);

	// Eviction
	graph::Node n3;
	n3.setId(3);
	cache.put("node_3", n3);
	EXPECT_FALSE(cache.contains("node_2")); // evicted
	EXPECT_TRUE(cache.contains("node_1"));
	EXPECT_TRUE(cache.contains("node_3"));
}

TEST(LRUCacheStringKeyTest, StringKeyNodeZeroCapacity) {
	graph::storage::LRUCache<std::string, graph::Node> cache(0);
	graph::Node n;
	n.setId(1);
	cache.put("node", n);
	EXPECT_EQ(cache.size(), 0U);
	EXPECT_EQ(cache.get("node").getId(), 0);
}

TEST(LRUCacheStringKeyTest, StringKeyEdgeValue) {
	graph::storage::LRUCache<std::string, graph::Edge> cache(2);
	graph::Edge e1;
	e1.setId(1);
	graph::Edge e2;
	e2.setId(2);

	cache.put("edge_1", e1);
	cache.put("edge_2", e2);
	EXPECT_EQ(cache.get("edge_1").getId(), 1);
	EXPECT_EQ(cache.get("missing").getId(), 0);

	// Update
	graph::Edge e1u;
	e1u.setId(1);
	e1u.setSourceNodeId(42);
	cache.put("edge_1", e1u);
	EXPECT_EQ(cache.get("edge_1").getSourceNodeId(), 42);

	// Evict
	graph::Edge e3;
	e3.setId(3);
	cache.put("edge_3", e3);
	EXPECT_FALSE(cache.contains("edge_2"));
}

TEST(LRUCacheStringKeyTest, StringKeyEdgeZeroCapacity) {
	graph::storage::LRUCache<std::string, graph::Edge> cache(0);
	graph::Edge e;
	e.setId(1);
	cache.put("e", e);
	EXPECT_EQ(cache.size(), 0U);
	EXPECT_EQ(cache.get("e").getId(), 0);
}

TEST(LRUCacheStringKeyTest, StringKeyPropertyValue) {
	graph::storage::LRUCache<std::string, graph::Property> cache(2);
	graph::Property p1(1, 100, 0);
	graph::Property p2(2, 200, 1);

	cache.put("prop_1", p1);
	cache.put("prop_2", p2);
	EXPECT_EQ(cache.get("prop_1").getId(), 1);
	EXPECT_EQ(cache.get("missing").getId(), 0);

	// Update
	graph::Property p1u(1, 999, 0);
	cache.put("prop_1", p1u);
	EXPECT_EQ(cache.size(), 2U);

	// Evict
	graph::Property p3(3, 300, 0);
	cache.put("prop_3", p3);
	EXPECT_FALSE(cache.contains("prop_2"));
}

TEST(LRUCacheStringKeyTest, StringKeyPropertyZeroCapacity) {
	graph::storage::LRUCache<std::string, graph::Property> cache(0);
	graph::Property p(1, 100, 0);
	cache.put("p", p);
	EXPECT_EQ(cache.size(), 0U);
	EXPECT_EQ(cache.get("p").getId(), 0);
}

TEST(LRUCacheStringKeyTest, StringKeyBlobValue) {
	graph::storage::LRUCache<std::string, graph::Blob> cache(2);
	graph::Blob b1;
	b1.setId(1);
	graph::Blob b2;
	b2.setId(2);

	cache.put("blob_1", b1);
	cache.put("blob_2", b2);
	EXPECT_EQ(cache.get("blob_1").getId(), 1);
	EXPECT_EQ(cache.get("missing").getId(), 0);

	// Update
	graph::Blob b1u;
	b1u.setId(1);
	b1u.setData("updated");
	cache.put("blob_1", b1u);
	EXPECT_EQ(cache.size(), 2U);

	// Evict
	graph::Blob b3;
	b3.setId(3);
	cache.put("blob_3", b3);
	EXPECT_FALSE(cache.contains("blob_2"));
}

TEST(LRUCacheStringKeyTest, StringKeyBlobZeroCapacity) {
	graph::storage::LRUCache<std::string, graph::Blob> cache(0);
	graph::Blob b;
	b.setId(1);
	cache.put("b", b);
	EXPECT_EQ(cache.size(), 0U);
	EXPECT_EQ(cache.get("b").getId(), 0);
}

TEST(LRUCacheStringKeyTest, StringKeyIndexValue) {
	graph::storage::LRUCache<std::string, graph::Index> cache(2);
	graph::Index i1;
	i1.setId(1);
	graph::Index i2;
	i2.setId(2);

	cache.put("idx_1", i1);
	cache.put("idx_2", i2);
	EXPECT_EQ(cache.get("idx_1").getId(), 1);
	EXPECT_EQ(cache.get("missing").getId(), 0);

	// Update
	graph::Index i1u;
	i1u.setId(1);
	i1u.setLevel(7);
	cache.put("idx_1", i1u);
	EXPECT_EQ(cache.get("idx_1").getLevel(), 7);

	// Evict
	graph::Index i3;
	i3.setId(3);
	cache.put("idx_3", i3);
	EXPECT_FALSE(cache.contains("idx_2"));
}

TEST(LRUCacheStringKeyTest, StringKeyIndexZeroCapacity) {
	graph::storage::LRUCache<std::string, graph::Index> cache(0);
	graph::Index i;
	i.setId(1);
	cache.put("i", i);
	EXPECT_EQ(cache.size(), 0U);
	EXPECT_EQ(cache.get("i").getId(), 0);
}

TEST(LRUCacheStringKeyTest, StringKeyStateValue) {
	graph::storage::LRUCache<std::string, graph::State> cache(2);
	graph::State s1;
	s1.setId(1);
	s1.setKey("key1");
	graph::State s2;
	s2.setId(2);
	s2.setKey("key2");

	cache.put("state_1", s1);
	cache.put("state_2", s2);
	EXPECT_EQ(cache.get("state_1").getId(), 1);
	EXPECT_EQ(cache.get("missing").getId(), 0);

	// Update
	graph::State s1u;
	s1u.setId(1);
	s1u.setKey("updated");
	cache.put("state_1", s1u);
	EXPECT_EQ(cache.get("state_1").getKey(), "updated");

	// Evict
	graph::State s3;
	s3.setId(3);
	cache.put("state_3", s3);
	EXPECT_FALSE(cache.contains("state_2"));
}

TEST(LRUCacheStringKeyTest, StringKeyStateZeroCapacity) {
	graph::storage::LRUCache<std::string, graph::State> cache(0);
	graph::State s;
	s.setId(1);
	cache.put("s", s);
	EXPECT_EQ(cache.size(), 0U);
	EXPECT_EQ(cache.get("s").getId(), 0);
}

// =============================================================================
// DirtyEntityRegistry tests for Property, Blob, State, Index types
// Focus on getAllDirtyInfos flushing-only path and markDirty for new entities
// =============================================================================

#include "graph/storage/DirtyEntityRegistry.hpp"
#include "graph/storage/data/DirtyEntityInfo.hpp"

// --- Property: getAllDirtyInfos with flushing-only entities ---

TEST(DirtyRegistryCoverageTest, PropertyFlushingOnlyInGetAllDirtyInfos) {
	graph::storage::DirtyEntityRegistry<graph::Property> registry;

	// Add entities 1, 2, 3 and move them all to flushing
	graph::Property p1(1, 100, 0);
	graph::Property p2(2, 200, 0);
	graph::Property p3(3, 300, 0);

	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, p1});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, p2});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, p3});
	registry.createFlushSnapshot();

	// Add entity 4 to active only (entity 1,2,3 are flushing-only)
	graph::Property p4(4, 400, 0);
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, p4});

	auto all = registry.getAllDirtyInfos();
	EXPECT_EQ(all.size(), 4UL);

	// Verify all entities are present - entities 1,2,3 come from flushing (not in active)
	std::vector<int64_t> ids;
	for (const auto &info : all) {
		ASSERT_TRUE(info.backup.has_value());
		ids.push_back(info.backup->getId());
	}
	std::sort(ids.begin(), ids.end());
	EXPECT_EQ(ids, (std::vector<int64_t>{1, 2, 3, 4}));
}

TEST(DirtyRegistryCoverageTest, PropertyMarkDirtyNewEntity) {
	graph::storage::DirtyEntityRegistry<graph::Property> registry;

	// Insert multiple new entities that weren't previously tracked
	for (int64_t i = 1; i <= 5; ++i) {
		graph::Property p(i, i * 100, 0);
		registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, p});
	}
	EXPECT_EQ(registry.size(), 5UL);

	// Each should be independently retrievable
	for (int64_t i = 1; i <= 5; ++i) {
		auto info = registry.getInfo(i);
		ASSERT_TRUE(info.has_value());
		EXPECT_EQ(info->changeType, graph::storage::EntityChangeType::CHANGE_ADDED);
		EXPECT_EQ(info->backup->getId(), i);
	}
}

// --- Blob: getAllDirtyInfos with flushing-only entities ---

TEST(DirtyRegistryCoverageTest, BlobFlushingOnlyInGetAllDirtyInfos) {
	graph::storage::DirtyEntityRegistry<graph::Blob> registry;

	graph::Blob b1;
	b1.setId(1);
	graph::Blob b2;
	b2.setId(2);
	graph::Blob b3;
	b3.setId(3);

	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, b1});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_MODIFIED, b2});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, b3});
	registry.createFlushSnapshot();

	// Only add entity 4 to active - entities 1,2,3 are flushing-only
	graph::Blob b4;
	b4.setId(4);
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, b4});

	auto all = registry.getAllDirtyInfos();
	EXPECT_EQ(all.size(), 4UL);

	std::vector<int64_t> ids;
	for (const auto &info : all) {
		ids.push_back(info.backup->getId());
	}
	std::sort(ids.begin(), ids.end());
	EXPECT_EQ(ids, (std::vector<int64_t>{1, 2, 3, 4}));
}

TEST(DirtyRegistryCoverageTest, BlobMarkDirtyNewEntity) {
	graph::storage::DirtyEntityRegistry<graph::Blob> registry;

	for (int64_t i = 1; i <= 5; ++i) {
		graph::Blob b;
		b.setId(i);
		registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, b});
	}
	EXPECT_EQ(registry.size(), 5UL);
	for (int64_t i = 1; i <= 5; ++i) {
		EXPECT_TRUE(registry.contains(i));
	}
}

// --- State: getAllDirtyInfos with flushing-only entities ---

TEST(DirtyRegistryCoverageTest, StateFlushingOnlyInGetAllDirtyInfos) {
	graph::storage::DirtyEntityRegistry<graph::State> registry;

	graph::State s1;
	s1.setId(1);
	s1.setKey("state1");
	graph::State s2;
	s2.setId(2);
	s2.setKey("state2");

	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, s1});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, s2});
	registry.createFlushSnapshot();

	// Add entity 3 to active only
	graph::State s3;
	s3.setId(3);
	s3.setKey("state3");
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, s3});

	auto all = registry.getAllDirtyInfos();
	EXPECT_EQ(all.size(), 3UL);

	std::vector<int64_t> ids;
	for (const auto &info : all) {
		ids.push_back(info.backup->getId());
	}
	std::sort(ids.begin(), ids.end());
	EXPECT_EQ(ids, (std::vector<int64_t>{1, 2, 3}));
}

TEST(DirtyRegistryCoverageTest, StateMarkDirtyNewEntity) {
	graph::storage::DirtyEntityRegistry<graph::State> registry;

	for (int64_t i = 1; i <= 5; ++i) {
		graph::State s;
		s.setId(i);
		s.setKey("key_" + std::to_string(i));
		registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, s});
	}
	EXPECT_EQ(registry.size(), 5UL);
	for (int64_t i = 1; i <= 5; ++i) {
		auto info = registry.getInfo(i);
		ASSERT_TRUE(info.has_value());
		EXPECT_EQ(info->backup->getKey(), "key_" + std::to_string(i));
	}
}

// --- Index: getAllDirtyInfos with flushing-only entities ---

TEST(DirtyRegistryCoverageTest, IndexFlushingOnlyInGetAllDirtyInfos) {
	graph::storage::DirtyEntityRegistry<graph::Index> registry;

	graph::Index i1;
	i1.setId(1);
	graph::Index i2;
	i2.setId(2);

	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, i1});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, i2});
	registry.createFlushSnapshot();

	// Add entity 3 to active - entities 1,2 are flushing-only
	graph::Index i3;
	i3.setId(3);
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, i3});

	auto all = registry.getAllDirtyInfos();
	EXPECT_EQ(all.size(), 3UL);

	std::vector<int64_t> ids;
	for (const auto &info : all) {
		ids.push_back(info.backup->getId());
	}
	std::sort(ids.begin(), ids.end());
	EXPECT_EQ(ids, (std::vector<int64_t>{1, 2, 3}));
}

TEST(DirtyRegistryCoverageTest, IndexMarkDirtyNewEntity) {
	graph::storage::DirtyEntityRegistry<graph::Index> registry;

	for (int64_t i = 1; i <= 5; ++i) {
		graph::Index idx;
		idx.setId(i);
		idx.setLevel(static_cast<int>(i));
		registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, idx});
	}
	EXPECT_EQ(registry.size(), 5UL);
	for (int64_t i = 1; i <= 5; ++i) {
		EXPECT_TRUE(registry.contains(i));
		auto info = registry.getInfo(i);
		ASSERT_TRUE(info.has_value());
		EXPECT_EQ(info->backup->getLevel(), static_cast<int>(i));
	}
}

// --- Mixed scenario: partial overlap between active and flushing ---

TEST(DirtyRegistryCoverageTest, PropertyPartialOverlapActiveAndFlushing) {
	graph::storage::DirtyEntityRegistry<graph::Property> registry;

	// Entities 1,2,3 go to flushing
	graph::Property p1(1, 100, 0);
	graph::Property p2(2, 200, 0);
	graph::Property p3(3, 300, 0);
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, p1});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, p2});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, p3});
	registry.createFlushSnapshot();

	// Override entity 2 in active, add new entity 4
	graph::Property p2u(2, 999, 0);
	graph::Property p4(4, 400, 0);
	registry.upsert({graph::storage::EntityChangeType::CHANGE_MODIFIED, p2u});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, p4});

	auto all = registry.getAllDirtyInfos();
	EXPECT_EQ(all.size(), 4UL); // entities 2,4 from active + entities 1,3 from flushing

	for (const auto &info : all) {
		if (info.backup->getId() == 2) {
			// Should be the active (newer) version
			EXPECT_EQ(info.changeType, graph::storage::EntityChangeType::CHANGE_MODIFIED);
		}
	}
}

TEST(DirtyRegistryCoverageTest, BlobPartialOverlapActiveAndFlushing) {
	graph::storage::DirtyEntityRegistry<graph::Blob> registry;

	graph::Blob b1, b2, b3;
	b1.setId(1);
	b2.setId(2);
	b3.setId(3);
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, b1});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, b2});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, b3});
	registry.createFlushSnapshot();

	// Override entity 1 in active, add entity 4
	graph::Blob b1u;
	b1u.setId(1);
	b1u.setData("modified");
	graph::Blob b4;
	b4.setId(4);
	registry.upsert({graph::storage::EntityChangeType::CHANGE_MODIFIED, b1u});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, b4});

	auto all = registry.getAllDirtyInfos();
	EXPECT_EQ(all.size(), 4UL);
}

TEST(DirtyRegistryCoverageTest, StatePartialOverlapActiveAndFlushing) {
	graph::storage::DirtyEntityRegistry<graph::State> registry;

	graph::State s1, s2;
	s1.setId(1);
	s1.setKey("original");
	s2.setId(2);
	s2.setKey("state2");
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, s1});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, s2});
	registry.createFlushSnapshot();

	// Override entity 1, add entity 3
	graph::State s1u;
	s1u.setId(1);
	s1u.setKey("updated");
	graph::State s3;
	s3.setId(3);
	registry.upsert({graph::storage::EntityChangeType::CHANGE_MODIFIED, s1u});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, s3});

	auto all = registry.getAllDirtyInfos();
	EXPECT_EQ(all.size(), 3UL);
}

TEST(DirtyRegistryCoverageTest, IndexPartialOverlapActiveAndFlushing) {
	graph::storage::DirtyEntityRegistry<graph::Index> registry;

	graph::Index i1, i2;
	i1.setId(1);
	i2.setId(2);
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, i1});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, i2});
	registry.createFlushSnapshot();

	// Override entity 2, add entity 3
	graph::Index i2u;
	i2u.setId(2);
	i2u.setLevel(10);
	graph::Index i3;
	i3.setId(3);
	registry.upsert({graph::storage::EntityChangeType::CHANGE_MODIFIED, i2u});
	registry.upsert({graph::storage::EntityChangeType::CHANGE_ADDED, i3});

	auto all = registry.getAllDirtyInfos();
	EXPECT_EQ(all.size(), 3UL);

	for (const auto &info : all) {
		if (info.backup->getId() == 2) {
			EXPECT_EQ(info.changeType, graph::storage::EntityChangeType::CHANGE_MODIFIED);
			EXPECT_EQ(info.backup->getLevel(), 10);
		}
	}
}
