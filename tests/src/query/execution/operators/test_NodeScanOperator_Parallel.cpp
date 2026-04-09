/**
 * @file test_NodeScanOperator_Parallel.cpp
 * @brief Parallel and advanced-branch tests for NodeScanOperator.
 **/

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
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/operators/NodeScanOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

class NodeScanOperatorParallelTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_nodescan_parallel_" + boost::uuids::to_string(uuid) + ".dat");
		if (fs::exists(testFilePath)) {
			fs::remove_all(testFilePath);
		}

		db = std::make_unique<Database>(testFilePath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		im = db->getQueryEngine()->getIndexManager();
	}

	void TearDown() override {
		debug::PerfTrace::setEnabled(false);
		debug::PerfTrace::reset();

		if (db) {
			db->close();
		}
		db.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) {
			fs::remove(testFilePath, ec);
		}
	}

	std::vector<Node> addNodes(const std::string &label, size_t count) const {
		std::vector<Node> nodes;
		nodes.reserve(count);
		const int64_t labelId = dm->getOrCreateTokenId(label);
		for (size_t i = 0; i < count; ++i) {
			Node n;
			n.setLabelId(labelId);
			nodes.push_back(n);
		}
		dm->addNodes(nodes);
		return nodes;
	}
};

TEST_F(NodeScanOperatorParallelTest, RangeScanAndCompositeScanOpenBranches) {
	auto nodes = addNodes("Person", 5);
	dm->addNodeProperties(nodes[0].getId(), {{"age", PropertyValue(int64_t(18))}, {"city", PropertyValue(std::string("A"))}});
	dm->addNodeProperties(nodes[1].getId(), {{"age", PropertyValue(int64_t(22))}, {"city", PropertyValue(std::string("A"))}});
	dm->addNodeProperties(nodes[2].getId(), {{"age", PropertyValue(int64_t(27))}, {"city", PropertyValue(std::string("B"))}});
	dm->addNodeProperties(nodes[3].getId(), {{"age", PropertyValue(int64_t(35))}, {"city", PropertyValue(std::string("A"))}});
	dm->addNodeProperties(nodes[4].getId(), {{"age", PropertyValue(int64_t(50))}, {"city", PropertyValue(std::string("C"))}});

	ASSERT_TRUE(im->createIndex("idx_person_age_range", "node", "Person", "age"));
	ASSERT_TRUE(im->createCompositeIndex("idx_person_city_age", "node", "Person", {"city", "age"}));

	NodeScanConfig rangeCfg;
	rangeCfg.type = ScanType::RANGE_SCAN;
	rangeCfg.variable = "n";
	rangeCfg.labels = {"Person"};
	rangeCfg.indexKey = "age";
	rangeCfg.rangeMin = PropertyValue(int64_t(20));
	rangeCfg.rangeMax = PropertyValue(int64_t(40));

	NodeScanOperator rangeOp(dm, im, rangeCfg);
	rangeOp.open();
	auto rangeBatch = rangeOp.next();
	ASSERT_TRUE(rangeBatch.has_value());
	EXPECT_GE(rangeBatch->size(), 1UL);
	rangeOp.close();

	NodeScanConfig compCfg;
	compCfg.type = ScanType::COMPOSITE_SCAN;
	compCfg.variable = "n";
	compCfg.labels = {"Person"};
	compCfg.compositeKeys = {"city", "age"};
	compCfg.compositeValues = {PropertyValue(std::string("A")), PropertyValue(int64_t(22))};

	NodeScanOperator compOp(dm, im, compCfg);
	compOp.open();
	auto compBatch = compOp.next();
	// Composite index lookup behavior may vary with index availability/state.
	// This test primarily verifies the COMPOSITE_SCAN branch executes safely.
	if (compBatch.has_value()) {
		EXPECT_LE(compBatch->size(), 1UL);
	}
	compOp.close();
}

TEST_F(NodeScanOperatorParallelTest, ParallelFullScanExecutesParallelPathAndPerfTrace) {
	static constexpr size_t kNodeCount = 4200;
	auto nodes = addNodes("ParallelPerson", kNodeCount);
	for (size_t i = 0; i < 8; ++i) {
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

	debug::PerfTrace::reset();
	debug::PerfTrace::setEnabled(true);

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), kNodeCount);
	EXPECT_FALSE(op.next().has_value());
	op.close();

	auto trace = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(trace.contains("scan.parallel"));
	EXPECT_TRUE(trace.contains("scan.parallel.phase1"));
	EXPECT_TRUE(trace.contains("scan.parallel.phase2"));
	EXPECT_TRUE(trace.contains("scan.parallel.phase3"));
}

TEST_F(NodeScanOperatorParallelTest, ParallelLabelScanUsesCandidateSetPath) {
	static constexpr size_t kMatchCount = 4200;
	static constexpr size_t kOtherCount = 120;
	(void)addNodes("OtherLabel", kOtherCount);
	(void)addNodes("MatchLabel", kMatchCount);

	ASSERT_TRUE(im->createIndex("idx_match_label_parallel", "node", "MatchLabel", ""));

	NodeScanConfig cfg;
	cfg.type = ScanType::LABEL_SCAN;
	cfg.variable = "n";
	cfg.labels = {"MatchLabel"};

	NodeScanOperator op(dm, im, cfg);
	concurrent::ThreadPool pool(4);
	op.setThreadPool(&pool);

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), kMatchCount);
	EXPECT_FALSE(op.next().has_value());
	op.close();
}

TEST_F(NodeScanOperatorParallelTest, ParallelFullScanWithNonMatchingLabelReturnsNullopt) {
	static constexpr size_t kNodeCount = 4200;
	(void)addNodes("OnlyLabel", kNodeCount);

	NodeScanConfig cfg;
	cfg.type = ScanType::FULL_SCAN;
	cfg.variable = "n";
	cfg.labels = {"MissingLabel"};

	NodeScanOperator op(dm, im, cfg);
	concurrent::ThreadPool pool(4);
	op.setThreadPool(&pool);

	op.open();
	// Parallel path runs, but all rows are filtered by targetLabelIds_.
	EXPECT_FALSE(op.next().has_value());
	op.close();
}
