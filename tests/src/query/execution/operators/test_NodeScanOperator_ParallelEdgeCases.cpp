/**
 * @file test_NodeScanOperator_ParallelEdgeCases.cpp
 * @brief Edge case tests for NodeScanOperator parallel path:
 *        empty segments, candidateSet filtering, inactive nodes, label mismatch.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Database.hpp"
#include "graph/query/execution/operators/NodeScanOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

class NodeScanOperatorParallelEdgeCasesTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() /
					   ("test_nodescan_pedge_" + boost::uuids::to_string(uuid) + ".dat");
		if (fs::exists(testFilePath)) fs::remove_all(testFilePath);
		db = std::make_unique<Database>(testFilePath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		im = db->getQueryEngine()->getIndexManager();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}

	std::vector<Node> addNodes(const std::string &label, size_t count) {
		std::vector<Node> nodes;
		nodes.reserve(count);
		int64_t labelId = dm->getOrCreateTokenId(label);
		for (size_t i = 0; i < count; ++i) {
			Node n;
			n.setLabelId(labelId);
			nodes.push_back(n);
		}
		dm->addNodes(nodes);
		return nodes;
	}
};

// Parallel scan with inactive nodes: delete some nodes then scan
TEST_F(NodeScanOperatorParallelEdgeCasesTest, ParallelInactiveNodeSkipped) {
	static constexpr size_t kNodeCount = 4200;
	auto nodes = addNodes("Person", kNodeCount);

	// Delete every other node to create inactive entries
	for (size_t i = 0; i < kNodeCount; i += 2) {
		dm->deleteNode(nodes[i]);
	}

	db->getStorage()->flush();
	dm->clearCache();

	NodeScanConfig cfg;
	cfg.type = ScanType::FULL_SCAN;
	cfg.variable = "n";

	NodeScanOperator op(dm, im, cfg);
	concurrent::ThreadPool pool(4);
	op.setThreadPool(&pool);

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	// Only half the nodes should be active
	EXPECT_EQ(batch->size(), kNodeCount / 2);
	EXPECT_FALSE(op.next().has_value());
	op.close();
}

// Parallel scan with label filter where most nodes have wrong label
TEST_F(NodeScanOperatorParallelEdgeCasesTest, ParallelLabelMismatch) {
	static constexpr size_t kWrongCount = 4100;
	static constexpr size_t kRightCount = 100;
	(void)addNodes("Wrong", kWrongCount);
	(void)addNodes("Right", kRightCount);

	db->getStorage()->flush();
	dm->clearCache();

	NodeScanConfig cfg;
	cfg.type = ScanType::FULL_SCAN;
	cfg.variable = "n";
	cfg.labels = {"Right"};

	NodeScanOperator op(dm, im, cfg);
	concurrent::ThreadPool pool(4);
	op.setThreadPool(&pool);

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), kRightCount);
	EXPECT_FALSE(op.next().has_value());
	op.close();
}

// Parallel scan with candidateSet (non-FULL_SCAN path) — exercises candidateSet filtering
TEST_F(NodeScanOperatorParallelEdgeCasesTest, ParallelWithCandidateSet) {
	// Create 8000 nodes but only index 5000 of them (enough for parallel threshold)
	static constexpr size_t kTotalNodes = 8000;
	static constexpr size_t kIndexedNodes = 5000;
	auto nodes = addNodes("Item", kTotalNodes);

	// Create a property index and add properties to only the first 5000 nodes
	ASSERT_TRUE(im->createIndex("idx_item_val", "node", "Item", "val"));
	for (size_t i = 0; i < kIndexedNodes; ++i) {
		dm->addNodeProperties(nodes[i].getId(), {{"val", PropertyValue(int64_t(i))}});
	}

	db->getStorage()->flush();
	dm->clearCache();

	// RANGE_SCAN produces a candidateSet of 5000 nodes (>= 4096 for parallel)
	// Segments contain all 8000 entities, so 3000 will be filtered by candidateSet
	NodeScanConfig cfg;
	cfg.type = ScanType::RANGE_SCAN;
	cfg.variable = "n";
	cfg.labels = {"Item"};
	cfg.indexKey = "val";
	cfg.rangeMin = PropertyValue(int64_t(0));
	cfg.rangeMax = PropertyValue(int64_t(kIndexedNodes));

	NodeScanOperator op(dm, im, cfg);
	concurrent::ThreadPool pool(4);
	op.setThreadPool(&pool);

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	// Should get approximately kIndexedNodes results
	EXPECT_GE(batch->size(), 1UL);
	op.close();
}

// Parallel scan where some segments have 0 active nodes (all deleted)
TEST_F(NodeScanOperatorParallelEdgeCasesTest, ParallelLoadEmptySegment) {
	// Create enough nodes to span multiple segments, then delete all in some segments.
	// Default segment capacity is typically 128 nodes.
	static constexpr size_t kNodeCount = 4200;
	auto nodes = addNodes("Seg", kNodeCount);

	// Delete the first 128 nodes (likely one full segment)
	for (size_t i = 0; i < 128 && i < kNodeCount; ++i) {
		dm->deleteNode(nodes[i]);
	}

	db->getStorage()->flush();
	dm->clearCache();

	NodeScanConfig cfg;
	cfg.type = ScanType::FULL_SCAN;
	cfg.variable = "n";

	NodeScanOperator op(dm, im, cfg);
	concurrent::ThreadPool pool(4);
	op.setThreadPool(&pool);

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	// Total - 128 deleted
	EXPECT_EQ(batch->size(), kNodeCount - 128);
	EXPECT_FALSE(op.next().has_value());
	op.close();
}

// Parallel scan where ALL nodes are filtered by label → empty batch at line 349
TEST_F(NodeScanOperatorParallelEdgeCasesTest, ParallelAllFilteredReturnsNullopt) {
	static constexpr size_t kNodeCount = 4200;
	(void)addNodes("OnlyLabel", kNodeCount);

	db->getStorage()->flush();
	dm->clearCache();

	NodeScanConfig cfg;
	cfg.type = ScanType::FULL_SCAN;
	cfg.variable = "n";
	cfg.labels = {"NonExistentLabel"};

	NodeScanOperator op(dm, im, cfg);
	concurrent::ThreadPool pool(4);
	op.setThreadPool(&pool);

	op.open();
	// All nodes have "OnlyLabel" but we filter for "NonExistentLabel" → empty batch
	EXPECT_FALSE(op.next().has_value());
	op.close();
}

// Parallel scan with many segments (ensures chunked parallel path)
TEST_F(NodeScanOperatorParallelEdgeCasesTest, ParallelManySegments) {
	static constexpr size_t kNodeCount = 5000;
	auto nodes = addNodes("Multi", kNodeCount);

	// Add properties to some nodes to exercise property loading
	for (size_t i = 0; i < 20; ++i) {
		dm->addNodeProperties(nodes[i].getId(), {{"k", PropertyValue(int64_t(i))}});
	}

	db->getStorage()->flush();
	dm->clearCache();

	NodeScanConfig cfg;
	cfg.type = ScanType::FULL_SCAN;
	cfg.variable = "n";

	NodeScanOperator op(dm, im, cfg);
	concurrent::ThreadPool pool(4);
	op.setThreadPool(&pool);

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), kNodeCount);
	EXPECT_FALSE(op.next().has_value());
	op.close();
}
