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
