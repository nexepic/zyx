/**
 * @file test_OperatorHeader_NullGuards.cpp
 * @brief Focused branch tests for header-only execution operators.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/query/execution/operators/CreateIndexOperator.hpp"
#include "graph/query/execution/operators/LimitOperator.hpp"
#include "graph/query/execution/operators/ProfileOperator.hpp"
#include "graph/query/execution/operators/ShowStatsOperator.hpp"
#include "graph/query/execution/operators/SkipOperator.hpp"
#include "graph/query/execution/operators/TraversalOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution::operators;

class OperatorHeaderDbFixture : public ::testing::Test {
protected:
	fs::path dbPath;
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() / ("test_op_headers_" + boost::uuids::to_string(uuid) + ".zyx");
		if (fs::exists(dbPath)) {
			fs::remove(dbPath);
		}

		db = std::make_unique<Database>(dbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		im = db->getQueryEngine()->getIndexManager();
	}

	void TearDown() override {
		if (db) {
			db->close();
		}
		db.reset();
		std::error_code ec;
		if (fs::exists(dbPath)) {
			fs::remove(dbPath, ec);
		}
	}
};

TEST(ProfileOperatorHeaderBranchTest, NullInnerOperatorCoversFallbackPaths) {
	ProfileOperator op(nullptr);

	op.open();
	EXPECT_FALSE(op.next().has_value());
	op.close();

	const auto vars = op.getOutputVariables();
	ASSERT_EQ(vars.size(), 3UL);
	EXPECT_EQ(vars[0], "phase");
	EXPECT_EQ(vars[1], "total_time_ms");
	EXPECT_EQ(vars[2], "calls");

	EXPECT_TRUE(op.getChildren().empty());
}

TEST(TraversalOperatorHeaderBranchTest, GetChildrenReturnsEmptyWhenChildMissing) {
	TraversalOperator op(nullptr, nullptr, "a", "e", "b", "KNOWS", "out");
	EXPECT_TRUE(op.getChildren().empty());
}

TEST(LimitOperatorHeaderBranchTest, OpenAndCloseWithoutChildCoverNullGuards) {
	LimitOperator op(nullptr, 5);
	EXPECT_NO_THROW(op.open());
	EXPECT_NO_THROW(op.close());
	ASSERT_EQ(op.getChildren().size(), 1UL);
	EXPECT_EQ(op.getChildren()[0], nullptr);
}

TEST(SkipOperatorHeaderBranchTest, OpenAndCloseWithoutChildCoverNullGuards) {
	SkipOperator op(nullptr, 3);
	EXPECT_NO_THROW(op.open());
	EXPECT_NO_THROW(op.close());
	ASSERT_EQ(op.getChildren().size(), 1UL);
	EXPECT_EQ(op.getChildren()[0], nullptr);
}

TEST_F(OperatorHeaderDbFixture, CreateIndexOperatorCompositePathAndMultiPropToString) {
	CreateIndexOperator op(
		im,
		"idx_person_name",
		"Person",
		std::vector<std::string>{"first", "last"});

	const auto desc = op.toString();
	EXPECT_NE(desc.find("first,last"), std::string::npos);

	op.open();
	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	ASSERT_EQ(batch1->size(), 1UL);
	EXPECT_TRUE((*batch1)[0].getValue("result").has_value());

	// executed_ guard branch
	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value());
	op.close();
}

TEST_F(OperatorHeaderDbFixture, ShowStatsOperatorCoversHitRateBranches) {
	auto &pool = dm->getPagePool();
	pool.clear();
	pool.resetStats();
	im->resetStats();

	// Make totalCache > 0 and totalIndex > 0.
	pool.putPage(123, std::vector<uint8_t>{0x01, 0x02});
	EXPECT_NE(pool.getPage(123), nullptr);       // hit
	EXPECT_EQ(pool.getPage(999999), nullptr);    // miss
	(void)im->findNodeIdsByLabel("Person");      // lookup count++

	ShowStatsOperator op(dm, im, ShowStatsOperator::CacheStats{0, 0});
	op.open();

	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 9UL);

	std::unordered_set<std::string> metrics;
	for (const auto &record : *batch) {
		auto metric = record.getValue("metric");
		ASSERT_TRUE(metric.has_value());
		metrics.insert(metric->toString());
	}

	EXPECT_EQ(metrics.count("page_pool_hit_rate"), 1UL);
	EXPECT_EQ(metrics.count("hit_rate"), 1UL);
	EXPECT_EQ(metrics.count("page_pool_hits"), 1UL);
	EXPECT_EQ(metrics.count("lookups"), 1UL);

	// executed_ guard branch
	EXPECT_FALSE(op.next().has_value());
	op.close();
}
