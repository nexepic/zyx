#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <unordered_map>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/operators/NodeTopKFastPathOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

class NodeTopKFastPathOperatorTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	fs::path testFilePath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath =
				fs::temp_directory_path() / ("test_node_topk_fast_path_" + boost::uuids::to_string(uuid) + ".dat");
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

	static std::string readString(const Record &record, const std::string &alias) {
		auto value = record.getValue(alias);
		EXPECT_TRUE(value.has_value());
		return value ? value->toString() : "";
	}
};

TEST_F(NodeTopKFastPathOperatorTest, ReturnsProjectedRowsInDescendingOrder) {
	static constexpr size_t kNodeCount = 300;
	for (size_t i = 0; i < kNodeCount; ++i) {
		addPerson({{"id", PropertyValue("user-" + std::to_string(i))},
				   {"score", PropertyValue(static_cast<int64_t>(i))}});
	}
	addLabeledNode({"Animal"}, {{"id", PropertyValue("animal")}, {"score", PropertyValue(int64_t{1000})}});
	db->getStorage()->flush();
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score"};

	NodeTopKFastPathOperator op(dm, im, config, requirements, {}, {{"id", "id"}}, "score", false, 3);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 3U);
	EXPECT_EQ(readString((*batch)[0], "id"), "user-299");
	EXPECT_EQ(readString((*batch)[1], "id"), "user-298");
	EXPECT_EQ(readString((*batch)[2], "id"), "user-297");
	EXPECT_FALSE(op.next().has_value());
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.topk"));
	EXPECT_TRUE(snapshot.contains("node_scan.load_node_metadata"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_properties"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_nodes"));
}

TEST_F(NodeTopKFastPathOperatorTest, SkipsRedundantChecksForLabelIndexCandidates) {
	ASSERT_TRUE(im->createIndex("idx_person_label_topk_operator", "node", "", ""));
	for (int64_t i = 0; i < 300; ++i) {
		addPerson({{"id", PropertyValue("user-" + std::to_string(i))}, {"score", PropertyValue(i)}});
	}
	addLabeledNode({"Animal"}, {{"id", PropertyValue("animal")}, {"score", PropertyValue(int64_t{1000})}});
	db->getStorage()->flush();
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	NodeScanConfig config;
	config.type = ScanType::LABEL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score"};

	NodeTopKFastPathOperator op(dm, im, config, requirements, {}, {{"id", "id"}}, "score", false, 1);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(readString((*batch)[0], "id"), "user-299");
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.topk"));
	EXPECT_FALSE(snapshot.contains("node_scan.label_check"));
}

TEST_F(NodeTopKFastPathOperatorTest, AppliesResidualPredicateBeforeRanking) {
	for (int64_t i = 0; i < 10; ++i) {
		addPerson({{"id", PropertyValue("user-" + std::to_string(i))},
				   {"score", PropertyValue(i)},
				   {"country", PropertyValue(i >= 5 ? "CN" : "US")}});
	}

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score", "country"};

	VectorizedPropertyPredicate predicate;
	predicate.variable = "u";
	predicate.propertyKey = "country";
	predicate.op = VectorPredicateOp::VPO_EQ;
	predicate.value = PropertyValue("US");

	NodeTopKFastPathOperator op(dm, im, config, requirements, {predicate}, {{"id", "id"}}, "score", false, 2);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 2U);
	EXPECT_EQ(readString((*batch)[0], "id"), "user-4");
	EXPECT_EQ(readString((*batch)[1], "id"), "user-3");
}

TEST_F(NodeTopKFastPathOperatorTest, LoadsOnlyRetainedProjectionProperties) {
	for (int64_t i = 0; i < 20; ++i) {
		addPerson({{"id", PropertyValue("user-" + std::to_string(i))},
				   {"score", PropertyValue(i)},
				   {"city", PropertyValue(i == 19 ? "Shanghai" : "Seattle")}});
	}

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score", "city", "missing"};

	NodeTopKFastPathOperator op(dm, im, config, requirements, {},
								{{"city", "city"}, {"score", "score"}, {"missing", "missing"}}, "score", false, 1);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(readString((*batch)[0], "city"), "Shanghai");
	EXPECT_EQ(readString((*batch)[0], "score"), "19");
	EXPECT_EQ(readString((*batch)[0], "missing"), "null");
}

TEST_F(NodeTopKFastPathOperatorTest, RepeatsTopKWithoutResultCache) {
	for (int64_t i = 0; i < 200; ++i) {
		addPerson({{"id", PropertyValue("user-" + std::to_string(i))}, {"score", PropertyValue(i)}});
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score"};

	NodeTopKFastPathOperator first(dm, im, config, requirements, {}, {{"id", "id"}}, "score", false, 3);
	first.open();
	auto firstBatch = first.next();
	ASSERT_TRUE(firstBatch.has_value());
	ASSERT_EQ(firstBatch->size(), 3U);
	EXPECT_EQ(readString((*firstBatch)[0], "id"), "user-199");

	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();
	NodeTopKFastPathOperator second(dm, im, config, requirements, {}, {{"id", "id"}}, "score", false, 3);
	second.open();
	auto secondBatch = second.next();
	ASSERT_TRUE(secondBatch.has_value());
	ASSERT_EQ(secondBatch->size(), 3U);
	EXPECT_EQ(readString((*secondBatch)[0], "id"), "user-199");
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.topk"));
	EXPECT_TRUE(snapshot.contains("node_scan.load_node_metadata"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_properties"));
	EXPECT_FALSE(snapshot.contains("node_scan.topk_cache"));
}

TEST_F(NodeTopKFastPathOperatorTest, UsesMetadataSortKeyPathWithoutColumnMaterialization) {
	for (int64_t i = 0; i < 200; ++i) {
		addPerson({{"score", PropertyValue(i)}});
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"score"};

	NodeTopKFastPathOperator op(dm, im, config, requirements, {}, {{"score", "score"}}, "score", false, 2);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 2U);
	EXPECT_EQ(readString((*batch)[0], "score"), "199");
	EXPECT_EQ(readString((*batch)[1], "score"), "198");
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.topk"));
	EXPECT_TRUE(snapshot.contains("node_scan.load_node_metadata"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_properties"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_property_entities"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_nodes"));
}

TEST_F(NodeTopKFastPathOperatorTest, ReturnsMultipleBatchesWhenLimitExceedsDefaultBatch) {
	const size_t rowCount = PhysicalOperator::DEFAULT_BATCH_SIZE + 5;
	for (size_t i = 0; i < rowCount; ++i) {
		addPerson({{"id", PropertyValue("user-" + std::to_string(i))},
				   {"score", PropertyValue(static_cast<int64_t>(i))}});
	}

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score"};

	NodeTopKFastPathOperator op(dm, im, config, requirements, {}, {{"score", "score"}}, "score", true,
								static_cast<int64_t>(rowCount));
	op.open();
	auto first = op.next();
	auto second = op.next();

	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(first->size(), PhysicalOperator::DEFAULT_BATCH_SIZE);
	EXPECT_EQ(second->size(), 5U);
	EXPECT_EQ(readString((*first)[0], "score"), "0");
	EXPECT_EQ(readString((*second)[4], "score"), std::to_string(rowCount - 1));
	EXPECT_FALSE(op.next().has_value());
	EXPECT_NE(op.toString().find("ASC LIMIT"), std::string::npos);
}

TEST_F(NodeTopKFastPathOperatorTest, HandlesEmptyProjectionAndEmptyResult) {
	addPerson({{"id", PropertyValue("first")}, {"score", PropertyValue(int64_t{1})}, {"country", PropertyValue("US")}});

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score", "country"};

	NodeTopKFastPathOperator noProjection(dm, im, config, requirements, {}, {}, "score", false, 1);
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();
	noProjection.open();
	EXPECT_FALSE(noProjection.next().has_value());
	EXPECT_TRUE(noProjection.getOutputVariables().empty());
	EXPECT_TRUE(debug::PerfTrace::snapshotAndReset().contains("node_scan.topk"));
	debug::PerfTrace::setEnabled(false);

	VectorizedPropertyPredicate predicate;
	predicate.variable = "u";
	predicate.propertyKey = "country";
	predicate.op = VectorPredicateOp::VPO_EQ;
	predicate.value = PropertyValue("CN");

	NodeTopKFastPathOperator noMatch(dm, im, config, requirements, {predicate}, {{"id", "id"}}, "score", false, 1);
	noMatch.open();
	EXPECT_FALSE(noMatch.next().has_value());
}

TEST_F(NodeTopKFastPathOperatorTest, CloseOpenAllowsReExecution) {
	addPerson({{"id", PropertyValue("first")}, {"score", PropertyValue(int64_t{1})}});

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score"};

	NodeTopKFastPathOperator op(dm, im, config, requirements, {}, {{"id", "id"}}, "score", false, 1);
	op.open();
	ASSERT_TRUE(op.next().has_value());
	op.close();

	addPerson({{"id", PropertyValue("second")}, {"score", PropertyValue(int64_t{2})}});
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(readString((*batch)[0], "id"), "second");
	EXPECT_EQ(op.getOutputVariables(), (std::vector<std::string>{"id"}));
	EXPECT_NE(op.toString().find("NodeTopKFastPath"), std::string::npos);
}
