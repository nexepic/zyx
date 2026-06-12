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
#include "graph/query/execution/operators/NodeCountScanOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

class NodeCountScanOperatorTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	fs::path testFilePath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath =
				fs::temp_directory_path() / ("test_node_count_scan_path_" + boost::uuids::to_string(uuid) + ".dat");
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

TEST_F(NodeCountScanOperatorTest, CountsActiveLabelCandidates) {
	addPerson();
	addPerson();
	addLabeledNode({"Animal"});

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};

	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.countOnly = true;

	NodeCountScanOperator op(dm, im, config, requirements, {}, "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 2);
}

TEST_F(NodeCountScanOperatorTest, CountsRowsMatchingPropertyPredicate) {
	addPerson({{"age", PropertyValue(int64_t{42})}});
	addPerson({{"age", PropertyValue(int64_t{7})}});
	addPerson({{"name", PropertyValue("Alice")}});

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
	predicate.op = VectorPredicateOp::VPO_EQ;
	predicate.value = PropertyValue(int64_t{42});

	NodeCountScanOperator op(dm, im, config, requirements, {predicate}, "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 1);
}

TEST_F(NodeCountScanOperatorTest, CountOnlyPropertyPredicateUsesMetadataBatchWhenClean) {
	static constexpr size_t kNodeCount = 300;
	for (size_t i = 0; i < kNodeCount; ++i) {
		addPerson({{"age", PropertyValue(static_cast<int64_t>(i % 3))}});
	}
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
	requirements.requiredProperties = {"age"};
	requirements.countOnly = true;

	VectorizedPropertyPredicate predicate;
	predicate.variable = "n";
	predicate.propertyKey = "age";
	predicate.op = VectorPredicateOp::VPO_EQ;
	predicate.value = PropertyValue(int64_t{1});

	NodeCountScanOperator op(dm, im, config, requirements, {predicate}, "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 100);
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(snapshot.contains("node_scan.load_node_metadata"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_nodes"));
	EXPECT_FALSE(snapshot.contains("node_scan.bulk_load_nodes"));
}

TEST_F(NodeCountScanOperatorTest, RepeatsPropertyPredicateCountsWithoutResultCache) {
	static constexpr size_t kNodeCount = 300;
	for (size_t i = 0; i < kNodeCount; ++i) {
		addPerson({{"age", PropertyValue(static_cast<int64_t>(i % 3))}});
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

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
	predicate.op = VectorPredicateOp::VPO_EQ;
	predicate.value = PropertyValue(int64_t{1});

	NodeCountScanOperator first(dm, im, config, requirements, {predicate}, "count");
	first.open();
	auto firstBatch = first.next();
	ASSERT_TRUE(firstBatch.has_value());
	EXPECT_EQ(readCount(*firstBatch), 100);

	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();
	NodeCountScanOperator second(dm, im, config, requirements, {predicate}, "count");
	second.open();
	auto secondBatch = second.next();
	ASSERT_TRUE(secondBatch.has_value());
	EXPECT_EQ(readCount(*secondBatch), 100);
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
	EXPECT_TRUE(snapshot.contains("node_scan.load_node_metadata"));
	EXPECT_FALSE(snapshot.contains("node_scan.count_cache"));
}

TEST_F(NodeCountScanOperatorTest, SkipsRedundantChecksForLabelIndexCandidatesWithPredicates) {
	ASSERT_TRUE(im->createIndex("idx_person_label_count_operator", "node", "", ""));
	for (int64_t i = 0; i < 300; ++i) {
		addPerson({{"age", PropertyValue(i % 3)}});
	}
	addLabeledNode({"Animal"}, {{"age", PropertyValue(int64_t{1})}});
	db->getStorage()->flush();
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	NodeScanConfig config;
	config.type = ScanType::LABEL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"age"};
	requirements.countOnly = true;

	VectorizedPropertyPredicate predicate;
	predicate.variable = "n";
	predicate.propertyKey = "age";
	predicate.op = VectorPredicateOp::VPO_EQ;
	predicate.value = PropertyValue(int64_t{1});

	NodeCountScanOperator op(dm, im, config, requirements, {predicate}, "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 100);
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
	EXPECT_FALSE(snapshot.contains("node_scan.label_check"));
}

TEST_F(NodeCountScanOperatorTest, ReturnsNulloptAfterFirstBatch) {
	addPerson();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.countOnly = true;

	NodeCountScanOperator op(dm, im, config, requirements, {}, "count");
	op.open();
	EXPECT_TRUE(op.next().has_value());
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(NodeCountScanOperatorTest, EmptyCandidatesProduceCountZero) {
	NodeScanConfig config;
	config.type = ScanType::LABEL_SCAN;
	config.variable = "n";
	config.labels = {"Missing"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.countOnly = true;

	NodeCountScanOperator op(dm, im, config, requirements, {}, "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 0);
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(NodeCountScanOperatorTest, CloseOpenAllowsReExecution) {
	addPerson();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.countOnly = true;

	NodeCountScanOperator op(dm, im, config, requirements, {}, "count");
	op.open();
	ASSERT_TRUE(op.next().has_value());
	EXPECT_FALSE(op.next().has_value());
	op.close();

	addPerson();
	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 2);
}

TEST_F(NodeCountScanOperatorTest, GetOutputVariablesAndToStringDescribeOperator) {
	NodeScanConfig config;
	config.variable = "n";
	NodeScanRequirements requirements;

	NodeCountScanOperator op(dm, im, config, requirements, {}, "total");

	EXPECT_EQ(op.getOutputVariables(), (std::vector<std::string>{"total"}));
	EXPECT_NE(op.toString().find("NodeCountScan"), std::string::npos);
	EXPECT_NE(op.toString().find("total"), std::string::npos);
}

TEST_F(NodeCountScanOperatorTest, PerfTraceEmitsNodeScanCountWhenEnabled) {
	addPerson();
	debug::PerfTrace::reset();
	debug::PerfTrace::setEnabled(true);

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.countOnly = true;

	NodeCountScanOperator op(dm, im, config, requirements, {}, "count");
	op.open();
	ASSERT_TRUE(op.next().has_value());

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
}

TEST_F(NodeCountScanOperatorTest, CountsFullScanCandidatesDirectlyWhenChecksAreSatisfiedByRequirements) {
	addPerson();
	addLabeledNode({"Animal"});

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.countOnly = true;
	requirements.needsActiveCheck = false;
	requirements.needsLabels = false;

	NodeCountScanOperator op(dm, im, config, requirements, {}, "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 2);
}

TEST_F(NodeCountScanOperatorTest, NonCountOnlyRequirementFallsBackToBatchCounting) {
	addPerson();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.countOnly = false;
	requirements.needsActiveCheck = false;
	requirements.needsLabels = false;

	NodeCountScanOperator op(dm, im, config, requirements, {}, "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 1);
}

TEST_F(NodeCountScanOperatorTest, PredicateDisablesDirectCandidateCounting) {
	addPerson({{"age", PropertyValue(int64_t{42})}});

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.countOnly = true;
	requirements.needsActiveCheck = false;
	requirements.needsLabels = false;
	VectorizedPropertyPredicate predicate;
	predicate.variable = "n";
	predicate.propertyKey = "age";
	predicate.op = VectorPredicateOp::VPO_EQ;
	predicate.value = PropertyValue(int64_t{42});

	NodeCountScanOperator op(dm, im, config, requirements, {predicate}, "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 0);
}

TEST_F(NodeCountScanOperatorTest, DirectCountFallsBackWhenResidualLabelsAreRequired) {
	ASSERT_TRUE(im->createIndex("idx_age_global_count", "node", "", "age"));
	addPerson({{"age", PropertyValue(int64_t{42})}});
	addLabeledNode({"Animal"}, {{"age", PropertyValue(int64_t{42})}});

	NodeScanConfig config;
	config.type = ScanType::PROPERTY_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	config.indexKey = "age";
	config.indexValue = PropertyValue(int64_t{42});
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.countOnly = true;

	NodeCountScanOperator op(dm, im, config, requirements, {}, "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 1);
}

TEST_F(NodeCountScanOperatorTest, CandidateCountingChecksResidualLabelsWhenActiveCheckIsDisabled) {
	addPerson();
	addLabeledNode({"Animal"});

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.countOnly = true;
	requirements.needsActiveCheck = false;
	requirements.needsLabels = true;

	NodeCountScanOperator op(dm, im, config, requirements, {}, "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 1);
}

TEST_F(NodeCountScanOperatorTest, DirectIndexCountHonorsDisabledRequirementChecks) {
	addPerson();
	addPerson();
	ASSERT_TRUE(im->createIndex("idx_person_label_count", "node", "", ""));

	NodeScanConfig config;
	config.type = ScanType::LABEL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.countOnly = true;
	requirements.needsActiveCheck = false;
	requirements.needsLabels = false;

	NodeCountScanOperator op(dm, im, config, requirements, {}, "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 2);
}
