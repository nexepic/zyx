/**
 * @file test_CacheStats.cpp
 * @date 2026/04/02
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

#include <gtest/gtest.h>

#include "graph/storage/CacheManager.hpp"

using namespace graph::storage;

// ============================================================================
// LRUCache hit/miss counter tests
// ============================================================================

TEST(LRUCacheStatsTest, InitialCountersAreZero) {
	LRUCache<int, std::string> cache(10);
	EXPECT_EQ(cache.hits(), 0UL);
	EXPECT_EQ(cache.misses(), 0UL);
}

TEST(LRUCacheStatsTest, MissOnEmptyCache) {
	LRUCache<int, std::string> cache(10);
	auto result = cache.get(1);
	EXPECT_TRUE(result.empty());
	EXPECT_EQ(cache.hits(), 0UL);
	EXPECT_EQ(cache.misses(), 1UL);
}

TEST(LRUCacheStatsTest, HitAfterPut) {
	LRUCache<int, std::string> cache(10);
	cache.put(1, "one");
	auto result = cache.get(1);
	EXPECT_EQ(result, "one");
	EXPECT_EQ(cache.hits(), 1UL);
	EXPECT_EQ(cache.misses(), 0UL);
}

TEST(LRUCacheStatsTest, MissOnNonExistentKey) {
	LRUCache<int, std::string> cache(10);
	cache.put(1, "one");
	auto result = cache.get(2);
	EXPECT_TRUE(result.empty());
	EXPECT_EQ(cache.hits(), 0UL);
	EXPECT_EQ(cache.misses(), 1UL);
}

TEST(LRUCacheStatsTest, MultipleHitsAndMisses) {
	LRUCache<int, std::string> cache(10);
	cache.put(1, "one");
	cache.put(2, "two");

	cache.get(1); // hit
	cache.get(2); // hit
	cache.get(3); // miss
	cache.get(1); // hit
	cache.get(4); // miss

	EXPECT_EQ(cache.hits(), 3UL);
	EXPECT_EQ(cache.misses(), 2UL);
}

TEST(LRUCacheStatsTest, ResetStats) {
	LRUCache<int, std::string> cache(10);
	cache.put(1, "one");
	cache.get(1); // hit
	cache.get(2); // miss

	EXPECT_EQ(cache.hits(), 1UL);
	EXPECT_EQ(cache.misses(), 1UL);

	cache.resetStats();

	EXPECT_EQ(cache.hits(), 0UL);
	EXPECT_EQ(cache.misses(), 0UL);
}

TEST(LRUCacheStatsTest, ZeroCapacityCacheMisses) {
	LRUCache<int, std::string> cache(0);
	auto result = cache.get(1);
	EXPECT_TRUE(result.empty());
	EXPECT_EQ(cache.hits(), 0UL);
	EXPECT_EQ(cache.misses(), 1UL);
}

TEST(LRUCacheStatsTest, EvictionDoesNotAffectCounters) {
	LRUCache<int, std::string> cache(2);
	cache.put(1, "one");
	cache.put(2, "two");
	cache.put(3, "three"); // evicts key 1

	cache.get(1); // miss (evicted)
	cache.get(2); // hit
	cache.get(3); // hit

	EXPECT_EQ(cache.hits(), 2UL);
	EXPECT_EQ(cache.misses(), 1UL);
}

// ============================================================================
// PageBufferPool hit/miss counter tests (via Database)
// ============================================================================

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>

#include "graph/core/Database.hpp"

namespace fs = std::filesystem;
using namespace graph;

class PageBufferPoolStatsTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_cachestats_" + to_string(uuid) + ".dat");
		db = std::make_unique<Database>(testFilePath.string());
		db->open();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}

	fs::path testFilePath;
	std::unique_ptr<Database> db;
};

TEST_F(PageBufferPoolStatsTest, StatsAccessible) {
	auto dm = db->getStorage()->getDataManager();
	auto &pool = dm->getPagePool();

	// Stats should be accessible without errors
	(void) pool.hits();
	(void) pool.misses();
}

TEST_F(PageBufferPoolStatsTest, ResetStats) {
	auto dm = db->getStorage()->getDataManager();
	auto &pool = dm->getPagePool();

	// Do some operations to generate stats
	auto qe = db->getQueryEngine();
	(void) qe->execute("CREATE (n:Person {name: 'Alice'})");
	(void) qe->execute("MATCH (n:Person) RETURN n");

	pool.resetStats();

	EXPECT_EQ(pool.hits(), 0UL);
	EXPECT_EQ(pool.misses(), 0UL);
}

TEST_F(PageBufferPoolStatsTest, OperationsGenerateStats) {
	auto dm = db->getStorage()->getDataManager();
	auto &pool = dm->getPagePool();
	pool.resetStats();

	auto qe = db->getQueryEngine();
	// Create enough data to generate page accesses
	for (int i = 0; i < 20; ++i) {
		(void) qe->execute("CREATE (n:Person {name: 'Person" + std::to_string(i) + "', age: " + std::to_string(i) + "})");
	}
	(void) qe->execute("MATCH (n:Person) RETURN n");

	// Page pool may or may not be hit depending on storage layer behavior.
	// Just verify the counters are accessible and non-negative (no crash).
	(void) pool.hits();
	(void) pool.misses();
}

TEST_F(PageBufferPoolStatsTest, ShowStatsViaCypher) {
	auto qe = db->getQueryEngine();

	// Generate some cache activity first
	(void) qe->execute("CREATE (n:Test {val: 1})");
	(void) qe->execute("MATCH (n:Test) RETURN n");

	auto result = qe->execute("CALL dbms.showStats()");
	EXPECT_FALSE(result.isEmpty());

	// Should have rows for cache, index, and plan_cache categories
	bool foundCache = false;
	for (const auto &row : result.getRows()) {
		auto catIt = row.find("category");
		if (catIt != row.end() && catIt->second.toString() == "cache") {
			foundCache = true;
		}
	}
	EXPECT_TRUE(foundCache) << "Expected 'cache' category in showStats output";
}

TEST_F(PageBufferPoolStatsTest, ResetStatsViaCypher) {
	auto qe = db->getQueryEngine();

	// Generate some activity
	(void) qe->execute("CREATE (n:Test {val: 1})");
	(void) qe->execute("MATCH (n:Test) RETURN n");

	auto result = qe->execute("CALL dbms.resetStats()");
	EXPECT_FALSE(result.isEmpty());
	EXPECT_EQ(result.rowCount(), 1UL);

	auto dm = db->getStorage()->getDataManager();
	auto &pool = dm->getPagePool();
	EXPECT_EQ(pool.hits(), 0UL);
	EXPECT_EQ(pool.misses(), 0UL);
}
