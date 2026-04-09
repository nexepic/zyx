/**
 * @file test_IndexStats.cpp
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

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;
using namespace graph;

class IndexStatsTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_indexstats_" + to_string(uuid) + ".dat");
		db = std::make_unique<Database>(testFilePath.string());
		db->open();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}

	query::QueryResult execute(const std::string &q) const {
		return db->getQueryEngine()->execute(q);
	}

	fs::path testFilePath;
	std::unique_ptr<Database> db;
};

// Test 1: Initial counters are zero after reset
TEST_F(IndexStatsTest, InitialCountersZero) {
	auto im = db->getQueryEngine()->getIndexManager();
	im->resetStats();
	EXPECT_EQ(im->lookups(), 0UL);
	EXPECT_EQ(im->indexHits(), 0UL);
}

// Test 2: Direct findNodeIdsByLabel call increments lookups
TEST_F(IndexStatsTest, DirectLabelLookupIncrementsStats) {
	auto im = db->getQueryEngine()->getIndexManager();
	im->resetStats();

	// Directly call the index lookup (label may or may not be indexed)
	(void) im->findNodeIdsByLabel("Person");
	EXPECT_GT(im->lookups(), 0UL) << "findNodeIdsByLabel should increment lookups";
}

// Test 2b: Index hits tracked when label has entries
TEST_F(IndexStatsTest, IndexHitsTrackedWhenFound) {
	auto im = db->getQueryEngine()->getIndexManager();

	// Create nodes and ensure label index is populated
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	im->resetStats();
	auto result = im->findNodeIdsByLabel("Person");

	EXPECT_GT(im->lookups(), 0UL);
	// If the result is non-empty, hits should also be > 0
	if (!result.empty()) {
		EXPECT_GT(im->indexHits(), 0UL);
	}
}

// Test 3: Lookup on non-existent label increments lookups but not hits
TEST_F(IndexStatsTest, MissOnNonExistentLabel) {
	auto im = db->getQueryEngine()->getIndexManager();
	im->resetStats();

	auto result = im->findNodeIdsByLabel("NonExistent");
	EXPECT_GT(im->lookups(), 0UL) << "Lookup should be counted even for missing label";
	EXPECT_EQ(im->indexHits(), 0UL) << "Non-existent label should not produce hits";
	EXPECT_TRUE(result.empty());
}

// Test 4: Reset stats clears counters
TEST_F(IndexStatsTest, ResetStatsClears) {
	auto im = db->getQueryEngine()->getIndexManager();

	(void) execute("CREATE (n:Person {name: 'Bob'})");
	(void) im->findNodeIdsByLabel("Person");

	EXPECT_GT(im->lookups(), 0UL);

	im->resetStats();
	EXPECT_EQ(im->lookups(), 0UL);
	EXPECT_EQ(im->indexHits(), 0UL);
}

// Test 5: Multiple lookups accumulate
TEST_F(IndexStatsTest, AccumulateStats) {
	auto im = db->getQueryEngine()->getIndexManager();

	(void) execute("CREATE (n:Person {name: 'Alice'})");
	(void) execute("CREATE (n:Animal {name: 'Dog'})");
	im->resetStats();

	(void) im->findNodeIdsByLabel("Person");
	auto lookupsAfterFirst = im->lookups();

	(void) im->findNodeIdsByLabel("Animal");
	auto lookupsAfterSecond = im->lookups();

	EXPECT_GT(lookupsAfterSecond, lookupsAfterFirst)
		<< "Additional lookups should increase counter";
}

// Test 6: Show stats includes index category
TEST_F(IndexStatsTest, ShowStatsIncludesIndex) {
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	auto result = execute("CALL dbms.showStats()");
	EXPECT_FALSE(result.isEmpty());

	bool foundIndexLookups = false;
	bool foundIndexHits = false;
	bool foundIndexHitRate = false;

	for (const auto &row : result.getRows()) {
		auto catIt = row.find("category");
		auto metricIt = row.find("metric");
		if (catIt != row.end() && catIt->second.toString() == "index") {
			if (metricIt != row.end()) {
				std::string metric = metricIt->second.toString();
				if (metric == "lookups") foundIndexLookups = true;
				if (metric == "hits") foundIndexHits = true;
				if (metric == "hit_rate") foundIndexHitRate = true;
			}
		}
	}

	EXPECT_TRUE(foundIndexLookups) << "showStats should include index.lookups";
	EXPECT_TRUE(foundIndexHits) << "showStats should include index.hits";
	EXPECT_TRUE(foundIndexHitRate) << "showStats should include index.hit_rate";
}

// Test 7: Reset stats via Cypher resets index counters
TEST_F(IndexStatsTest, ResetStatsViaCypher) {
	auto im = db->getQueryEngine()->getIndexManager();

	(void) execute("CREATE (n:Person {name: 'Alice'})");
	(void) im->findNodeIdsByLabel("Person");
	EXPECT_GT(im->lookups(), 0UL);

	(void) execute("CALL dbms.resetStats()");

	EXPECT_EQ(im->lookups(), 0UL);
	EXPECT_EQ(im->indexHits(), 0UL);
}

// Test 8: Show stats includes plan cache section
TEST_F(IndexStatsTest, ShowStatsIncludesPlanCache) {
	// Execute same query twice to generate plan cache hit
	(void) execute("CREATE (n:Person {name: 'Alice'})");
	(void) execute("MATCH (n:Person) RETURN n");
	(void) execute("MATCH (n:Person) RETURN n");

	auto result = execute("CALL dbms.showStats()");

	bool foundPlanCache = false;
	for (const auto &row : result.getRows()) {
		auto catIt = row.find("category");
		if (catIt != row.end() && catIt->second.toString() == "plan_cache") {
			foundPlanCache = true;
		}
	}
	EXPECT_TRUE(foundPlanCache) << "showStats should include plan_cache category";
}

// Test 9: findEdgeIdsByType increments stats
TEST_F(IndexStatsTest, EdgeLabelLookupIncrementsStats) {
	auto im = db->getQueryEngine()->getIndexManager();

	(void) execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");
	im->resetStats();

	(void) im->findEdgeIdsByType("KNOWS");
	EXPECT_GT(im->lookups(), 0UL) << "findEdgeIdsByType should increment lookups";
}
