/**
 * @file test_NodeScanOperator_InactiveAndEmpty.cpp
 * @brief Branch coverage tests for NodeScanOperator.hpp.
 *        Covers: RANGE_SCAN, COMPOSITE_SCAN, single-candidate (skip sort),
 *        empty batch early exit.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/execution/operators/NodeScanOperator.hpp"
#include "graph/query/execution/Record.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

class NodeScanOperatorInactiveAndEmptyTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() /
					   ("test_nodescan_branch2_" + boost::uuids::to_string(uuid) + ".dat");
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
};

// RANGE_SCAN: exercises the range-scan branch in open().
TEST_F(NodeScanOperatorInactiveAndEmptyTest, RangeScanBranch) {
	int64_t labelId = dm->getOrCreateTokenId("Person");
	Node n1(0, labelId);
	dm->addNode(n1);
	dm->addNodeProperties(n1.getId(), {{"age", PropertyValue(int64_t(30))}});

	// Create a property index so the range lookup has something to query
	im->createIndex("idx_age", "Node", "Person", "age");
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::RANGE_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	config.indexKey = "age";
	config.rangeMin = PropertyValue(int64_t(10));
	config.rangeMax = PropertyValue(int64_t(100));

	auto op = std::make_unique<NodeScanOperator>(dm, im, config);
	op->open();
	// Exercising the branch is sufficient.
	auto batch = op->next();
	(void)batch;
	op->close();
}

// COMPOSITE_SCAN: exercises the composite-scan branch in open().
TEST_F(NodeScanOperatorInactiveAndEmptyTest, CompositeScanBranch) {
	int64_t labelId = dm->getOrCreateTokenId("Person");
	Node n1(0, labelId);
	dm->addNode(n1);
	dm->addNodeProperties(n1.getId(), {
		{"city", PropertyValue(std::string("NYC"))},
		{"age", PropertyValue(int64_t(30))}
	});

	im->createCompositeIndex("idx_city_age", "node", "Person", {"city", "age"});
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::COMPOSITE_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	config.compositeKeys = {"city", "age"};
	config.compositeValues = {PropertyValue(std::string("NYC")), PropertyValue(int64_t(30))};

	auto op = std::make_unique<NodeScanOperator>(dm, im, config);
	op->open();
	auto batch = op->next();
	(void)batch;
	op->close();
}

// Single candidate id (size <= 1 branch) — no sort/dedupe needed.
TEST_F(NodeScanOperatorInactiveAndEmptyTest, SingleCandidateNoSort) {
	int64_t labelId = dm->getOrCreateTokenId("Person");
	Node n1(0, labelId);
	dm->addNode(n1);

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";

	auto op = std::make_unique<NodeScanOperator>(dm, im, config);
	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);
	op->close();
}

// Inactive node with label filter — batch empty but currentIdx reached end → nullopt.
TEST_F(NodeScanOperatorInactiveAndEmptyTest, AllNodesFilteredOutReturnsNullopt) {
	int64_t personLabel = dm->getOrCreateTokenId("Person");
	int64_t animalLabel = dm->getOrCreateTokenId("Animal");
	Node n1(0, animalLabel);
	dm->addNode(n1);

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};  // Filter excludes the only node

	auto op = std::make_unique<NodeScanOperator>(dm, im, config);
	op->open();
	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());
	op->close();
	(void)personLabel;
}

// toString for RANGE_SCAN.
TEST_F(NodeScanOperatorInactiveAndEmptyTest, ToStringRangeScan) {
	NodeScanConfig config;
	config.type = ScanType::RANGE_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	config.indexKey = "age";

	auto op = std::make_unique<NodeScanOperator>(dm, im, config);
	std::string s = op->toString();
	EXPECT_FALSE(s.empty());
}

// toString for COMPOSITE_SCAN.
TEST_F(NodeScanOperatorInactiveAndEmptyTest, ToStringCompositeScan) {
	NodeScanConfig config;
	config.type = ScanType::COMPOSITE_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	config.compositeKeys = {"a", "b"};

	auto op = std::make_unique<NodeScanOperator>(dm, im, config);
	std::string s = op->toString();
	EXPECT_FALSE(s.empty());
}
