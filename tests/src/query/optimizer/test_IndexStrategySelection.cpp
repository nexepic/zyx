/**
 * @file test_IndexStrategySelection.cpp
 * @brief Coverage tests for optimizer index-selection paths.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/optimizer/CostModel.hpp"
#include "graph/query/optimizer/Optimizer.hpp"
#include "graph/query/optimizer/rules/EnhancedIndexSelectionRule.hpp"
#include "graph/query/optimizer/rules/IndexPushdownRule.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query;
using namespace graph::query::logical;
using namespace graph::query::optimizer;
using namespace graph::query::optimizer::rules;

namespace {

Statistics makeStats() {
	Statistics stats;
	stats.totalNodeCount = 1000;

	LabelStatistics person;
	person.label = "Person";
	person.nodeCount = 100;

	PropertyStatistics age;
	age.distinctValueCount = 50;
	person.properties["age"] = age;

	PropertyStatistics city;
	city.distinctValueCount = 80;
	person.properties["city"] = city;

	PropertyStatistics score;
	score.distinctValueCount = 100;
	person.properties["score"] = score;

	stats.labelStats["Person"] = person;
	return stats;
}

} // namespace

class OptimizerIndexStrategyTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<indexes::IndexManager> im;
	fs::path dbPath;

	void SetUp() override {
		auto uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() / ("test_optimizer_index_" + boost::uuids::to_string(uuid) + ".db");
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
		if (fs::exists(dbPath)) {
			fs::remove(dbPath);
		}
	}
};

TEST_F(OptimizerIndexStrategyTest, IndexPushdownRuleCoversCompositeRangeLabelAndFullFallback) {
	ASSERT_TRUE(im->createIndex("idx_person_label", "node", "Person", ""));
	ASSERT_TRUE(im->createIndex("idx_person_age", "node", "Person", "age"));
	ASSERT_TRUE(im->createIndex("idx_person_score", "node", "Person", "score"));
	ASSERT_TRUE(im->createCompositeIndex("idx_person_city_age", "node", "Person", {"city", "age"}));

	IndexPushdownRule rule(im);

	CompositeEqualityPredicate comp;
	comp.keys = {"city", "age"};
	comp.values = {PropertyValue("A"), PropertyValue(int64_t(30))};
	auto compositeCfg = rule.apply("n", {"Person"}, "age", PropertyValue(int64_t(30)), {}, comp);
	EXPECT_EQ(compositeCfg.type, execution::ScanType::COMPOSITE_SCAN);
	EXPECT_EQ(compositeCfg.compositeKeys, std::vector<std::string>({"city", "age"}));
	ASSERT_EQ(compositeCfg.compositeValues.size(), 2UL);

	auto propertyCfg = rule.apply("n", {"Person"}, "age", PropertyValue(int64_t(21)));
	EXPECT_EQ(propertyCfg.type, execution::ScanType::PROPERTY_SCAN);
	EXPECT_EQ(propertyCfg.indexKey, "age");

	RangePredicate rp;
	rp.key = "score";
	rp.minValue = PropertyValue(int64_t(10));
	rp.maxValue = PropertyValue(int64_t(20));
	rp.minInclusive = false;
	rp.maxInclusive = true;
	auto rangeCfg = rule.apply("n", {"Person"}, "", PropertyValue(), {rp});
	EXPECT_EQ(rangeCfg.type, execution::ScanType::RANGE_SCAN);
	EXPECT_EQ(rangeCfg.indexKey, "score");
	EXPECT_FALSE(rangeCfg.minInclusive);
	EXPECT_TRUE(rangeCfg.maxInclusive);

	auto labelCfg = rule.apply("n", {"Person"}, "", PropertyValue());
	EXPECT_EQ(labelCfg.type, execution::ScanType::LABEL_SCAN);

	ASSERT_TRUE(im->dropIndexByName("idx_person_label"));
	auto fullCfg = rule.apply("n", {"Person"}, "", PropertyValue());
	EXPECT_EQ(fullCfg.type, execution::ScanType::FULL_SCAN);
}

TEST_F(OptimizerIndexStrategyTest, EnhancedIndexSelectionRuleCoversLabelPropertyRangeAndComposite) {
	ASSERT_TRUE(im->createIndex("idx_person_label", "node", "Person", ""));
	ASSERT_TRUE(im->createIndex("idx_person_age", "node", "Person", "age"));
	ASSERT_TRUE(im->createIndex("idx_person_score", "node", "Person", "score"));
	ASSERT_TRUE(im->createIndex("idx_person_city", "node", "Person", "city"));
	ASSERT_TRUE(im->createCompositeIndex("idx_person_city_age", "node", "Person", {"city", "age"}));

	EnhancedIndexSelectionRule rule(im);
	EXPECT_EQ(rule.getName(), "EnhancedIndexSelectionRule");

	const Statistics stats = makeStats();

	auto labelScan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	{
		std::unique_ptr<LogicalOperator> plan = std::move(labelScan);
		auto optimized = rule.apply(std::move(plan), stats);
		auto *scan = dynamic_cast<LogicalNodeScan *>(optimized.get());
		ASSERT_NE(scan, nullptr);
		EXPECT_EQ(scan->getPreferredScanType(), execution::ScanType::LABEL_SCAN);
	}

	auto propertyScan = std::make_unique<LogicalNodeScan>(
		"n",
		std::vector<std::string>{"Person"},
		std::vector<std::pair<std::string, PropertyValue>>{{"age", PropertyValue(int64_t(30))}});
	{
		std::unique_ptr<LogicalOperator> plan = std::move(propertyScan);
		auto optimized = rule.apply(std::move(plan), stats);
		auto *scan = dynamic_cast<LogicalNodeScan *>(optimized.get());
		ASSERT_NE(scan, nullptr);
		EXPECT_EQ(scan->getPreferredScanType(), execution::ScanType::PROPERTY_SCAN);
	}

	auto rangeScan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	rangeScan->setRangePredicates({RangePredicate{"score", PropertyValue(int64_t(1)), PropertyValue(int64_t(10)), true, true}});
	{
		std::unique_ptr<LogicalOperator> plan = std::move(rangeScan);
		auto optimized = rule.apply(std::move(plan), stats);
		auto *scan = dynamic_cast<LogicalNodeScan *>(optimized.get());
		ASSERT_NE(scan, nullptr);
		EXPECT_EQ(scan->getPreferredScanType(), execution::ScanType::RANGE_SCAN);
	}

	auto compositeScan = std::make_unique<LogicalNodeScan>(
		"n",
		std::vector<std::string>{"Person"},
		std::vector<std::pair<std::string, PropertyValue>>{
			{"city", PropertyValue("A")},
			{"age", PropertyValue(int64_t(30))}});
	{
		std::unique_ptr<LogicalOperator> plan = std::move(compositeScan);
		auto optimized = rule.apply(std::move(plan), stats);
		auto *scan = dynamic_cast<LogicalNodeScan *>(optimized.get());
		ASSERT_NE(scan, nullptr);
		EXPECT_TRUE(scan->getCompositeEquality().has_value());
		EXPECT_EQ(scan->getPreferredScanType(), execution::ScanType::COMPOSITE_SCAN);
	}
}

TEST_F(OptimizerIndexStrategyTest, OptimizerCoversStatisticsCollectorAndLegacyNodeScanPath) {
	ASSERT_TRUE(im->createIndex("idx_person_age", "node", "Person", "age"));

	Optimizer opt(im, dm);
	EXPECT_NE(opt.getStatisticsCollector(), nullptr);
	EXPECT_NO_THROW(opt.invalidateStatistics());

	auto plan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto optimized = opt.optimize(std::move(plan));
	ASSERT_NE(optimized, nullptr);

	auto cfg = opt.optimizeNodeScan("n", {"Person"}, "age", PropertyValue(int64_t(18)));
	EXPECT_EQ(cfg.type, execution::ScanType::PROPERTY_SCAN);
}

TEST(OptimizerCostModelAdditionalTest, RangeCompositeAndCrossJoinCardinality) {
	const Statistics stats = makeStats();

	EXPECT_DOUBLE_EQ(
		CostModel::rangeIndexCost(stats, "Person", "age"),
		100.0 * 0.3 * CostModel::INDEX_LOOKUP_COST);

	EXPECT_DOUBLE_EQ(
		CostModel::compositeIndexCost(stats, "Person", 2),
		100.0 * 0.01 * CostModel::INDEX_LOOKUP_COST);

	EXPECT_DOUBLE_EQ(CostModel::crossJoinCardinality(3.0, 4.0), 12.0);
}
