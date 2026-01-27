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