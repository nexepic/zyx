/**
 * @file test_StatisticsCollector_Sampling.cpp
 * @brief Focused branch tests for sampling paths in StatisticsCollector.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "graph/core/Database.hpp"
#include "graph/query/optimizer/StatisticsCollector.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/LabelIndex.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::optimizer;

class StatisticsCollectorSamplingTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath_ = fs::temp_directory_path() / ("test_stats_sampling_" + boost::uuids::to_string(uuid) + ".dat");

		db_ = std::make_unique<Database>(testDbPath_.string());
		db_->open();

		queryEngine_ = db_->getQueryEngine();
		dataManager_ = db_->getStorage()->getDataManager();
		indexManager_ = queryEngine_->getIndexManager();
		indexManager_->getNodeIndexManager()->getLabelIndex()->createIndex();
	}

	void TearDown() override {
		if (db_) {
			db_->close();
			db_.reset();
		}
		std::error_code ec;
		if (fs::exists(testDbPath_)) {
			fs::remove(testDbPath_, ec);
		}
	}

	fs::path testDbPath_;
	std::unique_ptr<Database> db_;
	std::shared_ptr<query::QueryEngine> queryEngine_;
	std::shared_ptr<storage::DataManager> dataManager_;
	std::shared_ptr<query::indexes::IndexManager> indexManager_;
};

TEST_F(StatisticsCollectorSamplingTest, ReservoirSamplingCoversReplacementAndSkipBranches) {
	const int64_t totalNodes = StatisticsCollector::MAX_SAMPLE_SIZE + 2000;
	const int64_t labelId = dataManager_->getOrCreateTokenId("ReservoirBranchLabel");

	std::vector<Node> nodes;
	nodes.reserve(static_cast<size_t>(totalNodes));
	for (int64_t i = 0; i < totalNodes; ++i) {
		Node n;
		n.setLabelId(labelId);
		nodes.push_back(n);
	}
	dataManager_->addNodes(nodes);

	StatisticsCollector collector(dataManager_, indexManager_);
	const auto stats = collector.collectLabelStats("ReservoirBranchLabel");

	EXPECT_EQ(stats.nodeCount, totalNodes);
	EXPECT_TRUE(stats.properties.empty());
}
