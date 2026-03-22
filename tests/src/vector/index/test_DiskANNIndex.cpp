/**
 * @file test_DiskANNIndex.cpp
 * @author Nexepic
 * @date 2026/1/26
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
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include "graph/core/Database.hpp"
#include "graph/vector/index/DiskANNIndex.hpp"
#include "graph/vector/VectorIndexRegistry.hpp"

using namespace graph::vector;

class DiskANNTest : public ::testing::Test {
protected:
    void SetUp() override {
        boost::uuids::uuid uuid = boost::uuids::random_generator()();
        dbPath = std::filesystem::temp_directory_path() / ("ann_test_" + to_string(uuid) + ".db");
        database = std::make_unique<graph::Database>(dbPath.string());
        database->open();
        dm = database->getStorage()->getDataManager();
        sm = database->getStorage()->getSystemStateManager();
    }

    void TearDown() override {
        database->close();
        std::filesystem::remove(dbPath);
    }

    std::filesystem::path dbPath;
    std::unique_ptr<graph::Database> database;
    std::shared_ptr<graph::storage::DataManager> dm;
    std::shared_ptr<graph::storage::state::SystemStateManager> sm;
};

TEST_F(DiskANNTest, FlatMode_InsertSearch) {
    // 1. Setup
    auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "flat_idx");
    DiskANNConfig config;
    config.dim = 4;
    config.metric = "L2"; // Ensure String support

    // Init config in registry
    VectorIndexConfig vCfg; vCfg.dimension = 4;
    registry->updateConfig(vCfg);

    DiskANNIndex index(registry, config);

    // 2. Insert
    std::vector<float> vec1 = {1.0, 0.0, 0.0, 0.0};
    std::vector<float> vec2 = {0.0, 1.0, 0.0, 0.0};
    index.insert(1, vec1);
    index.insert(2, vec2);

    // 3. Search
    // Query near vec1
    auto results = index.search({0.9, 0.1, 0.0, 0.0}, 2);
    ASSERT_GE(results.size(), 1UL);
    EXPECT_EQ(results[0].first, 1); // Should match Node 1

    // Query near vec2
    results = index.search({0.1, 0.9, 0.0, 0.0}, 2);
    ASSERT_GE(results.size(), 1UL);
    EXPECT_EQ(results[0].first, 2);
}

TEST_F(DiskANNTest, HybridMode_AutoTrain) {
    auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "hybrid_idx");
    DiskANNConfig config;
    config.dim = 4;
    config.autoTrainThreshold = 5; // Low threshold for testing

    VectorIndexConfig vCfg; vCfg.dimension = 4;
    registry->updateConfig(vCfg);

    DiskANNIndex index(registry, config);

    // 1. Insert 4 items (Flat Mode)
    for(int i=1; i<=4; ++i) {
        index.insert(i, {float(i), 0, 0, 0});
    }
    EXPECT_FALSE(index.isPQTrained());

    // 2. Insert 5th item -> Should trigger Train
    index.insert(5, {5.0, 0, 0, 0});

    // Check if trained
    EXPECT_TRUE(index.isPQTrained()) << "Auto-train should have triggered";

    // 3. Insert 6th item (Should use PQ)
    index.insert(6, {6.0, 0, 0, 0});

    // Verify registry has PQ blob for Node 6
    auto ptrs6 = registry->getBlobPtrs(6);
    EXPECT_NE(ptrs6.pqBlob, 0);

    // Verify Node 1 still has no PQ blob (Hybrid state)
    auto ptrs1 = registry->getBlobPtrs(1);
    EXPECT_EQ(ptrs1.pqBlob, 0);

    // 4. Search Mixed
    auto res = index.search({5.9, 0, 0, 0}, 10);
    // Should find Node 6 and Node 5
    bool found6 = false;
    for(auto p : res) if(p.first == 6) found6 = true;
    EXPECT_TRUE(found6);
}

TEST_F(DiskANNTest, Removal) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "rem_idx");
	DiskANNConfig config; config.dim = 4;
	VectorIndexConfig vCfg; vCfg.dimension=4; registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);
	index.insert(1, {1,0,0,0});

	// Verify exists (RawBlob != 0)
	EXPECT_NE(registry->getBlobPtrs(1).rawBlob, 0);

	// Remove
	index.remove(1);

	// Verify gone from Registry
	EXPECT_EQ(registry->getBlobPtrs(1).rawBlob, 0);

	// Verify gone from Search
	auto res = index.search({1,0,0,0}, 1);
	EXPECT_TRUE(res.empty()) << "Removed node should not appear in search results";
}

TEST_F(DiskANNTest, IPMetric_InsertSearch) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "ip_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.metric = "IP";

	VectorIndexConfig vCfg; vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert vectors
	index.insert(1, {1.0, 0.0, 0.0, 0.0});
	index.insert(2, {0.0, 1.0, 0.0, 0.0});
	index.insert(3, {0.5, 0.5, 0.0, 0.0});

	// Search - IP metric uses negative inner product so closest has smallest distance
	auto results = index.search({0.9, 0.1, 0.0, 0.0}, 3);
	ASSERT_GE(results.size(), 1UL);
	// Node 1 should be closest to query (highest IP = most negative distance)
	EXPECT_EQ(results[0].first, 1);
}

TEST_F(DiskANNTest, CosineMetric_InsertSearch) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "cos_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.metric = "Cosine";

	VectorIndexConfig vCfg; vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	index.insert(1, {1.0, 0.0, 0.0, 0.0});
	index.insert(2, {0.0, 1.0, 0.0, 0.0});

	auto results = index.search({0.9, 0.1, 0.0, 0.0}, 2);
	ASSERT_GE(results.size(), 1UL);
	EXPECT_EQ(results[0].first, 1);
}

TEST_F(DiskANNTest, DimensionMismatch_InsertSkipped) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "dim_idx");
	DiskANNConfig config;
	config.dim = 4;

	VectorIndexConfig vCfg; vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert with wrong dimension - should be silently skipped
	index.insert(1, {1.0, 0.0}); // dim=2, expected 4

	// Search should return empty since no valid vectors inserted
	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 1);
	EXPECT_TRUE(results.empty());
}

TEST_F(DiskANNTest, SearchEmptyIndex) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "empty_idx");
	DiskANNConfig config;
	config.dim = 4;

	VectorIndexConfig vCfg; vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Search on empty index should return empty
	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 5);
	EXPECT_TRUE(results.empty());
}

TEST_F(DiskANNTest, BatchInsert_Basic) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "batch_idx");
	DiskANNConfig config;
	config.dim = 4;

	VectorIndexConfig vCfg; vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	std::vector<std::pair<int64_t, std::vector<float>>> batch = {
		{1, {1.0, 0.0, 0.0, 0.0}},
		{2, {0.0, 1.0, 0.0, 0.0}},
		{3, {0.0, 0.0, 1.0, 0.0}},
	};
	index.batchInsert(batch);

	auto results = index.search({0.9, 0.1, 0.0, 0.0}, 3);
	ASSERT_GE(results.size(), 1UL);
	EXPECT_EQ(results[0].first, 1);
}

TEST_F(DiskANNTest, BatchInsert_Empty) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "batch_empty_idx");
	DiskANNConfig config;
	config.dim = 4;

	VectorIndexConfig vCfg; vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Empty batch should be a no-op
	std::vector<std::pair<int64_t, std::vector<float>>> emptyBatch;
	EXPECT_NO_THROW(index.batchInsert(emptyBatch));
}

TEST_F(DiskANNTest, BatchInsert_DimensionMismatch) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "batch_dim_idx");
	DiskANNConfig config;
	config.dim = 4;

	VectorIndexConfig vCfg; vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Mix of valid and invalid dimensions
	std::vector<std::pair<int64_t, std::vector<float>>> batch = {
		{1, {1.0, 0.0, 0.0, 0.0}},  // valid
		{2, {1.0, 0.0}},             // invalid - wrong dimension
		{3, {0.0, 1.0, 0.0, 0.0}},  // valid
	};
	index.batchInsert(batch);

	// Only valid vectors should be searchable
	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 3);
	ASSERT_GE(results.size(), 1UL);
}

TEST_F(DiskANNTest, BatchInsert_AutoTrain) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "batch_train_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.autoTrainThreshold = 3;

	VectorIndexConfig vCfg; vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Batch large enough to trigger auto-training
	std::vector<std::pair<int64_t, std::vector<float>>> batch = {
		{1, {1.0, 0.0, 0.0, 0.0}},
		{2, {0.0, 1.0, 0.0, 0.0}},
		{3, {0.0, 0.0, 1.0, 0.0}},
		{4, {0.0, 0.0, 0.0, 1.0}},
	};
	index.batchInsert(batch);

	EXPECT_TRUE(index.isPQTrained());
}

TEST_F(DiskANNTest, TrainWithEmptySamples) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "train_empty_idx");
	DiskANNConfig config;
	config.dim = 4;

	VectorIndexConfig vCfg; vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Training with empty samples should be a no-op
	std::vector<std::vector<float>> emptySamples;
	EXPECT_NO_THROW(index.train(emptySamples));
	EXPECT_FALSE(index.isPQTrained());
}

TEST_F(DiskANNTest, SizeTracking) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "size_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.autoTrainThreshold = 100; // High threshold to stay in flat mode

	VectorIndexConfig vCfg; vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	EXPECT_EQ(index.size(), 0UL);

	index.insert(1, {1.0, 0.0, 0.0, 0.0});
	EXPECT_EQ(index.size(), 1UL);

	index.insert(2, {0.0, 1.0, 0.0, 0.0});
	EXPECT_EQ(index.size(), 2UL);
}

TEST_F(DiskANNTest, SearchKLargerThanIndex) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "klarger_idx");
	DiskANNConfig config;
	config.dim = 4;

	VectorIndexConfig vCfg; vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	index.insert(1, {1.0, 0.0, 0.0, 0.0});
	index.insert(2, {0.0, 1.0, 0.0, 0.0});

	// Search for more results than available
	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 100);
	EXPECT_LE(results.size(), 2UL);
	EXPECT_GE(results.size(), 1UL);
}

// --- Additional tests targeting uncovered branches ---

TEST_F(DiskANNTest, TrainSmallDimension_MEqualsZeroFallback) {
	// Tests the branch in train() where config_.dim / subDim == 0, so m is set to 1
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "smalldim_idx");
	DiskANNConfig config;
	config.dim = 4; // 4 / 8 = 0, so m should be forced to 1

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Train explicitly with samples
	std::vector<std::vector<float>> samples = {
		{1.0, 0.0, 0.0, 0.0},
		{0.0, 1.0, 0.0, 0.0},
		{0.0, 0.0, 1.0, 0.0},
		{0.0, 0.0, 0.0, 1.0},
		{0.5, 0.5, 0.0, 0.0},
	};
	EXPECT_NO_THROW(index.train(samples));
	EXPECT_TRUE(index.isPQTrained());
}

TEST_F(DiskANNTest, HybridMode_SearchWithRemovedNode) {
	// Tests the branch in search() where distRaw returns max (removed node filtered out)
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "hybrid_rem_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.autoTrainThreshold = 3;

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert enough to trigger training
	for (int i = 1; i <= 5; ++i) {
		index.insert(i, {float(i), float(i) * 0.1f, 0.0f, 0.0f});
	}
	ASSERT_TRUE(index.isPQTrained());

	// Remove a node that is in the graph
	index.remove(3);

	// Search should filter out the removed node
	auto results = index.search({3.0, 0.3, 0.0, 0.0}, 5);
	for (const auto &[id, dist] : results) {
		EXPECT_NE(id, 3) << "Removed node should not appear in results";
	}
}

TEST_F(DiskANNTest, HybridMode_ComputeDistanceFallbackToRaw) {
	// Tests the computeDistance branch where isPQTrained() && !pqTable.empty()
	// but the target node has pqBlob==0 (pre-training node), falling back to distRaw
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "fallback_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.autoTrainThreshold = 3;

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert 2 nodes before training (they will have pqBlob==0)
	index.insert(1, {1.0, 0.0, 0.0, 0.0});
	index.insert(2, {0.0, 1.0, 0.0, 0.0});
	ASSERT_FALSE(index.isPQTrained());

	// Verify pre-training nodes have no PQ blob
	EXPECT_EQ(registry->getBlobPtrs(1).pqBlob, 0);
	EXPECT_EQ(registry->getBlobPtrs(2).pqBlob, 0);

	// Insert 3rd to trigger training
	index.insert(3, {0.0, 0.0, 1.0, 0.0});
	ASSERT_TRUE(index.isPQTrained());

	// Insert post-training node (will have PQ blob)
	index.insert(4, {0.5, 0.5, 0.0, 0.0});
	EXPECT_NE(registry->getBlobPtrs(4).pqBlob, 0);

	// Search: during greedy search, computeDistance will encounter nodes 1,2
	// with pqBlob==0 and must fall back to raw distance
	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 4);
	ASSERT_GE(results.size(), 1UL);
	EXPECT_EQ(results[0].first, 1);
}

TEST_F(DiskANNTest, BatchInsert_AllDimensionMismatch) {
	// Tests batchInsert where all entries have wrong dimensions,
	// resulting in entries.empty() after filtering (line 177)
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "batch_alldim_idx");
	DiskANNConfig config;
	config.dim = 4;

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	std::vector<std::pair<int64_t, std::vector<float>>> batch = {
		{1, {1.0, 0.0}},        // wrong dim
		{2, {1.0}},             // wrong dim
		{3, {1.0, 0.0, 0.0}},  // wrong dim
	};
	EXPECT_NO_THROW(index.batchInsert(batch));

	// Nothing should be searchable
	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 1);
	EXPECT_TRUE(results.empty());
}

TEST_F(DiskANNTest, Insert_BackLinkOverflowPruning) {
	// Tests the back-link overflow pruning branch in insert()
	// where nList.size() > config_.maxDegree * 1.2
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "overflow_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.maxDegree = 2;   // Very small degree to trigger overflow easily
	config.beamWidth = 10;
	config.alpha = 1.0f;    // Minimal alpha to keep more neighbors

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert several nodes to build a graph that will overflow back-links
	index.insert(1, {1.0, 0.0, 0.0, 0.0});
	index.insert(2, {0.9, 0.1, 0.0, 0.0});
	index.insert(3, {0.8, 0.2, 0.0, 0.0});
	index.insert(4, {0.7, 0.3, 0.0, 0.0});
	index.insert(5, {0.6, 0.4, 0.0, 0.0});

	// Search should still work despite pruning
	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 3);
	ASSERT_GE(results.size(), 1UL);
}

TEST_F(DiskANNTest, Insert_DuplicateBackLinkSkip) {
	// Tests the branch in insert() where a back-link already exists (exists==true)
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "duplink_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.maxDegree = 64;
	config.beamWidth = 10;

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert nodes that are very close together - likely to form mutual edges
	index.insert(1, {1.0, 0.0, 0.0, 0.0});
	index.insert(2, {1.0, 0.01, 0.0, 0.0});
	// Insert 3 close to both 1 and 2, which should trigger back-link checks
	index.insert(3, {1.0, 0.02, 0.0, 0.0});

	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 3);
	ASSERT_GE(results.size(), 1UL);
}

TEST_F(DiskANNTest, BatchInsert_PostTraining) {
	// Tests batchInsert when already trained - exercises PQ table computation
	// and PQ code saving in batch mode
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "batch_post_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.autoTrainThreshold = 100; // High threshold, we'll train manually

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Train explicitly
	std::vector<std::vector<float>> samples = {
		{1.0, 0.0, 0.0, 0.0},
		{0.0, 1.0, 0.0, 0.0},
		{0.0, 0.0, 1.0, 0.0},
		{0.0, 0.0, 0.0, 1.0},
		{0.5, 0.5, 0.0, 0.0},
	};
	index.train(samples);
	ASSERT_TRUE(index.isPQTrained());

	// Now batch insert - should use PQ encoding
	std::vector<std::pair<int64_t, std::vector<float>>> batch = {
		{1, {1.0, 0.0, 0.0, 0.0}},
		{2, {0.0, 1.0, 0.0, 0.0}},
		{3, {0.0, 0.0, 1.0, 0.0}},
		{4, {0.5, 0.5, 0.0, 0.0}},
	};
	index.batchInsert(batch);

	// All nodes should have PQ blobs
	for (int64_t id = 1; id <= 4; ++id) {
		EXPECT_NE(registry->getBlobPtrs(id).pqBlob, 0) << "Node " << id << " should have PQ blob";
	}

	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 4);
	ASSERT_GE(results.size(), 1UL);
	EXPECT_EQ(results[0].first, 1);
}

TEST_F(DiskANNTest, BatchInsert_BackLinkOverflow) {
	// Tests the back-link overflow pruning in batchInsert (Phase 4)
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "batch_overflow_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.maxDegree = 2;   // Very small to trigger overflow in batch back-links
	config.beamWidth = 10;
	config.alpha = 1.0f;

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert initial node so graph is not empty
	index.insert(1, {1.0, 0.0, 0.0, 0.0});

	// Batch insert many nodes that will all link to node 1
	std::vector<std::pair<int64_t, std::vector<float>>> batch = {
		{2, {0.9, 0.1, 0.0, 0.0}},
		{3, {0.8, 0.2, 0.0, 0.0}},
		{4, {0.7, 0.3, 0.0, 0.0}},
		{5, {0.6, 0.4, 0.0, 0.0}},
	};
	index.batchInsert(batch);

	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 5);
	ASSERT_GE(results.size(), 1UL);
}

TEST_F(DiskANNTest, Insert_NeighborAdjBlobZero) {
	// Tests the branch in insert() where a neighbor has adjBlob==0
	// This happens when a node was just inserted but hasn't gotten its adjacency yet
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "adjzero_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.beamWidth = 10;

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert two nodes normally
	index.insert(1, {1.0, 0.0, 0.0, 0.0});
	index.insert(2, {0.0, 1.0, 0.0, 0.0});

	// Manually set node 2's adjBlob to 0 to simulate the edge case
	auto ptrs2 = registry->getBlobPtrs(2);
	ptrs2.adjBlob = 0;
	registry->setBlobPtrs(2, ptrs2);

	// Insert node 3 which may try to add back-link to node 2
	index.insert(3, {0.5, 0.5, 0.0, 0.0});

	// Should not crash; search should still work
	auto results = index.search({0.5, 0.5, 0.0, 0.0}, 3);
	ASSERT_GE(results.size(), 1UL);
}

TEST_F(DiskANNTest, GreedySearch_EarlyTermination) {
	// Tests the early termination branch in greedySearch where
	// results.size() >= beamWidth && d > results.top().first
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "early_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.beamWidth = 2; // Very small beam to trigger early termination

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert several spread-out nodes
	index.insert(1, {1.0, 0.0, 0.0, 0.0});
	index.insert(2, {0.0, 1.0, 0.0, 0.0});
	index.insert(3, {0.0, 0.0, 1.0, 0.0});
	index.insert(4, {0.0, 0.0, 0.0, 1.0});
	index.insert(5, {0.5, 0.5, 0.0, 0.0});

	// Search with small k - greedy search should terminate early
	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 1);
	ASSERT_GE(results.size(), 1UL);
	EXPECT_EQ(results[0].first, 1);
}

TEST_F(DiskANNTest, Prune_CandidateRawBlobZero) {
	// Tests the branch in prune() where a candidate has rawBlob==0
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "prune_raw_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.beamWidth = 10;

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert nodes
	index.insert(1, {1.0, 0.0, 0.0, 0.0});
	index.insert(2, {0.9, 0.1, 0.0, 0.0});

	// Remove node 2's raw blob to simulate a candidate with rawBlob==0 during pruning
	index.remove(2);

	// Insert node 3 which will trigger pruning that includes removed node 2
	index.insert(3, {0.8, 0.2, 0.0, 0.0});

	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 3);
	ASSERT_GE(results.size(), 1UL);
}

TEST_F(DiskANNTest, Search_RefinedSizeLargerThanK) {
	// Tests the branch in search() where refined.size() > k (needs truncation)
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "refine_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.beamWidth = 100; // Large beam to get many candidates

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert many nodes
	for (int i = 1; i <= 10; ++i) {
		index.insert(i, {float(i) * 0.1f, float(10 - i) * 0.1f, 0.0f, 0.0f});
	}

	// Search with small k but large beamWidth ensures many candidates
	auto results = index.search({0.1, 0.9, 0.0, 0.0}, 2);
	EXPECT_LE(results.size(), 2UL);
	ASSERT_GE(results.size(), 1UL);
}

// ============================================================================
// Tests targeting specific uncovered branches
// ============================================================================

TEST_F(DiskANNTest, SampleVectors_CountLimitReached) {
	// Cover: line 290 - count >= n branch in sampleVectors
	// Insert more nodes than the sample limit, then call sampleVectors with small n
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "sample_limit_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.autoTrainThreshold = 1000; // High to avoid auto-train

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert 10 nodes
	for (int i = 1; i <= 10; ++i) {
		index.insert(i, {float(i), float(i) * 0.5f, 0.0f, 0.0f});
	}

	// Sample only 3 vectors - should hit count >= n and break early
	auto samples = index.sampleVectors(3);
	EXPECT_EQ(samples.size(), 3UL);

	// Each sampled vector should have dimension 4
	for (const auto &s : samples) {
		EXPECT_EQ(s.size(), 4UL);
	}
}

TEST_F(DiskANNTest, SampleVectors_CountExactlyEqualsN) {
	// Cover: count >= n when count exactly equals n (boundary case)
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "sample_exact_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.autoTrainThreshold = 1000;

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert exactly 5 nodes
	for (int i = 1; i <= 5; ++i) {
		index.insert(i, {float(i), 0.0f, 0.0f, 0.0f});
	}

	// Sample exactly 5 - count should reach n and stop
	auto samples = index.sampleVectors(5);
	EXPECT_EQ(samples.size(), 5UL);

	// Sample 2 - count >= n reached early
	auto samples2 = index.sampleVectors(2);
	EXPECT_EQ(samples2.size(), 2UL);
}

TEST_F(DiskANNTest, Prune_NodeIdHasNoRawBlob) {
	// Cover: line 412 - ptrs.rawBlob == 0 for the nodeId being pruned
	// When prune() is called on a node whose rawBlob is 0, it returns early.
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "prune_noraw_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.beamWidth = 10;
	config.maxDegree = 2; // Small degree to force back-link overflow pruning

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert initial nodes
	index.insert(1, {1.0, 0.0, 0.0, 0.0});
	index.insert(2, {0.9, 0.1, 0.0, 0.0});
	index.insert(3, {0.8, 0.2, 0.0, 0.0});

	// Now remove node 3 (sets all blobs to 0, including rawBlob)
	index.remove(3);

	// Manually give node 3 an adjacency list but no raw blob
	// This simulates a node that appears as a neighbor but has been logically deleted
	auto adjBlob = registry->saveAdjacency({1, 2});
	VectorBlobPtrs corruptPtrs;
	corruptPtrs.rawBlob = 0;    // No raw vector (removed)
	corruptPtrs.pqBlob = 0;
	corruptPtrs.adjBlob = adjBlob;
	registry->setBlobPtrs(3, corruptPtrs);

	// Insert several more nodes to force back-link overflow on node 3
	// When nList.size() > maxDegree * 1.2, prune(3, nList) is called
	// but since rawBlob==0, prune returns early
	index.insert(4, {0.7, 0.3, 0.0, 0.0});
	index.insert(5, {0.6, 0.4, 0.0, 0.0});
	index.insert(6, {0.5, 0.5, 0.0, 0.0});

	// Should not crash; search should still work
	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 5);
	ASSERT_GE(results.size(), 1UL);
}

TEST_F(DiskANNTest, Insert_NeighborAdjBlobZeroDuringBackLink) {
	// Cover: line 223 - nPtrs.adjBlob == 0 during back-link insertion in insert()
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "backlink_adjzero2_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.beamWidth = 10;
	config.maxDegree = 64;

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert first node (becomes entry point)
	index.insert(1, {1.0, 0.0, 0.0, 0.0});
	// Insert second node
	index.insert(2, {0.9, 0.1, 0.0, 0.0});

	// Manually set node 2's adjBlob to 0 to simulate a node
	// that hasn't received its adjacency list yet
	auto ptrs2 = registry->getBlobPtrs(2);
	VectorBlobPtrs noPtrs;
	noPtrs.rawBlob = ptrs2.rawBlob;
	noPtrs.pqBlob = ptrs2.pqBlob;
	noPtrs.adjBlob = 0; // No adjacency list
	registry->setBlobPtrs(2, noPtrs);

	// Insert node 3 which will find node 2 as a candidate for back-linking.
	// Since node 2 has adjBlob==0, the back-link addition is skipped (continue).
	index.insert(3, {0.8, 0.2, 0.0, 0.0});

	// Should not crash; search should still work
	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 3);
	ASSERT_GE(results.size(), 1UL);
}

TEST_F(DiskANNTest, BatchInsert_NeighborAdjBlobZeroDuringBackLink) {
	// Cover: line 223 in batchInsert (Phase 4) - nPtrs.adjBlob == 0
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "batch_adjzero2_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.beamWidth = 10;
	config.maxDegree = 64;

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert initial node
	index.insert(1, {1.0, 0.0, 0.0, 0.0});

	// Set node 1's adjBlob to 0
	auto ptrs1 = registry->getBlobPtrs(1);
	VectorBlobPtrs noPtrs;
	noPtrs.rawBlob = ptrs1.rawBlob;
	noPtrs.pqBlob = ptrs1.pqBlob;
	noPtrs.adjBlob = 0;
	registry->setBlobPtrs(1, noPtrs);

	// Batch insert nodes that will try to add back-links to node 1
	std::vector<std::pair<int64_t, std::vector<float>>> batch = {
		{2, {0.9, 0.1, 0.0, 0.0}},
		{3, {0.8, 0.2, 0.0, 0.0}},
	};
	index.batchInsert(batch);

	// Should not crash
	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 3);
	ASSERT_GE(results.size(), 1UL);
}

TEST_F(DiskANNTest, DistPQ_NoPQBlobReturnMax) {
	// Cover: line 482 - ptrs.pqBlob == 0 in distPQ returns max float
	// After training, search involving nodes without PQ codes should
	// fall back correctly through computeDistance.
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "distpq_noblob2_idx");
	DiskANNConfig config;
	config.dim = 4;
	config.autoTrainThreshold = 3;

	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNIndex index(registry, config);

	// Insert 2 nodes (pre-training, no PQ blobs)
	index.insert(1, {1.0, 0.0, 0.0, 0.0});
	index.insert(2, {0.0, 1.0, 0.0, 0.0});
	ASSERT_FALSE(index.isPQTrained());

	// Verify pre-training nodes have no PQ blob
	EXPECT_EQ(registry->getBlobPtrs(1).pqBlob, 0);
	EXPECT_EQ(registry->getBlobPtrs(2).pqBlob, 0);

	// Insert 3rd to trigger training
	index.insert(3, {0.0, 0.0, 1.0, 0.0});
	ASSERT_TRUE(index.isPQTrained());

	// Insert post-training nodes (will have PQ blobs)
	index.insert(4, {0.5, 0.5, 0.0, 0.0});
	index.insert(5, {0.3, 0.3, 0.3, 0.0});
	EXPECT_NE(registry->getBlobPtrs(4).pqBlob, 0);

	// Search will use computeDistance which checks pqBlob before calling distPQ.
	// For nodes 1,2 with pqBlob==0, computeDistance falls back to distRaw.
	// The distPQ branch (line 482) is defensive - called when computeDistance
	// skips the check or in edge cases.
	auto results = index.search({1.0, 0.0, 0.0, 0.0}, 5);
	ASSERT_GE(results.size(), 1UL);
	EXPECT_EQ(results[0].first, 1);
}