/**
 * @file test_DiskANNIndex_ThreadPoolSearch.cpp
 * @brief Ensures DiskANN search runs thread-pool parallel branches on large candidate sets.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Database.hpp"
#include "graph/vector/VectorIndexRegistry.hpp"
#include "graph/vector/index/DiskANNIndex.hpp"

using namespace graph::vector;

class DiskANNThreadPoolSearchTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dbPath = std::filesystem::temp_directory_path() / ("ann_tp_" + to_string(uuid) + ".zyx");
		database = std::make_unique<graph::Database>(dbPath.string());
		database->open();
		dm = database->getStorage()->getDataManager();
		sm = database->getStorage()->getSystemStateManager();
	}

	void TearDown() override {
		dm.reset();
		sm.reset();
		database->close();
		database.reset();
		std::error_code ec;
		std::filesystem::remove(dbPath, ec);
	}

	std::filesystem::path dbPath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::DataManager> dm;
	std::shared_ptr<graph::storage::state::SystemStateManager> sm;
};

TEST_F(DiskANNThreadPoolSearchTest, SearchUsesParallelLoadAndDistancePaths) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "tp_idx");
	VectorIndexConfig vCfg;
	vCfg.dimension = 4;
	registry->updateConfig(vCfg);

	DiskANNConfig cfg;
	cfg.dim = 4;
	cfg.beamWidth = 100;
	DiskANNIndex index(registry, cfg);

	for (int i = 1; i <= 64; ++i) {
		index.insert(i, {static_cast<float>(i), 1.0f, 0.0f, 0.0f});
	}

	graph::concurrent::ThreadPool pool(4);
	index.setThreadPool(&pool);

	const auto results = index.search({63.0f, 1.0f, 0.0f, 0.0f}, 10);
	ASSERT_FALSE(results.empty());
	EXPECT_LE(results.size(), 10UL);
}
