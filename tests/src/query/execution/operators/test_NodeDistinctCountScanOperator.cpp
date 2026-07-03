#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <unordered_map>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/operators/NodeDistinctCountScanOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

class NodeDistinctCountScanOperatorTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	fs::path testFilePath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() /
					   ("test_node_distinct_count_scan_path_" + boost::uuids::to_string(uuid) + ".dat");
		db = std::make_unique<Database>(testFilePath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		im = db->getQueryEngine()->getIndexManager();
		debug::PerfTrace::reset();
		debug::PerfTrace::setEnabled(false);
	}

	void TearDown() override {
		if (db) {
			db->close();
		}
		db.reset();
		std::error_code ec;
		fs::remove(testFilePath, ec);
		debug::PerfTrace::reset();
		debug::PerfTrace::setEnabled(false);
	}

	int64_t addLabeledNode(const std::vector<std::string> &labels,
						   const std::unordered_map<std::string, PropertyValue> &props = {}) {
		Node node(0, dm->getOrCreateTokenId(labels.front()));
		for (size_t i = 1; i < labels.size(); ++i) {
			node.addLabelId(dm->getOrCreateTokenId(labels[i]));
		}
		dm->addNode(node);
		if (!props.empty()) {
			dm->addNodeProperties(node.getId(), props);
		}
		return node.getId();
	}

	int64_t addPerson(const std::unordered_map<std::string, PropertyValue> &props = {}) {
		return addLabeledNode({"Person"}, props);
	}

	static int64_t readCount(const RecordBatch &batch, const std::string &alias = "count") {
		EXPECT_EQ(batch.size(), 1U);
		auto value = batch[0].getValue(alias);
		EXPECT_TRUE(value.has_value());
		return std::get<int64_t>(value->getVariant());
	}
};

TEST_F(NodeDistinctCountScanOperatorTest, CountsDistinctSelectedPropertyValues) {
	static constexpr size_t kNodeCount = 300;
	for (size_t i = 0; i < kNodeCount; ++i) {
		addPerson({{"country", PropertyValue(i % 2 == 0 ? "CN" : "US")}});
	}
	addPerson({{"country", PropertyValue(std::monostate{})}});
	addPerson();
	addLabeledNode({"Animal"}, {{"country", PropertyValue("CN")}});
	db->getStorage()->flush();
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"country"};
	requirements.countOnly = true;

	NodeDistinctCountScanOperator op(dm, im, config, requirements, {}, "country", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 2);
	EXPECT_FALSE(op.next().has_value());
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.distinct_count"));
	EXPECT_TRUE(snapshot.contains("node_scan.load_node_metadata"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_properties"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_nodes"));
}

TEST_F(NodeDistinctCountScanOperatorTest, SkipsRedundantChecksForLabelIndexCandidates) {
	ASSERT_TRUE(im->createIndex("idx_person_label_distinct_operator", "node", "", ""));
	for (int64_t i = 0; i < 300; ++i) {
		addPerson({{"country", PropertyValue(i % 2 == 0 ? "CN" : "US")}});
	}
	addLabeledNode({"Animal"}, {{"country", PropertyValue("JP")}});
	db->getStorage()->flush();
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	NodeScanConfig config;
	config.type = ScanType::LABEL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"country"};
	requirements.countOnly = true;

	NodeDistinctCountScanOperator op(dm, im, config, requirements, {}, "country", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 2);
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.distinct_count"));
	EXPECT_FALSE(snapshot.contains("node_scan.label_check"));
}

TEST_F(NodeDistinctCountScanOperatorTest, RepeatsDistinctCountsWithoutResultCache) {
	for (int64_t i = 0; i < 300; ++i) {
		addPerson({{"country", PropertyValue(i % 2 == 0 ? "CN" : "US")}});
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"country"};
	requirements.countOnly = true;

	NodeDistinctCountScanOperator first(dm, im, config, requirements, {}, "country", "count");
	first.open();
	auto firstBatch = first.next();
	ASSERT_TRUE(firstBatch.has_value());
	EXPECT_EQ(readCount(*firstBatch), 2);

	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();
	NodeDistinctCountScanOperator second(dm, im, config, requirements, {}, "country", "count");
	second.open();
	auto secondBatch = second.next();
	ASSERT_TRUE(secondBatch.has_value());
	EXPECT_EQ(readCount(*secondBatch), 2);
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.distinct_count"));
	EXPECT_TRUE(snapshot.contains("node_scan.load_node_metadata"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_properties"));
	EXPECT_FALSE(snapshot.contains("node_scan.distinct_count_cache"));
}

TEST_F(NodeDistinctCountScanOperatorTest, AppliesResidualPropertyPredicatesBeforeDistinctCount) {
	for (int64_t i = 0; i < 300; ++i) {
		addPerson({{"country", PropertyValue(i % 3 == 0 ? "CN" : "US")}, {"age", PropertyValue(i)}});
	}
	addPerson({{"country", PropertyValue(std::monostate{})}, {"age", PropertyValue(int64_t{1})}});
	addPerson({{"age", PropertyValue(int64_t{2})}});
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"country", "age"};
	requirements.countOnly = true;

	VectorizedPropertyPredicate predicate;
	predicate.variable = "n";
	predicate.propertyKey = "age";
	predicate.op = VectorPredicateOp::VPO_LT;
	predicate.value = PropertyValue(int64_t{3});

	NodeDistinctCountScanOperator op(dm, im, config, requirements, {predicate}, "country", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 2);
}

TEST_F(NodeDistinctCountScanOperatorTest, ResidualPredicateFallbackHandlesMissingDistinctColumn) {
	for (int64_t i = 0; i < 300; ++i) {
		addPerson({{"age", PropertyValue(i)}});
	}
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"age"};
	requirements.countOnly = true;

	VectorizedPropertyPredicate predicate;
	predicate.variable = "n";
	predicate.propertyKey = "age";
	predicate.op = VectorPredicateOp::VPO_GE;
	predicate.value = PropertyValue(int64_t{0});

	NodeDistinctCountScanOperator op(dm, im, config, requirements, {predicate}, "missing", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 0);
}

TEST_F(NodeDistinctCountScanOperatorTest, MetadataPathReturnsZeroWhenNoRowsHavePropertyStorage) {
	for (int64_t i = 0; i < 32; ++i) {
		addPerson();
	}
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"missing"};
	requirements.countOnly = true;

	NodeDistinctCountScanOperator op(dm, im, config, requirements, {}, "missing", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 0);
}

TEST_F(NodeDistinctCountScanOperatorTest, DistinguishesTypedValuesWithoutStringifying) {
	addPerson({{"code", PropertyValue(int64_t{1})}});
	addPerson({{"code", PropertyValue(std::string("1"))}});
	addPerson({{"code", PropertyValue(int64_t{1})}});
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"code"};
	requirements.countOnly = true;

	NodeDistinctCountScanOperator op(dm, im, config, requirements, {}, "code", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 2);
}

TEST_F(NodeDistinctCountScanOperatorTest, MetadataPathCountsTypedScalarDistinctValues) {
	addPerson({{"code", PropertyValue(false)}});
	addPerson({{"code", PropertyValue(true)}});
	addPerson({{"code", PropertyValue(1.5)}});
	addPerson({{"code", PropertyValue(TemporalDate{42})}});
	addPerson({{"code", PropertyValue(TemporalDateTime{84})}});
	addPerson({{"code", PropertyValue(TemporalDuration{0, 1, 2})}});
	addPerson({{"code", PropertyValue(TemporalDuration{0, 1, 2})}});
	addPerson({{"code", PropertyValue(std::monostate{})}});
	addPerson();
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"code"};
	requirements.countOnly = true;

	NodeDistinctCountScanOperator op(dm, im, config, requirements, {}, "code", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 6);
}

TEST_F(NodeDistinctCountScanOperatorTest, CloseOpenAllowsReExecution) {
	addPerson({{"country", PropertyValue("CN")}});
	addPerson({{"country", PropertyValue("US")}});

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"country"};
	requirements.countOnly = true;

	NodeDistinctCountScanOperator op(dm, im, config, requirements, {}, "country", "count");
	op.open();
	ASSERT_TRUE(op.next().has_value());
	op.close();

	addPerson({{"country", PropertyValue("JP")}});
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 3);
	EXPECT_EQ(op.getOutputVariables(), (std::vector<std::string>{"count"}));
	EXPECT_NE(op.toString().find("NodeDistinctCountScan"), std::string::npos);
}

TEST_F(NodeDistinctCountScanOperatorTest, CountsBlobBackedDistinctValuesFromMetadataFallback) {
	const std::string cn(600, 'c');
	const std::string us(600, 'u');
	for (int64_t i = 0; i < 160; ++i) {
		addPerson({{"payload", PropertyValue(i % 2 == 0 ? cn : us)}});
	}
	addPerson();
	addPerson({{"payload", PropertyValue(std::monostate{})}});
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"payload"};
	requirements.countOnly = true;

	NodeDistinctCountScanOperator op(dm, im, config, requirements, {}, "payload", "count");
	op.open();
	auto batch = op.next();
	const auto snapshot = debug::PerfTrace::snapshotAndReset();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 2);
}

TEST_F(NodeDistinctCountScanOperatorTest, BlobFallbackSkipsMissingAndNullDistinctValues) {
	const std::string largeOther(600, 'o');
	const std::string largeValue(600, 'v');
	addPerson({{"payload", PropertyValue(largeValue)}});
	addPerson({{"other", PropertyValue(largeOther)}});
	addPerson({{"payload", PropertyValue()}, {"other", PropertyValue(largeOther)}});
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"payload"};
	requirements.countOnly = true;

	NodeDistinctCountScanOperator op(dm, im, config, requirements, {}, "payload", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 1);
}

TEST_F(NodeDistinctCountScanOperatorTest, BlobFallbackIgnoresRowsWithoutDistinctProperty) {
	const std::string largeOther(600, 'o');
	addPerson({{"other", PropertyValue(largeOther)}});
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"other"};
	requirements.countOnly = true;

	NodeDistinctCountScanOperator op(dm, im, config, requirements, {}, "payload", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 0);
}
