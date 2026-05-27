#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <unordered_map>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/operators/NodeCountFastPathOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

class NodeCountFastPathOperatorTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	fs::path testFilePath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_node_count_fast_path_" + boost::uuids::to_string(uuid) + ".dat");
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

TEST_F(NodeCountFastPathOperatorTest, CountsActiveLabelCandidates) {
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

	NodeCountFastPathOperator op(dm, im, config, requirements, {}, "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 2);
}

TEST_F(NodeCountFastPathOperatorTest, CountsRowsMatchingPropertyPredicate) {
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

	NodeCountFastPathOperator op(dm, im, config, requirements, {predicate}, "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 1);
}

TEST_F(NodeCountFastPathOperatorTest, ReturnsNulloptAfterFirstBatch) {
	addPerson();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.countOnly = true;

	NodeCountFastPathOperator op(dm, im, config, requirements, {}, "count");
	op.open();
	EXPECT_TRUE(op.next().has_value());
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(NodeCountFastPathOperatorTest, EmptyCandidatesProduceCountZero) {
	NodeScanConfig config;
	config.type = ScanType::LABEL_SCAN;
	config.variable = "n";
	config.labels = {"Missing"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.countOnly = true;

	NodeCountFastPathOperator op(dm, im, config, requirements, {}, "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(readCount(*batch), 0);
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(NodeCountFastPathOperatorTest, CloseOpenAllowsReExecution) {
	addPerson();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.countOnly = true;

	NodeCountFastPathOperator op(dm, im, config, requirements, {}, "count");
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

TEST_F(NodeCountFastPathOperatorTest, GetOutputVariablesAndToStringDescribeOperator) {
	NodeScanConfig config;
	config.variable = "n";
	NodeScanRequirements requirements;

	NodeCountFastPathOperator op(dm, im, config, requirements, {}, "total");

	EXPECT_EQ(op.getOutputVariables(), (std::vector<std::string>{"total"}));
	EXPECT_NE(op.toString().find("NodeCountFastPath"), std::string::npos);
	EXPECT_NE(op.toString().find("total"), std::string::npos);
}

TEST_F(NodeCountFastPathOperatorTest, PerfTraceEmitsNodeScanCountWhenEnabled) {
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

	NodeCountFastPathOperator op(dm, im, config, requirements, {}, "count");
	op.open();
	ASSERT_TRUE(op.next().has_value());

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
}
