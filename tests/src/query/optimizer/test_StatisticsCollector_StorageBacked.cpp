/**
 * @file test_StatisticsCollector_StorageBacked.cpp
 * @brief Storage-backed tests for StatisticsCollector.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <climits>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/query/optimizer/StatisticsCollector.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/LabelIndex.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::optimizer;

class StatisticsCollectorStorageTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath_ = fs::temp_directory_path() / ("test_stats_collector_" + boost::uuids::to_string(uuid) + ".dat");

		db_ = std::make_unique<Database>(testDbPath_.string());
		db_->open();

		// Ensure QueryEngine/IndexManager are initialized and attached as observers.
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
		if (fs::exists(testDbPath_)) {
			fs::remove(testDbPath_);
		}
	}

	Node createNode(const std::string &label,
	                const std::unordered_map<std::string, PropertyValue> &properties = {}) {
		Node node;
		node.setLabelId(dataManager_->getOrCreateLabelId(label));
		node.setProperties(properties);
		dataManager_->addNode(node);
		return node;
	}

	Edge createEdge(int64_t src, int64_t dst, const std::string &label) {
		Edge edge;
		edge.setSourceNodeId(src);
		edge.setTargetNodeId(dst);
		edge.setLabelId(dataManager_->getOrCreateLabelId(label));
		dataManager_->addEdge(edge);
		return edge;
	}

	fs::path testDbPath_;
	std::unique_ptr<Database> db_;
	std::shared_ptr<query::QueryEngine> queryEngine_;
	std::shared_ptr<storage::DataManager> dataManager_;
	std::shared_ptr<query::indexes::IndexManager> indexManager_;
};

TEST_F(StatisticsCollectorStorageTest, CollectCountsNodesAndEdges) {
	const auto n1 = createNode("Person");
	const auto n2 = createNode("Person");
	const auto n3 = createNode("Company");
	(void) createEdge(n1.getId(), n2.getId(), "KNOWS");
	(void) createEdge(n2.getId(), n3.getId(), "WORKS_AT");

	StatisticsCollector collector(dataManager_, indexManager_);
	const auto stats = collector.collect();

	EXPECT_EQ(stats.totalNodeCount, 3);
	EXPECT_EQ(stats.totalEdgeCount, 2);
}

TEST_F(StatisticsCollectorStorageTest, CollectLabelStatsTracksDistinctAndNullValues) {
	createNode("Person", {
		{"name", PropertyValue(std::string("Alice"))},
		{"age", PropertyValue(int64_t(30))},
		{"nickname", PropertyValue()}
	});
	createNode("Person", {
		{"name", PropertyValue(std::string("Bob"))},
		{"age", PropertyValue(int64_t(30))},
		{"nickname", PropertyValue()}
	});
	createNode("Person", {
		{"name", PropertyValue(std::string("Carol"))},
		{"age", PropertyValue(int64_t(40))},
		{"nickname", PropertyValue(std::string("C"))}
	});

	StatisticsCollector collector(dataManager_, indexManager_);
	const auto labelStats = collector.collectLabelStats("Person");

	ASSERT_EQ(labelStats.nodeCount, 3);
	ASSERT_TRUE(labelStats.properties.contains("age"));
	ASSERT_TRUE(labelStats.properties.contains("name"));
	ASSERT_TRUE(labelStats.properties.contains("nickname"));

	EXPECT_EQ(labelStats.properties.at("age").distinctValueCount, 2);
	EXPECT_EQ(labelStats.properties.at("name").distinctValueCount, 3);
	EXPECT_EQ(labelStats.properties.at("nickname").distinctValueCount, 1);
	EXPECT_EQ(labelStats.properties.at("nickname").nullCount, 2);
}

TEST_F(StatisticsCollectorStorageTest, CollectLabelStatsHandlesMissingLabelAndNullDataManager) {
	createNode("Person");
	createNode("Person");

	StatisticsCollector normalCollector(dataManager_, indexManager_);
	const auto missing = normalCollector.collectLabelStats("NoSuchLabel");
	EXPECT_EQ(missing.nodeCount, 0);
	EXPECT_TRUE(missing.properties.empty());

	StatisticsCollector nullDmCollector(nullptr, indexManager_);
	const auto noDmStats = nullDmCollector.collectLabelStats("Person");
	EXPECT_EQ(noDmStats.nodeCount, 2);
	EXPECT_TRUE(noDmStats.properties.empty());
}

TEST_F(StatisticsCollectorStorageTest, CollectLabelStatsUsesReservoirSamplingForLargeLabels) {
	const int64_t totalNodes = StatisticsCollector::MAX_SAMPLE_SIZE + 5;
	std::vector<Node> nodes;
	nodes.reserve(static_cast<size_t>(totalNodes));

	const int64_t labelId = dataManager_->getOrCreateLabelId("ReservoirLabel");
	for (int64_t i = 0; i < totalNodes; ++i) {
		Node node;
		node.setLabelId(labelId);
		nodes.push_back(node);
	}
	dataManager_->addNodes(nodes);

	StatisticsCollector collector(dataManager_, indexManager_);
	const auto labelStats = collector.collectLabelStats("ReservoirLabel");

	EXPECT_EQ(labelStats.nodeCount, totalNodes);
	EXPECT_TRUE(labelStats.properties.empty());
}
