/**
 * @file test_VectorIndexRegistry.cpp
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

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/vector/VectorIndexRegistry.hpp"
#include "graph/vector/quantization/NativeProductQuantizer.hpp"

using namespace graph::vector;

class RegistryTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		dbPath = std::filesystem::temp_directory_path() / ("reg_test_" + to_string(uuid) + ".db");
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

TEST_F(RegistryTest, ConfigPersistence) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "idx1");

	VectorIndexConfig cfg;
	cfg.dimension = 128;
	cfg.metricType = 1;
	registry->updateConfig(cfg);

	// Re-open
	auto registry2 = std::make_shared<VectorIndexRegistry>(dm, sm, "idx1");
	auto readCfg = registry2->getConfig();

	EXPECT_EQ(readCfg.dimension, 128u);
	EXPECT_EQ(readCfg.metricType, 1u);
}

TEST_F(RegistryTest, BlobMapping) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "idx2");
	// Ensure mapping tree initialized
	VectorIndexConfig cfg;
	cfg.dimension = 4;
	registry->updateConfig(cfg);

	int64_t nodeId = 123;
	VectorBlobPtrs ptrs{1, 2, 3};
	registry->setBlobPtrs(nodeId, ptrs);

	auto readPtrs = registry->getBlobPtrs(nodeId);
	EXPECT_EQ(readPtrs.rawBlob, 1);
	EXPECT_EQ(readPtrs.pqBlob, 2);
	EXPECT_EQ(readPtrs.adjBlob, 3);
}

TEST_F(RegistryTest, ScanKeys) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "idx3");
	VectorIndexConfig cfg;
	cfg.dimension = 4;
	registry->updateConfig(cfg);

	// Insert mappings for multiple nodes
	for (int i = 1; i <= 10; ++i) {
		registry->setBlobPtrs(i, {1, 0, 0});
	}

	// Test scan
	auto ids = registry->getAllNodeIds(100);
	EXPECT_EQ(ids.size(), 10UL);

	// Check content (order might depend on B+Tree)
	std::set<int64_t> idSet(ids.begin(), ids.end());
	EXPECT_TRUE(idSet.count(1));
	EXPECT_TRUE(idSet.count(10));
}

TEST_F(RegistryTest, InvalidLoad) {
	// Try to load a registry that doesn't exist in State
	// VectorIndexRegistry constructor might initialize defaults,
	// but getConfig() should show dimension 0.
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "ghost_idx");
	EXPECT_EQ(registry->getConfig().dimension, 0u);
}

TEST_F(RegistryTest, SetBlobPtrs_UpdateExisting) {
	// Test updating blob pointers when mapping already exists
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "idx4");
	VectorIndexConfig cfg;
	cfg.dimension = 4;
	registry->updateConfig(cfg);

	int64_t nodeId = 100;
	VectorBlobPtrs ptrs1{10, 20, 30};
	registry->setBlobPtrs(nodeId, ptrs1);

	// Update with new values
	VectorBlobPtrs ptrs2{40, 50, 60};
	registry->setBlobPtrs(nodeId, ptrs2);

	// Config should be updated with new mapping root ID
	auto readCfg = registry->getConfig();
	EXPECT_GT(readCfg.mappingIndexId, 0);

	// Verify that blob pointers are updated
	auto readPtrs = registry->getBlobPtrs(nodeId);
	EXPECT_EQ(readPtrs.rawBlob, 40);
	EXPECT_EQ(readPtrs.pqBlob, 50);
	EXPECT_EQ(readPtrs.adjBlob, 60);
}

TEST_F(RegistryTest, LoadRawVector_LoadZero) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "idx6");
	VectorIndexConfig cfg;
	cfg.dimension = 8;
	registry->updateConfig(cfg);

	// Loading blob ID 0 should return empty vector
	auto vec = registry->loadRawVector(0);
	EXPECT_TRUE(vec.empty());
}

TEST_F(RegistryTest, SaveLoadPQCodes) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "idx7");
	VectorIndexConfig cfg;
	cfg.dimension = 16;
	registry->updateConfig(cfg);

	std::vector<uint8_t> codes = {1, 2, 3, 4, 5};
	auto blobId = registry->savePQCodes(codes);
	EXPECT_GT(blobId, 0);

	auto loadedCodes = registry->loadPQCodes(blobId);
	EXPECT_EQ(loadedCodes.size(), codes.size());
	for (size_t i = 0; i < codes.size(); ++i) {
		EXPECT_EQ(loadedCodes[i], codes[i]);
	}
}

TEST_F(RegistryTest, LoadPQCodes_EmptyBlobId) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "idx8");
	VectorIndexConfig cfg;
	cfg.dimension = 16;
	registry->updateConfig(cfg);

	// Loading blob ID 0 should return empty codes
	auto codes = registry->loadPQCodes(0);
	EXPECT_TRUE(codes.empty());
}

TEST_F(RegistryTest, SaveLoadAdjacency) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "idx9");
	VectorIndexConfig cfg;
	cfg.dimension = 16;
	registry->updateConfig(cfg);

	std::vector<int64_t> neighbors = {100, 200, 300, 400};
	auto blobId = registry->saveAdjacency(neighbors);
	EXPECT_GT(blobId, 0);

	auto loadedNeighbors = registry->loadAdjacency(blobId);
	EXPECT_EQ(loadedNeighbors.size(), neighbors.size());
	for (size_t i = 0; i < neighbors.size(); ++i) {
		EXPECT_EQ(loadedNeighbors[i], neighbors[i]);
	}
}

TEST_F(RegistryTest, LoadAdjacency_EmptyBlobId) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "idx10");
	VectorIndexConfig cfg;
	cfg.dimension = 16;
	registry->updateConfig(cfg);

	// Loading blob ID 0 should return empty adjacency list
	auto neighbors = registry->loadAdjacency(0);
	EXPECT_TRUE(neighbors.empty());
}

TEST_F(RegistryTest, GetAllNodeIds_ZeroMappingIndex) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "idx11");
	VectorIndexConfig cfg;
	cfg.dimension = 16;
	registry->updateConfig(cfg);

	// Without a mapping index, getAllNodeIds should return empty
	auto ids = registry->getAllNodeIds(100);
	EXPECT_TRUE(ids.empty());
}
