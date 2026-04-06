/**
 * @file test_VectorIndex.cpp
 * @author Nexepic
 * @date 2026/1/22
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
#include <memory>
#include <vector>
#include "graph/core/Database.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/vector/VectorIndexRegistry.hpp"
#include "graph/vector/core/VectorMetric.hpp"
#include "graph/vector/index/DiskANNIndex.hpp"
#include "graph/vector/quantization/NativeProductQuantizer.hpp"

using namespace graph::vector;

// ==============================================================================
// 1. Math Kernel Tests (SimSIMD Wrapper)
// ==============================================================================

TEST(VectorMetricTest, L2Distance) {
	// 3D vector: [1.0, 2.0, 3.0] vs [4.0, 6.0, 8.0]
	// Diff: [3, 4, 5] -> Sq: 9 + 16 + 25 = 50
	std::vector<float> a = {1.0f, 2.0f, 3.0f};
	std::vector<float> b = {4.0f, 6.0f, 8.0f};

	// Test F32 vs BF16 (Mixed Precision)
	auto bf_b = toBFloat16(b);
	float dist = VectorMetric::computeL2Sqr(a.data(), bf_b.data(), 3);

	// BF16 has precision loss, so use near check
	EXPECT_NEAR(dist, 50.0f, 0.5f);
}

TEST(VectorMetricTest, BF16_L2Distance) {
	std::vector<float> a = {1.5f, -2.5f};
	std::vector<float> b = {1.5f, 0.5f};
	// Diff: [0, -3] -> Sq: 9

	auto bf_a = toBFloat16(a);
	auto bf_b = toBFloat16(b);

	float dist = VectorMetric::computeL2Sqr(bf_a.data(), bf_b.data(), 2);
	EXPECT_NEAR(dist, 9.0f, 0.1f);
}

// ==============================================================================
// 2. Quantization Tests (NativePQ)
// ==============================================================================

class NativePQTest : public ::testing::Test {
protected:
	void SetUp() override {
		// ...
		std::mt19937 rng(42);
		std::normal_distribution<float> dist1(0.0f, 1.0f);
		std::normal_distribution<float> dist2(10.0f, 1.0f);

		for(int i=0; i<500; ++i) {
            std::vector<float> v(16);
            for(int d=0; d<16; ++d) v[d] = dist1(rng);
            trainData.push_back(v);
        }
		for (int i = 0; i < 500; ++i) {
			std::vector<float> v(16);
			for (int d = 0; d < 16; ++d) // FIX LOOP LIMIT
				v[d] = dist2(rng);
			trainData.push_back(v);
		}
	}
	std::vector<std::vector<float>> trainData;
};

TEST_F(NativePQTest, TrainAndEncode) {
	// Dim=16, M=4, Centroids=16
	// SubDim = 16/4 = 4
	NativeProductQuantizer pq(16, 4, 16);

	// Check data integrity before passing
	ASSERT_FALSE(trainData.empty());
	ASSERT_EQ(trainData[0].size(), 16UL);

	EXPECT_NO_THROW(pq.train(trainData));

	EXPECT_TRUE(pq.isTrained());

	// Test Encoding
	std::vector<float> query(16, 0.0f);
	auto codes = pq.encode(query);
	EXPECT_EQ(codes.size(), 4UL);

	// Test Distance Table
	auto table = pq.computeDistanceTable(query);
	EXPECT_EQ(table.size(), 64UL);

	// Verify distance logic
	float pqDist = NativeProductQuantizer::computeDistance(codes, table, 4, 16);
	EXPECT_GE(pqDist, 0.0f);
}

TEST_F(NativePQTest, Serialization) {
	// Match SetUp data dimension (16), Subspaces=4 (16/4)
	NativeProductQuantizer pq(16, 4, 16);
	pq.train(trainData); // trainData is 16-dim

	std::stringstream ss;
	pq.serialize(ss);

	// Reset stream for reading
	ss.seekg(0);
	auto loadedPQ = NativeProductQuantizer::deserialize(ss);

	EXPECT_TRUE(loadedPQ->isTrained());

	// Check if encoding produces same result
	std::vector<float> query(16, 5.0f);

	auto codes1 = pq.encode(query);
	auto codes2 = loadedPQ->encode(query);

	EXPECT_EQ(codes1, codes2);
}

// ==============================================================================
// 3. Storage & Integration Tests (Registry & DiskANN)
// ==============================================================================

class VectorDBTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Setup temporary DB
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		dbPath = std::filesystem::temp_directory_path() / ("vec_test_" + to_string(uuid) + ".db");

		database = std::make_unique<graph::Database>(dbPath.string());
		database->open();

		dataManager = database->getStorage()->getDataManager();
		// Assume SystemStateManager is available via storage or we create one
		// In real app, Database usually exposes this.
		// Here we construct one for testing if not exposed.
		stateManager = database->getStorage()->getSystemStateManager();
	}

	void TearDown() override {
		dataManager.reset();
		stateManager.reset();
		database->close();
		database.reset();
		std::error_code ec;
		std::filesystem::remove(dbPath, ec);
	}

	std::filesystem::path dbPath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::storage::state::SystemStateManager> stateManager;
};

TEST_F(VectorDBTest, RegistryBasicOps) {
	auto registry = std::make_shared<VectorIndexRegistry>(dataManager, stateManager, "test_idx");

	// 1. Config Update
	VectorIndexConfig cfg;
	cfg.dimension = 128;
	cfg.metricType = 0;
	registry->updateConfig(cfg);

	// 2. Blob Mapping
	int64_t nodeId = 100;
	VectorBlobPtrs ptrs {10, 20, 30};
	registry->setBlobPtrs(nodeId, ptrs); // This might update Root ID

	// 3. Reload (Simulate restart)
	auto registry2 = std::make_shared<VectorIndexRegistry>(dataManager, stateManager, "test_idx");
	// registry2 will now load the NEW config with the UPDATED Root ID

	EXPECT_EQ(registry2->getConfig().dimension, 128u);
	auto ptrsRead = registry2->getBlobPtrs(nodeId);
	EXPECT_EQ(ptrsRead.rawBlob, 10);
	EXPECT_EQ(ptrsRead.pqBlob, 20);
	EXPECT_EQ(ptrsRead.adjBlob, 30);
}

TEST_F(VectorDBTest, DiskANN_EndToEnd) {
	// 1. Create Index
	auto registry = std::make_shared<VectorIndexRegistry>(dataManager, stateManager, "ann_idx");
	DiskANNConfig cfg;
	cfg.dim = 8; // Small dim for testing
	cfg.maxDegree = 4;
	cfg.beamWidth = 10;

	// Sync registry config
	VectorIndexConfig regCfg;
	regCfg.dimension = 8;
	registry->updateConfig(regCfg);

	DiskANNIndex index(registry, cfg);

	// 2. Train PQ (Generate dummy data)
	std::vector<std::vector<float>> samples;
	std::mt19937 rng(42);
	std::uniform_real_distribution<float> dist(0.0f, 1.0f);
	for (int i = 0; i < 100; ++i) {
		std::vector<float> v(8);
		for (auto &x: v)
			x = dist(rng);
		samples.push_back(v);
	}
	index.train(samples);

	// 3. Insert Nodes
	// Insert 50 vectors
	for (int i = 0; i < 50; ++i) {
		// NodeID = i+1
		index.insert(i + 1, samples[i]);
	}

	// 4. Search
	// Query with the first vector, should return Node 1 as top result
	auto query = samples[0];
	auto results = index.search(query, 5);

	EXPECT_GE(results.size(), 1UL);
	EXPECT_EQ(results[0].first, 1); // ID match
	EXPECT_LT(results[0].second, 0.01f); // Distance approx 0

	// Check connectivity (Graph quality check)
	// Ensure we can reach deep nodes
	auto queryDeep = samples[49];
	auto resultsDeep = index.search(queryDeep, 5);
	bool found = false;
	for (auto &key: resultsDeep | std::views::keys) {
		if (key == 50)
			found = true;
	}
	EXPECT_TRUE(found) << "Graph should be connected enough to find the last inserted node";
}

TEST_F(VectorDBTest, LargeData_BlobStorage) {
	// Verify that large PQ codebooks are stored via Blob mechanism
	auto registry = std::make_shared<VectorIndexRegistry>(dataManager, stateManager, "large_idx");

	// Train with larger dimension to force a larger codebook
	// Dim 64, M 16 -> 16 * 256 * 4 * 4 bytes = ~64KB (Small enough for State usually, but let's test mechanism)
	NativeProductQuantizer pq(64, 16);

	std::vector<std::vector<float>> samples(256, std::vector<float>(64, 0.5f));
	pq.train(samples);

	registry->saveQuantizer(pq);

	// Check internal state: codebookKey should be set
	auto cfg = registry->getConfig();
	EXPECT_FALSE(cfg.codebookKey.empty());

	// Verify we can load it back
	auto loadedPQ = registry->loadQuantizer();
	EXPECT_NE(loadedPQ, nullptr);
	EXPECT_TRUE(loadedPQ->isTrained());
}
